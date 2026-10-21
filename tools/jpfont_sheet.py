# Check sheet for the JP font map: each game glyph next to its mapped character (MS Gothic).
#   python jpfont_sheet.py <glyphs.npy> <map.tsv> <first hex> <count> <out.png> [cols]
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

G = np.load(sys.argv[1])
mp = {}
for ln in open(sys.argv[2], encoding='utf-8'):
    p = ln.rstrip('\n').split('\t')
    if len(p) >= 2:
        mp[int(p[0], 16)] = p[1][:1]
first, count, out = int(sys.argv[3], 16), int(sys.argv[4]), sys.argv[5]
cols = int(sys.argv[6]) if len(sys.argv) > 6 else 16
S = 3                      # glyph scale
CW, CH = 16 * S * 2 + 14, 16 * S + 16
rows = (count + cols - 1) // cols
im = Image.new('L', (cols * CW, rows * CH), 0)
d = ImageDraw.Draw(im)
font = ImageFont.truetype('C:/Windows/Fonts/msgothic.ttc', 16 * S - 4)
small = ImageFont.truetype('C:/Windows/Fonts/msgothic.ttc', 11)
for n in range(count):
    i = first + n
    if i >= len(G):
        break
    x, y = (n % cols) * CW, (n // cols) * CH
    g = Image.fromarray((G[i] * 17).astype(np.uint8)).resize((16 * S, 16 * S), Image.NEAREST)
    im.paste(g, (x + 2, y + 13))
    d.text((x + 2, y), '%03x' % i, fill=140, font=small)
    ch = mp.get(i, '?')
    d.text((x + 16 * S + 8, y + 15), ch, fill=255, font=font)
im.save(out)
