/*
 * menu.c —— 设置菜单，最多三级（SPEC 5.5）
 *
 *   设置 (SETUP)
 *   ├ SPD MAX    num  100~900 step 50 rpm
 *   ├ REFLEX     enum OFF/LIGHT/STD/STRONG
 *   ├ GRIP SENS  num  1~10
 *   ├ DIRECTION  enum CW/CCW
 *   └ SYSTEM ▸
 *     ├ CALIB      read
 *     ├ UNIT       enum N.M/KGCM/LBIN
 *     ├ FACTORY    danger（确认条，默认停 CANCEL）
 *     └ ABOUT      read
 *
 * 手势（SPEC 5.2，短按前进、长按后退）：
 *   列表：滚动移光标 / 短按进入或编辑 / 长按返回
 *   编辑：滚动改值 / 短按保存 / 长按放弃（回滚）
 *   确认条：滚动切换 / 短按执行 / 长按取消
 *
 * 没有中文字库，先用英文标签；12px 中文留待嵌字库。
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "display.h"
#include "esp_log.h"
#include "menu.h"
#include "ui_widgets.h"

static const char *TAG = "menu";

#define ROW_H       24
#define ROW_Y0      18
#define ROW_LABEL_X 6
#define ROW_VAL_X   114

typedef enum { IT_NUM, IT_ENUM, IT_READ, IT_DANGER, IT_SUB } item_type_t;

typedef struct {
    const char *label;
    item_type_t type;
    int *val;                       /* num/enum 的当前值 */
    int min, max, step;             /* num */
    const char *const *opts;        /* enum 选项文本 */
    int opt_count;
    const char *read_text;          /* read 显示内容 */
} item_t;

/* ---- 设置值存储 ---- */
static int s_rpm_max = 450;
static int s_reflex  = 2;           /* STD */
static int s_grip    = 5;
static int s_reverse = 0;
static int s_unit    = 0;           /* N.M */
static int s_clock_h = 0;           /* RTC 对时：时 */
static int s_clock_m = 0;           /* RTC 对时：分 */

/* 把当前系统时间的 HH:MM 改成设定值（保留日期；内部 RTC 走时，掉电需重新对时） */
static void clock_apply(void)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    tmv.tm_hour = s_clock_h;
    tmv.tm_min  = s_clock_m;
    tmv.tm_sec  = 0;
    struct timeval tv = { .tv_sec = mktime(&tmv), .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static bool is_clock_item(const item_t *it)
{
    return it->val == &s_clock_h || it->val == &s_clock_m;
}

static const char *const OPT_REFLEX[] = { "OFF", "LIGHT", "STD", "STRONG" };
static const char *const OPT_DIR[]    = { "CW", "CCW" };
static const char *const OPT_UNIT[]   = { "N.M", "KGCM", "LBIN" };
static const char *const OPT_YN[]     = { "CANCEL", "SURE!" };

int menu_rpm_max(void)  { return s_rpm_max; }
bool menu_reverse(void) { return s_reverse != 0; }

/* ---- 菜单表 ---- */
static const item_t s_system_items[] = {
    { "CALIB",   IT_READ,   NULL, 0, 0, 0, NULL, 0, "--" },
    { "CLOCK H", IT_NUM,    &s_clock_h, 0, 23, 1, NULL, 0, NULL },
    { "CLOCK M", IT_NUM,    &s_clock_m, 0, 59, 1, NULL, 0, NULL },
    { "UNIT",    IT_ENUM,   &s_unit, 0, 0, 0, OPT_UNIT, 3, NULL },
    { "FACTORY", IT_DANGER, NULL, 0, 0, 0, OPT_YN, 2, NULL },
    { "ABOUT",   IT_READ,   NULL, 0, 0, 0, NULL, 0, "V0.1" },
};

static const item_t s_main_items[] = {
    { "SPD MAX",   IT_NUM,  &s_rpm_max, 100, 900, 50, NULL, 0, NULL },
    { "REFLEX",    IT_ENUM, &s_reflex, 0, 0, 0, OPT_REFLEX, 4, NULL },
    { "GRIP SENS", IT_NUM,  &s_grip, 1, 10, 1, NULL, 0, NULL },
    { "DIRECTION", IT_ENUM, &s_reverse, 0, 0, 0, OPT_DIR, 2, NULL },
    { "SYSTEM",    IT_SUB,  NULL, 0, 0, 0, NULL, 0, NULL },
};

typedef enum { LV_MAIN, LV_SYSTEM } level_t;

static struct {
    level_t level;
    int cursor;
    bool editing;                   /* 数值/枚举编辑态 */
    bool confirming;                /* 危险操作确认条 */
    int confirm_sel;                /* 0=CANCEL 1=SURE */
    int backup;                     /* 进入编辑态时的原值（长按回滚） */
    bool dirty;
} M;

/* 光标条的动画位置（浮点行号）：指数趋近 M.cursor，WouoUI 式可打断缓动 */
#define CURSOR_EASE_W   0.28f       /* 趋近权重：每帧走剩余距离的 28%（@60fps） */
static float s_cursor_pos;

static uint16_t s_fb[DISP_W * DISP_H];      /* 菜单整屏缓冲 */

static const item_t *cur_items(int *count)
{
    if (M.level == LV_MAIN) {
        *count = sizeof(s_main_items) / sizeof(s_main_items[0]);
        return s_main_items;
    }
    *count = sizeof(s_system_items) / sizeof(s_system_items[0]);
    return s_system_items;
}

/* ---- 绘制 ---- */
static void menu_render(void)
{
    disp_wait_done();       /* 先等上一笔 DMA 完成，再改写整屏缓冲（否则黑屏/漏字） */
    memset(s_fb, 0, sizeof(s_fb));
    ui_buf_select(s_fb, DISP_W, DISP_H);

    const uint16_t c_title = 0x52AA;                 /* 灰 */
    const uint16_t c_text  = 0x7BEF;                 /* 浅灰白 */
    const uint16_t c_hl    = ui_rgb565_be(24, 28, 36);   /* 光标条底色 */
    const uint16_t c_warn  = 0xF800;                 /* 红（host 序） */

    /* 标题 */
    ui_text_center(DISP_W / 2.0f, 2, M.level == LV_MAIN ? "SETUP" : "SYSTEM", 11.0f, c_title);
    ui_fill_rect(0, 15, DISP_W, 1, ui_rgb565_be(60, 60, 60));

    int count;
    const item_t *items = cur_items(&count);

    /* 光标趋近动画：未到位就继续标脏，下一帧接着走（菜单 60fps 渲染）。
     * 光标条画在连续的浮点位置上，行间滑动是逐像素的 */
    float cd = (float)M.cursor - s_cursor_pos;
    if (cd > -0.06f && cd < 0.06f) {
        s_cursor_pos = (float)M.cursor;
    } else {
        s_cursor_pos += cd * CURSOR_EASE_W;
        M.dirty = true;
    }
    {
        int hl_y = ROW_Y0 + (int)(s_cursor_pos * ROW_H) - 2;
        ui_fill_rect(0, hl_y, DISP_W, ROW_H - 2, c_hl);
    }

    for (int i = 0; i < count; i++) {
        int y = ROW_Y0 + i * ROW_H;
        const item_t *it = &items[i];

        char label[20];
        if (it->type == IT_SUB) {
            snprintf(label, sizeof(label), "%s >", it->label);
        } else {
            snprintf(label, sizeof(label), "%s", it->label);
        }
        ui_text(ROW_LABEL_X, y + 4, label, 12.0f, c_text);

        /* 右侧值 */
        char val[12];
        uint16_t vc = c_text;
        switch (it->type) {
        case IT_NUM:
            snprintf(val, sizeof(val), "%d", *it->val);
            if (M.editing && i == M.cursor) vc = 0x07FF;   /* 青色=编辑中 */
            break;
        case IT_ENUM:
            snprintf(val, sizeof(val), "%s", it->opts[*it->val % it->opt_count]);
            if (M.editing && i == M.cursor) vc = 0x07FF;
            break;
        case IT_DANGER:
            if (M.confirming && i == M.cursor) {
                snprintf(val, sizeof(val), "%s", OPT_YN[M.confirm_sel]);
                vc = M.confirm_sel ? c_warn : c_text;
            } else {
                snprintf(val, sizeof(val), "...");
            }
            break;
        case IT_READ:
            snprintf(val, sizeof(val), "%s", it->read_text);
            vc = c_title;
            break;
        default:
            val[0] = '\0';
            break;
        }
        if (val[0]) {
            ui_text_right(ROW_VAL_X, y + 4, val, 12.0f, vc);
        }
    }

    disp_blit(0, 0, DISP_W, DISP_H, s_fb);
}

/* ---- 对外接口 ---- */
void menu_open(void)
{
    M.level = LV_MAIN;
    M.cursor = 0;
    M.editing = false;
    M.confirming = false;
    M.dirty = true;

    /* 对时项同步当前 RTC 时间（未对过时保持上次设定值） */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_year >= 124) {
        s_clock_h = tmv.tm_hour;
        s_clock_m = tmv.tm_min;
    }
}

bool menu_handle(wheel_evt_t e, int d)
{
    int count;
    const item_t *items = cur_items(&count);
    const item_t *it = &items[M.cursor];

    switch (e) {
    case WHEEL_SCROLL:
        if (M.confirming) {
            M.confirm_sel = (M.confirm_sel + (d > 0 ? 1 : -1) + 2) % 2;
        } else if (M.editing) {
            if (it->type == IT_NUM) {
                int v = *it->val + d * it->step;
                if (v < it->min) v = it->min;
                if (v > it->max) v = it->max;
                *it->val = v;
                if (is_clock_item(it)) clock_apply();   /* 对时：滚动即生效 */
            } else if (it->type == IT_ENUM) {
                *it->val = (*it->val + (d > 0 ? 1 : -1) + it->opt_count) % it->opt_count;
            }
        } else {
            M.cursor = (M.cursor + (d > 0 ? 1 : -1) + count) % count;
        }
        M.dirty = true;
        break;

    case WHEEL_CLICK:
        if (M.confirming) {
            if (M.confirm_sel == 1) {
                ESP_LOGW(TAG, "factory reset (placeholder)");
            }
            M.confirming = false;
        } else if (M.editing) {
            M.editing = false;              /* 保存 */
        } else if (it->type == IT_NUM || it->type == IT_ENUM) {
            M.backup = *it->val;
            M.editing = true;
        } else if (it->type == IT_DANGER) {
            M.confirming = true;
            M.confirm_sel = 0;              /* 默认停 CANCEL */
        } else if (it->type == IT_SUB) {
            M.level = LV_SYSTEM;
            M.cursor = 0;
        }
        M.dirty = true;
        break;

    case WHEEL_LONG:
        if (M.confirming) {
            M.confirming = false;           /* 取消 */
            M.dirty = true;
        } else if (M.editing) {
            *(int *)it->val = M.backup;     /* 回滚 */
            if (is_clock_item(it)) clock_apply();   /* 对时回滚也要生效 */
            M.editing = false;
            M.dirty = true;
        } else if (M.level == LV_SYSTEM) {
            M.level = LV_MAIN;
            M.cursor = 0;
            M.dirty = true;
        } else {
            return false;                   /* 退出菜单回主界面 */
        }
        break;

    default:
        break;
    }
    return true;
}

void menu_draw_if_dirty(void)
{
    if (M.dirty) {
        M.dirty = false;
        menu_render();
    }
}
