# Glyph indices that decode to the given characters (to correct the kanji map).
import sys, jp_decode as J
m = J.charmap()
for ch in sys.argv[1]:
    print(ch, [hex(k) for k, v in m.items() if v == ch and k <= 0x8bd])
