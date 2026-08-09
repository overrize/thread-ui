/*
 * thread_anim.c —— 主界面螺纹动画
 *
 * 核心：螺旋线在 y 方向严格周期，t 每增加 2π，x 回到原值、y 前进一个牙距。
 * 所以整屏只是一张 TILE_H x 120 的贴图在纵向重复，每帧只做行拷贝。
 *
 * 贴图以 **调色板索引** 存（每像素 1 字节，8 色），不是直接存 RGB565：
 *   - flash 占用小：17*120 = 2040 字节，三个模式共用同一张
 *   - 换模式只改 8 个颜色，不用重新生成贴图
 *   - 和后面可能切的 SPI 1-1-1 硬件调色板模式是同一套数据
 */

#include <string.h>
#include "display.h"
#include "thread_anim.h"
#include "thread_tile.h"   /* 由 tools/ 里的导出脚本生成：const uint8_t THREAD_TILE[TILE_H][DISP_W] */

/* ---- 三个模式的基色（和 HTML 原型一致） ---- */
static const uint8_t MODE_BASE[MODE_COUNT][3] = {
    {0x2F, 0xA8, 0xFF},   /* 精密 蓝 */
    {0xFF, 0x8A, 0x2B},   /* 标准 橙 */
    {0x3E, 0xD6, 0x6A},   /* 棘轮 绿 */
};

/* 8 个调色板槽位的推导系数。负数 = 向白提亮的比例 */
static const float SLOT_F[8] = {
    0.00f,   /* 0 背景黑 */
    0.06f,   /* 1 描边（近黑，略带基色，避免死黑割裂） */
    0.20f,   /* 2 芯轴暗部 */
    0.34f,   /* 3 背面圈 */
    0.16f,   /* 4 牙谷遮蔽 */
    0.42f,   /* 5 牙面暗档 */
    0.72f,   /* 6 牙面中档 */
   -0.20f,   /* 7 牙峰（向白 20%） */
};

static uint16_t s_pal[8];                    /* 当前调色板（RGB565 大端） */
static uint16_t s_tile565[TILE_H][DISP_W];   /* 展开后的贴图，4KB */
static uint16_t *s_fb;                       /* 动画区帧缓冲，120*182*2 = 43.7KB */
static float s_offset;                       /* 小数累加器，整数部分才用于取行 */
static uint8_t s_base[3];                    /* 当前基色（含 morph 中间色） */

/* 分数行混合 LUT：[色A*8+色B][小数档位 0..15] -> 混合色（大端）。
 * 待机慢转时步进 <1px/帧，整数行拷贝会让整屏图案每隔几帧跳一行（微闪）。
 * 用相邻两行按小数部分插值，亚像素平滑。 */
#define FRAC_Q  16
static uint16_t s_blend_lut[8 * 8 * FRAC_Q];

static void rebuild_blend_lut(void)
{
    for (int a = 0; a < 8; a++) {
        for (int b = 0; b < 8; b++) {
            /* s_pal 是大端，先转回主机序拆通道 */
            uint16_t A = (uint16_t)((s_pal[a] >> 8) | (s_pal[a] << 8));
            uint16_t B = (uint16_t)((s_pal[b] >> 8) | (s_pal[b] << 8));
            uint16_t ar = (A >> 11) & 0x1F, ag = (A >> 5) & 0x3F, ab = A & 0x1F;
            uint16_t br = (B >> 11) & 0x1F, bg = (B >> 5) & 0x3F, bb = B & 0x1F;
            for (int q = 0; q < FRAC_Q; q++) {
                uint16_t r = (uint16_t)((ar * (FRAC_Q - q) + br * q + FRAC_Q / 2) / FRAC_Q);
                uint16_t g = (uint16_t)((ag * (FRAC_Q - q) + bg * q + FRAC_Q / 2) / FRAC_Q);
                uint16_t bl = (uint16_t)((ab * (FRAC_Q - q) + bb * q + FRAC_Q / 2) / FRAC_Q);
                uint16_t host = (uint16_t)((r << 11) | (g << 5) | bl);
                s_blend_lut[(a * 8 + b) * FRAC_Q + q] = (uint16_t)((host >> 8) | (host << 8));
            }
        }
    }
}

/* 当前生效的基色（模式切换渐变时是插值后的中间色），供 UI 数字配色 */
void thread_get_base_color(uint8_t out[3])
{
    out[0] = s_base[0]; out[1] = s_base[1]; out[2] = s_base[2];
}

static uint16_t slot_color(const uint8_t base[3], float f)
{
    float r, g, b;
    if (f >= 0) { r = base[0] * f;  g = base[1] * f;  b = base[2] * f; }
    else {
        float t = -f;
        r = base[0] + (255 - base[0]) * t;
        g = base[1] + (255 - base[1]) * t;
        b = base[2] + (255 - base[2]) * t;
    }
    return RGB565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* 按基色重建调色板并展开贴图。模式切换 / 变色动画的每一步都调它，
 * 成本只有 2040 次查表，几十微秒。 */
void thread_set_color(const uint8_t base[3])
{
    s_base[0] = base[0]; s_base[1] = base[1]; s_base[2] = base[2];
    for (int i = 0; i < 8; i++) s_pal[i] = slot_color(base, SLOT_F[i]);
    for (int y = 0; y < TILE_H; y++)
        for (int x = 0; x < DISP_W; x++)
            s_tile565[y][x] = s_pal[THREAD_TILE[y][x] & 7];
    rebuild_blend_lut();
}

void thread_set_mode(int mode)
{
    thread_set_color(MODE_BASE[mode % MODE_COUNT]);
}

/* 导出模式基色（供上层在此基础上做 TURBO 红等变体） */
void thread_mode_color(int mode, uint8_t out[3])
{
    const uint8_t *c = MODE_BASE[mode % MODE_COUNT];
    out[0] = c[0]; out[1] = c[1]; out[2] = c[2];
}

void thread_init(void)
{
    s_fb = disp_alloc(DISP_W * DISP_ANIM_H * 2);
    thread_set_mode(MODE_STD);
    s_offset = 0;
}

/* 步进：牙距 * rpm / 3600 像素每帧（60fps）
 * 注意 Nyquist：超过 TILE_H/2 = 8.5 px/帧（约 1800rpm）螺纹会看起来倒转 */
void thread_advance(float rpm, float dt, int reverse)
{
    float px = TILE_H * rpm / 60.0f * dt;
    if (px > TILE_H * 0.45f) px = TILE_H * 0.45f;   /* 卡在 Nyquist 之内 */
    s_offset += reverse ? -px : px;
    while (s_offset < 0)       s_offset += TILE_H;
    while (s_offset >= TILE_H) s_offset -= TILE_H;
}

/* 棘轮：一次咬合直接跳一个牙 */
void thread_detent(void)
{
    s_offset += 1.0f;
    if (s_offset >= TILE_H) s_offset -= TILE_H;
}

/* 合成 + 推屏。每行一次 LUT 查表（相邻两贴图行按小数偏移混合）。 */
void thread_render(void)
{
    /* 先等上一帧 DMA 完成再合成 —— 否则写 s_fb 会撞上正在进行的
     * DMA 读（同一块缓冲），推出去的就是新旧混合的数据，整屏微闪 */
    disp_wait_done();

    int off = (int)s_offset;
    int q = (int)((s_offset - (float)off) * FRAC_Q);
    if (q >= FRAC_Q) q = FRAC_Q - 1;

    for (int y = 0; y < DISP_ANIM_H; y++) {
        int r0 = (y + off) % TILE_H;
        int r1 = (r0 + 1) % TILE_H;
        const uint8_t *t0 = THREAD_TILE[r0];
        const uint8_t *t1 = THREAD_TILE[r1];
        uint16_t *dst = &s_fb[y * DISP_W];
        for (int x = 0; x < DISP_W; x++) {
            dst[x] = s_blend_lut[((t0[x] & 7) * 8 + (t1[x] & 7)) * FRAC_Q + q];
        }
    }

    /* 底部 12 行向下渐隐，让螺纹沉进压条而不是被硬切。
     * 用移位做衰减，避免每像素乘法。 */
    for (int i = 0; i < 12; i++) {
        int y = DISP_ANIM_H - 12 + i;
        int shift = (i * 4) / 12 + 1;              /* 1..4 档 */
        uint16_t *row = &s_fb[y * DISP_W];
        for (int x = 0; x < DISP_W; x++) {
            uint16_t c = (row[x] >> 8) | (row[x] << 8);   /* 转回小端算 */
            uint16_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            r >>= shift; g >>= shift; b >>= shift;
            c = (r << 11) | (g << 5) | b;
            row[x] = (c >> 8) | (c << 8);
        }
    }

    disp_blit(0, DISP_ANIM_Y, DISP_W, DISP_ANIM_H, s_fb);
    /* 注意：不能在这里再 wait 一次 —— 每次 blit 只产生一个完成信号，
     * 每帧等两次会让下一次等待必超时(100ms)，帧率崩到 9。
     * 帧首的一次等待已保证上一帧 DMA 完成，s_fb 可安全写。 */
}
