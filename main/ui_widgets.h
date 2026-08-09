#pragma once
#include <stdint.h>
#include <stdbool.h>

/* 状态条：模式徽标 | 转速 | 电量。locked 时整体灰显 */
void ui_draw_topbar(int mode, int rpm, bool locked, const uint8_t base[3]);

/* 压条：扭矩大数字（torque_x100 = 扭矩×100）+ 单位，颜色跟随模式基色。
 * turbo=true 时数字改红色（STD 第四档 TURBO）；
 * at_limit=true 时整个压条改为红色 "MAX" 印章（扭矩到上限，不是"到位"） */
void ui_draw_scrim(int torque_x100, const uint8_t base[3], bool turbo, bool at_limit);

/* 休眠屏：整屏暗色，居中显示时钟 "HH:MM"（RTC），下方电量占位框（电量待接硬件） */
void ui_draw_sleep(const char *clock_text);

/* ---- 通用绘制接口（菜单等模块用）：先 ui_buf_select 选缓冲，再绘制 ---- */
void ui_buf_select(uint16_t *buf, int w, int h);
uint16_t ui_rgb565_be(uint8_t r, uint8_t g, uint8_t b);
void ui_put(int x, int y, uint16_t color_be);
void ui_fill_rect(int x, int y, int w, int h, uint16_t color_be);
void ui_text(float x, float y, const char *str, float size, uint16_t color_host);
void ui_text_center(float cx, float y, const char *str, float size, uint16_t color_host);
void ui_text_right(float rx, float y, const char *str, float size, uint16_t color_host);
/* 竖排文本：逆时针 90°（从下往上读），(x, y) 为旋转后左上角 */
void ui_text_vertical(float x, float y, const char *str, float size, uint16_t color_host);
