# Locate dumped textures (texdump/<key>.bin: texels + palette) in the unpacked game files.
#   python texfind.py <texdump> <mdt dir> [--limit=N]
import os, sys, glob

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = dict((a[2:].split('=', 1) + [''])[:2] for a in sys.argv[1:] if a.startswith('--'))
dump, mdt = args[0], args[1]
limit = int(opt.get('limit', '40'))

info = {}
for line in open(os.path.join(dump, 'index.txt')):
    p = line.split()
    f = dict(x.split('=') for x in p[2:])
    w, h = map(int, p[1].split('x'))
    info[p[0]] = (w, h, int(f['psm'], 16))

files = sorted(glob.glob(os.path.join(mdt, '*.MDT')))
data = {f: open(f, 'rb').read() for f in files}
print('loaded', len(files), 'files', flush=True)

n = 0
for key, (w, h, psm) in info.items():
    bp = os.path.join(dump, key + '.bin')
    if not os.path.exists(bp) or psm not in (0x13, 0x14):
        continue
    raw = open(bp, 'rb').read()
    tex = raw[:w * h]
    pal = raw[w * h:]
    row = h // 3
    if psm == 0x13:
        pat = tex[row * w: row * w + min(w, 64)]
    else:
        seg = tex[row * w: row * w + min(w, 128)]
        pat = bytes(seg[i] | seg[i + 1] << 4 for i in range(0, len(seg) - 1, 2))
    palpat = pal[:32]
    hits, phits = [], []
    for f, d in data.items():
        i = d.find(pat)
        if i >= 0:
            hits.append((os.path.basename(f), i, d.count(pat)))
        j = d.find(palpat) if len(set(palpat)) > 4 else -1
        if j >= 0:
            phits.append((os.path.basename(f), j))
    print(key, f'{w}x{h} psm={psm:02x}', 'tex:', hits[:4], len(hits), '| pal:', phits[:3], len(phits), flush=True)
    n += 1
    if n >= limit:
        break
