# Side-by-side crops: original (as the game shows it, bilinear) vs. upscaled versions.
#   python texcompare.py <out.png> <texdump> <dir1> [<dir2> ...] --keys=k1,k2 [--crop=96] [--zoom=2]
import os, sys
from PIL import Image, ImageDraw

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict((a[2:].split('=', 1) + [''])[:2] for a in sys.argv[1:] if a.startswith('--'))
out, orig_dir, dirs = args[0], args[1], args[2:]
keys = [k for k in opt['keys'].split(',') if k]
crop = int(opt.get('crop', '96'))      # original texels per side
zoom = int(opt.get('zoom', '2'))       # display magnification of the 2x images
labels = ['Original'] + [os.path.basename(d.rstrip('/\\')) for d in dirs]
if opt.get('labels'):
    labels = opt['labels'].split(',')
panel = crop * 2 * zoom
rows = []
for k in keys:
    o = Image.open(os.path.join(orig_dir, k + '.png')).convert('RGBA')
    w, h = o.size
    c = min(crop, w, h)
    x0 = int(opt.get('x', (w - c) // 2)) if len(keys) == 1 else (w - c) // 2
    y0 = int(opt.get('y', (h - c) // 2)) if len(keys) == 1 else (h - c) // 2
    ims = [o.resize((w * 2, h * 2), Image.BILINEAR)]
    for d in dirs:
        ims.append(Image.open(os.path.join(d, k + '.png')).convert('RGBA'))
    row = []
    for im in ims:
        s = im.width // w
        p = im.crop((x0 * s, y0 * s, (x0 + c) * s, (y0 + c) * s)).resize((c * 2 * zoom, c * 2 * zoom), Image.NEAREST)
        bg = Image.new('RGBA', p.size, (40, 40, 48, 255))
        bg.alpha_composite(p)
        row.append(bg.convert('RGB'))
    rows.append(row)
sheet = Image.new('RGB', (len(labels) * (panel + 6), len(rows) * (panel + 6) + 20), (20, 20, 24))
d = ImageDraw.Draw(sheet)
for i, l in enumerate(labels):
    d.text((i * (panel + 6) + 4, 4), l, fill=(255, 255, 255))
for r, row in enumerate(rows):
    for i, p in enumerate(row):
        sheet.paste(p, (i * (panel + 6), 20 + r * (panel + 6)))
sheet.save(out)
print(sheet.size)
