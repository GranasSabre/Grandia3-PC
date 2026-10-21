# Maps the glyphs of the Japanese Grandia III font (16x16, 4 bpp; GR3.MDT chunk 10) to Unicode
# characters by template matching against Windows Japanese fonts.
#   python jpfont_ocr.py <glyphs.npy> <out.tsv>
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

glyphs = np.load(sys.argv[1]).astype(np.float32)  # n x 16 x 16, values 0..15
out = sys.argv[2]

# candidate characters: JIS X 0208 (all of Shift-JIS double-byte) + ASCII
cands = []
level2 = set()
for hi in list(range(0x81, 0xA0)) + list(range(0xE0, 0xEB)):
    for lo in list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)):
        try:
            ch = bytes([hi, lo]).decode('cp932')
        except UnicodeDecodeError:
            continue
        if ch.strip() and ch not in cands:
            cands.append(ch)
            if (hi << 8 | lo) >= 0x989F:  # JIS X 0208 level 2 kanji (rare): small penalty
                level2.add(ch)
for c in range(0x21, 0x7F):
    if chr(c) not in cands:
        cands.append(chr(c))

FONTS = [('C:/Windows/Fonts/msgothic.ttc', 0), ('C:/Windows/Fonts/YuGothB.ttc', 0), ('C:/Windows/Fonts/YuGothM.ttc', 0)]


def render(ch, font, size, bold):
    im = Image.new('L', (32, 32), 0)
    d = ImageDraw.Draw(im)
    bb = d.textbbox((0, 0), ch, font=font)
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    d.text((16 - w / 2 - bb[0], 16 - h / 2 - bb[1]), ch, fill=255, font=font, stroke_width=1 if bold else 0, stroke_fill=255)
    return np.asarray(im, np.float32)


def norm(v):
    v = v - v.mean(axis=-1, keepdims=True)
    n = np.linalg.norm(v, axis=-1, keepdims=True)
    return v / np.maximum(n, 1e-6)


# game glyphs: centre them in 32x32 by their bounding box (the font's baseline/advance differ)
def centred(g):
    im = np.zeros((32, 32), np.float32)
    ys, xs = np.nonzero(g > 3)
    if len(ys) == 0:
        return im
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    c = g[y0:y1, x0:x1]
    oy, ox = 16 - (y1 - y0) // 2, 16 - (x1 - x0) // 2
    im[oy:oy + c.shape[0], ox:ox + c.shape[1]] = c
    return im


def blur(a):
    return np.asarray(Image.fromarray(a.astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.8)), np.float32)


G = np.stack([blur(centred(g) * 17) for g in glyphs]).reshape(len(glyphs), -1)
Gn = norm(G)
best = np.full(len(glyphs), -1.0)
bestc = [''] * len(glyphs)
for path, idx in FONTS:
    for size in (14, 15, 16):
        for bold in (False, True):
            font = ImageFont.truetype(path, size, index=idx)
            C = np.stack([blur(centred(render(ch, font, size, bold) / 17.0) * 17) for ch in cands]).reshape(len(cands), -1)
            S = Gn @ norm(C).T  # glyphs x candidates
            S -= 0.04 * np.array([c in level2 for c in cands], np.float32)[None, :]
            j = S.argmax(axis=1)
            s = S[np.arange(len(glyphs)), j]
            upd = s > best
            best[upd] = s[upd]
            for i in np.nonzero(upd)[0]:
                bestc[i] = cands[j[i]]
            print(path, size, bold, 'mean best %.3f' % best.mean(), flush=True)
FIXED = {0x24: '　', 0x25: '‼', 0x26: '⁉', 0x27: '！', 0x28: '？', 0x29: '～', 0x2A: '…', 0x2B: '―', 0x2C: '・',
         0x2D: '♥', 0x2E: 'ø', 0x2F: '：', 0x30: '（', 0x31: '）', 0x32: '『', 0x33: '』'}
for k in range(10):
    FIXED[0x34 + k] = chr(0xFF10 + k)
for k in range(26):
    FIXED[0x3E + k] = chr(0xFF21 + k)
HIRA = 'ぁあぃいぅうぇえぉおかがきぎくぐけげこごさざしじすずせぜそぞただちっつづてでとどなにぬねのはばぱひびぴふぶぷへべぺほぼぽまみむめもゃやゅゆょよらりるれろわをん'
KATA = 'ァアィイゥウェエォオカガキギクグケゲコゴサザシジスズセゼソゾタダチッツヅテデトドナニヌネノハバパヒビピフブプヘベペホボポマミムメモャヤュユョヨンラリルレロワヲヴヵヶ'
for k, ch in enumerate(HIRA):
    FIXED[0x58 + k] = ch
for k, ch in enumerate(KATA):
    FIXED[0x58 + len(HIRA) + k] = ch
assert FIXED[0xA0] == 'り' and FIXED[0xE0] == 'ホ' and FIXED[0xEE] == 'ン' and FIXED[0xF8] == 'ヶ', (FIXED[0xA0], FIXED[0xE0])
for k, ch in enumerate('ゎヮぢヂ○♪＋ー－＝≡■　／［］→'):
    FIXED[0xF9 + k] = ch
for i, ch in FIXED.items():
    if i < len(glyphs):
        bestc[i] = ch
        best[i] = 1.0
with open(out, 'w', encoding='utf-8') as f:
    for i in range(len(glyphs)):
        f.write('%04x\t%s\t%.3f\n' % (i, bestc[i], best[i]))
print('done')
