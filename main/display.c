/*
 * display.c —— CO5300 / 1.6" 228x460 AMOLED（华夏彩 H0160S003AM），QSPI，走 esp_lcd 的 panel_io 层
 *
 * 为什么不用 esp_lcd_panel 驱动：
 *   我们只需要「设窗口 + 推像素」，直接用 panel_io 更少坑，也更容易对着手册调。
 *   规格书第 8 节初始化代码是空的，下面用 CO5300 通用最小序列，拿到厂家代码后替换 s_init。
 *
 * QSPI 帧格式（CO5300 / SH8601 同族）：
 *   写寄存器： 0x02 | 0x00 | cmd | 0x00 + 参数，全程单线
 *   写像素：   0x32 | 0x00 | 0x2C | 0x00 + 像素，地址单线、数据四线
 *   没有 DC 脚，命令和数据靠帧头区分。
 *
 * UI 仍按 120x240 画（display.h 的 DISP_W/H），居中贴到物理屏上；
 * 物理屏其余区域在 init 时清黑。后续重排版时再把 DISP_W/H 改成整屏。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "disp";

/* ---- 引脚：ESP32-S31 Korvo-1 + FPC-DIP 转接板，杜邦线 ----
 * 屏 FPC 脚号 → 信号 → GPIO。注意屏上 D(0)=D2、D(1)=D3。
 * VCI_EN 目前直接拉 3.3V，TE 暂未接。 */
#define PIN_SCLK   12          /* FPC18 WRX/SCL */
#define PIN_CS     10          /* FPC15 CSX     */
#define PIN_D0     11          /* FPC16 SDI/RDX */
#define PIN_D1     13          /* FPC17 DCX     */
#define PIN_D2     14          /* FPC20 D(0)    */
#define PIN_D3      9          /* FPC19 D(1)    */
#define PIN_RST     8          /* FPC14 RESET   */
#define PIN_TE     18          /* FPC21 TE：和 15/16/17 同组的空闲 Touch 脚，开机自检计数验证 */

#define LCD_HOST        SPI2_HOST
#define LCD_PCLK_HZ     (40 * 1000 * 1000)   /* 整帧 210KB 约 10.5ms，快于面板扫描 16.7ms；杜邦线花屏时降 20M */

/* S31 GPSPI DMA 单次上限 32767 字节，分块越大间隙越少、像素流越均匀 */
#define LCD_MAX_TRANSFER_SZ  32000           /* 接近 S31 DMA 单次上限 32767，整帧推送分块更少 */

/* 物理面板 */
#define PANEL_W    DISP_PANEL_W
#define PANEL_H    DISP_PANEL_H

/* 面板在控制器 GRAM 里的偏移。画面整体错位时改这里（CO5300 要求偶数） */
#define X_OFFSET   36          /* 串口交互校准实测：四边白框贴边、右侧无杂纹 */
#define Y_OFFSET   0

/* UI 区域在物理屏上的原点：居中。CO5300 窗口起点和宽高都要偶数，这两个值都是偶数 */
#define UI_X0      ((PANEL_W - DISP_W) / 2)
#define UI_Y0      ((PANEL_H - DISP_H) / 2)

/* 上电自检：RGBW 四色条停留时长，确认颜色顺序和对位后可改 0 */
#define SELFTEST_MS  1500

/* 偏移校准：开机进入串口交互校准，UI 不启动。
 * 串口按键 a/d 左右 ±2、A/D ±10，w/s 上下 ±2、W/S ±10，实时重画，不用重启。
 * 调到右侧杂纹消失、四边白框贴边后，把打印的值填进 X/Y_OFFSET 并改 0 */
#define CALIB_OFFSET  0

#define QSPI_CMD(c)    ((0x02u << 24) | ((uint32_t)(c) << 8))
#define QSPI_PIXEL     ((0x32u << 24) | (0x2Cu << 8))

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_done;
static SemaphoreHandle_t s_cnt;      /* 整屏条带推送用：每完成一笔 +1，配合 s_inflight 做双缓冲 */
static int s_inflight;
static SemaphoreHandle_t s_te;     /* TE 上升沿 = 面板进入消隐期 */
static bool s_te_ok;              /* 开机自检测到 TE 脉冲才启用同步 */
static uint32_t s_te_miss_total;  /* 累计等不到 TE 的次数 */
static int s_te_miss_run;         /* 连续等不到 TE 的次数：面板掉电复位后 TE 会停 */
static uint8_t s_bright = 0xFF;   /* 最近一次设置的亮度，面板恢复时重设 */

static bool on_trans_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    xSemaphoreGiveFromISR(s_cnt, &hp);
    return hp == pdTRUE;
}

static void wr(uint8_t cmd, const void *p, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, QSPI_CMD(cmd), p, n);
}

/* GRAM 原始坐标设窗口 + 推像素（异步 DMA） */
static void gram_blit(int x0, int y0, int w, int h, const uint16_t *px)
{
    int x1 = x0 + w - 1, y1 = y0 + h - 1;
    uint8_t ca[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    uint8_t ra[4] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };

    wr(0x2A, ca, 4);                                          /* CASET */
    wr(0x2B, ra, 4);                                          /* RASET */
    esp_lcd_panel_io_tx_color(s_io, QSPI_PIXEL, px, w * h * 2);
}

/* 物理屏坐标（加 GRAM 偏移） */
static int s_xoff = X_OFFSET, s_yoff = Y_OFFSET;   /* 校准模式下可改 */

static void panel_blit(int x, int y, int w, int h, const uint16_t *px)
{
    gram_blit(x + s_xoff, y + s_yoff, w, h, px);
}

/* 整屏按横条刷成纯色。RGBW 四色条用于确认颜色顺序和对位 */
static void panel_fill_bands(const uint16_t *colors, int n_bands)
{
    const int strip_h = 20;
    uint16_t *buf = disp_alloc(PANEL_W * strip_h * 2);
    if (!buf) {
        ESP_LOGE(TAG, "strip alloc failed");
        return;
    }
    for (int y = 0; y < PANEL_H; y += strip_h) {
        int h = (y + strip_h > PANEL_H) ? PANEL_H - y : strip_h;
        uint16_t c = colors[(y * n_bands) / PANEL_H];
        disp_wait_done();
        for (int i = 0; i < PANEL_W * h; i++) buf[i] = c;
        panel_blit(0, y, PANEL_W, h, buf);
    }
    disp_wait_done();
    xSemaphoreGive(s_done);      /* 保持「先给一次」的约定，见 disp_init */
    free(buf);
}

/* 校准图（物理屏坐标）：黑底 + 2px 白边框 + 四角色块 + 中心十字
 *   左上 红 / 右上 绿 / 左下 蓝 / 右下 黄。四角色块和白框都完整可见 = 偏移正确 */
static uint16_t calib_px(int x, int y)
{
    const int c = 16;
    if (x < 2 || y < 2 || x >= PANEL_W - 2 || y >= PANEL_H - 2) return RGB565(255, 255, 255);
    if (x < c && y < c) return RGB565(255, 0, 0);
    if (x >= PANEL_W - c && y < c) return RGB565(0, 255, 0);
    if (x < c && y >= PANEL_H - c) return RGB565(0, 0, 255);
    if (x >= PANEL_W - c && y >= PANEL_H - c) return RGB565(255, 255, 0);
    if (abs(x - PANEL_W / 2) < 1 || abs(y - PANEL_H / 2) < 1) return RGB565(128, 128, 128);
    return 0;
}

/* on=false 时在当前偏移处刷黑，用于移动前擦掉旧图 */
static void calib_draw(bool on)
{
    const int strip_h = 10;
    uint16_t *buf = disp_alloc(PANEL_W * strip_h * 2);
    if (!buf) {
        ESP_LOGE(TAG, "calib alloc failed");
        return;
    }
    for (int y = 0; y < PANEL_H; y += strip_h) {
        int h = (y + strip_h > PANEL_H) ? PANEL_H - y : strip_h;
        disp_wait_done();
        for (int r = 0; r < h; r++)
            for (int x = 0; x < PANEL_W; x++) buf[r * PANEL_W + x] = on ? calib_px(x, y + r) : 0;
        panel_blit(0, y, PANEL_W, h, buf);
    }
    disp_wait_done();
    xSemaphoreGive(s_done);
    free(buf);
}

static void calib_offset_loop(void)
{
    calib_draw(true);
    printf("\n=== offset calib: a/d x-+2  A/D x-+10  w/s y-+2  W/S y-+10 ===\n");
    printf("X_OFFSET=%d Y_OFFSET=%d\n", s_xoff, s_yoff);
    for (;;) {
        int ch = getchar();
        if (ch == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        int dx = 0, dy = 0;
        switch (ch) {
        case 'a': dx = -2;  break;
        case 'd': dx = 2;   break;
        case 'A': dx = -10; break;
        case 'D': dx = 10;  break;
        case 'w': dy = -2;  break;
        case 's': dy = 2;   break;
        case 'W': dy = -10; break;
        case 'S': dy = 10;  break;
        default: continue;
        }
        if (s_xoff + dx < 0 || s_yoff + dy < 0) continue;
        calib_draw(false);
        s_xoff += dx;
        s_yoff += dy;
        calib_draw(true);
        printf("X_OFFSET=%d Y_OFFSET=%d\n", s_xoff, s_yoff);
    }
}

static void IRAM_ATTR on_te(void *arg)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_te, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/* TE 自检：0x35 打开后 TE 每帧一个脉冲（60Hz）。轮询 200ms 数上升沿，
 * 约 12 个 = TE 线通 + 命令确实写进了屏；0 = 线没接/接错脚/命令没生效 */
static void te_selftest(void)
{
    gpio_config_t te = {
        .pin_bit_mask = 1ULL << PIN_TE,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&te);
    wr(0x35, (uint8_t[]){ 0x00 }, 1);        /* Sleep Out 之后再开一次 TE，防止睡眠中下发被忽略 */
    vTaskDelay(pdMS_TO_TICKS(20));
    int level0 = gpio_get_level(PIN_TE);
    int edges = 0, last = gpio_get_level(PIN_TE);
    for (int i = 0; i < 200 * 20; i++) {       /* 50us 采样一次，共 200ms */
        int v = gpio_get_level(PIN_TE);
        if (v && !last) edges++;
        last = v;
        esp_rom_delay_us(50);
    }
    if (edges >= 8 && edges <= 20) {
        ESP_LOGI(TAG, "TE ok: %d edges / 200ms on GPIO%d, vsync on", edges, PIN_TE);
        s_te = xSemaphoreCreateBinary();
        gpio_set_intr_type(PIN_TE, GPIO_INTR_POSEDGE);
        esp_err_t err = gpio_install_isr_service(0);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {   /* 别的模块已装过也行 */
            gpio_isr_handler_add(PIN_TE, on_te, NULL);
            s_te_ok = true;
        }
    } else {
        ESP_LOGW(TAG, "TE abnormal: %d edges / 200ms on GPIO%d (expect ~12), idle level %d",
                 edges, PIN_TE, level0);
    }
}

/* 硬件复位：低 ≥10ms，拉高后等 120ms */
static void panel_hw_reset(void)
{
    gpio_config_t rst = { .pin_bit_mask = 1ULL << PIN_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* CO5300 通用最小初始化（非厂家代码，拿到后替换），到 Sleep Out 为止 */
static void panel_cmd_init(void)
{
    wr(0xFE, (uint8_t[]){ 0x00 }, 1);        /* 切回用户命令页 */
    wr(0xC4, (uint8_t[]){ 0x80 }, 1);        /* SPI 模式控制：QSPI 写 */
    wr(0x3A, (uint8_t[]){ 0x55 }, 1);        /* RGB565 */
    wr(0x35, (uint8_t[]){ 0x00 }, 1);        /* TE 开（V-blank） */
    wr(0x36, (uint8_t[]){ 0x00 }, 1);        /* MADCTL：不旋转，RGB 顺序 */
    wr(0x53, (uint8_t[]){ 0x20 }, 1);        /* 亮度控制使能 */
    wr(0x63, (uint8_t[]){ 0xFF }, 1);        /* HBM 亮度上限 */
    wr(0x51, (uint8_t[]){ 0x00 }, 1);        /* 先 0 亮度，避免开屏闪花屏 */

    wr(0x11, NULL, 0);                       /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(120));
}

/* ------------------------------------------------------------------
 * 面板初始化
 *
 * 上电时序（规格书 9.2）：IOVCC → VCI → RESX 拉高 → 初始化 → Sleep Out → 写图 → Display On
 * IOVCC/VCI/VCI_EN/VBAT 目前都接开发板 3.3V 一起上（VBAT 规格 3.7~4.2V，3.3V 实测可点亮；
 * 单独电源会因模组漏电 ~0.3V 触发数控电源 REP 反灌保护）。这里只管 RESX 之后的部分。
 * ------------------------------------------------------------------ */
esp_err_t disp_init(void)
{
    s_done = xSemaphoreCreateBinary();
    s_cnt = xSemaphoreCreateCounting(32, 0);
    /* 约定：所有绘制函数在【改写缓冲前】调 disp_wait_done() 等上一笔 DMA 完成，
     * 而不是推完再等。预给一次信号量，让第一个等待者不会空等 100ms。 */
    xSemaphoreGive(s_done);

    panel_hw_reset();

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .data0_io_num = PIN_D0,
        .data1_io_num = PIN_D1,
        .data2_io_num = PIN_D2,
        .data3_io_num = PIN_D3,
        .data4_io_num = -1, .data5_io_num = -1,   /* 不显式置 -1 会默认 0，把 GPIO0 也占了 */
        .data6_io_num = -1, .data7_io_num = -1,
        .max_transfer_sz = LCD_MAX_TRANSFER_SZ,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,                 /* 整帧约 7 块，一次全部入队，CPU 不被卡住 */
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .on_color_trans_done = on_trans_done,
        .flags = { .quad_mode = 1 },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    panel_cmd_init();

    /* 规格书时序：先写图，再 Display On */
    static const uint16_t rgbw[4] = {
        RGB565(255, 0, 0), RGB565(0, 255, 0), RGB565(0, 0, 255), RGB565(255, 255, 255),
    };
    static const uint16_t black[1] = { 0 };
    panel_fill_bands(SELFTEST_MS ? rgbw : black, SELFTEST_MS ? 4 : 1);

    wr(0x29, NULL, 0);                       /* Display On */
    vTaskDelay(pdMS_TO_TICKS(10));
    disp_brightness(0x80);                   /* 自检含整条白色带；VBAT 暂接 3.3V，满亮度易欠压，先 50% */
    te_selftest();

    if (SELFTEST_MS) {
        ESP_LOGI(TAG, "self-test: top->bottom should be R/G/B/W");
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_MS));
        panel_fill_bands(black, 1);
    }
    if (CALIB_OFFSET) {
        calib_offset_loop();                 /* 不返回，UI 不启动 */
    }

    ESP_LOGI(TAG, "CO5300 ready, panel %dx%d, UI %dx%d @(%d,%d), QSPI %d MHz",
             PANEL_W, PANEL_H, DISP_W, DISP_H, UI_X0, UI_Y0, LCD_PCLK_HZ / 1000000);
    xQueueReset(s_cnt);                      /* 自检推送留下的计数清零，s_inflight 从 0 开始 */
    return ESP_OK;
}

/* UI 坐标设窗口 + 推像素。px 必须是 DMA 可达的内部 RAM，且已按大端存好 */
void disp_blit(int x, int y, int w, int h, const uint16_t *px)
{
    panel_blit(x + UI_X0, y + UI_Y0, w, h, px);
}

void disp_wait_done(void)
{
    xSemaphoreTake(s_done, pdMS_TO_TICKS(100));
}

/* 亮度 0~255。AMOLED 防烧屏、AOD 待机都用它 */
void disp_brightness(uint8_t v)
{
    s_bright = v;
    wr(0x51, &v, 1);
}

/* 整屏坐标（228x460，已含 GRAM 偏移）异步推一块。缓冲在传完前不能改 */
void disp_flush_panel(int x, int y, int w, int h, const uint16_t *px)
{
    s_inflight++;
    panel_blit(x, y, w, h, px);
}

/* 等下一个 TE 上升沿（面板开始消隐）。TE 不可用时立即返回 false */
bool disp_te_available(void) { return s_te_ok; }

bool disp_wait_te(int timeout_ms)
{
    if (!s_te_ok) return false;
    xSemaphoreTake(s_te, 0);                 /* 丢掉旧的，等新的一个 */
    if (xSemaphoreTake(s_te, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        s_te_miss_run = 0;
        return true;
    }
    s_te_miss_total++;
    s_te_miss_run++;
    return false;
}

uint32_t disp_te_misses(void) { return s_te_miss_total; }

/* 连续 10 帧没有 TE：面板多半掉电复位了（复位后睡眠 + 关显示 + TE 关，屏全黑，主控不知道） */
bool disp_panel_lost(void) { return s_te_ok && s_te_miss_run >= 10; }

/* 面板掉线后重新走一遍复位 + 初始化 + 开显示。调用方保证此时没有在途的整屏推送 */
void disp_panel_recover(void)
{
    ESP_LOGW(TAG, "panel lost (no TE for %d frames), re-init", s_te_miss_run);
    panel_hw_reset();
    panel_cmd_init();
    static const uint16_t black[1] = { 0 };
    panel_fill_bands(black, 1);
    wr(0x29, NULL, 0);                       /* Display On */
    vTaskDelay(pdMS_TO_TICKS(10));
    wr(0x51, &s_bright, 1);
    xQueueReset(s_cnt);
    s_inflight = 0;
    s_te_miss_run = 0;
}

/* 等到在途传输数 ≤ n（0 = 全部传完）。两块条带缓冲轮转时传 1 */
void disp_wait_inflight(int n)
{
    while (s_inflight > n) {
        if (xSemaphoreTake(s_cnt, pdMS_TO_TICKS(200)) == pdTRUE) {
            s_inflight--;
        } else {
            ESP_LOGW(TAG, "flush timeout (inflight=%d)", s_inflight);
            s_inflight = n;
        }
    }
}

/* DMA 可达的内部 SRAM。帧缓冲绝对不要放 PSRAM */
void *disp_alloc(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}
