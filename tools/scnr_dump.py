# Translation work material for one map: English markup per message and the Japanese messages as
# glyph images (read directly from the game font).
#   python scnr_dump.py <map> <us mdt dir> <jp mdt dir> <jp font .npy> <out dir> [first] [count]
import os, sys
import numpy as np
from PIL import Image, ImageDraw
import scnr_text as S

name, us_dir, jp_dir, font, out = sys.argv[1:6]
first = int(sys.argv[6]) if len(sys.argv) > 6 else 0
count = int(sys.argv[7]) if len(sys.argv) > 7 else 40
G = np.load(font)
us = S.script(open(os.path.join(us_dir, name + '.MDT'), 'rb').read())
jp = S.script(open(os.path.join(jp_dir, name + '.MDT'), 'rb').read())
tu, tj = S.texts(us, False), S.texts(jp, True)
os.makedirs(out, exist_ok=True)
print('%s: %d EN, %d JP messages' % (name, len(tu), len(tj)))


def jp_glyph_lines(raw):
    """lines of glyph indices; page breaks become an empty marker line (-2)"""
    lines = [[]]
    for kind, b in S.tokens(bytes(x for x in raw)):
        if kind == 'c':
            if b[0] == 0x07:
                lines.append([-2])
                lines.append([])
            elif len(b) == 2 and b[1] >= 0xF0:
                lines[-1].append(0xD4 + (b[1] - 0xF0) * 0xD0 + b[0] - 0x20)
            continue
        for x in b:
            if x == 0x08:
                lines.append([])
            else:
                lines[-1].append(x + 4 - 0x20)
    return [l for l in lines if l]


def render(raw):
    lines = jp_glyph_lines(raw) or [[0x24]]
    W = max(len(l) for l in lines) * 17 + 4
    im = Image.new('L', (max(W, 40), len(lines) * 18 + 4), 25)
    d = ImageDraw.Draw(im)
    for r, l in enumerate(lines):
        if l == [-2]:
            d.line((2, r * 18 + 10, im.width - 2, r * 18 + 10), fill=90)
            continue
        for c, g in enumerate(l):
            if 0 <= g < len(G):
                im.paste(Image.fromarray((G[g] * 17).astype(np.uint8)), (2 + c * 17, 2 + r * 18))
    return im


last = min(first + count, len(tj))
ims = [render(tj[k][1]) for k in range(first, last)]
if ims:
    Wt = max(i.width for i in ims) + 40
    Ht = sum(i.height + 4 for i in ims)
    sheet = Image.new('L', (Wt, Ht), 10)
    d = ImageDraw.Draw(sheet)
    y = 0
    for k, im in zip(range(first, last), ims):
        d.text((2, y + 4), str(k), fill=200)
        sheet.paste(im, (36, y))
        y += im.height + 4
    sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST).save(os.path.join(out, '%s_jp_%04d.png' % (name, first)))
with open(os.path.join(out, '%s_en.txt' % name), 'w', encoding='utf-8') as f:
    for k, (o, raw) in enumerate(tu):
        sp = us[o - 2] | us[o - 1] << 8
        f.write('%d\t[%02x]\t%s\n' % (k, sp, S.to_markup(raw)))
