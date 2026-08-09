/*
 * input_wheel.c —— 横置滚轮（正交编码器）+ 轮子本身作为按键
 *
 * 手势语法（全局统一，不能有例外）：
 *   短按 = 前进/确定    双击 = 换模式（仅主界面）    长按 = 后退/取消
 *
 * 防误触：一旦检测到滚动就取消长按计时 —— 拧螺丝时手指压住轮子顺手转，
 * 不应该触发返回。
 */

#include "input_wheel.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Korvo-1 上 GPIO4/5/6 被 I2S 音频占用，改用空闲脚（仅复用 Touch/以太网，未用） */
#define PIN_ENC_A   15
#define PIN_ENC_B   16
#define PIN_BTN     17

#define DETENT_PULSES   4        /* EC11 一格四个边沿，按你的轮子实测改 */
#define LONG_MS       500
#define DBL_MS        260

static pcnt_unit_handle_t s_pcnt;
static wheel_cb_t s_cb;
static void *s_ctx;

static int64_t s_press_t;        /* 按下时刻 */
static int64_t s_last_click;     /* 上一次短按时刻，用于双击判定 */
static bool s_long_fired;
static bool s_moved_while_down;  /* 按住期间是否转过 */
static bool s_pending_click;

void wheel_init(wheel_cb_t cb, void *ctx)
{
    s_cb = cb; s_ctx = ctx;

    pcnt_unit_config_t uc = { .high_limit = 1000, .low_limit = -1000 };
    pcnt_new_unit(&uc, &s_pcnt);

    pcnt_glitch_filter_config_t gf = { .max_glitch_ns = 1000 };
    pcnt_unit_set_glitch_filter(s_pcnt, &gf);

    pcnt_chan_config_t ch_a = { .edge_gpio_num = PIN_ENC_A, .level_gpio_num = PIN_ENC_B };
    pcnt_chan_config_t ch_b = { .edge_gpio_num = PIN_ENC_B, .level_gpio_num = PIN_ENC_A };
    pcnt_channel_handle_t a, b;
    pcnt_new_channel(s_pcnt, &ch_a, &a);
    pcnt_new_channel(s_pcnt, &ch_b, &b);
    pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    pcnt_unit_enable(s_pcnt);
    pcnt_unit_clear_count(s_pcnt);
    pcnt_unit_start(s_pcnt);

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
}

/* 在 UI 任务里每 5ms 调一次 */
void wheel_poll(void)
{
    int64_t now = esp_timer_get_time() / 1000;

    /* --- 旋转 --- */
    int raw = 0;
    pcnt_unit_get_count(s_pcnt, &raw);
    int detents = raw / DETENT_PULSES;
    if (detents != 0) {
        pcnt_unit_clear_count(s_pcnt);
        if (s_press_t) s_moved_while_down = true;      /* 按住时转 -> 取消长按 */
        s_cb(WHEEL_SCROLL, detents, s_ctx);
    }

    /* --- 按键 --- */
    bool down = gpio_get_level(PIN_BTN) == 0;

    if (down && !s_press_t) {                           /* 刚按下 */
        s_press_t = now; s_long_fired = false; s_moved_while_down = false;
        s_cb(WHEEL_PRESS, 0, s_ctx);                    /* 按下沿立即上报（震动反馈挂这里） */
    }
    else if (down && s_press_t && !s_long_fired && !s_moved_while_down
             && now - s_press_t >= LONG_MS) {
        s_long_fired = true;
        s_pending_click = false;                        /* 长按吞掉待定的短按 */
        s_cb(WHEEL_LONG, 0, s_ctx);
    }
    else if (!down && s_press_t) {                      /* 抬起 */
        if (!s_long_fired && !s_moved_while_down) {
            if (s_pending_click && now - s_last_click <= DBL_MS) {
                s_pending_click = false;
                s_cb(WHEEL_DOUBLE, 0, s_ctx);
            } else {
                s_pending_click = true;
                s_last_click = now;
            }
        }
        s_press_t = 0;
    }

    /* 双击窗口过期，补发单击。主界面因此有 260ms 延迟，菜单里可以在
     * 上层直接把 WHEEL_CLICK 提前处理掉（菜单没有双击语义）。 */
    if (s_pending_click && now - s_last_click > DBL_MS) {
        s_pending_click = false;
        s_cb(WHEEL_CLICK, 0, s_ctx);
    }
}
