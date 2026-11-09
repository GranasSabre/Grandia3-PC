# Builds lang/sub_<language>.tsv from sub_en.tsv and a translation of its distinct lines.
#   python make_sub_lang.py <sub_en.tsv> <translation dir> <language> <out.tsv>
# Translation files <dir>/<language>_*.tsv: "<n><TAB><text>", n = index of the distinct English text
# in order of first appearance (the same numbering as the src_*.txt work files). An empty text drops
# the subtitle (OCR junk in the English script). French: no-break spaces before ! ? : ; and inside
# guillemets, so a line never wraps before them.
import glob
import os
import sys

src, tr_dir, lang, out = sys.argv[1:5]
rows = [l.rstrip('\n').split('\t') for l in open(src, encoding='utf-8') if l.strip() and not l.startswith('#')]
order = {}
for r in rows:
    order.setdefault(r[3], len(order))
tr = {}
for f in sorted(glob.glob(os.path.join(tr_dir, lang + '_*.tsv'))):
    for ln in open(f, encoding='utf-8'):
        ln = ln.rstrip('\n')
        if not ln:
            continue
        n, _, text = ln.partition('\t')
        tr[int(n)] = text.strip()
missing = [n for n in range(len(order)) if n not in tr]
if missing:
    sys.exit('missing translations: %s' % ' '.join('%04d' % n for n in missing[:30]))
NB = ' '


def fix(t):
    if lang == 'fr':
        for p in '!?:;»':
            t = t.replace(' ' + p, NB + p)
        t = t.replace('« ', '«' + NB)
    return t


n_out = 0
with open(out, 'w', encoding='utf-8', newline='\n') as fo:
    fo.write('# cutscene subtitles (%s): track tag (GR3_STR.STZ), start, end (seconds from the track start), text\n' % lang)
    for r in rows:
        t = tr[order[r[3]]]
        if t:
            fo.write('\t'.join(r[:3] + [fix(t)]) + '\n')
            n_out += 1
print('%s: %d lines (%d distinct texts, %d dropped)' % (out, n_out, len(order), len(rows) - n_out))
