# Second-generation matcher for the JP font: candidates rendered into the 16x16 cell (4x supersampled,
# centred on the em box), compared against the game glyph over small shifts; top candidates per glyph.
#   python jpfont_ocr2.py <glyphs.npy> <first hex> <last hex> <out.tsv>
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

G = np.load(sys.argv[1]).astype(np.float32) / 15.0
first, last, out = int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4]
idx = np.arange(first, min(last + 1, len(G)))

cands = []
for hi in list(range(0x88, 0xA0)) + list(range(0xE0, 0xEB)):  # JIS X 0208 kanji (levels 1 and 2)
    for lo in list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)):
        try:
            ch = bytes([hi, lo]).decode('cp932')
        except UnicodeDecodeError:
            continue
        if ch.strip() and ch not in cands:
            cands.append(ch)
lvl2 = np.array([c.encode('cp932') >= b'\x98\x9f' for c in cands])

FONTS = ['C:/Windows/Fonts/msgothic.ttc', 'C:/Windows/Fonts/YuGothR.ttc', 'C:/Windows/Fonts/YuGothM.ttc', 'C:/Windows/Fonts/YuGothB.ttc']
SS = 4
PAD = 2  # shift range


def render_all(path, em, stroke=0):
    font = ImageFont.truetype(path, em * SS)
    C = np.zeros((len(cands), 16, 16), np.float32)
    for k, ch in enumerate(cands):
        im = Image.new('L', (16 * SS, 16 * SS), 0)
        ImageDraw.Draw(im).text((8 * SS, 8 * SS), ch, fill=255, font=font, anchor='mm', stroke_width=stroke, stroke_fill=255)
        C[k] = np.asarray(im.resize((16, 16), Image.BOX), np.float32) / 255.0
    return C


def norm(v):
    v = v - v.mean(axis=1, keepdims=True)
    return v / np.maximum(np.linalg.norm(v, axis=1, keepdims=True), 1e-6)


# shifted copies of the game glyphs, padded so shifts do not cut strokes
Gp = np.zeros((len(idx), 16 + 2 * PAD, 16 + 2 * PAD), np.float32)
Gp[:, PAD:PAD + 16, PAD:PAD + 16] = G[idx]
best = np.full((len(idx), len(cands)), -1.0, np.float32)
VARIANTS = [(f, em, 0) for f in FONTS for em in (15, 16)]
for path, em, stroke in VARIANTS:
        C = render_all(path, em, stroke)
        Cp = np.zeros((len(cands), 16 + 2 * PAD, 16 + 2 * PAD), np.float32)
        Cp[:, PAD:PAD + 16, PAD:PAD + 16] = C
        Cn = norm(Cp.reshape(len(cands), -1))
        for dy in range(-PAD, PAD + 1):
            for dx in range(-PAD, PAD + 1):
                Gs = np.roll(np.roll(Gp, dy, axis=1), dx, axis=2).reshape(len(idx), -1)
                S = norm(Gs) @ Cn.T
                np.maximum(best, S, out=best)
        print(path, em, stroke, flush=True)
best -= 0.02 * lvl2[None, :]
top = np.argsort(-best, axis=1)[:, :6]
with open(out, 'w', encoding='utf-8') as f:
    for r, i in enumerate(idx):
        f.write('%04x\t%s\t%s\n' % (i, ''.join(cands[j] for j in top[r]), ' '.join('%.3f' % best[r, j] for j in top[r])))
