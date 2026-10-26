# French letters for the game font: prototype of the runtime's glyph builder (text_hook.cpp
# make_glyphs) plus a preview.  python fr_glyphs.py <GR3.MDT> <out.png> [text]
# Extra glyphs live in unused kana slots: German 0x98..0x9E, French 0x9F..0xB0 (script byte /
# font code = glyph + 0x20, field menu Shift-JIS 82 <glyph + 0x0B>).
# A letter is made from the game's own base letter plus an accent template; capitals are squeezed
# by two rows (the two rows most like their upper neighbour) to make room above.
import sys
from PIL import Image

# (char, glyph, base, accent)
FR = [('à', 0x9F, 'a', 'grave'), ('â', 0xA0, 'a', 'circ'), ('ç', 0xA1, 'c', 'cedil'),
      ('é', 0xA2, 'e', 'acute'), ('è', 0xA3, 'e', 'grave'), ('ê', 0xA4, 'e', 'circ'),
      ('ë', 0xA5, 'e', 'dots'), ('î', 0xA6, 'i', 'circ'), ('ï', 0xA7, 'i', 'dots'),
      ('ô', 0xA8, 'o', 'circ'), ('ù', 0xA9, 'u', 'grave'), ('û', 0xAA, 'u', 'circ'),
      ('À', 0xAB, 'A', 'grave'), ('Ç', 0xAC, 'C', 'cedil'), ('É', 0xAD, 'E', 'acute'),
      ('È', 0xAE, 'E', 'grave'), ('Ê', 0xAF, 'E', 'circ'), ('Ô', 0xB0, 'O', 'circ')]

# templates: columns cx-2..cx+3 (cx = left of the two centre columns); '#' = full ink
LOW = {'acute': ['...##.', '..##..', '.##...'], 'grave': ['.##...', '..##..', '...##.'],
       'circ': ['..##..', '.####.', '##..##'], 'dots': ['##..##', '##..##', '......']}
CAP = {'acute': ['...##.', '.##...'], 'grave': ['.##...', '...##.'],
       'circ': ['..##..', '##..##'], 'dots': ['##..##', '......']}
NARROW = {'circ': ['.#.', '#.#'], 'dots': ['#.#', '...']}   # over i: columns cx-1..cx+1
CEDIL = ['..##..', '.##...']                                # rows 14, 15


def font(gr3):
    d = open(gr3, 'rb').read()
    sig = bytes([0xfb, 0x05, 0xfa, 0x03, 0xfe, 0x04, 0xfd, 0x0a, 0xfd, 0x0a, 0xfe, 0x0c, 0xfe, 0x0c, 0xfe, 0x02])
    t = d.find(sig)
    gl, wd = {}, {}
    for i in range(0x1C0):
        b = d[t + 0x380 + i * 128:t + 0x380 + i * 128 + 128]
        gl[i] = [[(b[y * 8 + x // 2] >> 4) if x & 1 else (b[y * 8 + x // 2] & 15) for x in range(16)] for y in range(16)]
        o, a = d[t + i * 2], d[t + i * 2 + 1]
        wd[i] = (o - 256 if o > 127 else o, a)
    return gl, wd


def make(gl, wd, base, accent):
    g = [r[:] for r in gl[ord(base) - 0x20]]
    off, adv = wd[ord(base) - 0x20]
    ink = [(x, y) for y in range(16) for x in range(16) if g[y][x] > 4]
    top = min(y for x, y in ink)
    x0, x1 = min(x for x, y in ink), max(x for x, y in ink)
    cx = (x0 + x1) // 2
    if base == 'i':                       # no dot; a little wider for the accent
        for y in range(5):
            g[y] = [0] * 16
        top = 5
        off, adv = off + 1, adv + 2
        tpl, left, row0 = NARROW[accent], cx - 1, top - 3
    elif accent == 'cedil':
        tpl, left, row0 = CEDIL, cx - 2, 14
    elif base.isupper():
        # drop the two rows most like the row above them, shift what is above down
        rows = [y for y in range(top + 1, 15)]
        diff = sorted(rows, key=lambda y: (sum(abs(a - b) for a, b in zip(g[y], g[y - 1])), abs(y - 8)))
        drop = set()
        for y in diff:
            if y not in drop and y - 1 not in drop and y + 1 not in drop:
                drop.add(y)
            if len(drop) == 2:
                break
        kept = [g[y] for y in range(16) if y not in drop]
        g = [[0] * 16, [0] * 16] + kept
        tpl, left, row0 = CAP[accent], cx - 2, 0
    else:
        tpl, left, row0 = LOW[accent], cx - 2, top - 4
    for r, line in enumerate(tpl):
        for c, ch in enumerate(line):
            if ch == '#' and 0 <= left + c < 16 and 0 <= row0 + r < 16:
                g[row0 + r][left + c] = 15
    return g, (off, adv)


def render(gl, wd, extra, text, scale=3):
    w = sum(wd[ord(c) - 0x20][1] + 1 if c not in extra else extra[c][1][1] + 1 for c in text) + 20
    img = Image.new('RGB', (w * scale, 22 * scale), (20, 30, 60))
    x = 6
    for c in text:
        g, (o, a) = extra[c] if c in extra else (gl[ord(c) - 0x20], wd[ord(c) - 0x20])
        for y in range(16):
            for xx in range(16):
                v = g[y][xx]
                if v:
                    px = x + o + xx
                    col = (v * 17, v * 17, v * 17)
                    for sy in range(scale):
                        for sx in range(scale):
                            img.putpixel(((px) * scale + sx, (y + 3) * scale + sy), col)
        x += a + 1
    return img


if __name__ == '__main__':
    gl, wd = font(sys.argv[1])
    extra = {c: make(gl, wd, b, a) for c, _, b, a in FR}
    lines = sys.argv[3:] or ['àâçéèêëîïôùû ÀÇÉÈÊÔ',
                             'Élise, ça a été une forêt où',
                             "À toi ! Ève, Noël, naïf, maître, Ô",
                             'Hé ! Ça suffit ! Être là-bas, déjà.']
    imgs = [render(gl, wd, extra, t) for t in lines]
    out = Image.new('RGB', (max(i.width for i in imgs), sum(i.height for i in imgs)))
    y = 0
    for i in imgs:
        out.paste(i, (0, y)); y += i.height
    out.save(sys.argv[2])
