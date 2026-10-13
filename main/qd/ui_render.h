/*
 * ui_render.h —— 228×460 主界面渲染器（纯 C，无 ESP-IDF 依赖；固件与主机模拟器共用）
 *
 * 用法（每帧）：
 *   ui_frame_begin(&state, &cells);          // 算每帧常量 + 渲染槽格带（约 73 行，含 2D 景深）
 *   for (y = 0; y < 460; y += STRIP_H)
 *       ui_render_rows(y, y + STRIP_H, buf);  // 输出 RGB565，大端（SPI 线序），宽 228
 *
 * 内存：槽格带缓冲 73×228×3×2 ≈ 100 KB（静态，放内部 SRAM）。
 *       条带缓冲由调用方分配（DMA 可达），每行 456 B。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "demo_state.h"

#define UI_W 228
#define UI_H 460

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(void);
void ui_frame_begin(const qd_state_t *s, const qd_cells_t *c);

/* ui_frame_begin 的分步版，供双核并行（本工程 qd_app.c 用）：
 *   ui_frame_setup → [composite 按行] → 同步 → [hblur 按行] → 同步 → [vblur 按列] → 同步
 * 行号相对槽格带起点，范围 [0, UI_BAND_ROWS)；列范围 [0, UI_W)。 */
#define UI_BAND_ROWS 73
void ui_frame_setup(const qd_state_t *s, const qd_cells_t *c);
void ui_band_composite(int r0, int r1);
void ui_band_hblur(int r0, int r1);
void ui_band_vblur(int x0, int x1);
/* 渲染 [y0, y1) 行到 dst（UI_W × (y1−y0) 个 uint16，大端 RGB565）。可从两个核并行调用不同行段 */
void ui_render_rows(int y0, int y1, uint16_t *dst);

/* 点亮测试图：色条 + 四角标记 + 偶数对齐格。用来确认偏移、颜色顺序、字节序 */
void ui_test_pattern_rows(int y0, int y1, uint16_t *dst, int phase);

#ifdef __cplusplus
}
#endif
