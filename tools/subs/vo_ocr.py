"""OCR of the ADR script pages rendered by pdf_ocrprep2.py.
usage: vo_ocr.py <render dir> <out.tsv> [first] [last]
Per page: find the table grid, OCR each column strip with Tesseract (TSV word boxes), assign words to
rows, flag words that live only in the scan background (struck through / scribbled over) and rows with
red pen ink."""
import sys, os, subprocess, csv, json
import numpy as np
from PIL import Image

DBG = bool(os.environ.get('VO_DBG'))
TESS = r'C:\Program Files\Tesseract-OCR\tesseract.exe'
rdir, out = sys.argv[1], sys.argv[2]
first = int(sys.argv[3]) if len(sys.argv) > 3 else 6
last = int(sys.argv[4]) if len(sys.argv) > 4 else 103
tmp = os.path.join(rdir, '_cell.png')


def runs(mask, minlen):
    """index ranges [a, b) where mask is True for at least minlen consecutive entries"""
    out, i, n = [], 0, len(mask)
    while i < n:
        if mask[i]:
            j = i
            while j < n and mask[j]: j += 1
            if j - i >= minlen: out.append((i, j))
            i = j
        else:
            i += 1
    return out


def grid(dark):
    h, w = dark.shape
    # vertical lines: columns dark over a long stretch
    # longest vertical run of dark pixels per column: table rules are long, text never is
    d = np.vstack([np.zeros((1, w), bool), dark, np.zeros((1, w), bool)]).astype(np.int8)
    longest = np.zeros(w, int)
    for x in range(w):
        e = np.flatnonzero(np.diff(d[:, x]))
        if len(e): longest[x] = (e[1::2] - e[0::2]).max()
    vcols = [((a + b) // 2) for a, b in runs(longest > 300, 2)]
    if len(vcols) < 4:
        return None
    x0, x1 = vcols[0], vcols[-1]
    rowfrac = dark[:, x0:x1].mean(axis=1)
    hrows = [((a + b) // 2) for a, b in runs(rowfrac > 0.6, 2)]
    return vcols, hrows


def ocr(img, psm=6, extra=()):
    img.save(tmp)
    r = subprocess.run([TESS, tmp, 'stdout', '--psm', str(psm), '-l', 'eng', *extra, 'tsv'],
                       capture_output=True, text=True, encoding='utf-8')
    words = []
    for row in csv.DictReader(r.stdout.splitlines(), delimiter='\t', quoting=csv.QUOTE_NONE):
        t = (row.get('text') or '').strip()
        if row.get('level') == '5' and t:
            words.append(dict(text=t, x=int(row['left']), y=int(row['top']), w=int(row['width']),
                              h=int(row['height']), conf=float(row['conf']), line=(row['block_num'], row['par_num'], row['line_num'])))
    return words


rows_out = []
for pi in range(first, last + 1):
    ims = [Image.open(os.path.join(rdir, '%s%03d.png' % (k, pi))) for k in ('p', 'm', 'ink', 'g')]
    fills = (255, 255, 0, 255)
    def load(angle):
        a = [im.rotate(angle, Image.NEAREST, fillcolor=f) if angle else im for im, f in zip(ims, fills)]
        return (np.array(a[0]) < 128, np.array(a[1]) < 128, np.array(a[2]) > 0, np.array(a[3]) < 200)
    full, mlay, ink, gray = load(0)
    g = grid(full)
    if not g or len(g[0]) != 6 or len(g[1]) < 3:
        # skewed scan: pick the angle that gives the sharpest horizontal rules
        small = ims[0].resize((ims[0].width // 4, ims[0].height // 4))
        best = max((np.var((np.array(small.rotate(a / 20, Image.BILINEAR, fillcolor=255)) < 128).sum(axis=1)), a / 20)
                   for a in range(-40, 41))
        full, mlay, ink, gray = load(best[1])
        g = grid(full)
        print('page', pi, 'deskew %.2f deg' % best[1], flush=True)
    if not g or len(g[1]) < 3:
        print('page', pi, 'FAILED grid', g, flush=True); continue
    vcols, hrows = g
    if len(vcols) != 6:
        print('page', pi, 'columns', vcols)
    # erase grid lines for OCR
    clean = full.copy()
    for x in vcols: clean[:, max(0, x - 4):x + 5] = False
    for y in hrows: clean[max(0, y - 4):y + 5, vcols[0]:vcols[-1]] = False
    cimg = Image.fromarray(np.where(clean, 0, 255).astype(np.uint8))
    top, bot = hrows[1] if len(hrows) > 1 else hrows[0], hrows[-1]   # skip header row (B C F J)
    bands = [(a, b) for a, b in zip(hrows, hrows[1:]) if a >= top]
    cols = list(zip(vcols, vcols[1:]))  # num, B, C, F, J
    words = {}
    for ci, (xa, xb) in enumerate(cols):
        strip = cimg.crop((xa + 5, top + 5, xb - 4, bot - 4))
        extra = ('-c', 'tessedit_char_whitelist=0123456789') if ci == 0 else ()
        ws = ocr(strip, 6, extra)
        for wd in ws:
            wd['x'] += xa + 5; wd['y'] += top + 5
        words[ci] = ws
    # a missing rule merges rows: line numbers sit at the bottom of their row, so split below each one
    split = []
    for ya, yb in bands:
        nums = sorted(wd['y'] + wd['h'] for wd in words[0] if ya <= wd['y'] + wd['h'] / 2 < yb)
        cuts = [ya] + [y + 12 for y in nums[:-1]] + [yb]
        split += list(zip(cuts, cuts[1:]))
    bands = split
    for (ya, yb) in bands:
        cell = {}
        flags = set()
        for ci in range(len(cols)):
            ws = [wd for wd in words[ci] if ya <= wd['y'] + wd['h'] / 2 < yb]
            # group into text lines by y
            ws.sort(key=lambda wd: (wd['y'], wd['x']))
            lines = []
            for wd in ws:
                cy = wd['y'] + wd['h'] / 2
                if lines and abs(lines[-1][0] - cy) < 22:
                    lines[-1][1].append(wd)
                else:
                    lines.append([cy, [wd]])
            texts = []
            for cy, lw in lines:
                lw.sort(key=lambda wd: wd['x'])
                parts, nbg = [], 0
                for wd in lw:
                    x0, y0, x1, y1 = wd['x'], wd['y'], wd['x'] + wd['w'], wd['y'] + wd['h']
                    fd = full[y0:y1, x0:x1].sum()
                    md = mlay[y0:y1, x0:x1].sum()
                    if ci == 4 and fd > 0 and md / fd < 0.5:
                        nbg += 1   # word only in the scan background: underlined, struck or scribbled over
                    if ci >= 3 and wd['conf'] < 70:
                        flags.add('lowconf')
                    parts.append(wd['text'])
                line = ' '.join(parts)
                if ci == 4 and nbg * 2 >= len(lw):
                    # a rule through the middle of the line = struck (old version), a rule below it =
                    # underlined (new version). Letters never bridge the gaps between words, a rule does.
                    ly0 = int(np.median([wd['y'] for wd in lw])); lh = int(np.median([wd['h'] for wd in lw]))
                    gaps = [(a['x'] + a['w'] + 2, b['x'] - 2) for a, b in zip(lw, lw[1:]) if b['x'] - a['x'] - a['w'] > 8]
                    # also the space inside merged tokens ("He-might-hold-") is bridged: test the whole span too
                    def bridged(y_from, y_to):
                        fr = []
                        for gx0, gx1 in gaps:
                            band = gray[ly0 + y_from:ly0 + y_to, gx0:gx1]
                            fr.append(band.mean(axis=1).max() if band.size else 0.0)
                        return fr
                    smid = bridged(int(lh * 0.4), int(lh * 0.7))
                    slow = bridged(int(lh * 0.8), lh + 12)
                    if DBG: print('   bg line %-40s mid %s low %s' % (line[:40], ' '.join('%.2f' % v for v in smid), ' '.join('%.2f' % v for v in slow)))
                    if smid and sum(v > 0.3 for v in smid) * 2 >= len(smid):
                        line = '-' + line
                    elif not slow or sum(v > 0.6 for v in slow) * 2 <= len(slow):
                        line = '?' + line
                        flags.add('bg')
                texts.append(line)
            cell[ci] = texts
        if ink[ya:yb, cols[1][0]:cols[-1][1]].sum() > 200:
            flags.add('pen')
        if not any(cell[ci] for ci in range(1, len(cols))):
            continue
        rows_out.append(dict(page=pi, y0=ya, y1=yb, num=' '.join(cell[0]), tc=' | '.join(cell[1]),
                             scene=' '.join(cell[2]), who=' '.join(cell[3]),
                             text=' / '.join(cell[4]), flags=','.join(sorted(flags))))
    print('page', pi, 'rows', len(bands), flush=True)

with open(out, 'w', encoding='utf-8', newline='') as f:
    wr = csv.writer(f, delimiter='\t')
    wr.writerow(['page', 'y0', 'y1', 'num', 'tc', 'scene', 'who', 'text', 'flags'])
    for r in rows_out:
        wr.writerow([r[k] for k in ('page', 'y0', 'y1', 'num', 'tc', 'scene', 'who', 'text', 'flags')])
print('rows', len(rows_out))
