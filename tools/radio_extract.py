# Flight radio messages (30000000.MDT, flight script opcode 16 00 00 01 <param u16> <text> 00).
# Text bytes are font codes: ASCII, space 0x80, line break 0x08; the first line is the speaker.
# Writes <out.tsv>: n, file offset, byte length, lines, EN (\n markup), JP (decoded); one row per
# message in script order. EN and JP have the same script structure: the opcode occurrences align
# 1:1 by index.
#   python radio_extract.py <us 30000000.MDT> <jp 30000000.MDT> <out.tsv>
import re, sys
import jp_decode

BSN = chr(92) + 'n'


def occurrences(d):
    out = []
    for m in re.finditer(re.escape(bytes([0x16, 0, 0, 1])), d):
        o = m.start()
        out.append((o, d[o + 6:d.find(bytes(1), o + 6)]))
    return out


def is_text(t):
    return len(t) >= 3 and sum(1 for b in t if 0x20 <= b < 0x7f or b in (8, 0x80)) / len(t) >= 0.95


def en_text(t):
    return ''.join(BSN if b == 8 else ' ' if b == 0x80 else chr(b) if 0x20 <= b < 0x7f else '{%02x}' % b for b in t)


def jp_text(t):
    """radio bytes are font codes: single byte b = glyph b + 4, pair (b, F0..FF) as in script text"""
    m = jp_decode.charmap()
    out, i = '', 0
    while i < len(t):
        b = t[i]
        if b == 8:
            out += BSN
        elif i + 1 < len(t) and t[i + 1] >= 0xF0:
            out += m.get(0xD4 + (t[i + 1] - 0xF0) * 0xD0 + b, '?')
            i += 1
        else:
            out += m.get(b + 4, '{%02x}' % b)
        i += 1
    return out


if __name__ == '__main__':
    us, jp, out = sys.argv[1:4]
    d, j = open(us, 'rb').read(), open(jp, 'rb').read()
    oe, oj = occurrences(d), occurrences(j)
    assert len(oe) == len(oj)
    n = 0
    with open(out, 'w', encoding='utf-8', newline='\n') as fo:
        for (o, t), (_, tj) in zip(oe, oj):
            if not is_text(t):
                continue
            fo.write('%d\t%x\t%d\t%d\t%s\t%s\n' % (n, o, len(t), t.count(8) + 1, en_text(t), jp_text(tj)))
            n += 1
    print(n, 'messages')
