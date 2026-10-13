/*
 * demo_state.c —— 从原型逐行移植。改行为时先改原型，再同步这里。
 */
#include "demo_state.h"
#include <math.h>
#include <string.h>

#define PI_F      3.14159265358979
#define SLOTS     12
#define PITCH     (2.0 * 3.14159265358979323846 / SLOTS)

/* 四个模式（SPEC §4）。模式色全部避开铜色一带 */
const qd_mode_t QD_MODE[QD_MODES] = {
    {"精密",  {0x38, 0xBD, 0xFF}, 0.35, 1.00, 200, 300, false},
    {"标准",  {0xF2, 0x8E, 0x4A}, 1.20, 3.00, 450, 900, false},
    {"棘轮",  {0x4B, 0xE0, 0x8A}, 2.40, 3.00,   0,   0, false},
    {"TURBO", {0xFF, 0x3B, 0x7A}, 3.00, 3.00, 900, 900, true },
};

/* 转动（本工程约定，替代 SPEC §3.4 的 gamma 0.6 + 按 60fps 标定的每帧封顶）：
 *   转子速度按时间定义、与 rpm 严格线性：rate = MAX_RATE × rpm / 900（槽距/秒）
 *   精密 200 → 1.6 · 标准 450 → 3.6 · TURBO 900 → 7.2 槽距/秒（= 原规格 60fps 下的 0.12 槽距/帧）
 *   每帧只保留防频闪硬上限 FRAME_CAP（< 0.5 槽距）。
 * 原实现在 30fps 下每帧需求翻倍，标准(0.16)和 TURBO(0.24)都被 0.12 截断，看起来一样快。 */
#define MAX_RATE   7.2      /* 槽距/秒 @ 900 rpm */
#define CALM_RATE  3.0      /* 减少动态：= 原 CALM_STEP 0.05 × 60 */
#define RPM_REF    900.
#define FRAME_CAP  0.45     /* 槽距/帧，防频闪/倒转 */

/* 棘轮（SPEC §4.1） */
#define STROKE     (PITCH * 1.5)
#define T_DRIVE    0.42
#define T_RETURN   0.30
static const double CLICKS[3] = {0.06, 0.135, 0.21};

/* 槽格（SPEC §5.3） */
#define PEAK_SHOW     4.0
#define HOLD_READ     1.1
#define DRAIN_DELAY   0.30
#define DRAIN_STAGGER 0.035
#define DRAIN_FADE    0.40
#define FADE_RATE     2.2

static inline double clampf(double v, double a, double b) { return v < a ? a : (v > b ? b : v); }
static inline double minf(double a, double b) { return a < b ? a : b; }
static inline double maxf(double a, double b) { return a > b ? a : b; }
static inline double ease(double t) { return t * t * (3 - 2 * t); }   /* smoothstep，无过冲 */
static inline double dir_sign(const qd_state_t *s) { return s->rev ? -1.0 : 1.0; }

void qd_state_init(qd_state_t *s, int mode)
{
    bool calm = s->calm; double speed = s->speed > 0 ? s->speed : 1.0;
    double now = s->now, demo_t = s->demo_t; int cycle = s->cycle; bool rev = s->rev;
    memset(s, 0, sizeof(*s));
    s->mode = mode; s->calm = calm; s->speed = speed;
    s->now = now; s->demo_t = demo_t; s->cycle = cycle; s->rev = rev;
}

bool qd_demo_tick(qd_state_t *s, double dt, bool *on)
{
    double prev = s->demo_t;
    s->demo_t = fmod(s->demo_t + dt, 6.5);
    bool wrapped = s->demo_t < prev;
    if (wrapped) { s->cycle++; s->rev = (s->cycle % 2) == 1; }
    *on = s->demo_t > 0.6 && s->demo_t < 4.4;
    return wrapped;
}

static void ratchet_step(qd_state_t *s, double dt, const qd_mode_t *M, bool tighten)
{
    if (s->ph == 0) { s->ph = 1; s->ph_t = 0; s->th0 = s->theta; }
    double dir = dir_sign(s);
    s->ph_t += dt;
    if (s->ph == 1) {
        double t = minf(1, s->ph_t / T_DRIVE);
        s->theta = s->th0 + dir * STROKE * ease(t);
        if (t >= 1) {
            s->ph = 2; s->ph_t = 0; s->th1 = s->th0 + dir * STROKE; s->clicks++;
            if (tighten) {
                s->torque = minf(M->tq, round((s->torque + M->tq * 0.14) * 1000) / 1000);
                if (s->torque >= M->tq) s->done = true;
            }
        }
    } else {
        /* 回程：转子完全静止（v1.4.3 起不再"顶一下回弹"），"咔"只体现在槽格微亮 */
        s->theta = s->th1;
        double f = 0;
        for (int i = 0; i < 3; i++) f = maxf(f, 1 - fabs(s->ph_t - CLICKS[i]) / 0.03);
        s->click_flash = f;
        if (s->ph_t >= T_RETURN) { s->ph = 1; s->ph_t = 0; s->th0 = s->th1; s->theta = s->th1; }
    }
    s->rpm = 0;
}

void qd_state_step(qd_state_t *s, double dt, bool on)
{
    double th_prev = s->theta;
    const qd_mode_t *M = &QD_MODE[s->mode];
    bool rat = qd_is_ratchet(s);

    if (on && !s->was_on) { s->rt = 0; s->peak = 0; s->done = false; s->draining = false; s->drained = false; }
    if (!on && s->was_on && s->done && !s->rev) { s->draining = true; s->rel_at = s->now; }
    if (!on && s->was_on && s->rev) { s->rel_rev = s->now; s->rel_rev_valid = true; }
    if (on && !s->was_on) s->rel_rev_valid = false;
    if (s->done && !s->pd) s->done_at = s->now;
    s->pd = s->done;
    s->was_on = on;

    double breakaway = M->tq * 0.85;
    if (on && !(s->done && !s->rev)) {
        s->rt += dt; s->load = minf(1, s->load + dt * 0.9);
        if (!s->rev) {
            if (rat) ratchet_step(s, dt, M, true);
            else {
                s->torque = minf(M->tq_max, s->torque + dt * 0.85 * (1 - s->torque / (M->tq * 1.3)));
                double droop = 1 - 0.45 * (s->torque / maxf(0.01, M->tq));
                s->rpm += (M->rpm * maxf(0.15, droop) - s->rpm) * minf(1, dt * 3.2);
                if (s->torque >= M->tq) { s->torque = M->tq; s->done = true; }
            }
        } else {
            /* 松出：先冲到破坏扭矩，松动后迅速回落，转速随之上来 */
            double tgt = s->rt < 0.7 ? breakaway * sin(minf(1, s->rt / 0.7) * PI_F / 2) : breakaway * 0.12;
            s->torque += (tgt - s->torque) * minf(1, dt * (s->rt < 0.7 ? 12 : 2.5));
            s->peak = maxf(s->peak, s->torque);
            if (rat) ratchet_step(s, dt, M, false);
            else { double tr = s->rt < 0.7 ? M->rpm * 0.08 : M->rpm; s->rpm += (tr - s->rpm) * minf(1, dt * 3); }
        }
    } else if (!on) {
        s->load += (0 - s->load) * minf(1, dt * 4);
        s->rpm += (0 - s->rpm) * minf(1, dt * 2.2); if (s->rpm < 3) s->rpm = 0;
        bool holding = s->draining && s->now - s->rel_at < HOLD_READ;
        if (!holding) {
            s->torque += (0 - s->torque) * minf(1, dt * 3);
            if (s->torque < 0.02) { s->torque = 0; s->done = false; s->clicks = 0; }
        }
        if (rat && s->ph != 0) {                 /* 松手：停在当前行程的落点 */
            if (s->ph == 1) s->theta = s->th0 + dir_sign(s) * STROKE * ease(minf(1, s->ph_t / T_DRIVE));
            s->ph = 0; s->click_flash = 0;
        }
    }

    if (!rat) {
        double u = minf(1, fabs(s->rpm) / RPM_REF);
        double rate = (s->calm ? CALM_RATE : MAX_RATE) * u * s->speed;   /* 槽距/秒 */
        double step = fmin(FRAME_CAP * PITCH, rate * PITCH * dt);
        s->theta += dir_sign(s) * step;
    }
    s->sweep = (double)fabs(s->theta - th_prev);
}

bool qd_show_peak(const qd_state_t *s)
{
    return s->rev && s->peak > 0 && !(s->rel_rev_valid && s->now - s->rel_rev > PEAK_SHOW);
}

void qd_cells_prep(qd_state_t *s, qd_cells_t *c)
{
    double prep_dt = s->have_last_prep ? minf(0.1, maxf(0, s->now - s->last_prep)) : 0;
    s->last_prep = s->now; s->have_last_prep = true;
    const qd_mode_t *M = &QD_MODE[s->mode];
    const int N = QD_CELLS;
    bool fwd = !s->rev;
    double frac = fwd ? clampf(s->torque / M->tq, 0, 1) : clampf(s->torque / M->tq_max, 0, 1);
    double v = frac * N;
    int hold_j = -1;
    if (qd_show_peak(s)) {
        hold_j = (int)floor(clampf(s->peak / M->tq_max, 0, 1) * N - 1e-6f);
        if (hold_j > N - 1) hold_j = N - 1;
    }
    double since = (s->done && fwd) ? s->now - s->done_at : 99;
    double wave = since < 0.35 ? since / 0.35 * (N + 2) - 1 : -99;
    for (int i = 0; i < N; i++) {
        int j = fwd ? i : N - 1 - i;
        double k = clampf(v - j, 0, 1);
        if (s->done && fwd && !s->drained) k = 1;
        if (s->drained) k = 0;
        if (s->draining) {
            double t = s->now - s->rel_at, delay = DRAIN_DELAY + j * DRAIN_STAGGER;
            k = 1 - clampf((t - delay) / DRAIN_FADE, 0, 1);
            if (t > DRAIN_DELAY + (N - 1) * DRAIN_STAGGER + DRAIN_FADE + 0.05) { s->draining = false; s->drained = true; }
        }
        double kd = s->kd[i];
        k = k >= kd ? k : maxf(k, kd - FADE_RATE * prep_dt);   /* 只升不骤降：变暗速率 ≤ 2.2/s */
        s->kd[i] = k;
        double wv = maxf(0, 1 - fabs(j - wave) / 1.4);
        c->k[i] = k; c->st[i] = (uint8_t)j; c->hold[i] = (j == hold_j); c->add[i] = minf(1, wv);
    }
}
