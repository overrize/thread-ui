/*
 * demo_state.h —— 演示状态机（与 prototype/screwdriver-ui.html 的 step() / ratchetStep() / prepCells() 一致）
 *
 * 纯 C、无 ESP-IDF 依赖：固件和 tools/host_sim 共用同一份代码。
 * 所有时长单位都是秒；动画按真实经过时间推进，不按帧（换帧率观感不变）。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define QD_MODES  4
#define QD_CELLS 15

typedef struct {
    const char *name;
    uint8_t col[3];          /* 模式色 */
    double tq, tq_max;        /* 目标扭矩 / 上限（N·m） */
    double rpm, rpm_max;      /* 目标转速 / 上限；棘轮为 0 */
    bool turbo;
} qd_mode_t;

extern const qd_mode_t QD_MODE[QD_MODES];

typedef struct {
    int   mode;
    bool  rev;               /* false = 正转拧紧，true = 反转松出 */
    double theta;            /* 转子角（rad）。用 double：连续运行几小时也不丢精度 */
    double th0, th1;         /* 棘轮行程起点 / 终点 */
    double rpm, load, torque, peak, rt, sweep;
    bool  done, pd, was_on;
    int   clicks;
    int   ph;                /* 棘轮：0 无，1 驱动行程，2 回程 */
    double ph_t, click_flash;
    /* 槽格 */
    bool  draining, drained;
    double rel_at, done_at;
    bool  rel_rev_valid; double rel_rev;
    double kd[QD_CELLS];
    bool  have_last_prep; double last_prep;
    /* 演示 */
    double now;               /* 秒 */
    double demo_t; int cycle;
    bool  calm;              /* 减少动态 */
    double speed;             /* 动画速度倍率（默认 1.0） */
} qd_state_t;

typedef struct {
    double   k[QD_CELLS];     /* 每格亮度 0..1 */
    uint8_t st[QD_CELLS];    /* 沿旋向序号 */
    bool    hold[QD_CELLS];  /* 峰值空心框 */
    double   add[QD_CELLS];   /* 叠加白光（到位冲击波） */
} qd_cells_t;

void qd_state_init(qd_state_t *s, int mode);
/* 自动演示：6.5 s 一个周期，0.6–4.4 s 按住；每个周期换一次旋向。返回本周期是否刚结束 */
bool qd_demo_tick(qd_state_t *s, double dt, bool *on);
void qd_state_step(qd_state_t *s, double dt, bool on);
void qd_cells_prep(qd_state_t *s, qd_cells_t *c);
/* 松出时右侧是否显示峰值 */
bool qd_show_peak(const qd_state_t *s);
static inline bool qd_is_ratchet(const qd_state_t *s) { return QD_MODE[s->mode].rpm_max == 0; }
