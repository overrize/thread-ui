/*
 * qd_app.c —— QuantDriver 整屏 UI v1.4.5（228x460）的帧循环
 *
 * UI 渲染与状态机原样来自 QuantDriver 点亮包（main/qd/，与 HTML 原型逐帧对照过）；
 * 推屏走本工程的 display.c（QSPI 引脚、GRAM 偏移、上电时序以我们实测为准）。
 *
 * 帧流程：推进状态机（按真实经过时间）→ ui_frame_begin（槽格带 + 文字）
 *        → 整帧模式（默认）：10 条带渲染进整帧缓冲（双核各半）→ 一次性推整帧
 *          条带模式（整帧缓冲分配失败时退回）：渲染一条推一条，两块缓冲轮转
 *
 * 为什么默认整帧：条带模式一帧的推送摊在 ~70ms 里，跨好几次面板扫描（16.7ms），撕裂线很多。
 * 整帧一次推完（40MHz 约 10.5ms）快于扫描，最多一条撕裂线；TE 接通后从消隐期开始推可完全消除。
 *
 * 整帧流水线（两个任务）：
 *   渲染任务：begin(N+1) ──取 fb_free──→ rows(N+1) 写 fb ──给 frame_ready──→ begin(N+2) …
 *   推屏任务：取 frame_ready → 等 TE → 推整帧 → 等 DMA 完 → 给 fb_free
 *   begin 不碰 fb，所以它和「等 TE + 推屏」重叠；有 TE 时帧率按 60/n 取整，
 *   每帧 begin+rows ≈ 34ms 也能稳定在 33.3ms 周期（30fps）。
 *
 * 目前是自动演示：四个模式轮流「拧紧 → 到位 → 泄能 → 松出」。
 * BOOT 键（GPIO61）短按换模式，长按切换「减少动态」。旋钮/编码器还没接进 qd_state。
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "qd_app.h"
#include "demo_state.h"
#include "ui_render.h"

static const char *TAG = "qd";

#define STRIP_H          46    /* 偶数（CO5300 地址对齐），460 / 46 = 10 条 */
#define TARGET_FPS       30    /* 上限；实际帧率看串口日志 */
#define DEMO_START_MODE  1     /* 0 精密 1 标准 2 棘轮 3 TURBO */
#define DEMO_ROTATE      1     /* 每个「拧紧 + 松出」循环后自动换下一个模式 */
#define QD_BRIGHTNESS    128   /* 0~255，约 50%。VBAT 暂接 3.3V（低于规格），亮度高时升压欠压关断 → 黑屏；VBAT 改 4V 后可回 204 */
#define PIN_BUTTON       61    /* BOOT 键 */

_Static_assert(UI_W == DISP_PANEL_W && UI_H == DISP_PANEL_H, "UI 尺寸与面板不一致");
_Static_assert(STRIP_H % 2 == 0 && UI_H % STRIP_H == 0, "STRIP_H 必须是偶数且整除 460");

static uint16_t *s_buf[2];
static uint16_t *s_fb;           /* 整帧缓冲 228x460x2 ≈ 205KB，内部 DMA RAM */
static SemaphoreHandle_t s_frame_ready, s_fb_free;   /* 整帧流水线握手 */
static volatile bool s_force_recover;                /* 串口 'r'：手动重新初始化面板 */

/* ---------------- 双核：把 [a, b) 一分为二，辅助核算后半段 ----------------
 * job(lo, hi, ctx) 必须只依赖自己那一段；par() 返回时两段都已完成（同时充当阶段间的同步点）。 */
typedef void (*job_fn)(int lo, int hi, void *ctx);

static SemaphoreHandle_t s_go, s_fin;
static volatile job_fn s_job;
static volatile int s_job_lo, s_job_hi;
static void *volatile s_job_ctx;

static void helper_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        s_job(s_job_lo, s_job_hi, s_job_ctx);
        xSemaphoreGive(s_fin);
    }
}

static void par(job_fn job, int a, int b, int split, void *ctx)
{
    s_job = job;
    s_job_lo = split;
    s_job_hi = b;
    s_job_ctx = ctx;
    xSemaphoreGive(s_go);
    job(a, split, ctx);
    xSemaphoreTake(s_fin, portMAX_DELAY);
}

/* 行渲染任务：ctx = 第 base_y 行对应的输出地址 */
typedef struct { uint16_t *dst; int base_y; } rows_ctx_t;

static void job_rows(int lo, int hi, void *ctx)
{
    rows_ctx_t *c = ctx;
    ui_render_rows(lo, hi, c->dst + (lo - c->base_y) * UI_W);
}

/* 整帧交错：按 IL_ROWS 行一块，偶数块给主核、奇数块给辅助核（lo = 奇偶）。
 * 上下半屏负载差很多（下半有槽格带和大号数字），交错后两核基本均衡 */
#define IL_ROWS 4
static void job_rows_il(int lo, int hi, void *ctx)
{
    uint16_t *fb = ctx;
    for (int y = lo * IL_ROWS; y < UI_H; y += 2 * IL_ROWS) {
        int y1 = y + IL_ROWS > UI_H ? UI_H : y + IL_ROWS;
        ui_render_rows(y, y1, fb + y * UI_W);
    }
}

/* 槽格带三步也按奇偶交错（lo = 奇偶）：带底几行没有转子、两侧列模糊半径不同，对半切不均衡 */
static void job_composite(int lo, int hi, void *ctx)
{
    for (int r = lo; r < UI_BAND_ROWS; r += 2) ui_band_composite(r, r + 1);
}
static void job_hblur(int lo, int hi, void *ctx)
{
    for (int r = lo; r < UI_BAND_ROWS; r += 2) ui_band_hblur(r, r + 1);
}
static void job_vblur(int lo, int hi, void *ctx)
{
    for (int x = lo; x < UI_W; x += 2) ui_band_vblur(x, x + 1);
}

/* ui_frame_begin 的双核版：合成和横向模糊按行分，纵向模糊按列分，都是奇偶交错 */
static int64_t s_prof[4];           /* setup / composite / hblur / vblur 累计 us，日志里打印平均 */

static void frame_begin_par(const qd_state_t *st, const qd_cells_t *cells)
{
    int64_t t0 = esp_timer_get_time();
    ui_frame_setup(st, cells);
    int64_t t1 = esp_timer_get_time();
    par(job_composite, 0, 2, 1, NULL);
    int64_t t2 = esp_timer_get_time();
    par(job_hblur, 0, 2, 1, NULL);
    int64_t t3 = esp_timer_get_time();
    par(job_vblur, 0, 2, 1, NULL);
    int64_t t4 = esp_timer_get_time();
    s_prof[0] += t1 - t0; s_prof[1] += t2 - t1; s_prof[2] += t3 - t2; s_prof[3] += t4 - t3;
}

static void render_strip(int y0, int y1, uint16_t *dst)
{
    rows_ctx_t c = { dst, y0 };
    par(job_rows, y0, y1, y0 + ((y1 - y0) / 2 & ~1), &c);
}

/* ---------------- BOOT 键：返回 1 = 短按，2 = 长按（≥0.6s） ---------------- */
static int button_poll(void)
{
    static int64_t down_at = -1;
    static bool long_fired;
    bool down = gpio_get_level(PIN_BUTTON) == 0;
    int64_t now = esp_timer_get_time();
    if (down && down_at < 0) { down_at = now; long_fired = false; }
    if (down && !long_fired && now - down_at > 600000) { long_fired = true; return 2; }
    if (!down && down_at >= 0) {
        bool was_short = !long_fired && now - down_at > 30000;   /* 30ms 去抖 */
        down_at = -1;
        if (was_short) return 1;
    }
    return 0;
}

/* 推屏任务：等一帧画好 → 等 TE（消隐期开始）→ 推整帧 → DMA 领先一段后就把缓冲还给渲染任务
 *
 * 追着 DMA 写：DMA 从上往下读 fb，整帧 10.5ms（≈0.023ms/行）；渲染两核合计 ≈0.048ms/行，
 * 只有 DMA 的一半快。让 DMA 先跑 ≥1ms（≈44 行）再放渲染进来，渲染永远追不上 DMA 读的位置，
 * 不会写坏还没推出去的行。省掉的是「等整帧推完」的 ~9ms。 */
#define CHASE_LEAD_TICKS 4     /* vTaskDelay(4) @1kHz = 3~4ms 领先（2 时顶部偶发细花线，加大验证） */
static void flush_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_frame_ready, portMAX_DELAY);
        disp_wait_te(20);                              /* 无 TE 时立即返回 */
        disp_flush_panel(0, 0, UI_W, UI_H, s_fb);
        vTaskDelay(CHASE_LEAD_TICKS);
        xSemaphoreGive(s_fb_free);
        disp_wait_inflight(0);                         /* 下一帧至少还要 ~19ms 才画好，这里早就推完了 */
        if (disp_panel_lost() || s_force_recover) {     /* 面板欠压复位后自动拉起来 / 手动 */
            s_force_recover = false;
            disp_panel_recover();
        }
    }
}

/* 串口调试命令：r = 重新初始化面板（验证黑屏是否为面板升压锁死） */
static void console_task(void *arg)
{
    for (;;) {
        int ch = getchar();
        if (ch == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        if (ch == 'r') { ESP_LOGW(TAG, "console: force panel re-init"); s_force_recover = true; }
    }
}

/* 帧缓冲里非黑像素占比（每 61 个像素抽 1 个）。黑屏时它 > 0 = 画面在、是面板没亮 */
static int fb_lit_pct(void)
{
    if (!s_fb) return -1;
    int lit = 0, n = 0;
    for (int i = 0; i < UI_W * UI_H; i += 61, n++) lit += s_fb[i] != 0;
    return lit * 100 / n;
}

static void demo_task(void *arg)
{
    ui_init();
    static qd_state_t st;
    static qd_cells_t cells;
    memset(&st, 0, sizeof st);
    st.speed = 1.0;
    qd_state_init(&st, DEMO_START_MODE);

    int64_t t_prev = esp_timer_get_time(), t_log = t_prev;
    int frames = 0;
    int64_t acc_begin = 0, acc_rows = 0, acc_wait = 0;
    const int64_t period_us = 1000000 / TARGET_FPS;
    TickType_t wake = xTaskGetTickCount();

    for (;;) {
        int64_t t0 = esp_timer_get_time();
        double dt = (t0 - t_prev) * 1e-6;
        t_prev = t0;
        if (dt > 0.1) dt = 0.1;                        /* 卡顿后不跳变 */

        int b = button_poll();
        if (b == 1) { qd_state_init(&st, (st.mode + 1) % QD_MODES); ESP_LOGI(TAG, "mode -> %d", st.mode); }
        if (b == 2) { st.calm = !st.calm; ESP_LOGI(TAG, "calm %s", st.calm ? "on" : "off"); }

        st.now += dt;
        bool on;
        bool wrapped = qd_demo_tick(&st, dt, &on);
        if (wrapped && DEMO_ROTATE && st.cycle % 2 == 0) qd_state_init(&st, (st.mode + 1) % QD_MODES);
        qd_state_step(&st, dt, on);
        qd_cells_prep(&st, &cells);

        int64_t t1 = esp_timer_get_time();
        frame_begin_par(&st, &cells);
        int64_t t2 = esp_timer_get_time(), t_rows = 0, t_wait = 0;

        if (s_fb) {
            int64_t a = esp_timer_get_time();
            xSemaphoreTake(s_fb_free, portMAX_DELAY);  /* 上一帧推完才能改缓冲（begin 已与推屏重叠） */
            int64_t m = esp_timer_get_time();
            par(job_rows_il, 0, 2, 1, s_fb);           /* 整帧：两核交错分块，只同步一次 */
            t_rows = esp_timer_get_time() - m;
            t_wait = m - a;
            xSemaphoreGive(s_frame_ready);             /* 交给推屏任务：等 TE 再推 */
        } else {
            for (int i = 0, y = 0; y < UI_H; y += STRIP_H, i++) {
                int64_t a = esp_timer_get_time();
                disp_wait_inflight(1);                 /* 两块缓冲：保证这块已经传完 */
                int64_t m = esp_timer_get_time();
                render_strip(y, y + STRIP_H, s_buf[i & 1]);
                int64_t z = esp_timer_get_time();
                disp_flush_panel(0, y, UI_W, STRIP_H, s_buf[i & 1]);
                t_wait += m - a;
                t_rows += z - m;
            }
        }
        frames++;
        acc_begin += t2 - t1;
        acc_rows += t_rows;
        acc_wait += t_wait;

        if (t0 - t_log > 2000000) {
            float sec = (t0 - t_log) * 1e-6f;
            static uint32_t te_miss_prev;
            uint32_t te_miss = disp_te_misses();
            ESP_LOGI(TAG, "%.1f fps | begin %.1f ms (setup %.1f comp %.1f hblur %.1f vblur %.1f), rows %.1f ms, wait fb %.1f ms | TE miss %u | lit %d%% | mode %d%s",
                     frames / sec, acc_begin / 1000.f / frames,
                     s_prof[0] / 1000.f / frames, s_prof[1] / 1000.f / frames,
                     s_prof[2] / 1000.f / frames, s_prof[3] / 1000.f / frames,
                     acc_rows / 1000.f / frames, acc_wait / 1000.f / frames,
                     (unsigned)(te_miss - te_miss_prev), fb_lit_pct(), st.mode, st.calm ? " calm" : "");
            te_miss_prev = te_miss;
            memset(s_prof, 0, sizeof s_prof);
            frames = 0;
            acc_begin = acc_rows = acc_wait = 0;
            t_log = t0;
        }
        /* 限帧：快于目标帧率时让出 CPU；慢于目标时至少让一个 tick 喂看门狗 */
        TickType_t per = pdMS_TO_TICKS(period_us / 1000);
        if (s_fb && disp_te_available()) {
            /* TE 定节奏：fb_free 的等待已经让出 CPU */
        } else if (per && esp_timer_get_time() - t0 < period_us) {
            xTaskDelayUntil(&wake, per);
        } else {
            wake = xTaskGetTickCount();
            vTaskDelay(1);
        }
    }
}

esp_err_t qd_app_start(void)
{
    gpio_config_t bc = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&bc);

    s_fb = disp_alloc(UI_W * UI_H * 2);
    if (!s_fb) {
        ESP_LOGW(TAG, "frame buffer alloc failed (largest DMA block %u B), fall back to strips",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        s_buf[0] = disp_alloc(UI_W * STRIP_H * 2);
        s_buf[1] = disp_alloc(UI_W * STRIP_H * 2);
        if (!s_buf[0] || !s_buf[1]) {
            ESP_LOGE(TAG, "strip alloc failed (internal DMA RAM)");
            return ESP_ERR_NO_MEM;
        }
    }
    disp_brightness(QD_BRIGHTNESS);

    s_go = xSemaphoreCreateBinary();
    s_fin = xSemaphoreCreateBinary();
    if (s_fb) {
        s_frame_ready = xSemaphoreCreateBinary();
        s_fb_free = xSemaphoreCreateBinary();
        xSemaphoreGive(s_fb_free);                     /* 开始时缓冲空闲 */
        xTaskCreatePinnedToCore(flush_task, "qd_flush", 3072, NULL, 7, NULL, 1);
        xTaskCreatePinnedToCore(console_task, "qd_con", 3072, NULL, 2, NULL, 0);
    }
    xTaskCreatePinnedToCore(helper_task, "qd_r1", 6144, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(demo_task, "qd_r0", 8192, NULL, 6, NULL, 1);
    ESP_LOGI(TAG, "QuantDriver UI v1.4.5 %dx%d, %s", UI_W, UI_H, s_fb ? "full-frame" : "strip");
    return ESP_OK;
}
