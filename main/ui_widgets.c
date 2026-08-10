/*
 * ui_widgets.c —— 状态条 / 压条绘制
 *
 * 状态条（行 0..13）：模式徽标 | 转速 | 电量（电量暂无输入，静态显示）
 * 压条（行 196..239）：扭矩大数字（30px 抗锯齿，模式基色）+ 单位
 *
 * 没有中文字库，先用英文缩写；3x5 点阵字 + 4x4 超采样抗锯齿，
 * 支持任意浮点字号。颜色统一按 SPI 线序（大端 RGB565）写入。
 */

#include <string.h>
#include <math.h>

#include "display.h"
#include "ui_widgets.h"

/* ---------- 3x5 点阵字库 ---------- */

typedef struct { char ch; uint8_t rows[5]; } glyph_t;

static const glyph_t s_font[] = {
    {'0',{7,5,5,5,7}}, {'1',{2,6,2,2,7}}, {'2',{7,1,7,4,7}}, {'3',{7,1,7,1,7}},
    {'4',{5,5,7,1,1}}, {'5',{7,4,7,1,7}}, {'6',{7,4,7,5,7}}, {'7',{7,1,1,2,2}},
    {'8',{7,5,7,5,7}}, {'9',{7,5,7,1,7}},
    {'.',{0,0,0,0,2}}, {':',{0,2,0,2,0}}, {'%', {5,1,2,4,5}}, {' ',{0,0,0,0,0}},
    {'A',{2,5,7,5,5}}, {'B',{6,5,6,5,6}}, {'C',{3,4,4,4,3}}, {'D',{6,5,5,5,6}},
    {'E',{7,4,7,4,7}}, {'F',{7,4,7,4,4}}, {'G',{3,4,5,5,3}}, {'H',{5,5,7,5,5}},
    {'I',{7,2,2,2,7}}, {'J',{1,1,1,5,2}}, {'K',{5,5,6,5,5}}, {'L',{4,4,4,4,7}},
    {'M',{5,7,7,5,5}}, {'N',{6,5,5,5,5}}, {'O',{7,5,5,5,7}}, {'P',{7,5,7,4,4}},
    {'Q',{7,5,5,7,1}}, {'R',{6,5,6,5,5}}, {'S',{3,4,2,1,6}}, {'T',{7,2,2,2,2}},
    {'U',{5,5,5,5,7}}, {'V',{5,5,5,5,2}}, {'W',{5,5,7,7,5}}, {'X',{5,5,2,5,5}},
    {'Y',{5,5,2,2,2}}, {'Z',{7,1,2,4,7}},
};

static const uint8_t *find_glyph(char c)
{
    for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
        if (s_font[i].ch == c) return s_font[i].rows;
    }
    return NULL;
}

/* ---------- 区域帧缓冲 ---------- */

static uint16_t s_top[DISP_W * DISP_BAR_H];      /* 状态条 */
static uint16_t s_scrim[DISP_W * DISP_SCRIM_H];  /* 压条 */

/* 以下绘制都在某个区域缓冲上进行，坐标是区域内坐标 */
static int s_bw, s_bh;
static uint16_t *s_buf;

/* 供菜单等其它模块使用：先 ui_buf_select() 选缓冲，再调 ui_* 绘制 */
void ui_buf_select(uint16_t *buf, int w, int h)
{
    s_buf = buf; s_bw = w; s_bh = h;
}

static inline uint16_t rgb565be(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

uint16_t ui_rgb565_be(uint8_t r, uint8_t g, uint8_t b)
{
    return rgb565be(r, g, b);
}

static void put_px(int x, int y, uint16_t color_be)
{
    if ((unsigned)x < (unsigned)s_bw && (unsigned)y < (unsigned)s_bh) {
        s_buf[y * s_bw + x] = color_be;
    }
}

void ui_put(int x, int y, uint16_t color_be)
{
    put_px(x, y, color_be);
}

void ui_fill_rect(int x, int y, int w, int h, uint16_t color_be)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            put_px(xx, yy, color_be);
}

static void blend_px(int x, int y, uint16_t fg_host, uint8_t alpha)
{
    if ((unsigned)x >= (unsigned)s_bw || (unsigned)y >= (unsigned)s_bh) return;
    uint16_t *p = &s_buf[y * s_bw + x];
    uint16_t bg = (uint16_t)((*p >> 8) | (*p << 8));
    uint16_t r = ((bg >> 11) & 0x1F) * (255 - alpha) + ((fg_host >> 11) & 0x1F) * alpha;
    uint16_t g = ((bg >> 5) & 0x3F) * (255 - alpha) + ((fg_host >> 5) & 0x3F) * alpha;
    uint16_t b = (bg & 0x1F) * (255 - alpha) + (fg_host & 0x1F) * alpha;
    uint16_t out = (uint16_t)(((r / 255) << 11) | ((g / 255) << 5) | (b / 255));
    *p = (uint16_t)((out >> 8) | (out << 8));
}

#define AA_SS 4

static void glyph_aa(float x, float y, float size, char c, uint16_t color_host)
{
    const uint8_t *gl = find_glyph(c);
    if (!gl || size < 2.0f) return;
    const float w = size * 3.0f / 5.0f;
    const int x0 = (int)floorf(x), x1 = (int)ceilf(x + w);
    const int y0 = (int)floorf(y), y1 = (int)ceilf(y + size);

    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            int cov = 0;
            for (int sy = 0; sy < AA_SS; sy++) {
                float v = (py + (sy + 0.5f) / AA_SS - y) / size * 5.0f;
                if (v < 0.0f || v >= 5.0f) continue;
                int row = (int)v;
                for (int sx = 0; sx < AA_SS; sx++) {
                    float u = (px + (sx + 0.5f) / AA_SS - x) / w * 3.0f;
                    if (u < 0.0f || u >= 3.0f) continue;
                    if (gl[row] & (0b100 >> (int)u)) cov++;
                }
            }
            if (cov) blend_px(px, py, color_host, (uint8_t)(cov * 255 / (AA_SS * AA_SS)));
        }
    }
}

/* 左对齐文本，返回结束 x */
static float text_aa(float x, float y, const char *str, float size, uint16_t color_host)
{
    const float adv = size * 3.0f / 5.0f + size * 0.15f;
    while (*str) {
        char c = *str++;
        glyph_aa(x, y, size, c, color_host);   /* 注意定义是 (x, y, size, c, ...) */
        x += adv;
    }
    return x;
}

void ui_text(float x, float y, const char *str, float size, uint16_t color_host)
{
    text_aa(x, y, str, size, color_host);
}

void ui_text_center(float cx, float y, const char *str, float size, uint16_t color_host)
{
    const float adv = size * 3.0f / 5.0f + size * 0.15f;
    float total = strlen(str) * adv - size * 0.15f;
    text_aa(cx - total / 2.0f, y, str, size, color_host);
}

void ui_text_right(float rx, float y, const char *str, float size, uint16_t color_host)
{
    const float adv = size * 3.0f / 5.0f + size * 0.15f;
    float total = strlen(str) * adv - size * 0.15f;
    text_aa(rx - total, y, str, size, color_host);
}

static void text_aa_centered(float cx, float y, const char *str, float size, uint16_t color_host)
{
    const float adv = size * 3.0f / 5.0f + size * 0.15f;
    float total = strlen(str) * adv - size * 0.15f;
    text_aa(cx - total / 2.0f, y, str, size, color_host);
}

static void text_aa_right(float rx, float y, const char *str, float size, uint16_t color_host)
{
    const float adv = size * 3.0f / 5.0f + size * 0.15f;
    float total = strlen(str) * adv - size * 0.15f;
    text_aa(rx - total, y, str, size, color_host);
}

/* 竖排文本：先横向渲染再逆时针转 90°（从下往上读），贴在 (x, y) 左上角 */
void ui_text_vertical(float x, float y, const char *str, float size, uint16_t color_host)
{
    static uint16_t tmp[48 * 16];               /* 小缓冲，够放几个字符 */
    const float adv = size * 3.0f / 5.0f + size * 0.15f;
    int tw = (int)ceilf(strlen(str) * adv - size * 0.15f);
    int th = (int)ceilf(size);
    if (tw > 48 || th > 16) {
        return;
    }

    uint16_t *save_buf = s_buf; int save_w = s_bw, save_h = s_bh;
    memset(tmp, 0, sizeof(tmp));
    s_buf = tmp; s_bw = tw; s_bh = th;
    text_aa(0, 0, str, size, color_host);
    s_buf = save_buf; s_bw = save_w; s_bh = save_h;

    for (int sy = 0; sy < th; sy++) {
        for (int sx = 0; sx < tw; sx++) {
            uint16_t v = tmp[sy * tw + sx];
            if (v) {
                put_px((int)x + sy, (int)y + (tw - 1 - sx), v);
            }
        }
    }
}

/* ---------- 对外接口 ---------- */

static const char *const MODE_TAG[3] = { "PRE", "STD", "RCH" };

/* 电量颜色规则：充电绿；<15% 红；<50% 黄；其余白 */
static void batt_rgb(int pct, bool charging, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (charging)      { *r = 60;  *g = 220; *b = 100; }
    else if (pct < 15) { *r = 255; *g = 60;  *b = 60;  }
    else if (pct < 50) { *r = 255; *g = 200; *b = 40;  }
    else               { *r = 240; *g = 240; *b = 240; }
}

/* 状态条：模式徽标 | 转速 | 电量（颜色按电量规则，填充宽度=电量百分比） */
void ui_draw_topbar(int mode, int rpm, bool locked, const uint8_t base[3],
                    int batt_pct, bool batt_charging)
{
    disp_wait_done();                           /* 先等上一笔 DMA 完成，再改写缓冲 */
    s_buf = s_top; s_bw = DISP_W; s_bh = DISP_BAR_H;
    memset(s_top, 0, sizeof(s_top));

    uint16_t c_mode = (uint16_t)(((base[0] & 0xF8) << 8) | ((base[1] & 0xFC) << 3) | (base[2] >> 3));
    uint16_t c_dim = 0x52AA;                    /* 灰 */
    uint16_t c_text = locked ? c_dim : c_mode;

    /* 模式徽标 */
    text_aa(4, 2, MODE_TAG[mode % 3], 9.0f, c_text);

    /* 转速（锁定时灰显） */
    char num[8];
    int v = rpm, n = 0;
    char rev[8];
    if (v == 0) rev[n++] = '0';
    while (v) { rev[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; i++) num[i] = rev[n - 1 - i];
    num[n] = '\0';
    text_aa_right(92, 2, num, 9.0f, locked ? c_dim : 0x7BEF);

    /* 电量：框 + 按百分比填充；白/绿(充电)/黄(<50%)/红(<15%) */
    uint8_t br, bg, bb;
    batt_rgb(batt_pct, batt_charging, &br, &bg, &bb);
    uint16_t bat = rgb565be(br, bg, bb);
    for (int x = 102; x <= 114; x++) { put_px(x, 3, bat); put_px(x, 10, bat); }
    for (int y = 3; y <= 10; y++) { put_px(102, y, bat); put_px(114, y, bat); }
    put_px(115, 5, bat); put_px(115, 6, bat); put_px(115, 7, bat); put_px(115, 8, bat);
    if (batt_pct < 0) batt_pct = 0;
    if (batt_pct > 100) batt_pct = 100;
    int fillw = batt_pct * 9 / 100;             /* 内腔 x 104..112 共 9px */
    for (int y = 5; y <= 8; y++)
        for (int x = 104; x < 104 + fillw; x++) put_px(x, y, bat);

    disp_blit(0, 0, DISP_W, DISP_BAR_H, s_top);
}

/* 数字按 "x.xx" 格式化（整数部分恒为一位；负数/超界钳到 0.00~9.99） */
static void fmt_x100(char buf[6], int v)
{
    if (v < 0) v = 0;
    if (v > 999) v = 999;
    buf[4] = '\0';
    buf[3] = '0' + (v % 10); v /= 10;
    buf[2] = '0' + (v % 10); v /= 10;
    buf[1] = '.';
    buf[0] = '0' + (v % 10);
}

/* 压条双态布局（view_t 由上层做趋近动画）：
 *   view_t=0 平常：白色反馈扭矩大数字在主位，设定上限在顶部条带(14px)
 *   view_t=1 调节：设定上限放大到主位，反馈缩小上浮到条带，两者换位
 *   单位 N.M 加大为 13px，颜色跟随主位（白 <-> 设定色）
 * turbo 时设定值红色；at_limit 时整个压条改 MAX 印章 */
void ui_draw_scrim(int set_x100, int fb_x100, float view_t,
                   const uint8_t base[3], bool turbo, bool at_limit)
{
    disp_wait_done();                           /* 先等上一笔 DMA 完成，再改写缓冲 */
    s_buf = s_scrim; s_bw = DISP_W; s_bh = DISP_SCRIM_H;
    memset(s_scrim, 0, sizeof(s_scrim));

    if (at_limit) {
        /* 扭矩到上限印章：红框 + MAX（原型里的"到位"章，按需求改成"到上限"语义） */
        uint16_t be_red = rgb565be(255, 60, 60);
        for (int x = 28; x <= 91; x++) {
            put_px(x, 8, be_red); put_px(x, 9, be_red);
            put_px(x, 32, be_red); put_px(x, 33, be_red);
        }
        for (int y = 8; y <= 33; y++) {
            put_px(28, y, be_red); put_px(29, y, be_red);
            put_px(90, y, be_red); put_px(91, y, be_red);
        }
        text_aa_centered(60.0f, 13.0f, "MAX", 16.0f, 0xF926);
    } else {
        char buf[6];
        /* 设定色：模式基色，TURBO 红 */
        uint8_t sr = turbo ? 255 : base[0];
        uint8_t sg = turbo ? 73  : base[1];
        uint8_t sb = turbo ? 38  : base[2];
        uint16_t c_set = (uint16_t)(((sr & 0xF8) << 8) | ((sg & 0xFC) << 3) | (sb >> 3));

        /* 设定上限：顶部条带(16px) <-> 主位大数字(28px)。
         * 左对齐 x=13 = 大数字左缘，换位时只有字号/纵向移动，无横向跳变 */
        fmt_x100(buf, set_x100);
        text_aa(13.0f, 2.0f + 22.0f * view_t, buf, 16.0f + 12.0f * view_t, c_set);

        /* 实时反馈扭矩：白色，28px 占满主位(y24..52) <-> 顶部条带 */
        fmt_x100(buf, fb_x100);
        text_aa(13.0f, 24.0f - 22.0f * view_t, buf, 28.0f - 12.0f * view_t, 0xFFFF);

        /* 单位：颜色跟随主位（白 -> 设定色，随 view_t 插值） */
        uint8_t ur = (uint8_t)(255 + ((int)sr - 255) * view_t);
        uint8_t ug = (uint8_t)(255 + ((int)sg - 255) * view_t);
        uint8_t ub = (uint8_t)(255 + ((int)sb - 255) * view_t);
        uint16_t c_unit = (uint16_t)(((ur & 0xF8) << 8) | ((ug & 0xFC) << 3) | (ub >> 3));
        /* 竖放（从下往上读），与大数字同高区间；屏幕底部 ~6 行被外壳遮挡，内容上移 */
        ui_text_vertical(103, 22, "N.M", 13.0f, c_unit);
    }

    disp_blit(0, DISP_SCRIM_Y, DISP_W, DISP_SCRIM_H, s_scrim);
    /* 注意：不在末尾 wait —— 约定下一笔绘制的开头统一 disp_wait_done()，
     * 每笔 blit 恰好配一次等待，多等一次会让等待方空转 100ms 超时 */
}

/* 休眠屏：整屏暗色，时钟 + 电量占位框 */
void ui_draw_sleep(const char *clock_text)
{
    static uint16_t *s_sleep;
    if (!s_sleep) s_sleep = disp_alloc(DISP_W * DISP_H * 2);
    if (!s_sleep) return;

    disp_wait_done();                           /* 先等上一笔 DMA 完成，再改写缓冲 */
    ui_buf_select(s_sleep, DISP_W, DISP_H);
    memset(s_sleep, 0, DISP_W * DISP_H * 2);

    ui_text_center(60.0f, 96.0f, clock_text, 22.0f, 0x4208);   /* 暗灰，配合低亮度 */

    /* 电量占位框（等接电量检测后填充电量） */
    uint16_t bat = rgb565be(70, 70, 70);
    for (int x = 47; x <= 71; x++) { put_px(x, 140, bat); put_px(x, 151, bat); }
    for (int y = 140; y <= 151; y++) { put_px(47, y, bat); put_px(71, y, bat); }
    put_px(72, 143, bat); put_px(72, 144, bat); put_px(72, 147, bat); put_px(72, 148, bat);

    disp_blit(0, 0, DISP_W, DISP_H, s_sleep);
}
