# Japanese script text -> Unicode, using the font map (kana/symbols from jp_font_map.tsv, kanji from
# jp_kanji_ocr2.tsv top candidates plus the manual corrections in jp_kanji_fix.tsv).
import os
import scnr_text as S

WORK = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'work')
_map = None


def charmap():
    global _map
    if _map is None:
        m = {}
        for ln in open(os.path.join(WORK, 'jp_font_map.tsv'), encoding='utf-8'):
            p = ln.rstrip('\n').split('\t')
            if len(p) >= 2 and int(p[0], 16) <= 0x109:
                m[int(p[0], 16)] = p[1]
        for ln in open(os.path.join(WORK, 'jp_kanji_ocr2.tsv'), encoding='utf-8'):
            p = ln.rstrip('\n').split('\t')
            if int(p[0], 16) > 0x109:
                m[int(p[0], 16)] = p[1][:1]
        for ln in open(os.path.join(WORK, 'jp_kanji_fix.tsv'), encoding='utf-8'):
            if ln.startswith('#') or '\t' not in ln:
                continue
            p = ln.rstrip('\n').split('\t')
            m[int(p[0], 16)] = p[1]
        _map = m
    return _map


def glyph_of(kind, b):
    if kind == 'c' and len(b) == 2 and b[1] >= 0xF0:
        return 0xD4 + (b[1] - 0xF0) * 0xD0 + b[0] - 0x20
    return None


def decode(raw, ctrl=True):
    """markup like scnr_text.to_markup: \\n line break, {hex} control sequences"""
    m = charmap()
    s = ''
    for kind, b in S.tokens(raw):
        g = glyph_of(kind, b)
        if g is not None:
            s += m.get(g, '〓')
        elif kind == 'c' and len(b) == 1 and 0x20 <= b[0] < 0x40:
            g = b[0] + 4 - 0x20  # glyphs 05..21 are (almost) blank spacing glyphs
            s += ' ' if 5 <= g <= 0x21 else m.get(g, '〓')
        elif kind == 'c':
            if ctrl:
                s += '{' + b.hex() + '}'
        else:
            s += ''.join('\\n' if x == 8 else m.get(x + 4 - 0x20, '〓') for x in b)
    return s


def glyphs(raw):
    out = []
    for kind, b in S.tokens(raw):
        g = glyph_of(kind, b)
        if g is not None:
            out.append(g)
        elif kind == 't':
            out += [x + 4 - 0x20 for x in b if x != 8]
    return out
