# Simulate ui_widgets.c rendering (topbar + scrim) and dump PNGs
import math, struct, zlib

FONT = {
 '0':[7,5,5,5,7],'1':[2,6,2,2,7],'2':[7,1,7,4,7],'3':[7,1,7,1,7],'4':[5,5,7,1,1],
 '5':[7,4,7,1,7],'6':[7,4,7,5,7],'7':[7,1,1,2,2],'8':[7,5,7,5,7],'9':[7,5,7,1,7],
 '.':[0,0,0,0,2],':':[0,2,0,2,0],'%':[5,1,2,4,5],' ':[0,0,0,0,0],
 'A':[2,5,7,5,5],'B':[6,5,6,5,6],'C':[3,4,4,4,3],'D':[6,5,5,5,6],'E':[7,4,7,4,7],
 'F':[7,4,7,4,4],'G':[3,4,5,5,3],'H':[5,5,7,5,5],'I':[7,2,2,2,7],'J':[1,1,1,5,2],
 'K':[5,5,6,5,5],'L':[4,4,4,4,7],'M':[5,7,7,5,5],'N':[6,5,5,5,5],'O':[7,5,5,5,7],
 'P':[7,5,7,4,4],'Q':[7,5,5,7,1],'R':[6,5,6,5,5],'S':[3,4,2,1,6],'T':[7,2,2,2,2],
 'U':[5,5,5,5,7],'V':[5,5,5,5,2],'W':[5,5,7,7,5],'X':[5,5,2,5,5],'Y':[5,5,2,2,2],
 'Z':[7,1,2,4,7],
}

class Buf:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[0] * w for _ in range(h)]  # host RGB565
    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = c
    def blend(self, x, y, fg, a):
        if not (0 <= x < self.w and 0 <= y < self.h): return
        bg = self.px[y][x]
        r = ((bg >> 11) & 31) * (255 - a) + ((fg >> 11) & 31) * a
        g = ((bg >> 5) & 63) * (255 - a) + ((fg >> 5) & 63) * a
        b = (bg & 31) * (255 - a) + (fg & 31) * a
        self.px[y][x] = ((r // 255) << 11) | ((g // 255) << 5) | (b // 255)

SS = 4
def glyph_aa(buf, x, y, size, ch, color):
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
                buf.blend(px, py, color, cov * 255 // (SS * SS))

def text(buf, x, y, s, size, color):
    adv = size * 3 / 5 + size * 0.15
    for ch in s:
        glyph_aa(buf, x, y, ch, size, color)
        x += adv
    return x

def text_c(buf, cx, y, s, size, color):
    adv = size * 3 / 5 + size * 0.15
    text(buf, cx - (len(s) * adv - size * 0.15) / 2, y, s, size, color)

def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def save(buf, name):
    raw = b''
    for row in buf.px:
        raw += b'\x00'
        for v in row:
            r = ((v >> 11) & 31) * 255 // 31
            g = ((v >> 5) & 63) * 255 // 63
            b = (v & 31) * 255 // 31
            raw += bytes([r, g, b])
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', buf.w, buf.h, 8, 2, 0, 0, 0)) + \
          chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
    open(name, 'wb').write(png)

# ---- topbar ----
base = (0xFF, 0x8A, 0x2B)  # STD orange
top = Buf(120, 14)
c_mode = rgb565(*base)
text(top, 4, 2, "STD", 9.0, c_mode)
text(top, 92 - (3 * (9 * 0.6 + 1.35) - 1.35), 2, "450", 9.0, 0x7BEF)
bat = rgb565(140, 140, 140)
for x in range(102, 115): top.put(x, 3, bat); top.put(x, 10, bat)
for y in range(3, 11): top.put(102, y, bat); top.put(114, y, bat)
for y in range(5, 9):
    for x in range(104, 113): top.put(x, y, c_mode)
save(top, 'sim_topbar.png')

# ---- scrim ----
scr = Buf(120, 44)
text_c(scr, 60, 0, "1.20", 30.0, c_mode)
text_c(scr, 60, 34, "N.M", 7.0, 0x52AA)
save(scr, 'sim_scrim.png')
print("ok")
