# Applies a correction TSV (key<TAB>text, # comments allowed) to the translation files: each line
# replaces the entry with the same key in whichever <de dir>/b*.tsv holds it.
#   python apply_fix.py <de dir> <fix.tsv>
import glob, os, sys

de_dir, fix = sys.argv[1], sys.argv[2]
fixes = {}
for ln in open(fix, encoding='utf-8'):
    ln = ln.rstrip('\n')
    if len(ln) > 17 and ln[16] == '\t' and not ln.startswith('#'):
        fixes[ln[:16]] = ln[17:]
left = dict(fixes)
for tf in sorted(glob.glob(os.path.join(de_dir, '*.tsv'))):
    lines = open(tf, encoding='utf-8').read().split('\n')
    changed = False
    for i, ln in enumerate(lines):
        k = ln[:16]
        if len(ln) > 17 and ln[16] == '\t' and k in left:
            if ln[17:] != left[k]:
                lines[i] = k + '\t' + left[k]
                changed = True
            del left[k]
    if changed:
        open(tf, 'w', encoding='utf-8').write('\n'.join(lines))
for k in left:
    print('key %s not found' % k)
print('%d corrections applied' % (len(fixes) - len(left)))
