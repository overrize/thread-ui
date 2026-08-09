#pragma once
#include <stdint.h>

enum { MODE_PRECISE = 0, MODE_STD = 1, MODE_RATCHET = 2, MODE_COUNT = 3 };

void thread_init(void);
void thread_set_mode(int mode);
void thread_set_color(const uint8_t base[3]);
void thread_mode_color(int mode, uint8_t out[3]);                 /* 导出模式基色 */
void thread_advance(float rpm, float dt, int reverse);
void thread_detent(void);
void thread_render(void);
void thread_get_base_color(uint8_t out[3]);
