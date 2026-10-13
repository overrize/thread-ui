#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* ---- 屏幕分区（和 HTML 原型一致） ----
 *   0  .. 13   状态条：模式 / 转速 / 电量        —— 变化时才刷
 *  14  .. 185  螺纹动画区                        —— 每帧刷
 * 186  .. 239  底部压条：反馈扭矩大数字 + 设定上限条带 —— 变化时才刷
 */
#define DISP_W        120
#define DISP_H        240

/* 物理面板（CO5300 1.6" AMOLED）。新 UI 按整屏画，见 qd_app.c */
#define DISP_PANEL_W  228
#define DISP_PANEL_H  460

#define DISP_BAR_H     14
#define DISP_ANIM_Y    14
#define DISP_ANIM_H   172
#define DISP_SCRIM_Y  186
#define DISP_SCRIM_H   54

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

/* 整屏条带推送（不加 UI 居中原点，只加 GRAM 偏移） */
void  disp_flush_panel(int x, int y, int w, int h, const uint16_t *px);
void  disp_wait_inflight(int n);
bool  disp_wait_te(int timeout_ms);   /* TE 不可用时立即返回 false */
bool  disp_te_available(void);
uint32_t disp_te_misses(void);       /* 累计等不到 TE 的次数 */
bool  disp_panel_lost(void);         /* 连续多帧无 TE：面板掉电复位了 */
void  disp_panel_recover(void);      /* 重新复位 + 初始化面板 */
