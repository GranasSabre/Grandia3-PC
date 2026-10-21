# Survey of all scenario-script messages: counts per version and the control bytes used in them.
#   python scnr_survey.py <us mdt dir> <jp mdt dir>
import collections, glob, os, sys
import scnr_text as S

us_dir, jp_dir = sys.argv[1], sys.argv[2]
tot = mism = words = 0
ctrl = collections.Counter()
ex = {}
for f in sorted(glob.glob(os.path.join(us_dir, '*.MDT'))):
    u = S.script(open(f, 'rb').read())
    if not u:
        continue
    tu = S.texts(u, False)
    jf = os.path.join(jp_dir, os.path.basename(f))
    tj = S.texts(S.script(open(jf, 'rb').read()), True) if os.path.exists(jf) else []
    tot += len(tu)
    if len(tu) != len(tj):
        mism += 1
    for o, r in tu:
        words += len(S.dec_en(r).split())
        for i, b in enumerate(r):
            if b < 0x40 and b != 0x08:
                ctrl[b] += 1
                ex.setdefault(b, r[max(0, i - 3):i + 6].hex(' '))
print('messages', tot, 'words', words, 'maps with count mismatch', mism)
for k, v in sorted(ctrl.items()):
    print('%02x %7d  e.g. %s' % (k, v, ex[k]))
