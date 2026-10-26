"""Contact sheets of the flagged rows (text column, or the whole row for timecode problems).
usage: vo_review.py <render dir> <raw.tsv> <clean.tsv> <outdir>"""
import csv, os, re, sys
from PIL import Image, ImageDraw

rdir, rawp, cleanp, out = sys.argv[1:5]
os.makedirs(out, exist_ok=True)
raw = [r for r in csv.DictReader(open(rawp, encoding='utf-8'), delimiter='\t') if r['page'] != '10']
cl = list(csv.DictReader(open(cleanp, encoding='utf-8'), delimiter='\t'))
X = (199, 346, 770, 1018, 1360, 2184)   # table columns at render scale 4
todo = []
for r, c in zip(raw, cl):
    if not re.search(r'[A-Za-z]{2}', c['text'] + c['alt']):
        continue
    f = set(c['flags'].split(','))
    if f & {'unclear', 'pen', 'notc', 'tcbad'}:
        whole = bool(f & {'notc', 'tcbad'})
        todo.append((c['i'], int(r['page']), int(r['y0']), int(r['y1']), whole))
pages = {}
crops = []
for i, p, y0, y1, whole in todo:
    if p not in pages: pages[p] = Image.open(os.path.join(rdir, 'g%03d.png' % p))
    im = pages[p].crop((X[1] if whole else X[4], max(0, y0 - 12), X[5] + 10, y1 + 12))
    s = 0.55
    im = im.resize((int(im.width * s), int(im.height * s)))
    lab = Image.new('L', (im.width, 22), 255)
    ImageDraw.Draw(lab).text((4, 4), '#' + i, fill=0)
    tile = Image.new('L', (im.width, im.height + 22), 255)
    tile.paste(lab, (0, 0)); tile.paste(im, (0, 22))
    crops.append(tile)
# pack into sheets: two columns, max 1500 px tall
sheet_w, max_h = 1100, 1500
sheets, colx, cury, cur = [], 0, 0, None
def new():
    return Image.new('L', (sheet_w, max_h), 255)
cur = new(); col = 0
for t in crops:
    w = 1100 if t.width > 550 else 550
    if t.width > 550:
        if col == 1 or cury + t.height > max_h:
            # full-width tiles start on a fresh row band
            pass
    if cury + t.height > max_h:
        if col == 0 and t.width <= 550:
            col, cury = 1, colstart if False else 0
        else:
            sheets.append(cur); cur = new(); col, cury = 0, 0
    cur.paste(t, (col * 550, cury)); cury += t.height + 6
    if t.width > 550: col = 0
sheets.append(cur)
for n, s in enumerate(sheets):
    # trim empty bottom
    bbox = Image.eval(s, lambda v: 255 - v).getbbox()
    s.crop((0, 0, sheet_w, bbox[3] + 4 if bbox else max_h)).save(os.path.join(out, 'rev%02d.png' % n))
print(len(todo), 'rows on', len(sheets), 'sheets')
