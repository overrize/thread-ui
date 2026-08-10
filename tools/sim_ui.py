#!/usr/bin/env python3
# sim_ui.py —— thread_ui 的 PC 端渲染/交互模拟器
#
# 把固件的渲染逻辑(螺纹动画 / 状态条 / 双态压条 / 菜单 / 休眠屏)在 PC 上
# 用同一套数学跑一遍,输出 PNG。先在电脑上确认布局和交互,再烧录。
#
# 用法:
#   python tools/sim_ui.py                # 输出一套默认演示帧到 sim_out/
#   python tools/sim_ui.py click scroll 3 frames 30 double frames 45
#
# 事件:
#   scroll <n>  滚动 n 格(锁定期被忽略,和固件一致)
#   click       短按:解锁扭矩 / 菜单选择
#   double      双击:换模式(带回锁 + 螺纹快速滚动动画)
#   long        长按:进/出菜单
#   frames <n>  空闲推进 n 帧(1/60s),驱动趋近动画/回锁/休眠计时
#   run <n>     扣扳机 n 帧(螺纹按转速设定转动;棘轮模式咬牙)
#   limit       切换"扭矩到上限"状态(MAX 印章演示)
#
# 与固件的对应关系:
#   app_main.c     -> UI 类(扭矩模型/扭矩锁/休眠/颜色与滚动动画)
#   thread_anim.c  -> Thread 类(调色板推导、分数行混合、底部渐隐)
#   ui_widgets.c   -> AA 文字 + 双态压条 + 休眠屏(同一套 3x5 字库 4x4 超采样)
#   menu.c         -> Menu 类(同一套手势状态机 + 光标趋近)
#   thread_tile.h  -> 直接解析 C 数组,保持同一份贴图数据

import math, os, re, struct, sys, zlib

W, H = 120, 240
BAR_H, ANIM_Y, ANIM_H, SCRIM_Y, SCRIM_H = 14, 14, 172, 186, 54
TILE_H = 17

HERE = os.path.dirname(os.path.abspath(__file__))
TILE_H_PATH = os.path.join(HERE, "..", "main", "thread_tile.h")
OUT_DIR = os.path.join(HERE, "..", "sim_out")

# ---------------- 3x5 字库(与 ui_widgets.c 相同) ----------------
FONT = {
 '0':[7,5,5,5,7],'1':[2,6,2,2,7],'2':[7,1,7,4,7],'3':[7,1,7,1,7],'4':[5,5,7,1,1],
 '5':[7,4,7,1,7],'6':[7,4,7,5,7],'7':[7,1,1,2,2],'8':[7,5,7,5,7],'9':[7,5,7,1,7],
 '.':[0,0,0,0,2],':':[0,2,0,2,0],'%':[5,1,2,4,5],' ':[0,0,0,0,0],'>':[4,2,1,2,4],
 'A':[2,5,7,5,5],'B':[6,5,6,5,6],'C':[3,4,4,4,3],'D':[6,5,5,5,6],'E':[7,4,7,4,7],
 'F':[7,4,7,4,4],'G':[3,4,5,5,3],'H':[5,5,7,5,5],'I':[7,2,2,2,7],'J':[1,1,1,5,2],
 'K':[5,5,6,5,5],'L':[4,4,4,4,7],'M':[5,7,7,5,5],'N':[6,5,5,5,5],'O':[7,5,5,5,7],
 'P':[7,5,7,4,4],'Q':[7,5,5,7,1],'R':[6,5,6,5,5],'S':[3,4,2,1,6],'T':[7,2,2,2,2],
 'U':[5,5,5,5,7],'V':[5,5,5,5,2],'W':[5,5,7,7,5],'X':[5,5,2,5,5],'Y':[5,5,2,2,2],
 'Z':[7,1,2,4,7],'!':[2,2,2,0,2],
}
SS = 4

def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

class Canvas:
    def __init__(self, w=W, h=H):
        self.w, self.h = w, h
        self.px = [[0] * w for _ in range(h)]
    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c
    def fill(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.put(xx, yy, c)
    def blend(self, x, y, fg, a):
        if not (0 <= x < self.w and 0 <= y < self.h): return
        bg = self.px[y][x]
        r = ((bg >> 11) & 31) * (255 - a) + ((fg >> 11) & 31) * a
        g = ((bg >> 5) & 63) * (255 - a) + ((fg >> 5) & 63) * a
        b = (bg & 31) * (255 - a) + (fg & 31) * a
        self.px[y][x] = ((r // 255) << 11) | ((g // 255) << 5) | (b // 255)
    def paste(self, other, ox, oy):
        for y in range(other.h):
            for x in range(other.w):
                v = other.px[y][x]
                if 0 <= ox + x < self.w and 0 <= oy + y < self.h:
                    self.px[oy + y][ox + x] = v
    def save(self, path):
        raw = b''
        for row in self.px:
            raw += b'\x00'
            for v in row:
                raw += bytes([((v >> 11) & 31) * 255 // 31,
                              ((v >> 5) & 63) * 255 // 63,
                              (v & 31) * 255 // 31])
        def chunk(t, d):
            c = t + d
            return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c))
        with open(path, 'wb') as f:
            f.write(b'\x89PNG\r\n\x1a\n'
                    + chunk(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 2, 0, 0, 0))
                    + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def glyph_aa(cv, x, y, size, ch, color):
    gl = FONT.get(ch)
    if not gl or size < 2: return
    w = size * 3 / 5
    for py in range(int(math.floor(y)), int(math.ceil(y + size))):
        for px in range(int(math.floor(x)), int(math.ceil(x + w))):
            cov = 0
            for sy in range(SS):
                v = (py + (sy + 0.5) / SS - y) / size * 5
                if v < 0 or v >= 5: continue
                row = int(v)
                for sx in range(SS):
                    u = (px + (sx + 0.5) / SS - x) / w * 3
                    if u < 0 or u >= 3: continue
                    if gl[row] & (4 >> int(u)): cov += 1
            if cov:
                cv.blend(px, py, color, cov * 255 // (SS * SS))

def text(cv, x, y, s, size, color):
    adv = size * 3 / 5 + size * 0.15
    for ch in s:
        glyph_aa(cv, x, y, size, ch, color)   # 定义是 (cv, x, y, size, ch, color)
        x += adv
    return x

def text_c(cv, cx, y, s, size, color):
    adv = size * 3 / 5 + size * 0.15
    return text(cv, cx - (len(s) * adv - size * 0.15) / 2, y, s, size, color)

def text_r(cv, rx, y, s, size, color):
    adv = size * 3 / 5 + size * 0.15
    return text(cv, rx - (len(s) * adv - size * 0.15), y, s, size, color)

def text_v(cv, x, y, s, size, color):
    """竖排文本：横向渲染后逆时针 90°(从下往上读)"""
    adv = size * 3 / 5 + size * 0.15
    tw = int(math.ceil(len(s) * adv - size * 0.15))
    th = int(math.ceil(size))
    tmp = Canvas(tw, th)
    text(tmp, 0, 0, s, size, color)
    for sy in range(th):
        for sx in range(tw):
            v = tmp.px[sy][sx]
            if v:
                cv.put(int(x) + sy, int(y) + (tw - 1 - sx), v)

# ---------------- 螺纹(与 thread_anim.c 相同) ----------------
MODE_BASE = [(0x2F, 0xA8, 0xFF), (0xFF, 0x8A, 0x2B), (0x3E, 0xD6, 0x6A)]
MODE_PRECISE, MODE_STD, MODE_RATCHET = 0, 1, 2
TURBO_RED = (0xFF, 0x2D, 0x2D)
SLOT_F = [0.00, 0.06, 0.20, 0.34, 0.16, 0.42, 0.72, -0.20]

def load_tile():
    src = open(TILE_H_PATH, encoding='utf-8').read()
    body = src[src.index('{'):src.rindex('}')]
    rows = [[int(v) for v in row.split(',') if v.strip()]
            for row in re.findall(r'\{([0-9,]+)\}', body)]
    assert len(rows) == TILE_H and all(len(r) == W for r in rows), \
        f"tile parse: {len(rows)} rows"
    return rows

TILE = load_tile()

def palette(base):
    def slot(f):
        if f >= 0:
            r, g, b = base[0] * f, base[1] * f, base[2] * f
        else:
            t = -f
            r = base[0] + (255 - base[0]) * t
            g = base[1] + (255 - base[1]) * t
            b = base[2] + (255 - base[2]) * t
        return rgb565(int(r), int(g), int(b))
    return [slot(f) for f in SLOT_F]

class Thread:
    def __init__(self):
        self.offset = 0.0
        self.set_base(MODE_BASE[MODE_STD])
    def set_base(self, base):
        self.pal = palette(base)
    def advance(self, rpm, dt):
        px = TILE_H * rpm / 60.0 * dt
        px = min(px, TILE_H * 0.45)         # Nyquist 上限,同固件
        self.offset = (self.offset + px) % TILE_H
    def detent(self):
        self.offset = (self.offset + 1.0) % TILE_H
    def render_rows(self):
        """返回动画区 182 行 RGB565(分数行混合 + 底部 12 行渐隐)"""
        off = int(self.offset)
        q = min(15, int((self.offset - off) * 16))
        lut = {}
        for a in range(8):
            for b in range(8):
                A, B = self.pal[a], self.pal[b]
                ar, ag, ab = (A >> 11) & 31, (A >> 5) & 63, A & 31
                br, bg, bb = (B >> 11) & 31, (B >> 5) & 63, B & 31
                lut[(a, b)] = (((ar * (16 - q) + br * q + 8) // 16) << 11
                               | ((ag * (16 - q) + bg * q + 8) // 16) << 5
                               | ((ab * (16 - q) + bb * q + 8) // 16))
        rows = []
        for y in range(ANIM_H):
            r0 = (y + off) % TILE_H
            r1 = (r0 + 1) % TILE_H
            rows.append([lut[(a & 7, b & 7)] for a, b in zip(TILE[r0], TILE[r1])])
        for i in range(12):
            y = ANIM_H - 12 + i
            shift = (i * 4) // 12 + 1
            row = rows[y]
            for x in range(W):
                c = row[x]
                r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
                row[x] = ((r >> shift) << 11) | ((g >> shift) << 5) | (b >> shift)
        return rows

# ---------------- 状态条 / 压条 / 休眠屏(与 ui_widgets.c 相同) ----------------
MODE_TAG = ["PRE", "STD", "RCH"]

def batt_rgb(pct, charging):
    """电量颜色规则:充电绿;<15% 红;<50% 黄;其余白"""
    if charging: return (60, 220, 100)
    if pct < 15: return (255, 60, 60)
    if pct < 50: return (255, 200, 40)
    return (240, 240, 240)

def draw_topbar(cv, mode, rpm, tq_locked, base, batt_pct=100, batt_charging=False):
    bar = Canvas(W, BAR_H)
    c_mode = rgb565(*[int(c) for c in base])
    c_dim = 0x52AA
    c_text = c_dim if tq_locked else c_mode
    text(bar, 4, 2, MODE_TAG[mode % 3], 9.0, c_text)
    text_r(bar, 92, 2, str(rpm), 9.0, c_dim if tq_locked else 0x7BEF)
    bat = rgb565(*batt_rgb(batt_pct, batt_charging))
    for x in range(102, 115): bar.put(x, 3, bat); bar.put(x, 10, bat)
    for y in range(3, 11): bar.put(102, y, bat); bar.put(114, y, bat)
    for y in (5, 6, 7, 8): bar.put(115, y, bat)
    fillw = min(100, max(0, batt_pct)) * 9 // 100
    for y in range(5, 9):
        for x in range(104, 104 + fillw): bar.put(x, y, bat)
    cv.paste(bar, 0, 0)

def fmt_x100(v):
    v = min(999, max(0, v))
    return f"{v // 100}.{v % 100:02d}"

def draw_scrim(cv, set_x100, fb_x100, view_t, base, turbo, at_limit):
    """双态压条:view_t=0 反馈大/设定小,view_t=1 设定放大到主位(与固件同公式)"""
    sc = Canvas(W, SCRIM_H)
    if at_limit:
        red = rgb565(255, 60, 60)
        for x in range(28, 92):
            sc.put(x, 8, red); sc.put(x, 9, red); sc.put(x, 32, red); sc.put(x, 33, red)
        for y in range(8, 34):
            sc.put(28, y, red); sc.put(29, y, red); sc.put(90, y, red); sc.put(91, y, red)
        text_c(sc, 60, 13, "MAX", 16.0, 0xF926)
    else:
        sr, sg, sb = (255, 73, 38) if turbo else tuple(int(c) for c in base)
        c_set = rgb565(sr, sg, sb)
        # 设定上限:顶部条带(16px) <-> 主位大数字(28px);左对齐 x=13
        text(sc, 13, 2 + 22 * view_t, fmt_x100(set_x100), 16 + 12 * view_t, c_set)
        # 实时反馈:白色,28px 占满主位(y24..52) <-> 顶部条带
        text(sc, 13, 24 - 22 * view_t, fmt_x100(fb_x100), 28 - 12 * view_t, 0xFFFF)
        # 单位:加大,颜色跟随主位(白 -> 设定色)
        c_unit = rgb565(*[int(255 + (c - 255) * view_t) for c in (sr, sg, sb)])
        text_v(sc, 103, 22, "N.M", 13.0, c_unit)
    cv.paste(sc, 0, SCRIM_Y)

def draw_sleep(cv, clock_text):
    """休眠屏:暗底 + 时钟 + 电量占位框(固件里还配合降亮度,模拟只画内容)"""
    text_c(cv, 60, 96, clock_text, 22.0, 0x4208)
    bat = rgb565(70, 70, 70)
    for x in range(47, 72): cv.put(x, 140, bat); cv.put(x, 151, bat)
    for y in range(140, 152): cv.put(47, y, bat); cv.put(71, y, bat)
    for y in (143, 144, 147, 148): cv.put(72, y, bat)

# ---------------- 菜单(与 menu.c 相同的状态机 + 光标趋近) ----------------
OPT_REFLEX = ["OFF", "LIGHT", "STD", "STRONG"]
OPT_DIR = ["CW", "CCW"]
OPT_UNIT = ["N.M", "KGCM", "LBIN"]
OPT_YN = ["CANCEL", "SURE!"]
ROW_H, ROW_Y0 = 24, 18
CURSOR_EASE_W = 0.28

class Menu:
    def __init__(self):
        self.rpm_max, self.reflex, self.grip = 450, 2, 5
        self.reverse, self.unit = 0, 0
        self.clock_h, self.clock_m = 0, 0       # RTC 对时(模拟不联动系统时间)
        self.level, self.cursor = 'MAIN', 0
        self.cursor_pos = 0.0                   # 光标动画位置(浮点行号)
        self.editing = self.confirming = False
        self.confirm_sel = 0
        self.backup = 0
    def items(self):
        if self.level == 'MAIN':
            return [("SPD MAX", 'num', 'rpm_max', 100, 900, 50, None),
                    ("REFLEX", 'enum', 'reflex', 0, 0, 0, OPT_REFLEX),
                    ("GRIP SENS", 'num', 'grip', 1, 10, 1, None),
                    ("DIRECTION", 'enum', 'reverse', 0, 0, 0, OPT_DIR),
                    ("SYSTEM", 'sub', None, 0, 0, 0, None)]
        return [("CALIB", 'read', None, 0, 0, 0, "--"),
                ("CLOCK H", 'num', 'clock_h', 0, 23, 1, None),
                ("CLOCK M", 'num', 'clock_m', 0, 59, 1, None),
                ("UNIT", 'enum', 'unit', 0, 0, 0, OPT_UNIT),
                ("FACTORY", 'danger', None, 0, 0, 0, OPT_YN),
                ("ABOUT", 'read', None, 0, 0, 0, "V0.1")]
    def handle(self, e, d=0):
        items = self.items()
        label, typ, attr, mn, mx, step, opts = items[self.cursor]
        if e == 'scroll':
            if self.confirming:
                self.confirm_sel = (self.confirm_sel + (1 if d > 0 else -1)) % 2
            elif self.editing:
                v = getattr(self, attr)
                if typ == 'num':
                    v = min(mx, max(mn, v + d * step))
                else:
                    v = (v + (1 if d > 0 else -1)) % len(opts)
                setattr(self, attr, v)
            else:
                self.cursor = (self.cursor + (1 if d > 0 else -1)) % len(items)
        elif e == 'click':
            if self.confirming:
                self.confirming = False
            elif self.editing:
                self.editing = False
            elif typ in ('num', 'enum'):
                self.backup = getattr(self, attr)
                self.editing = True
            elif typ == 'danger':
                self.confirming, self.confirm_sel = True, 0
            elif typ == 'sub':
                self.level, self.cursor = 'SYSTEM', 0
        elif e == 'long':
            if self.confirming:
                self.confirming = False
            elif self.editing:
                setattr(self, attr, self.backup)
                self.editing = False
            elif self.level == 'SYSTEM':
                self.level, self.cursor = 'MAIN', 0
            else:
                return False
        return True
    def step(self):
        """光标趋近(每帧调用,与固件 CURSOR_EASE_W 一致)"""
        cd = self.cursor - self.cursor_pos
        if -0.06 < cd < 0.06:
            self.cursor_pos = float(self.cursor)
        else:
            self.cursor_pos += cd * CURSOR_EASE_W
    def render(self, cv):
        c_title, c_text = 0x52AA, 0x7BEF
        c_hl = rgb565(24, 28, 36)
        c_warn = 0xF800
        text_c(cv, W / 2, 2, "SETUP" if self.level == 'MAIN' else "SYSTEM", 11.0, c_title)
        cv.fill(0, 15, W, 1, rgb565(60, 60, 60))
        # 光标条画在连续浮点位置上(先画,文字叠在上)
        hl_y = ROW_Y0 + int(self.cursor_pos * ROW_H) - 2
        cv.fill(0, hl_y, W, ROW_H - 2, c_hl)
        for i, (label, typ, attr, mn, mx, step, opts) in enumerate(self.items()):
            y = ROW_Y0 + i * ROW_H
            text(cv, 6, y + 4, label + (" >" if typ == 'sub' else ""), 12.0, c_text)
            val, vc = "", c_text
            if typ == 'num':
                val = str(getattr(self, attr))
                if self.editing and i == self.cursor: vc = 0x07FF
            elif typ == 'enum':
                val = opts[getattr(self, attr) % len(opts)]
                if self.editing and i == self.cursor: vc = 0x07FF
            elif typ == 'danger':
                if self.confirming and i == self.cursor:
                    val = OPT_YN[self.confirm_sel]
                    vc = c_warn if self.confirm_sel else c_text
                else:
                    val = "..."
            elif typ == 'read':
                val, vc = opts, c_title
            if val:
                text_r(cv, 114, y + 4, val, 12.0, vc)

# ---------------- 整屏状态机(与 app_main.c 相同) ----------------
STD_TORQUE = [0.20, 0.40, 0.60, 1.00]
STD_TURBO_GEAR = 3
PRECISE_STEP, PRECISE_MIN, PRECISE_MAX = 0.02, 0.02, 0.60
RATCHET_TORQUE = 1.20
SLEEP_AFTER_S = 30.0
TQ_LOCK_AFTER_S = 8.0
SPIN_RPM = 3600.0
SPIN_TIME = 0.7

class UI:
    def __init__(self):
        self.th = Thread()
        self.screen = 'HOME'                    # HOME / MENU / SLEEP
        self.mode = MODE_STD
        self.std_gear = 1
        self.precise_torque = 0.10
        self.rpm_set = [200, 450, 300]
        self.rpm_now = 0.0
        self.trigger = False
        self.tq_locked = True                   # 上电即锁
        self.tq_feedback = 0.0                  # 反馈扭矩(固件里等主电调,模拟占位)
        self.at_limit = False
        # 颜色动画:指数趋近
        self.color_cur = [float(c) for c in MODE_BASE[MODE_STD]]
        self.color_target = list(MODE_BASE[MODE_STD])
        # 模式切换滚动动画
        self.spin, self.spin_t = False, 0.0
        # 压条趋近
        self.view_t = 0.0
        self.disp_set = 0.0
        self.disp_fb = 0.0
        # 计时(虚拟秒)
        self.t = 0.0
        self.last_activity = 0.0
        self.tq_activity = 0.0
        self.menu = Menu()

    def current_torque(self):
        if self.mode == MODE_STD: return STD_TORQUE[self.std_gear]
        if self.mode == MODE_PRECISE: return self.precise_torque
        return RATCHET_TORQUE

    def effective_color(self):
        if self.mode == MODE_STD and self.std_gear == STD_TURBO_GEAR:
            return TURBO_RED
        return MODE_BASE[self.mode]

    def start_morph(self):
        self.color_target = [float(c) for c in self.effective_color()]

    # ---- 事件(对应 on_wheel) ----
    def event(self, e, d=0):
        self.last_activity = self.t
        if self.screen == 'SLEEP':              # 休眠中任何输入只唤醒
            self.screen = 'HOME'
            return
        if self.screen == 'MENU':
            if not self.menu.handle(e, d):
                self.screen = 'HOME'
            return
        if e == 'scroll':
            if self.tq_locked: return           # 扭矩锁:滚动忽略
            if self.mode == MODE_STD:
                was = self.std_gear == STD_TURBO_GEAR
                self.std_gear = min(STD_TURBO_GEAR, max(0, self.std_gear + d))
                if (self.std_gear == STD_TURBO_GEAR) != was:
                    self.start_morph()
            elif self.mode == MODE_PRECISE:
                self.precise_torque = min(PRECISE_MAX, max(PRECISE_MIN,
                                          self.precise_torque + d * PRECISE_STEP))
            # 棘轮:固定扭矩,滚动不响应
            self.tq_activity = self.t
        elif e == 'click':
            if self.tq_locked:                  # 短按只做解锁,不开启内容
                self.tq_locked = False
                self.tq_activity = self.t
        elif e == 'double':
            self.mode = (self.mode + 1) % 3
            self.tq_locked = True               # 换模式回锁
            self.start_morph()
            self.spin, self.spin_t = True, 0.0
        elif e == 'long':
            self.screen = 'MENU'
            self.menu = Menu()

    # ---- 每帧推进 ----
    def step(self, dt):
        self.t += dt
        # 颜色指数趋近(k=dt*6,末段 <=2 直接到位)
        k = min(1.0, dt * 6.0)
        for i in range(3):
            d = self.color_target[i] - self.color_cur[i]
            if abs(d) <= 2:
                self.color_cur[i] = self.color_target[i]
            else:
                self.color_cur[i] += d * k
        self.th.set_base([int(round(c)) for c in self.color_cur])
        # 模式切换滚动(0.7s 正弦包络)
        if self.spin:
            self.spin_t += dt / SPIN_TIME
            if self.spin_t >= 1.0:
                self.spin = False
            else:
                self.th.advance(SPIN_RPM * math.sin(math.pi * self.spin_t), dt)
        # 运动:螺纹速度只跟随主电机转速,待机停转
        if self.mode == MODE_RATCHET:
            if self.trigger:
                self.th.detent()                # 模拟:0.18s 咬一格由 run 事件节奏近似
            self.rpm_now = 0.0
        else:
            target = self.rpm_set[self.mode] if self.trigger else 0.0
            self.rpm_now += (target - self.rpm_now) * (dt * 6.0)
            self.th.advance(self.rpm_now, dt)
        # 压条趋近(k=dt*10)
        k2 = min(1.0, dt * 10.0)
        vt = 0.0 if self.tq_locked else 1.0
        self.view_t += (vt - self.view_t) * k2
        if abs(vt - self.view_t) < 0.01: self.view_t = vt
        self.disp_set += (self.current_torque() - self.disp_set) * k2
        if abs(self.current_torque() - self.disp_set) < 0.002:
            self.disp_set = self.current_torque()
        self.disp_fb += (self.tq_feedback - self.disp_fb) * k2
        if abs(self.tq_feedback - self.disp_fb) < 0.002:
            self.disp_fb = self.tq_feedback
        # 菜单光标
        self.menu.step()
        # 扭矩锁自动回锁(8s 无滚动)
        if not self.tq_locked and self.t - self.tq_activity >= TQ_LOCK_AFTER_S:
            self.tq_locked = True
        # 休眠(30s 无操作且未扣扳机)
        if not self.trigger and self.screen != 'SLEEP' \
                and self.t - self.last_activity >= SLEEP_AFTER_S:
            self.screen = 'SLEEP'

    # ---- 出图 ----
    def frame(self):
        cv = Canvas()
        if self.screen == 'SLEEP':
            up_min = int(self.t // 60)
            draw_sleep(cv, f"{min(99, up_min // 60):02d}:{up_min % 60:02d}")
        elif self.screen == 'MENU':
            self.menu.render(cv)
        else:
            for y, row in enumerate(self.th.render_rows()):
                cv.px[ANIM_Y + y] = row
            base = [int(round(c)) for c in self.color_cur]
            draw_topbar(cv, self.mode, self.rpm_set[self.mode], self.tq_locked, base)
            draw_scrim(cv,
                       int(self.disp_set * 100 + 0.5),
                       int(self.disp_fb * 100 + 0.5),
                       self.view_t, base,
                       self.mode == MODE_STD and self.std_gear == STD_TURBO_GEAR,
                       self.at_limit)
        return cv

def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    ui = UI()
    args = sys.argv[1:]
    seq = []
    i = 0
    while i < len(args):
        if args[i] in ('scroll', 'frames', 'run'):
            seq.append((args[i], int(args[i + 1]))); i += 2
        elif args[i] in ('click', 'double', 'long', 'limit'):
            seq.append((args[i], 0)); i += 1
        else:
            i += 1
    if not seq:
        # 默认演示:锁定滚动被吞 -> 短按解锁 -> 滚到 TURBO(变红) -> 双击换模式
        # -> 长按进菜单(光标动画) -> 退出 -> 回锁 -> 休眠 -> 唤醒
        seq = [('scroll', 2), ('click', 0), ('scroll', 2), ('frames', 20),
               ('run', 30), ('double', 0), ('frames', 45), ('double', 0),
               ('frames', 45), ('long', 0), ('scroll', 2), ('frames', 20),
               ('long', 0), ('limit', 0), ('frames', 10), ('limit', 0),
               ('frames', 500), ('frames', 1400), ('click', 0), ('frames', 10)]
    for idx, (e, d) in enumerate(seq):
        if e == 'frames':
            for _ in range(d):
                ui.trigger = False
                ui.step(1 / 60)
        elif e == 'run':
            for _ in range(d):
                ui.trigger = True
                ui.last_activity = ui.t
                ui.step(1 / 60)
            ui.trigger = False
        elif e == 'limit':
            ui.at_limit = not ui.at_limit
        else:
            ui.event(e, d)
        ui.frame().save(os.path.join(OUT_DIR, f"{idx:02d}_{e}{d if d else ''}.png"))
    print(f"{len(seq)} frames -> {OUT_DIR}")

if __name__ == '__main__':
    main()
