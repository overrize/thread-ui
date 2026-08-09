/*
 * knob_uart.c —— 与 ESP32 触觉旋钮控制板的 Serial2 通信（ASCII 行协议）
 *
 * 我们是主机侧，旋钮板是从机，115200 8N1，每条命令一行（\n 结尾，≤64 字符）。
 *
 * 安全机制：旋钮板上电后 Serial2 默认锁定，锁定状态下只接受 magic/ping。
 * 必须先 magic\n 解锁（期望 OK MAGIC），之后才能切模式或查询状态。
 *
 * 命令集（Serial2 只允许这些，调参命令 width/range/wheelvp 等仅 USB 调试用）：
 *   magic  -> OK MAGIC          解锁
 *   ping   -> PONG 0/1          心跳（0=未解锁 1=已解锁）
 *   lock   -> OK LOCK           重新锁定
 *   status -> STAT mode=<mode>,m0=<rad>,vel=<rad/s>,pos=<knob_value>,sub=<sub>,wv=<rad/s>,uq0=<V>
 *   0..4   -> OK <mode>         模式切换：0 保护/下电，1 码盘/棘轮，2 回中拨轮，3 虚拟飞轮，4 速度测试
 *   vib <gain> -> OK vib <gain> 按键/点击震动反馈（CLICK 预设），gain 0..2，推荐 0.8..1.0
 *
 * mode 1 主动上报（解锁后周期推送，滚动事件从这里取增量）：
 *   KNOB value=<knob_value>,sub=<sub>,min=-48,max=48
 *   value=当前档位（默认 -48..48），sub=齿内位置（接近 0 在齿心）
 *
 * 错误返回：
 *   ERR LOCKED          未解锁就发控制命令（本侧收到后置回锁定态，重新握手）
 *   ERR UNSUPPORTED     不支持的命令
 *   ERR LINE_TOO_LONG   命令超过 64 字符
 *
 * 交互流程：magic -> OK MAGIC -> 1 -> OK 1 -> 监听 KNOB 上报，按需 status，退出前可 lock。
 *
 * 接线（S31 侧）：
 *   GPIO33 (TX) -> 旋钮板 GPIO16 (RX)
 *   GPIO34 (RX) <- 旋钮板 GPIO17 (TX)
 *   GND 共地，115200 8N1
 */

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "knob_uart.h"

static const char *TAG = "knob";

#define KNOB_UART       UART_NUM_1
#define PIN_KNOB_TX     33
#define PIN_KNOB_RX     34
#define KNOB_BAUD       115200

#define KNOB_LINE_MAX        80          /* 协议上限 64 字符 + 余量 */

static bool s_unlocked;
static knob_stat_t s_stat;
static volatile int s_pos_delta;    /* 待消费的 value 增量（主循环取走） */
static int s_last_value;
static bool s_last_value_valid;

/* 读一行（含超时），返回不含 \n 的长度，0=超时，-1=过长被截断 */
static int read_line(char *buf, int cap, int timeout_ms)
{
    int n = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint8_t ch;
        int r = uart_read_bytes(KNOB_UART, &ch, 1, pdMS_TO_TICKS(20));
        if (r == 1) {
            if (ch == '\n') {
                buf[n] = '\0';
                return n;
            }
            if (ch == '\r') {
                continue;
            }
            if (n < cap - 1) {
                buf[n++] = (char)ch;
            } else {
                /* 丢弃直到行尾 */
                while (uart_read_bytes(KNOB_UART, &ch, 1, pdMS_TO_TICKS(50)) == 1 && ch != '\n') {}
                buf[0] = '\0';
                return -1;
            }
        }
    }
    buf[0] = '\0';
    return 0;
}

static void send_raw(const char *line)
{
    uart_write_bytes(KNOB_UART, line, strlen(line));
    uart_write_bytes(KNOB_UART, "\n", 1);
}

bool knob_send(const char *cmd)
{
    if (!s_unlocked || !cmd || strlen(cmd) > 64) {
        return false;
    }
    send_raw(cmd);
    return true;
}

bool knob_set_mode(int mode)
{
    if (mode < 0 || mode > 4) {
        return false;
    }
    char cmd[2] = { (char)('0' + mode), '\0' };
    return knob_send(cmd);
}

bool knob_vib(float gain)
{
    /* 固件限制 gain 0..2，按键点击推荐 0.8..1.0 */
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 2.0f) gain = 2.0f;
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "vib %.2f", gain);
    return knob_send(cmd);
}

bool knob_is_unlocked(void)
{
    return s_unlocked;
}

void knob_get_status(knob_stat_t *out)
{
    *out = s_stat;
}

/* UI 主循环调用：取走累积的旋钮档位增量（0 = 无） */
int knob_take_pos_delta(void)
{
    int d = s_pos_delta;
    s_pos_delta = 0;
    return d;
}

static void handle_line(char *line)
{
    if (strncmp(line, "KNOB ", 5) == 0) {
        /* mode 1 主动上报：KNOB value=<knob_value>,sub=<sub>,min=-48,max=48 */
        int value, min, max;
        float sub;
        int n = sscanf(line, "KNOB value=%d,sub=%f,min=%d,max=%d",
                       &value, &sub, &min, &max);
        if (n == 4) {
            if (s_last_value_valid) {
                s_pos_delta += value - s_last_value;
            }
            s_last_value = value;
            s_last_value_valid = true;
        }
    } else if (strncmp(line, "STAT ", 5) == 0) {
        knob_stat_t st;
        int n = sscanf(line, "STAT mode=%d,m0=%f,vel=%f,pos=%d,sub=%f,wv=%f,uq0=%f",
                       &st.mode, &st.m0, &st.vel, &st.pos, &st.sub, &st.wv, &st.uq0);
        if (n == 7) {
            st.valid = true;
            s_stat = st;
        }
    } else if (strncmp(line, "ERR", 3) == 0) {
        ESP_LOGW(TAG, "board: %s", line);
        if (strcmp(line, "ERR LOCKED") == 0) {
            s_unlocked = false;
        }
    }
    /* OK MAGIC / OK <mode> / PONG x 在任务里按上下文处理，这里忽略 */
}

/* 发命令后等指定应答；期间的异步行（KNOB 上报等）交给 handle_line 吸收。
 * 对端可能在我们 reboot 前就已解锁并正在推 KNOB 流，不能假设下一行就是应答 */
static bool wait_reply(const char *expect, int timeout_ms)
{
    char line[KNOB_LINE_MAX];
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        int n = read_line(line, sizeof(line), 100);
        if (n > 0) {
            if (strcmp(line, expect) == 0) {
                return true;
            }
            handle_line(line);
        }
    }
    return false;
}

static void knob_task(void *arg)
{
    char line[KNOB_LINE_MAX];

    while (1) {
        /* --- 解锁握手 + 切到 mode 1（码盘） --- */
        if (!s_unlocked) {
            uart_flush_input(KNOB_UART);
            send_raw("magic");
            if (wait_reply("OK MAGIC", 600)) {
                send_raw("1");
                if (wait_reply("OK 1", 600)) {
                    s_unlocked = true;
                    s_last_value_valid = false;
                    ESP_LOGI(TAG, "knob board unlocked, mode 1 (detent)");
                    continue;
                }
                ESP_LOGW(TAG, "mode 1 switch not confirmed");
            } else {
                ESP_LOGW(TAG, "unlock not confirmed");
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* --- 监听 mode 1 主动上报（KNOB ...） --- */
        int n = read_line(line, sizeof(line), 500);
        if (n > 0) {
            handle_line(line);
        }
    }
}

void knob_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = KNOB_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(KNOB_UART, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(KNOB_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(KNOB_UART, PIN_KNOB_TX, PIN_KNOB_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(knob_task, "knob", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "knob uart on TX=%d RX=%d @%d", PIN_KNOB_TX, PIN_KNOB_RX, KNOB_BAUD);
}
