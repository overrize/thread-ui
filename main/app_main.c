/*
 * app_main.c —— 60fps 主循环 + 交互状态机
 *
 * 交互模型：
 *   按下沿 = 立即震动反馈（不等手势判定）
 *   短按   = 解锁扭矩调节 / 菜单里选择（不开启任何内容）
 *   长按   = 进菜单 / 出菜单
 *   双击   = 主界面切换模式（精密 / 标准 / 棘轮）
 *   扭矩锁 = 防误触：上电/换模式/解锁后 8s 无滚动即锁定，锁定中滚动被忽略，
 *            状态条和压条灰显；和休眠是两回事
 *   休眠   = 30s 无操作自动降亮度，显示开机时长 + 电量占位（不是黑屏）；
 *            任意输入唤醒，动画回主界面
 *
 * 扭矩模型（按模式）：
 *   标准 STD：4 档 0.2/0.4/0.6/TURBO 1.0 Nm，TURBO 档整体变红（螺纹+数字）
 *   精密 PRE：0.02 Nm 步进，范围 0.02~0.60 Nm
 *   棘轮 RCH：固定 1.2 Nm（扭矩扳手上限），滚动不可调
 *
 * 螺纹动画速度只跟随主电机转速（待机停转，无慢转保底）。
 *
 * 时间分配（实测前的估算，@40MHz SPI）：
 *   合成动画区   ~0.3 ms   CPU
 *   推屏 43.7KB  ~7.0 ms   DMA，CPU 空闲
 *   数字脏矩形   ~0.9 ms   仅数值变化时
 *   余量         ~8 ms
 */

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "display.h"
#include "thread_anim.h"
#include "input_wheel.h"
#include "ui_widgets.h"
#include "menu.h"
#include "knob_uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "ui";

typedef enum { SCR_HOME, SCR_MENU, SCR_SLEEP } screen_t;

/* ---- 扭矩模型参数 ---- */
#define STD_GEARS         4
#define STD_TURBO_GEAR    3                       /* 第 4 档 = TURBO */
static const float STD_TORQUE[STD_GEARS] = { 0.20f, 0.40f, 0.60f, 1.00f };
#define PRECISE_STEP      0.02f
#define PRECISE_MIN       0.02f
#define PRECISE_MAX       0.60f
#define RATCHET_TORQUE    1.20f                   /* 扭矩扳手上限 */

/* ---- 休眠参数 ---- */
#define SLEEP_AFTER_US    (30 * 1000 * 1000)      /* 30s 无操作休眠 */
#define SLEEP_BRIGHTNESS  24                      /* 休眠亮度（AOD 式暗屏，不黑屏） */
#define WAKE_BRIGHTNESS   255

/* ---- 扭矩锁参数 ---- */
#define TQ_LOCK_AFTER_US  (8 * 1000 * 1000)       /* 解锁后 8s 无滚动自动回锁 */

static struct {
    screen_t screen;
    int   mode;
    int   std_gear;             /* STD 当前挡位 0..3 */
    float precise_torque;       /* PRECISE 当前扭矩 */
    int   rpm_set[MODE_COUNT];
    float rpm_now;
    bool  running, reverse;
    bool  tq_locked;            /* 扭矩锁：防误触。短按解锁，8s 无滚动自动回锁 */
    int64_t tq_activity;        /* 最近一次扭矩调节（解锁/滚动），us */
    bool  at_limit;             /* 扭矩到上限（待接主电调信号，见主循环 TODO） */
    /* 颜色动画：指数趋近目标色（WouoUI 式，天然可打断，无时间线） */
    uint8_t color_target[3];
    bool  color_anim;
    /* 模式切换的螺纹快速滚动（时间线 0.7s 正弦包络） */
    bool  spin; float spin_t;
    /* 棘轮 */
    float ratchet_acc; int teeth;
    /* 休眠计时 */
    int64_t last_activity;      /* 最近一次输入活动，us */
} S = {
    .screen = SCR_HOME, .mode = MODE_STD,
    .std_gear = 1,
    .precise_torque = 0.10f,
    .rpm_set = {200, 450, 300},
    .tq_locked = true,           /* 上电即锁，防止上手误调 */
};

static bool s_trigger;          /* 扳机：接你的握力/开关输入 */
static bool s_dirty_top = true, s_dirty_scrim = true;   /* UI 区域脏标记 */

/* TURBO 档的动画基色（螺纹、徽标一起变红） */
static const uint8_t TURBO_RED[3] = { 0xFF, 0x2D, 0x2D };

static float current_torque(void)
{
    switch (S.mode) {
    case MODE_STD:     return STD_TORQUE[S.std_gear];
    case MODE_PRECISE: return S.precise_torque;
    default:           return RATCHET_TORQUE;
    }
}

/* 当前模式/挡位对应的动画基色：STD 第 4 档 TURBO 为红，其余为模式基色 */
static void effective_color(uint8_t out[3])
{
    if (S.mode == MODE_STD && S.std_gear == STD_TURBO_GEAR) {
        out[0] = TURBO_RED[0]; out[1] = TURBO_RED[1]; out[2] = TURBO_RED[2];
    } else {
        thread_mode_color(S.mode, out);
    }
}

/* 颜色动画目标设为当前有效颜色（换模式、进出 TURBO 都用）。
 * 趋近过程在主循环里做，中途改目标直接从当前色继续，天然可打断 */
static void start_morph(void)
{
    effective_color(S.color_target);
    S.color_anim = true;
}

static void draw_scrim(void)
{
    uint8_t base[3];
    thread_get_base_color(base);
    if (S.tq_locked) {
        base[0] = base[1] = base[2] = 90;   /* 扭矩锁定时压条灰显：调不了 */
    }
    ui_draw_scrim((int)(current_torque() * 100 + 0.5f), base,
                  S.mode == MODE_STD && S.std_gear == STD_TURBO_GEAR && !S.tq_locked,
                  S.at_limit);
}

static void draw_topbar(void)
{
    uint8_t base[3];
    thread_get_base_color(base);
    ui_draw_topbar(S.mode, S.rpm_set[S.mode], S.tq_locked, base);
}

/* ---------------- 休眠 / 唤醒 ---------------- */

static char s_sleep_drawn[8];               /* 休眠屏已显示的时钟文本 */

static void enter_sleep(void)
{
    S.screen = SCR_SLEEP;
    disp_brightness(SLEEP_BRIGHTNESS);
    s_sleep_drawn[0] = '\0';                /* 强制主循环立即重画休眠屏 */
}

static void wake_screen(void)
{
    S.screen = SCR_HOME;                    /* 动画回归主界面 */
    disp_brightness(WAKE_BRIGHTNESS);
    s_dirty_top = true;
    s_dirty_scrim = true;
}

/* ---------------- 输入事件 ---------------- */
static void on_wheel(wheel_evt_t e, int d, void *ctx)
{
    S.last_activity = esp_timer_get_time();

    /* 物理按下沿：立即震动反馈，不参与手势判定（单击要等 260ms 双击窗口，太慢） */
    if (e == WHEEL_PRESS) {
        knob_vib(2.0f);
    }

    /* 休眠中：任何输入只负责唤醒，事件本身被吞掉 */
    if (S.screen == SCR_SLEEP) {
        wake_screen();
        return;
    }
    if (e == WHEEL_PRESS) {
        return;
    }

    if (S.screen == SCR_MENU) {
        /* 菜单态：全部交给菜单（短按=选择，长按=退出回主界面） */
        if (!menu_handle(e, d)) {
            S.screen = SCR_HOME;
            s_dirty_top = true;
            s_dirty_scrim = true;
        }
        return;
    }

    switch (e) {
    case WHEEL_SCROLL:
        if (S.tq_locked) break;             /* 扭矩锁：防误触，滚动直接忽略 */
        if (S.mode == MODE_STD) {
            /* 滚动切挡，一格一挡；进出 TURBO 档触发红/橙渐变 */
            bool was_turbo = S.std_gear == STD_TURBO_GEAR;
            int g = S.std_gear + d;
            S.std_gear = g < 0 ? 0 : (g >= STD_GEARS ? STD_GEARS - 1 : g);
            if ((S.std_gear == STD_TURBO_GEAR) != was_turbo) {
                start_morph();
            }
        } else if (S.mode == MODE_PRECISE) {
            float t = S.precise_torque + d * PRECISE_STEP;
            S.precise_torque = t < PRECISE_MIN ? PRECISE_MIN
                             : (t > PRECISE_MAX ? PRECISE_MAX : t);
        }
        /* 棘轮：扭矩固定为扳手上限，滚动不响应 */
        S.tq_activity = esp_timer_get_time();
        s_dirty_scrim = true;
        break;

    case WHEEL_CLICK:
        /* 短按只做解锁/选择，不开启任何内容 */
        if (S.tq_locked) {
            S.tq_locked = false;
            S.tq_activity = esp_timer_get_time();
            s_dirty_top = true;
            s_dirty_scrim = true;
        }
        break;

    case WHEEL_DOUBLE:                       /* 仅主界面：换模式 */
        if (S.screen == SCR_HOME && !S.running) {
            S.mode = (S.mode + 1) % MODE_COUNT;
            S.teeth = 0;
            S.tq_locked = true;             /* 换模式后回锁，防止带着旧手感误调 */
            start_morph();
            S.spin = true;                  /* 模式切换：螺纹快速滚动一下，更有真实感 */
            S.spin_t = 0.0f;
            s_dirty_top = true;
            s_dirty_scrim = true;
            if (S.mode == MODE_STD) {
                knob_set_mode(1);           /* STD 模式电调必须在码盘 mode 1 */
            }
        }
        break;

    case WHEEL_LONG:
        S.screen = SCR_MENU;                /* 长按进菜单（菜单内长按退出） */
        menu_open();
        break;

    case WHEEL_PRESS:
        break;                              /* 已在函数入口处理，不会到这里 */
    }
}

/* ---------------- 主循环 ---------------- */
void app_main(void)
{
    ESP_ERROR_CHECK(disp_init());
    thread_init();
    wheel_init(on_wheel, NULL);
    knob_init();                            /* 触觉旋钮板 UART 通信 */
    disp_brightness(WAKE_BRIGHTNESS);
    S.last_activity = esp_timer_get_time();

    /* GRAM 上电是随机噪声：状态条和压条从未写过会显示花屏。
     * 这里把两块区域清黑一次（之后由各自的脏矩形刷新接管）。 */
    uint16_t *black = disp_alloc(DISP_W * DISP_SCRIM_H * 2);
    if (black) {
        memset(black, 0, DISP_W * DISP_SCRIM_H * 2);
        disp_blit(0, 0, DISP_W, DISP_BAR_H, black);          /* 状态条 */
        disp_wait_done();
        disp_blit(0, DISP_SCRIM_Y, DISP_W, DISP_SCRIM_H, black); /* 压条 */
        disp_wait_done();
        heap_caps_free(black);
    }

    /* 首次绘制状态条和压条 */
    draw_topbar();
    draw_scrim();

    const TickType_t period = pdMS_TO_TICKS(16);   /* 60fps */
    TickType_t last_wake = xTaskGetTickCount();
    int64_t t_prev = esp_timer_get_time();

    uint32_t frames = 0;
    int64_t fps_window_start = esp_timer_get_time();
    int64_t render_time_us = 0;

    while (1) {
        int64_t t_now = esp_timer_get_time();
        float dt = (t_now - t_prev) / 1000000.0f;
        t_prev = t_now;

        /* 输入每 ~5ms 采一次：一帧内轮询 3 次 */
        for (int i = 0; i < 3; i++) wheel_poll();

        /* 触觉旋钮板的位置增量也注入为滚动事件 */
        {
            int kd = knob_take_pos_delta();
            if (kd != 0) {
                on_wheel(WHEEL_SCROLL, kd, NULL);
            }
        }

        /* --- 休眠屏：低亮度显示 RTC 时钟 + 电量，分钟变化才重画 --- */
        if (S.screen == SCR_SLEEP) {
            char clk[8];
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            if (tmv.tm_year >= 124) {               /* 已对时（>=2024）：显示 RTC 时间 */
                snprintf(clk, sizeof(clk), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            } else {                                /* 未对时：回落为开机时长 */
                uint32_t up_min = (uint32_t)(t_now / 60000000);
                uint32_t h = up_min / 60;
                if (h > 99) h = 99;
                snprintf(clk, sizeof(clk), "%02lu:%02lu",
                         (unsigned long)h, (unsigned long)(up_min % 60));
            }
            if (strcmp(clk, s_sleep_drawn) != 0) {
                memcpy(s_sleep_drawn, clk, sizeof(clk));
                ui_draw_sleep(clk);
            }
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(50));
            continue;
        }

        /* 无操作 30s 且没在干活 -> 休眠 */
        if (!s_trigger && t_now - S.last_activity >= SLEEP_AFTER_US) {
            enter_sleep();
            continue;
        }

        /* 扭矩锁自动回锁：解锁后 8s 无滚动 */
        if (!S.tq_locked && t_now - S.tq_activity >= TQ_LOCK_AFTER_US) {
            S.tq_locked = true;
            s_dirty_top = true;
            s_dirty_scrim = true;
        }

        /* STD 模式下每 500ms 核对一次电调模式：被外部改掉就拉回 mode 1（码盘）。
         * 其它 UI 模式不动电调；解锁握手时 knob 任务本身就会切到 mode 1。 */
        {
            static int64_t s_knob_sync;
            if (S.mode == MODE_STD && t_now - s_knob_sync >= 500000) {
                s_knob_sync = t_now;
                knob_stat_t st;
                knob_get_status(&st);
                if (st.valid && st.mode != 1) {
                    ESP_LOGW(TAG, "knob board in mode %d, forcing mode 1", st.mode);
                    knob_set_mode(1);
                } else if (!st.valid) {
                    ESP_LOGW(TAG, "knob status not received yet (unlocked=%d)",
                             knob_is_unlocked());
                }
                knob_send("status");    /* 请求最新状态，供下一轮核对 */
            }
        }

        /* TODO: 主电调"扭矩到上限"信号接进来后在这里置/清 S.at_limit，
         * 变化时 s_dirty_scrim = true，压条会显示 MAX 印章（不是"到位"）。 */

        /* --- 菜单态：菜单接管屏幕，螺纹动画停帧 --- */
        if (S.screen == SCR_MENU) {
            menu_draw_if_dirty();
            vTaskDelayUntil(&last_wake, period);
            continue;
        }

        /* --- 颜色动画：指数趋近目标色（每帧走固定比例，可打断） --- */
        if (S.color_anim) {
            uint8_t cur[3], nxt[3];
            thread_get_base_color(cur);
            float k = dt * 6.0f;                    /* 时间常数 ~167ms，与帧率无关 */
            if (k > 1.0f) k = 1.0f;
            bool done = true;
            for (int i = 0; i < 3; i++) {
                int d = (int)S.color_target[i] - (int)cur[i];
                if (d == 0) {
                    nxt[i] = cur[i];
                } else if (d >= -2 && d <= 2) {
                    nxt[i] = S.color_target[i];     /* 末段直接到位，避免四舍五入卡死 */
                } else {
                    int v = (int)lroundf((float)cur[i] + (float)d * k);
                    nxt[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
                    done = false;
                }
            }
            thread_set_color(nxt);
            if (done) S.color_anim = false;         /* 到位后不再做无意义计算 */
            s_dirty_top = true;
            s_dirty_scrim = true;
        }

        /* --- 模式切换的螺纹快速滚动：0.7s 正弦包络，起停平滑 --- */
        if (S.spin) {
            S.spin_t += dt / 0.7f;
            if (S.spin_t >= 1.0f) {
                S.spin = false;
            } else {
                float rpm = 3600.0f * sinf((float)M_PI * S.spin_t);
                thread_advance(rpm, dt, menu_reverse());
            }
        }

        /* --- 运动：螺纹速度只跟随主电机转速，待机停转 --- */
        if (S.mode == MODE_RATCHET) {
            /* 棘轮：不连续转，0.18s 咬一格 */
            if (s_trigger) {
                S.ratchet_acc += dt;
                if (S.ratchet_acc >= 0.18f) {
                    S.ratchet_acc = 0; S.teeth++;
                    thread_detent();
                }
            } else S.ratchet_acc = 0;
            S.rpm_now = 0;
        } else {
            int cap = menu_rpm_max();
            int set = S.rpm_set[S.mode] > cap ? cap : S.rpm_set[S.mode];
            float target = s_trigger ? set : 0;
            S.rpm_now += (target - S.rpm_now) * (dt * 6.0f);
            thread_advance(S.rpm_now, dt, menu_reverse());
        }

        /* --- 出图 --- */
        int64_t t0 = esp_timer_get_time();
        thread_render();
        render_time_us += esp_timer_get_time() - t0;
        frames++;

        if (t_now - fps_window_start >= 2 * 1000 * 1000) {
            float secs = (float)(t_now - fps_window_start) / 1e6f;
            ESP_LOGI(TAG, "FPS %.1f, avg render+push %.2f ms",
                     frames / secs, (float)render_time_us / frames / 1000.0f);
            frames = 0;
            render_time_us = 0;
            fps_window_start = t_now;
        }

        /* 状态条和压条：脏矩形刷新；颜色动画期间每帧重刷 */
        if (S.color_anim) {
            s_dirty_top = true;
            s_dirty_scrim = true;
        }
        if (s_dirty_top) {
            draw_topbar();
            s_dirty_top = false;
        }
        if (s_dirty_scrim) {
            draw_scrim();
            s_dirty_scrim = false;
        }

        vTaskDelayUntil(&last_wake, period);
    }
}
