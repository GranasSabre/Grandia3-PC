# Contact sheet of dumped textures: python texsheet.py <texdump dir> <out.png> [--min=N] [--cols=N] [--start=N] [--count=N]
import os, sys
from PIL import Image, ImageDraw

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict(a[2:].split('=') for a in sys.argv[1:] if a.startswith('--') and '=' in a)
src, out = args[0], args[1]
minsize = int(opt.get('min', 16))
cols = int(opt.get('cols', 8))
start = int(opt.get('start', 0))
count = int(opt.get('count', 64))
cell = int(opt.get('cell', 160))

names = []
idx = os.path.join(src, 'index.txt')
if os.path.exists(idx):
    for line in open(idx):
        k = line.split()[0]
        names.append((k, line.strip()))
else:
    names = [(f[:16], f) for f in sorted(os.listdir(src)) if f.endswith('.png')]
items = []
for k, info in names:
    p = os.path.join(src, k + '.png')
    if not os.path.exists(p):
        continue
    im = Image.open(p)
    if im.width < minsize and im.height < minsize:
        continue
    items.append((k, info, im))
items = items[start:start + count]
rows = (len(items) + cols - 1) // cols
sheet = Image.new('RGB', (cols * cell, rows * (cell + 14)), (40, 40, 48))
d = ImageDraw.Draw(sheet)
for i, (k, info, im) in enumerate(items):
    x, y = (i % cols) * cell, (i // cols) * (cell + 14)
    rgba = im.convert('RGBA')
    s = min((cell - 4) / rgba.width, (cell - 4) / rgba.height)
    t = rgba.resize((max(1, int(rgba.width * s)), max(1, int(rgba.height * s))), Image.NEAREST)
    bg = Image.new('RGBA', t.size, (255, 0, 255, 255))
    bg.alpha_composite(t)
    sheet.paste(bg.convert('RGB'), (x + 2, y + 2))
    d.text((x + 2, y + cell - 1), f'{i + start} {k[:8]} {im.width}x{im.height}', fill=(220, 220, 220))
sheet.save(out)
print(len(items), 'textures')
