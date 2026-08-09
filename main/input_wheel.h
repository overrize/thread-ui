#pragma once

typedef enum { WHEEL_SCROLL, WHEEL_PRESS, WHEEL_CLICK, WHEEL_DOUBLE, WHEEL_LONG } wheel_evt_t;
typedef void (*wheel_cb_t)(wheel_evt_t evt, int delta, void *ctx);

void wheel_init(wheel_cb_t cb, void *ctx);
void wheel_poll(void);
