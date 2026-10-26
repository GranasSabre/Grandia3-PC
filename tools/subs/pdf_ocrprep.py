"""Render the mixed-raster scan PDF (JPEG background + CCITT G4 text masks) to PNG pages.
usage: pdfrender.py <pdf> <outdir> [scale px/pt] [first] [last]"""
import re, sys, zlib, os, io, struct
from PIL import Image

pdf, out = sys.argv[1], sys.argv[2]
scale = float(sys.argv[3]) if len(sys.argv) > 3 else 2.5
os.makedirs(out, exist_ok=True)
d = open(pdf, 'rb').read()

offs = {}
for m in re.finditer(rb'(?<![0-9])(\d+)\s+0\s+obj', d):
    offs[int(m.group(1))] = m.end()

def body(n):
    s = offs[n]
    return d[s:d.find(b'endobj', s)]

def num(n):
    return int(re.match(rb'\s*(\d+)', body(n)).group(1))

def stream(n):
    s = offs[n]
    m = re.compile(rb'stream\r?\n').search(d, s)
    dic = d[s:m.start()]
    ln = re.search(rb'/Length\s+(\d+)(\s+0\s+R)?', dic)
    L = num(int(ln.group(1))) if ln.group(2) else int(ln.group(1))
    return dic, d[m.end():m.end() + L]

def g4_image(w, h, data):
    # minimal TIFF around the CCITT G4 data
    tags = [(256, 4, w), (257, 4, h), (258, 3, 1), (259, 3, 4), (262, 3, 0),
            (273, 4, 0), (277, 3, 1), (278, 4, h), (279, 4, len(data))]
    hdr = 8; ifd = 2 + 12 * len(tags) + 4
    dataoff = hdr + ifd
    b = io.BytesIO(); b.write(b'II*\x00' + struct.pack('<I', 8) + struct.pack('<H', len(tags)))
    for t, typ, v in tags:
        if t == 273: v = dataoff
        if typ == 3: b.write(struct.pack('<HHIHH', t, typ, 1, v, 0))
        else: b.write(struct.pack('<HHII', t, typ, 1, v))
    b.write(struct.pack('<I', 0)); b.write(data)
    b.seek(0)
    im = Image.open(b); im.load()
    return im.convert('L')  # PhotometricInterpretation 0: 0 = white -> mask paint where black

pages = []
for m in re.finditer(rb'(\d+)\s+0\s+obj\s*<<\s*/Type\s*/Page\b', d):
    pages.append(int(m.group(1)))

first = int(sys.argv[4]) if len(sys.argv) > 4 else 1
last = int(sys.argv[5]) if len(sys.argv) > 5 else len(pages)
for pi, pn in enumerate(pages, 1):
    if pi < first or pi > last: continue
    pb = body(pn)
    mb = [float(x) for x in re.search(rb'/MediaBox\s*\[([^\]]+)\]', pb).group(1).split()]
    W, H = mb[2] - mb[0], mb[3] - mb[1]
    xo = {k.decode(): int(v) for k, v in re.findall(rb'/(\w+)\s+(\d+)\s+0\s+R', re.search(rb'/XObject\s*<<(.*?)>>', pb, re.S).group(1))}
    cref = re.search(rb'/Contents\s+(\d+)\s+0\s+R', pb)
    dic, cs = stream(int(cref.group(1)))
    if b'/FlateDecode' in dic: cs = zlib.decompress(cs)
    canvas = Image.new('L', (int(W * scale), int(H * scale)), 255)
    full = Image.new('L', canvas.size, 255)
    ink = Image.new('L', canvas.size, 0)
    color = (0, 0, 0); cm = None
    for tok in re.finditer(rb'([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+rg|([-\d.\s]+)\s+cm|/(\w+)\s+Do', cs):
        if tok.group(1):
            color = tuple(int(float(tok.group(i)) * 255 + 0.5) for i in (1, 2, 3))
        elif tok.group(4):
            cm = [float(x) for x in tok.group(4).split()][-6:]
        else:
            name = tok.group(5).decode(); dic, raw = stream(xo[name])
            a, b_, c, dd, e, f = cm
            x0, y0 = int(e * scale), int((H - f - dd) * scale)
            tw, th = max(1, int(round(a * scale))), max(1, int(round(dd * scale)))
            w = int(re.search(rb'/Width\s+(\d+)', dic).group(1)); h = int(re.search(rb'/Height\s+(\d+)', dic).group(1))
            if b'/DCTDecode' in dic:
                if b'/FlateDecode' in dic: raw = zlib.decompress(raw)
                im = Image.open(io.BytesIO(raw)).convert('L').resize((tw, th), Image.BICUBIC)
                full.paste(im, (x0, y0))
            elif b'/CCITTFaxDecode' in dic:
                im = g4_image(w, h, raw)
                # ImageMask: sample value 0 paints (Decode [0 1] default); G4 photometric 0 -> black pixels = 1 bits
                # PIL gives white=255 for 0 bits with WhiteIsZero, so paint where the PIL pixel is dark.
                mask = im.point(lambda v: 255 if v < 128 else 0).resize((tw, th), Image.BILINEAR)
                r, g, b = color
                if r - max(g, b) <= 60:
                    canvas.paste(0, (x0, y0, x0 + tw, y0 + th), mask)
                    full.paste(0, (x0, y0, x0 + tw, y0 + th), mask)
                elif r - max(g, b) > 60:
                    ink.paste(255, (x0, y0, x0 + tw, y0 + th), mask)
    canvas.point(lambda v: 0 if v < 128 else 255).save(os.path.join(out, 'm%03d.png' % pi))
    full.point(lambda v: 0 if v < 150 else 255).save(os.path.join(out, 'p%03d.png' % pi))
    full.save(os.path.join(out, 'g%03d.png' % pi))
    ink.save(os.path.join(out, 'ink%03d.png' % pi))
    print('page', pi, canvas.size)
