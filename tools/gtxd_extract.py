# Extracts every GTXD texture of the game as <key>.png (the renderer's replacement key, one file per
# palette) plus catalog.csv, the input of upscale_textures.py.
#   python gtxd_extract.py <outdir> <file or dir> [...]     (dirs: *.MDT and *.DAT inside)
import csv, glob, os, sys
import numpy as np
from PIL import Image
import gtxd

out = sys.argv[1]
srcs = []
for a in sys.argv[2:]:
    if os.path.isdir(a):
        srcs += sorted(glob.glob(os.path.join(a, '*.MDT')) + glob.glob(os.path.join(a, '*.DAT')))
    else:
        srcs.append(a)
os.makedirs(out, exist_ok=True)
seen = set()
rows = []
total = 0
for f in srcs:
    d = open(f, 'rb').read()
    for t in gtxd.find_all(d):
        total += 1
        try:
            tex = gtxd.texels(d, t)
            pals = gtxd.palettes(d, t)
        except (ValueError, IndexError):
            continue
        for pi, pal in enumerate(pals):
            k = gtxd.key(tex, pal, t.psm, t.w, t.h)
            if k in seen:
                continue
            seen.add(k)
            name = '%016x' % k
            Image.fromarray(gtxd.rgba(tex, pal), 'RGBA').save(os.path.join(out, name + '.png'))
            rows.append([name, os.path.basename(f), '%x' % t.off, t.name, t.w, t.h, '%02x' % t.psm, pi, t.npal])
with open(os.path.join(out, 'catalog.csv'), 'w', newline='', encoding='utf-8') as fh:
    w = csv.writer(fh)
    w.writerow(['key', 'file', 'offset', 'name', 'w', 'h', 'psm', 'palette', 'palettes'])
    w.writerows(rows)
print(total, 'GTXD entries,', len(rows), 'unique textures')
