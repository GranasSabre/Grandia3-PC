# Contexts of given glyphs in the Japanese script texts (to identify a kanji from its words).
#   python jp_context.py <jp mdt dir> <glyph hex> [...]
import glob, os, sys
import scnr_text as S, jp_decode as J

want = [int(x, 16) for x in sys.argv[2:]]
found = {w: [] for w in want}
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.MDT'))):
    s = S.script(open(f, 'rb').read())
    for o, raw in S.texts(s, True):
        g = J.glyphs(raw)
        for w in want:
            if w in g and len(found[w]) < 4:
                t = J.decode(raw, False).replace('\n', '')
                found[w].append(t[:60])
for w in want:
    print('%03x' % w, J.charmap().get(w), ' | '.join(found[w]))
