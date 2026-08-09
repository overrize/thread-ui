#!/usr/bin/env python3
# sim_ui.py —— thread_ui 的 PC 端渲染/交互模拟器
#
# 把固件的渲染逻辑(螺纹动画 / 状态条 / 压条 / 菜单)在 PC 上用同一套
# 数学跑一遍,输出 PNG。先在电脑上确认布局和交互,再烧录。
#
# 用法:
#   python tools/sim_ui.py                # 输出一套默认演示帧到 sim_out/
#   python tools/sim_ui.py scroll 3 click scroll -2 long   # 自定义事件脚本
#
# 事件: scroll <n> / click / double / long / frames <n>(动画推进 n 帧)
#
# 与固件的对应关系:
#   thread_anim.c  -> Thread 类(调色板推导、行拷贝、底部渐隐)
#   ui_widgets.c   -> AA 文字(同一套 3x5 字库 + 4x4 超采样)
#   menu.c         -> Menu 类(同一套手势状态机)
#   thread_tile.h  -> 直接解析 C 数组,保持同一份贴图数据

import math, os, re, struct, sys, zlib

W, H = 120, 240
BAR_H, ANIM_Y, ANIM_H, SCRIM_Y, SCRIM_H = 14, 14, 182, 196, 44
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

# ---------------- 螺纹(与 thread_anim.c 相同) ----------------
MODE_BASE = [(0x2F, 0xA8, 0xFF), (0xFF, 0x8A, 0x2B), (0x3E, 0xD6, 0x6A)]
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
        self.set_base(MODE_BASE[1])
    def set_base(self, base):
        self.pal = palette(base)
        self.tile565 = [[self.pal[v & 7] for v in row] for row in TILE]
    def advance(self, rpm, dt):
        px = TILE_H * rpm / 60.0 * dt
        px = min(px, TILE_H * 0.45)
        self.offset = (self.offset + px) % TILE_H
    def render_rows(self):
        """返回动画区 182 行 RGB565(分数行混合 + 底部 12 行渐隐)"""
        off = int(self.offset)
        q = min(15, int((self.offset - off) * 16))
        # 混合 LUT:(a, b, q) -> rgb565
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

# ---------------- 状态条 / 压条(与 ui_widgets.c 相同) ----------------
MODE_TAG = ["PRE", "STD", "RCH"]

def draw_topbar(cv, mode, rpm, locked, base):
    bar = Canvas(W, BAR_H)
    c_mode = rgb565(*base)
    c_dim = 0x52AA
    c_text = c_dim if locked else c_mode
    text(bar, 4, 2, MODE_TAG[mode % 3], 9.0, c_text)
    text_r(bar, 92, 2, str(rpm), 9.0, c_dim if locked else 0x7BEF)
    bat = rgb565(140, 140, 140)
    for x in range(102, 115): bar.put(x, 3, bat); bar.put(x, 10, bat)
    for y in range(3, 11): bar.put(102, y, bat); bar.put(114, y, bat)
    for y in (5, 6, 7, 8): bar.put(115, y, bat)
    for y in range(5, 9):
        for x in range(104, 113): bar.put(x, y, c_mode)
    cv.paste(bar, 0, 0)

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

def draw_scrim(cv, torque_x100, base):
    sc = Canvas(W, SCRIM_H)
    s = f"{torque_x100 // 100}.{torque_x100 % 100:02d}"
    text_c(sc, 53, 0, s, 28.0, rgb565(*base))
    text_v(sc, 100, 5, "N.M", 9.0, 0x52AA)
    cv.paste(sc, 0, SCRIM_Y)

# ---------------- 菜单(与 menu.c 相同的状态机) ----------------
OPT_REFLEX = ["OFF", "LIGHT", "STD", "STRONG"]
OPT_DIR = ["CW", "CCW"]
OPT_UNIT = ["N.M", "KGCM", "LBIN"]
OPT_YN = ["CANCEL", "SURE!"]
ROW_H, ROW_Y0 = 24, 18

class Menu:
    def __init__(self):
        self.rpm_max, self.reflex, self.grip = 450, 2, 5
        self.reverse, self.unit = 0, 0
        self.level, self.cursor = 'MAIN', 0
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
    def render(self, cv):
        c_title, c_text = 0x52AA, 0x7BEF
        c_hl = rgb565(24, 28, 36)
        c_warn = 0xF800
        text_c(cv, W / 2, 2, "SETUP" if self.level == 'MAIN' else "SYSTEM", 11.0, c_title)
        cv.fill(0, 15, W, 1, rgb565(60, 60, 60))
        for i, (label, typ, attr, mn, mx, step, opts) in enumerate(self.items()):
            y = ROW_Y0 + i * ROW_H
            if i == self.cursor:
                cv.fill(0, y - 2, W, ROW_H - 2, c_hl)
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

# ---------------- 整屏合成 ----------------
class UI:
    def __init__(self):
        self.th = Thread()
        self.mode = 1
        self.torque = [35, 120, 240]     # x100
        self.rpm_set = [200, 450, 300]
        self.locked = False
        self.screen = 'HOME'
        self.menu = Menu()
        self.base = MODE_BASE[1]
    def frame(self):
        cv = Canvas()
        if self.screen == 'HOME':
            for y, row in enumerate(self.th.render_rows()):
                cv.px[ANIM_Y + y] = row
            draw_topbar(cv, self.mode, self.rpm_set[self.mode], self.locked, self.base)
            draw_scrim(cv, self.torque[self.mode], self.base)
        else:
            self.menu.render(cv)
        return cv
    def event(self, e, d=0):
        if self.screen == 'MENU':
            if not self.menu.handle(e, d):
                self.screen = 'HOME'
            return
        if e == 'scroll':
            t = self.torque[self.mode] + d * 5
            self.torque[self.mode] = min(300, max(10, t))
        elif e == 'click':
            self.screen = 'MENU'
            self.menu.__init__()
        elif e == 'double':
            self.mode = (self.mode + 1) % 3
            self.base = MODE_BASE[self.mode]
            self.th.set_base(self.base)
        elif e == 'long':
            self.locked = not self.locked

def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    ui = UI()
    args = sys.argv[1:]
    seq = []
    i = 0
    while i < len(args):
        if args[i] in ('scroll', 'frames'):
            seq.append((args[i], int(args[i + 1]))); i += 2
        elif args[i] in ('click', 'double', 'long'):
            seq.append((args[i], 0)); i += 1
        else:
            i += 1
    if not seq:
        seq = [('frames', 30), ('scroll', 2), ('double', 0), ('frames', 20),
               ('click', 0), ('scroll', 1), ('click', 0), ('scroll', 2),
               ('long', 0), ('scroll', 4), ('click', 0), ('scroll', 2),
               ('click', 0), ('scroll', 1), ('long', 0)]
    for idx, (e, d) in enumerate(seq):
        if e == 'frames':
            for _ in range(d):
                ui.th.advance(20, 1 / 60)
        else:
            ui.event(e, d)
        ui.frame().save(os.path.join(OUT_DIR, f"{idx:02d}_{e}{d if d else ''}.png"))
    print(f"{len(seq)} frames -> {OUT_DIR}")

if __name__ == '__main__':
    main()
