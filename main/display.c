/*
 * display.c —— SH8501A / 120x240 AMOLED，4 线 SPI，走 esp_lcd 的 panel_io 层
 *
 * 为什么不用 esp_lcd_panel 驱动：
 *   esp_lcd 官方没有 SH8501A 的 panel 驱动，同厂的 esp_lcd_sh8601 命令集接近但不完全一样。
 *   我们只需要「设窗口 + 推像素」，直接用 panel_io 反而更少坑，也更容易对着手册调。
 */

#include <string.h>
#include "display.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_sh8501.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "disp";

/* ---- 引脚：ESP32-S31 Korvo-1 实际接线（GPIO matrix, SPI2） ---- */
#define PIN_SCLK    9
#define PIN_MOSI   11
#define PIN_CS     12
#define PIN_DC     10
#define PIN_RST     8
#define PIN_TE     -1          /* 模块没引出。改板引出后填 GPIO 号，见 disp_wait_te() */

#define LCD_HOST        SPI2_HOST
#define LCD_PCLK_HZ     (40 * 1000 * 1000)   /* GPIO matrix 上限 40MHz；50MHz 需改接 IO MUX 专用脚 */

/* S31 GPSPI DMA 单次上限 32767 字节，分块越大间隙越少、像素流越均匀 */
#define LCD_MAX_TRANSFER_SZ  16384

/* 面板在控制器 GRAM 里的偏移。校准网格实测：左边框不可见、上边框偏细，
 * 先用 0，若画面左右/上下错位再改这里。 */
#define X_OFFSET   0
#define Y_OFFSET   0

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_done;

static bool on_trans_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

/* ------------------------------------------------------------------
 * 面板初始化
 *
 * 用已验证的厂商组件 esp_lcd_sh8501（osptek，组件在 components/ 下），
 * 内含这块 0.95" 120x240 玻璃的完整 GIP/gamma 初始化表。
 * 注意：命令走 32 位帧（00 00 00 <cmd>, DC 低），与该面板实测一致。
 * ------------------------------------------------------------------ */
esp_err_t disp_init(void)
{
    s_done = xSemaphoreCreateBinary();
    /* 约定：所有绘制函数在【改写缓冲前】调 disp_wait_done() 等上一笔 DMA 完成，
     * 而不是推完再等。预给一次信号量，让第一个等待者不会空等 100ms。 */
    xSemaphoreGive(s_done);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_MAX_TRANSFER_SZ,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_CS,
        .dc_gpio_num = PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 32,      /* 厂商帧格式：00 00 00 <cmd>, DC 低 */
        .lcd_param_bits = 8,
        .on_color_trans_done = on_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    sh8501_vendor_config_t vendor_config = {
        .flags = { .use_qspi_interface = 0 },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8501(s_io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    ESP_LOGI(TAG, "SH8501A ready, %dx%d @%d MHz", DISP_W, DISP_H, LCD_PCLK_HZ / 1000000);
    return ESP_OK;
}

/* 设窗口 + 推像素。px 必须是 DMA 可达的内部 RAM，且已按大端存好 */
void disp_blit(int x, int y, int w, int h, const uint16_t *px)
{
    uint8_t ca[4] = { (x + X_OFFSET) >> 8, (x + X_OFFSET) & 0xFF,
                      (x + X_OFFSET + w - 1) >> 8, (x + X_OFFSET + w - 1) & 0xFF };
    uint8_t pa[4] = { (y + Y_OFFSET) >> 8, (y + Y_OFFSET) & 0xFF,
                      (y + Y_OFFSET + h - 1) >> 8, (y + Y_OFFSET + h - 1) & 0xFF };

    esp_lcd_panel_io_tx_param(s_io, 0x2A, ca, 4);          /* CASET */
    esp_lcd_panel_io_tx_param(s_io, 0x2B, pa, 4);          /* PASET */
    esp_lcd_panel_io_tx_color(s_io, 0x2C, px, w * h * 2);  /* RAMWR + 数据，异步 DMA */
}

void disp_wait_done(void)
{
    xSemaphoreTake(s_done, pdMS_TO_TICKS(100));
}

/* 亮度 0~255。AMOLED 防烧屏、AOD 待机都用它 */
void disp_brightness(uint8_t v)
{
    esp_lcd_panel_io_tx_param(s_io, 0x51, &v, 1);
}

/* DMA 可达的内部 SRAM。帧缓冲绝对不要放 PSRAM */
void *disp_alloc(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}
