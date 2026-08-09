#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* ---- 屏幕分区（和 HTML 原型一致） ----
 *   0  .. 13   状态条：模式 / 转速 / 电量        —— 变化时才刷
 *  14  .. 195  螺纹动画区                        —— 每帧刷
 * 196  .. 239  底部压条：扭矩大数字              —— 变化时才刷
 */
#define DISP_W        120
#define DISP_H        240

#define DISP_BAR_H     14
#define DISP_ANIM_Y    14
#define DISP_ANIM_H   182
#define DISP_SCRIM_Y  196
#define DISP_SCRIM_H   44

/* 螺纹贴图：一个牙距高，纵向无限重复 */
#define TILE_H         17

/* RGB565，按 SPI 线序（大端）存 */
#define RGB565(r, g, b)                                                   \
    (uint16_t)((((uint16_t)((r) & 0xF8) << 8 | (uint16_t)((g) & 0xFC) << 3 \
      | (uint16_t)(b) >> 3) >> 8) |                                        \
     (((uint16_t)((r) & 0xF8) << 8 | (uint16_t)((g) & 0xFC) << 3           \
      | (uint16_t)(b) >> 3) << 8))

esp_err_t disp_init(void);
void  disp_blit(int x, int y, int w, int h, const uint16_t *px);
void  disp_wait_done(void);
void  disp_brightness(uint8_t v);
void *disp_alloc(size_t bytes);
