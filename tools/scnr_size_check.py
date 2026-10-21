# Checks the message window size rule used by the runtime text hook against every English message:
# width = widest line of (advance + 1), height = 20 px per line, over all pages.
#   python scnr_size_check.py <us mdt dir> <GR3.MDT>
import collections, glob, os, sys
import scnr_text as S

mdt_dir, gr3 = sys.argv[1], sys.argv[2]
d = open(gr3, 'rb').read()
FONT = 0x34700
WT = d[FONT + 0x1A0:FONT + 0x520]


def size(raw):
    w = lw = 0
    lines = maxlines = 1
    for kind, b in S.tokens(raw):
        if kind == 'c':
            if b[0] == 0x07:
                w, lw, lines = max(w, lw), 0, 1
            elif len(b) == 2 and b[1] >= 0xF0:
                lw += 13
            continue
        for x in b:
            if x == 0x08:
                w, lw = max(w, lw), 0
                lines += 1
                maxlines = max(maxlines, lines)
            elif x >= 0x40:
                lw += WT[(x - 0x40) * 2 + 1] + 1
    return max(w, lw), maxlines * 20


ok = bad = 0
diff = collections.Counter()
ex = []
for f in sorted(glob.glob(os.path.join(mdt_dir, '*.MDT'))):
    s = S.script(open(f, 'rb').read())
    for o, raw in S.texts(s, False):
        hw = s[o - 9] | s[o - 8] << 8
        hh = s[o - 7] | s[o - 6] << 8
        w, h = size(raw)
        if (w, h) == (hw, hh):
            ok += 1
        else:
            bad += 1
            diff[(w - hw, h - hh)] += 1
            if len(ex) < 12:
                ex.append((os.path.basename(f), hw, hh, w, h, S.to_markup(raw)[:90]))
print('match', ok, 'differ', bad)
print(diff.most_common(12))
for e in ex:
    print(e)
