# Sanity check of the manual kanji corrections: the corrected character should match its own glyph
# better than the neighbouring glyphs (catches corrections written one cell off).
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
G = np.load(sys.argv[1]).astype(np.float32) / 15.0
fixes = []
for ln in open(sys.argv[2], encoding='utf-8'):
    if ln.startswith('#') or '\t' not in ln:
        continue
    h, ch = ln.rstrip('\n').split('\t')
    fixes.append((int(h, 16), ch))
FONTS = ['C:/Windows/Fonts/msgothic.ttc', 'C:/Windows/Fonts/YuGothR.ttc', 'C:/Windows/Fonts/YuGothM.ttc', 'C:/Windows/Fonts/YuGothB.ttc']
fonts = [ImageFont.truetype(f, em * 4) for f in FONTS for em in (15, 16)]
P = 2


def rend(ch, font):
    im = Image.new('L', (64, 64), 0)
    ImageDraw.Draw(im).text((32, 32), ch, fill=255, font=font, anchor='mm')
    return np.asarray(im.resize((16, 16), Image.BOX), np.float32) / 255.0


def nrm(v):
    v = v - v.mean()
    return v / max(np.linalg.norm(v), 1e-6)


def score(gi, ch):
    best = -1
    g = np.zeros((20, 20), np.float32); g[P:P + 16, P:P + 16] = G[gi]
    for f in fonts:
        c = np.zeros((20, 20), np.float32); c[P:P + 16, P:P + 16] = rend(ch, f)
        cn = nrm(c.ravel())
        for dy in range(-P, P + 1):
            for dx in range(-P, P + 1):
                best = max(best, float(nrm(np.roll(np.roll(g, dy, 0), dx, 1).ravel()) @ cn))
    return best


for gi, ch in fixes:
    if gi < 0x10a:
        continue
    s0 = score(gi, ch)
    sn = max(score(gi - 1, ch), score(gi + 1, ch))
    if sn > s0 - 0.02 or s0 < 0.70:
        print('%03x %s own %.3f neighbour %.3f' % (gi, ch, s0, sn))
