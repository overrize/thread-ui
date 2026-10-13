/*
 * ui_render.c —— 主界面渲染（与原型 v1.4.5 的 renderFor() / composite() / base() / statusBar() 对应）
 *
 * 管线（每像素行）：
 *   底色（转子查表着色 / 槽格带缓冲 / 黑）→ 两侧机壳暗边 + 玻璃反光 → 文字与状态条 → 圆角 → RGB565 大端
 *
 * 与原型的已知差异（肉眼几乎不可辨，见 docs/BRINGUP_ESP32S31.md §8）：
 *   - 转子景深只做沿表面方向（≈水平）的预模糊，不做竖直方向
 *   - 字体用开源字体替代 PingFang / SF Pro
 *   - TURBO 辉光为预烘焙形状，强度随负载变化而不是半径随负载变化
 */
#include "ui_render.h"
#include "gen/rotor_lut.h"
#include "gen/ui_font.h"
#include <math.h>
#include <stdio.h>
#include <string.h>


#define INSET 20
#define L_X   INSET
#define R_X   (UI_W - INSET)
#define WIRE_LO_Q8 236             /* 横纹暗段 0.92 */

/* 槽格带样式 A（SPEC §5.2） */
#define HALO_K  0.38f   /* 光晕强度 */

#define RGB(r, g, b) {r, g, b}
static const uint8_t C_DIM[3] = RGB(0x7A, 0x82, 0x92), C_DIMMER[3] = RGB(0x56, 0x5C, 0x68);
static const uint8_t C_WHITE[3] = RGB(255, 255, 255), C_BLACK[3] = RGB(0, 0, 0);
static const uint8_t C_RPM[3] = RGB(0xE4, 0xE6, 0xEB), C_ARROW[3] = RGB(0x9A, 0x9A, 0xA2);
static const uint8_t C_UNIT[3] = RGB(0xA8, 0xAF, 0xBC), C_UNIT_T[3] = RGB(0xC9, 0xB8, 0xC0);
static const uint8_t C_BAR[3] = RGB(0x0B, 0x0B, 0x0C), C_BARLINE[3] = RGB(0x1C, 0x1C, 0x1F);

/* ---------------- 每帧常量 ---------------- */
typedef struct {
    const ui_bm_t *bm;     /* NULL = 矩形 */
    int16_t x, y;          /* 位图：笔位 + 基线；矩形：左上角 */
    int16_t w, h;          /* 矩形尺寸 */
    uint8_t c[3];
    uint8_t a;             /* 整体不透明度（0..255） */
} op_t;

#define MAX_OPS 64
static struct {
    int32_t theta_q16;
    int heat_q8, glow_q8, ember_q8, rm, gm, bm;
    bool turbo;
    uint8_t col[3];
    /* 每格每帧预算好的整数量（Q8），槽格合成里不再有浮点 */
    int k8[QD_CELLS];          /* 亮度 k × 256 */
    int cc[QD_CELLS][3];       /* 格色（样式 A：模式色 × (0.70 + 0.30t)，t > 0.8 混 35% 白） */
    int ad8[QD_CELLS];         /* 叠加白光 min(1, add + click·0.15·k) × 256 */
    int hk8[QD_CELLS];         /* 光晕系数 0.38 × pulse × k × 256；k < 0.02 记 −1（不参与） */
    bool hold[QD_CELLS];
    op_t ops[MAX_OPS];
    int n_ops;
} F;

/* 普通静态变量 = 内部 SRAM 的 .bss（不占固件镜像）。不要加 DRAM_ATTR（会进 .data，镜像平白多 100 KB）；
 * 也不要放 PSRAM（每帧整块读写，PSRAM 带宽会拖慢渲染） */
static uint16_t BANDBUF[BAND_ROWS][UI_W][3];
_Static_assert(UI_BAND_ROWS == BAND_ROWS, "ui_render.h 的 UI_BAND_ROWS 与 rotor_lut.h 不一致");

/* 槽格带 2D 景深核（每列固定）：三角权重 max(0, 1−|k|/(rad+1))，与原型 blurInto() 一致 */
#define KMAX 5
static uint8_t  K_RI[UI_W];
static uint16_t K_W[UI_W][KMAX + 1];   /* Q8，k = 0..ri */
static uint32_t K_SUM[UI_W];
/* a / K_SUM[x] 的精确替代：m = ceil(2^40 / sum)，q = (a·m) >> 40。
 * a ≤ 1023·256·11 ≈ 2.9M、sum ∈ [256, 3072) 时与整数除法逐值相同（已穷举 sum、抽查 a 验证）。
 * 变量除法在 RV32 上几十周期，景深每帧约 9 万次，换成乘法省几 ms */
static uint64_t K_INV[UI_W];
#define DIV_KSUM(a, x) ((uint32_t)(((uint64_t)(a) * K_INV[x]) >> 40))

void ui_init(void)
{
    for (int x = 0; x < UI_W; x++) {
        float rad = ROT_COL_BLUR_Q4[x] / 16.f;
        int ri = (int)ceilf(rad);
        if (ri > KMAX) ri = KMAX;
        K_RI[x] = (uint8_t)ri;
        uint32_t sum = 0;
        for (int k = 0; k <= ri; k++) {
            float w = ri ? fmaxf(0, 1 - k / (rad + 1)) : 1;
            K_W[x][k] = (uint16_t)lroundf(w * 256);
            sum += K_W[x][k] * (k ? 2 : 1);
        }
        K_SUM[x] = sum;
        K_INV[x] = ((1ULL << 40) + sum - 1) / sum;
    }
}

/* ---------------- 转子着色：每像素一次乘加 + 一次查表 ---------------- */
static inline void rotor_px(int x, int sy2, int rowk, int *R, int *G, int *B)
{
    int32_t ph = ROT_COL_A[x] + ((sy2 * ROT_COL_B[x]) >> 1) - F.theta_q16;
    const uint8_t *pr = ROT_PROFILE[ROT_COL_LVL[x]][(ph >> 8) & 0xFF];
    int32_t st = (sy2 * ROT_COL_D[x]) >> 1;
    int wire = ((st >> 15) & 1) ? WIRE_LO_Q8 : 256;
    int kt = (wire * F.heat_q8) >> 8;                       /* 齿面：横纹 × 发热 */
    int km = 256 + (((kt - 256) * pr[3] + 128) >> 8);       /* 只作用于齿面 */
    int lf = ROT_COL_LUMF[x];
    int r = ((pr[0] * km) >> 8) * lf >> 8;
    int g = ((pr[1] * km) >> 8) * lf >> 8;
    int b = ((pr[2] * km) >> 8) * lf >> 8;
    int fall = ROT_COL_FALL[x];
    int gw = ((pr[4] * F.glow_q8) >> 8) * fall >> 8;       /* 槽底透出模式色 */
    r += (F.col[0] * gw) >> 8; g += (F.col[1] * gw) >> 8; b += (F.col[2] * gw) >> 8;
    int s = ROT_COL_SPECF[x];                               /* 高光 */
    r += s; g += (s * 200) >> 8; b += (s * 160) >> 8;
    if (F.turbo) {                                          /* TURBO：铜体发红 + 槽缝余烬 */
        r = (r * F.rm) >> 8; g = (g * F.gm) >> 8; b = (b * F.bm) >> 8;
        int e = ((pr[4] * F.ember_q8) >> 8) * fall >> 8;
        r += (200 * e) >> 8; g += (40 * e) >> 8; b += (30 * e) >> 8;
    }
    *R = (r * rowk) >> 8; *G = (g * rowk) >> 8; *B = (b * rowk) >> 8;
}

/* ---------------- 槽格带（原型 composite()，样式 A；整数版） ----------------
 * 底托 ×0.45 去色 50% → 格：×0.60 + 唇光 + 格色混合 + 冲击波白光 + 峰值框；非格：最近亮格的光晕 */
static const int LIP_RGB[3] = {40, 36, 34};               /* 未亮格的受光唇 */
static const int LIP_LIT[3] = {179, 168, 154};            /* 亮格唇光 (255,240,220) × 0.7 */
static inline void composite(int x, int y, int *c)
{
    const band_px_t *p = &BAND_PX[(y - BAND_Y0) * UI_W + x];
    int fall = ROT_COL_FALL[x];                             /* Q8（0..255） */
    if (p->flags & 1) {
        int l = (77 * c[0] + 151 * c[1] + 28 * c[2]) >> 8;  /* 0.30 / 0.59 / 0.11 */
        for (int j = 0; j < 3; j++) c[j] = (((c[j] + l) >> 1) * 115) >> 8;   /* 去色 50%，× 0.45 */
    }
    if (p->cell != 255) {
        int i = p->cell, k8 = F.k8[i], lip = p->lip;
        for (int j = 0; j < 3; j++) c[j] = ((c[j] * 154) >> 8) + ((LIP_RGB[j] * lip) >> 8);   /* × 0.60 */
        if (k8 > 3) {
            int e8 = (fall * k8) >> 8;
            for (int j = 0; j < 3; j++)
                c[j] = ((c[j] * (256 - k8) + F.cc[i][j] * e8) >> 8) + ((((LIP_LIT[j] * lip) >> 8) * e8) >> 8);
        }
        int ad = (F.ad8[i] * fall) >> 8;
        for (int j = 0; j < 3; j++) c[j] += (115 * ad) >> 8;                       /* 255 × 0.45 */
        if (F.hold[i] && (p->flags & 2)) c[0] = c[1] = c[2] = fall;
        return;
    }
    int best = -1, bd = 255;
    for (int m = 0; m < 3; m++) {
        int j = p->ibase - 1 + m;
        if (p->d[m] == 255 || j < 0 || j >= QD_CELLS || F.hk8[j] < 0) continue;
        if (p->d[m] < bd) { bd = p->d[m]; best = j; }
    }
    if (best >= 0) {
        int h = (((HALO_EXP[bd] * F.hk8[best]) >> 8) * fall) >> 8;
        for (int j = 0; j < 3; j++) c[j] += (F.col[j] * h) >> 8;
    }
}

/* 槽格带分三步，每步内部互不依赖，可两个核各算一段（步与步之间要同步）：
 *   1. 合成   ：按行 [r0, r1)（相对 BAND_Y0）
 *   2. 横向模糊：按行 [r0, r1)
 *   3. 纵向模糊：按列 [x0, x1)
 * 单核时依次调用三步、范围取全部即可（ui_frame_begin 就是这么做的）。 */
void ui_band_composite(int r0, int r1)
{
    for (int y = BAND_Y0 + r0; y < BAND_Y0 + r1; y++) {
        int rowk = ROT_ROW_K[y], sy2 = 2 * y + 1 - ROT_CY_X2;
        uint16_t (*row)[3] = BANDBUF[y - BAND_Y0];
        for (int x = 0; x < UI_W; x++) {
            int r = 0, g = 0, b = 0;
            if (rowk) rotor_px(x, sy2, rowk, &r, &g, &b);
            int c[3] = {r, g, b};
            composite(x, y, c);
            for (int j = 0; j < 3; j++) row[x][j] = (uint16_t)(c[j] < 0 ? 0 : (c[j] > 1023 ? 1023 : c[j]));
        }
    }
}

/* 2D 景深：先横向（全部 73 行），再纵向（只输出 BAND_OUT 行），核随列变 */
/* 对称核：a = c·w0 + Σ(左k + 右k)·wk，乘法减半；整数求和与逐点乘加结果完全相同。
 * 横向：离边缘 ≥ ri 的列不用钳位（绝大多数列），只有两端几列走钳位路径。 */
void ui_band_hblur(int r0, int r1)
{
    uint16_t tmp[UI_W][3];
    for (int r = r0; r < r1; r++) {
        memcpy(tmp, BANDBUF[r], sizeof(tmp));
        for (int x = 0; x < UI_W; x++) {
            int ri = K_RI[x];
            if (!ri) continue;
            const uint16_t *w = K_W[x];
            uint32_t a0 = tmp[x][0] * w[0], a1 = tmp[x][1] * w[0], a2 = tmp[x][2] * w[0];
            if (x >= ri && x + ri < UI_W) {
                for (int k = 1; k <= ri; k++) {
                    const uint16_t *L = tmp[x - k], *R = tmp[x + k];
                    a0 += (L[0] + R[0]) * w[k]; a1 += (L[1] + R[1]) * w[k]; a2 += (L[2] + R[2]) * w[k];
                }
            } else {
                for (int k = 1; k <= ri; k++) {
                    int xl = x - k < 0 ? 0 : x - k, xr = x + k >= UI_W ? UI_W - 1 : x + k;
                    a0 += (tmp[xl][0] + tmp[xr][0]) * w[k]; a1 += (tmp[xl][1] + tmp[xr][1]) * w[k]; a2 += (tmp[xl][2] + tmp[xr][2]) * w[k];
                }
            }
            BANDBUF[r][x][0] = DIV_KSUM(a0, x); BANDBUF[r][x][1] = DIV_KSUM(a1, x); BANDBUF[r][x][2] = DIV_KSUM(a2, x);
        }
    }
}

/* 纵向：输出行离槽格带上下边都 ≥ 7 行（BAND_OUT 比 BAND 各缩 7），核半径 ≤ KMAX=5，永远不用钳位 */
_Static_assert(BAND_OUT_Y0 - BAND_Y0 >= KMAX && BAND_Y1 - BAND_OUT_Y1 >= KMAX, "纵向模糊需要钳位");
void ui_band_vblur(int x0, int x1)
{
    uint16_t col[BAND_ROWS][3];
    for (int x = x0; x < x1; x++) {
        int ri = K_RI[x];
        if (!ri) continue;
        const uint16_t *w = K_W[x];
        for (int r = 0; r < BAND_ROWS; r++) { col[r][0] = BANDBUF[r][x][0]; col[r][1] = BANDBUF[r][x][1]; col[r][2] = BANDBUF[r][x][2]; }
        for (int r = BAND_OUT_Y0 - BAND_Y0; r <= BAND_OUT_Y1 - BAND_Y0; r++) {
            uint32_t a0 = col[r][0] * w[0], a1 = col[r][1] * w[0], a2 = col[r][2] * w[0];
            for (int k = 1; k <= ri; k++) {
                const uint16_t *U = col[r - k], *D = col[r + k];
                a0 += (U[0] + D[0]) * w[k]; a1 += (U[1] + D[1]) * w[k]; a2 += (U[2] + D[2]) * w[k];
            }
            BANDBUF[r][x][0] = DIV_KSUM(a0, x); BANDBUF[r][x][1] = DIV_KSUM(a1, x); BANDBUF[r][x][2] = DIV_KSUM(a2, x);
        }
    }
}

/* ---------------- 文字 / 矩形绘制列表 ---------------- */
static void op_bm(const ui_bm_t *bm, int x, int y, const uint8_t c[3], int a)
{
    if (F.n_ops >= MAX_OPS) return;
    op_t *o = &F.ops[F.n_ops++];
    o->bm = bm; o->x = x; o->y = y; o->w = o->h = 0;
    memcpy(o->c, c, 3); o->a = (uint8_t)(a < 0 ? 0 : (a > 255 ? 255 : a));
}
static void op_rect(int x, int y, int w, int h, const uint8_t c[3], int a)
{
    if (F.n_ops >= MAX_OPS) return;
    op_t *o = &F.ops[F.n_ops++];
    o->bm = NULL; o->x = x; o->y = y; o->w = w; o->h = h;
    memcpy(o->c, c, 3); o->a = (uint8_t)a;
}
/* 固定字符串：align 0 = 左，1 = 右（x 为右边界）。返回宽度（原型 measureRuns） */
static int op_str(int id, int x, int y, const uint8_t c[3], int align)
{
    const ui_bm_t *bm = &UI_STR[id];
    op_bm(bm, align ? x - bm->adv : x, y, c, 255);
    return bm->adv;
}
static int dig_index(char ch) { return ch == '.' ? 10 : ch - '0'; }
/* 数字串。tabular = 等宽数字（原型 num()），否则按字形本身宽度（原型 txt()）。返回宽度 */
static int op_num(const ui_digits_t *f, const char *s, int x, int y, const uint8_t c[3], int a, bool tabular, int align)
{
    int total = 0;
    for (const char *p = s; *p; p++) {
        const ui_bm_t *g = &f->g[dig_index(*p)];
        total += (tabular && *p != '.') ? f->adv0 : g->adv;
    }
    int cx = align ? x - total : x;
    for (const char *p = s; *p; p++) {
        const ui_bm_t *g = &f->g[dig_index(*p)];
        int cell = (tabular && *p != '.') ? f->adv0 : g->adv;
        op_bm(g, cx + (cell - g->adv) / 2, y, c, a);
        cx += cell;
    }
    return total;
}

static void build_ops(const qd_state_t *s)
{
    const qd_mode_t *M = &QD_MODE[s->mode];
    const uint8_t *acc = F.col;
    char buf[16];
    F.n_ops = 0;

    /* 转速行（SPEC §2.1：y=70，一行；棘轮为"分度 N 格"） */
    const int RPM_Y = 70;
    if (qd_is_ratchet(s)) {
        op_str(STR_FENDU, L_X, RPM_Y, C_DIM, 0);
        snprintf(buf, sizeof buf, "%d", s->clicks);
        op_num(&UI_DIG_RPM, buf, UI_RAT_X0, RPM_Y, C_RPM, 255, true, 0);
        op_str(STR_GE, UI_RAT_UNIT_X, RPM_Y, C_DIM, 0);
    } else {
        snprintf(buf, sizeof buf, "%d", (int)lround(fabs(s->rpm)));
        op_num(&UI_DIG_RPM, buf, L_X - 1, RPM_Y, C_RPM, 255, true, 0);
        op_str(STR_RPM, UI_RPM_UNIT_X, RPM_Y, C_DIM, 0);
        op_str(M->rpm_max > 300 ? STR_LIM_RPM900 : STR_LIM_RPM300, R_X, RPM_Y, C_DIMMER, 1);
    }

    /* 扭矩标签行（y=372）：扭矩 · 拧紧/松出 …… 上限 / 峰值 / 满 */
    const int TL = 372;
    int lw = op_str(STR_TQ, L_X, TL, C_DIM, 0);
    int dw = op_str(STR_DOT, L_X + lw + 6, TL, C_DIMMER, 0);
    op_str(s->rev ? STR_LOOSE : STR_TIGHT, L_X + lw + dw + 12, TL, s->rev ? C_WHITE : acc, 0);
    if (qd_show_peak(s)) {
        snprintf(buf, sizeof buf, "%.2f", s->peak);
        int pw = op_num(&UI_DIG_PEAK, buf, R_X, TL, C_WHITE, 255, false, 1);
        op_str(STR_PEAK, R_X - pw - 8, TL, C_DIM, 1);
    } else if (M->turbo) {
        op_str(STR_FULL300, R_X, TL, acc, 1);
    } else {
        op_str(M->tq_max > 1.5f ? STR_LIM_TQ300 : STR_LIM_TQ100, R_X, TL, C_DIMMER, 1);
    }

    /* 扭矩数字 + N·m（y=442） */
    const int TQ_Y = 442;
    snprintf(buf, sizeof buf, "%.2f", s->torque);
    if (!M->turbo) {
        op_num(&UI_DIG_TQ, buf, L_X - 3, TQ_Y, C_WHITE, 255, true, 0);
        op_str(STR_NM, UI_TQ_UNIT_X, TQ_Y, C_UNIT, 0);
    } else {
        /* TURBO：前倾（字模已预斜切）+ 模式色辉光（强度随负载）+ 接近满扭矩时数字发热 */
        float k = s->torque / M->tq_max;
        uint8_t hot[3] = {255, (uint8_t)lroundf(255 - 95 * k * k), (uint8_t)lroundf(255 - 110 * k * k)};
        int ga = (int)lroundf(255 * (0.55f + 0.45f * s->load));
        op_num(&UI_DIG_TQT_GLOW, buf, L_X - 3, TQ_Y, acc, ga, true, 0);
        op_bm(&UI_STR_GLOW_NM_TURBO, UI_TQT_UNIT_X, TQ_Y, acc, ga);
        op_num(&UI_DIG_TQT, buf, L_X - 3, TQ_Y, hot, 255, true, 0);
        op_str(STR_NM_TURBO, UI_TQT_UNIT_X, TQ_Y, C_UNIT_T, 0);
    }

    /* 状态条（原型 statusBar()）：最后画，盖在最上面 */
    op_rect(0, 0, UI_W, LUT_BAR_H, C_BAR, 255);
    op_rect(0, LUT_BAR_H - 1, UI_W, 1, C_BARLINE, 255);
    int cw = UI_CHIP_W[s->mode];
    op_rect(INSET, 3, cw, 20, acc, 255);
    op_str(STR_MODE_0 + s->mode, INSET + 5, M->turbo ? 19 : 20, C_BLACK, 0);
    op_str(s->rev ? STR_CCW : STR_CW, INSET + cw + 8, 21, s->rev ? C_WHITE : C_ARROW, 0);
    if (s->done) op_str(STR_DONE, INSET + cw + 34, 20, acc, 0);
    /* 电量：原型 strokeRect(173.5, 6.5, 30, 14)、线宽 1.5 —— 中心线 1 px 实色，两侧各 0.25 覆盖 */
    {
        const int x0 = 173, x1 = 203, y0 = 6, y1 = 20;     /* 中心线像素 */
        op_rect(x0 - 1, y0 - 1, x1 - x0 + 3, 1, C_ARROW, 64);  op_rect(x0 - 1, y1 + 1, x1 - x0 + 3, 1, C_ARROW, 64);
        op_rect(x0, y0 + 1, x1 - x0 + 1, 1, C_ARROW, 64);      op_rect(x0, y1 - 1, x1 - x0 + 1, 1, C_ARROW, 64);
        op_rect(x0 - 1, y0, 1, y1 - y0 + 1, C_ARROW, 64);      op_rect(x1 + 1, y0, 1, y1 - y0 + 1, C_ARROW, 64);
        op_rect(x0 + 1, y0 + 1, 1, y1 - y0 - 1, C_ARROW, 64);  op_rect(x1 - 1, y0 + 1, 1, y1 - y0 - 1, C_ARROW, 64);
        op_rect(x0, y0, x1 - x0 + 1, 1, C_ARROW, 255);         op_rect(x0, y1, x1 - x0 + 1, 1, C_ARROW, 255);
        op_rect(x0, y0, 1, y1 - y0 + 1, C_ARROW, 255);         op_rect(x1, y0, 1, y1 - y0 + 1, C_ARROW, 255);
        op_rect(UI_W - INSET - 32, 9, 20, 9, C_ARROW, 255);    /* 电量块 */
        op_rect(UI_W - INSET - 4, 10, 2, 7, C_ARROW, 255);     /* 正极 */
    }
}

void ui_frame_setup(const qd_state_t *s, const qd_cells_t *c)
{
    const qd_mode_t *M = &QD_MODE[s->mode];
    double ph = s->theta / (2.0 * 3.14159265358979323846 / 12);
    ph -= floor(ph);
    F.theta_q16 = (int32_t)(ph * 65536.0);
    F.heat_q8 = (int)lroundf((0.85f + 0.30f * s->load) * 256);
    F.glow_q8 = (int)lroundf(0.30f * s->load * 256);
    F.turbo = M->turbo;
    float h = 0.30f + 0.45f * s->load;
    F.rm = (int)lroundf((1 + 0.10f * h) * 256); F.gm = (int)lroundf((1 - 0.30f * h) * 256); F.bm = (int)lroundf((1 - 0.22f * h) * 256);
    F.ember_q8 = (int)lroundf(s->load * 0.28f * 256);
    memcpy(F.col, M->col, 3);
    float pulse = (M->turbo && !s->calm) ? 1 + 0.08f * (float)s->load * sinf((float)s->now * 4) : 1;
    for (int i = 0; i < QD_CELLS; i++) {
        float k = (float)c->k[i], t = c->st[i] / (float)(QD_CELLS - 1), m = 0.70f + 0.30f * t;
        F.k8[i] = (int)lroundf(k * 256);
        for (int j = 0; j < 3; j++) {
            float v = M->col[j] * m; if (t > 0.8f) v += (255 - v) * 0.35f;
            F.cc[i][j] = (int)lroundf(v);
        }
        F.ad8[i] = (int)lroundf(fminf(1, (float)c->add[i] + (float)s->click_flash * 0.15f * k) * 256);
        F.hk8[i] = k < 0.02f ? -1 : (int)lroundf(HALO_K * pulse * k * 256);
        F.hold[i] = c->hold[i];
    }
    build_ops(s);
}

void ui_frame_begin(const qd_state_t *s, const qd_cells_t *c)
{
    ui_frame_setup(s, c);
    ui_band_composite(0, BAND_ROWS);
    ui_band_hblur(0, BAND_ROWS);
    ui_band_vblur(0, UI_W);
}

/* ---------------- 行合成 ---------------- */
static inline uint16_t pack565be(int r, int g, int b)
{
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));              /* SPI 先发高字节 */
}

static void draw_ops_row(int y, int16_t (*row)[3])
{
    for (int i = 0; i < F.n_ops; i++) {
        const op_t *o = &F.ops[i];
        if (!o->bm) {
            if (y < o->y || y >= o->y + o->h) continue;
            int x0 = o->x < 0 ? 0 : o->x, x1 = o->x + o->w > UI_W ? UI_W : o->x + o->w;
            for (int x = x0; x < x1; x++)
                for (int j = 0; j < 3; j++) row[x][j] += ((o->c[j] - row[x][j]) * o->a) / 255;
            continue;
        }
        const ui_bm_t *bm = o->bm;
        int top = o->y + bm->oy;
        if (y < top || y >= top + bm->h) continue;
        const uint8_t *src = &UI_ALPHA[bm->off + (uint32_t)(y - top) * bm->w];
        int left = o->x + bm->ox;
        for (int i2 = 0; i2 < bm->w; i2++) {
            int x = left + i2;
            if (x < 0 || x >= UI_W || !src[i2]) continue;
            int a = o->a == 255 ? src[i2] : (src[i2] * o->a) / 255;
            for (int j = 0; j < 3; j++) row[x][j] += ((o->c[j] - row[x][j]) * a) / 255;
        }
    }
}

void ui_render_rows(int y0, int y1, uint16_t *dst)
{
    int16_t row[UI_W][3];
    for (int y = y0; y < y1; y++, dst += UI_W) {
        /* 1. 底色 */
        if (y >= BAND_OUT_Y0 && y <= BAND_OUT_Y1) {
            const uint16_t (*b)[3] = (const uint16_t (*)[3])BANDBUF[y - BAND_Y0];
            for (int x = 0; x < UI_W; x++) { row[x][0] = b[x][0] > 255 ? 255 : b[x][0]; row[x][1] = b[x][1] > 255 ? 255 : b[x][1]; row[x][2] = b[x][2] > 255 ? 255 : b[x][2]; }
        } else if (ROT_ROW_K[y]) {
            int rowk = ROT_ROW_K[y], sy2 = 2 * y + 1 - ROT_CY_X2;
            for (int x = 0; x < UI_W; x++) {
                int r, g, b; rotor_px(x, sy2, rowk, &r, &g, &b);
                row[x][0] = r > 255 ? 255 : r; row[x][1] = g > 255 ? 255 : g; row[x][2] = b > 255 ? 255 : b;
            }
        } else {
            memset(row, 0, sizeof row);
        }
        /* 2. 机壳暗边 + 静止的斜向玻璃反光（原型 frame()，状态条以下） */
        if (y >= LUT_BAR_H) {
            for (int x = 0; x < UI_W; x++) {
                int v = ROT_COL_VIG[x];
                if (v < 255) { row[x][0] = row[x][0] * v / 255; row[x][1] = row[x][1] * v / 255; row[x][2] = row[x][2] * v / 255; }
            }
            /* 渐变方向 (W, 290)，起点 (0, BAR_H)：t∈[.32,.40]，峰值 .36 处 alpha .05（整数，坐标 ×2 去掉 0.5） */
            const int32_t DEN2 = 2 * (UI_W * UI_W + 290 * 290);
            const int32_t C36 = DEN2 * 36 / 100, D04 = DEN2 * 4 / 100;
            int32_t base2 = (2 * y + 1 - 2 * LUT_BAR_H) * 290;
            int xa = (C36 - D04 - base2) / (2 * UI_W) - 1, xb = (C36 + D04 - base2) / (2 * UI_W) + 1;
            if (xa < 0) xa = 0;
            if (xb > UI_W - 1) xb = UI_W - 1;
            for (int x = xa; x <= xb; x++) {
                int32_t num = (2 * x + 1) * UI_W + base2, dd = num - C36; if (dd < 0) dd = -dd;
                if (dd >= D04) continue;
                int a = (int)((int64_t)(D04 - dd) * 13 / D04);      /* 0.05 × 256 ≈ 13 */
                row[x][0] += (210 * a) >> 8; row[x][1] += (230 * a) >> 8; row[x][2] += (255 * a) >> 8;
            }
        }
        /* 3. 文字、状态条 */
        draw_ops_row(y, row);
        /* 4. 圆角（16 px，抗锯齿） */
        int cy = y < CORNER_R ? y : (y >= UI_H - CORNER_R ? UI_H - 1 - y : -1);
        if (cy >= 0) {
            for (int i = 0; i < CORNER_R; i++) {
                int a = CORNER_A[cy * CORNER_R + i];
                for (int j = 0; j < 3; j++) { row[i][j] = row[i][j] * a / 255; row[UI_W - 1 - i][j] = row[UI_W - 1 - i][j] * a / 255; }
            }
        }
        for (int x = 0; x < UI_W; x++) dst[x] = pack565be(row[x][0], row[x][1], row[x][2]);
    }
}

/* ---------------- 点亮测试图 ---------------- */
void ui_test_pattern_rows(int y0, int y1, uint16_t *dst, int phase)
{
    static const uint8_t BARS[8][3] = {{255,255,255},{255,255,0},{0,255,255},{0,255,0},{255,0,255},{255,0,0},{0,0,255},{0,0,0}};
    for (int y = y0; y < y1; y++, dst += UI_W) {
        for (int x = 0; x < UI_W; x++) {
            int r, g, b;
            if (y < 200) {                                   /* 上部：8 条色条（白 黄 青 绿 品 红 蓝 黑） */
                const uint8_t *c = BARS[x * 8 / UI_W]; r = c[0]; g = c[1]; b = c[2];
            } else if (y < 230) { r = x * 255 / (UI_W - 1); g = b = 0; }     /* R 渐变 */
            else if (y < 260) { g = x * 255 / (UI_W - 1); r = b = 0; }       /* G 渐变 */
            else if (y < 290) { b = x * 255 / (UI_W - 1); r = g = 0; }       /* B 渐变 */
            else if (y < 320) { r = g = b = x * 255 / (UI_W - 1); }           /* 灰阶 */
            else {                                           /* 下部：2×2 棋盘，按 phase 翻转 —— 看偶数对齐与撕裂 */
                int on = (((x >> 1) + (y >> 1) + phase) & 1);
                r = g = b = on ? 200 : 30;
            }
            /* 1 px 白边框：四边都应完整可见；缺哪边 = 偏移/尺寸不对 */
            if (x == 0 || y == 0 || x == UI_W - 1 || y == UI_H - 1) r = g = b = 255;
            /* 四角色块：左上红、右上绿、左下蓝、右下白 */
            if (x >= 4 && x < 24 && y >= 4 && y < 24) { r = 255; g = b = 0; }
            if (x >= UI_W - 24 && x < UI_W - 4 && y >= 4 && y < 24) { g = 255; r = b = 0; }
            if (x >= 4 && x < 24 && y >= UI_H - 24 && y < UI_H - 4) { b = 255; r = g = 0; }
            if (x >= UI_W - 24 && x < UI_W - 4 && y >= UI_H - 24 && y < UI_H - 4) { r = g = b = 255; }
            dst[x] = pack565be(r, g, b);
        }
    }
}
