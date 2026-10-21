# Parallel dump of Grandia III dialogue (US English vs. Japanese) from unpacked MDT files.
# Dialogue block: 16 00 00 01 <portrait> 00 <speaker> 08 <line> 08 <line> 08 ... 00
#   English: ASCII, 0x80 = space, 0x08 = line break.
#   Japanese: 1 byte b -> glyph b+4; 2 bytes (b, F0..FF) -> glyph 0xD4 + (F-0xF0)*0xD0 + b.
#   python script_dump.py <us.MDT> <jp.MDT> <fontmap.tsv> [out.txt]
import sys

PAT = b'\x16\x00\x00\x01'


def blocks(d):
    out = []
    p = d.find(PAT)
    while p >= 0:
        s = p + 6
        e = d.find(b'\x00', s)
        raw = d[s:e]
        if 0 < len(raw) < 600 and raw.count(b'\x08') >= 1:
            out.append(raw)
        p = d.find(PAT, p + 4)
    return out


def dec_en(raw):
    return raw.replace(b'\x80', b' ').decode('latin1').split('\x08')


def load_map(path):
    m = {}
    for line in open(path, encoding='utf-8'):
        i, ch, _ = line.rstrip('\n').split('\t')
        m[int(i, 16)] = ch
    return m


def dec_jp(raw, m):
    parts, cur, i = [], [], 0
    while i < len(raw):
        b = raw[i]
        if i + 1 < len(raw) and raw[i + 1] >= 0xF0:
            cur.append(m.get(0xD4 + (raw[i + 1] - 0xF0) * 0xD0 + b, '?'))
            i += 2
            continue
        if b == 0x08:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(m.get(b + 4, '?'))
        i += 1
    parts.append(''.join(cur))
    return parts


if __name__ == '__main__':
    us = blocks(open(sys.argv[1], 'rb').read())
    jp = blocks(open(sys.argv[2], 'rb').read())
    m = load_map(sys.argv[3])
    out = open(sys.argv[4], 'w', encoding='utf-8') if len(sys.argv) > 4 else sys.stdout
    out.write('# %s: %d EN blocks, %d JP blocks\n' % (sys.argv[1], len(us), len(jp)))
    for k in range(max(len(us), len(jp))):
        e = [x for x in dec_en(us[k]) if x] if k < len(us) else []
        j = [x for x in dec_jp(jp[k], m) if x] if k < len(jp) else []
        out.write('--- %d\n' % k)
        out.write('JP  %s: %s\n' % (j[0] if j else '', ' '.join(j[1:])))
        out.write('EN  %s: %s\n' % (e[0] if e else '', ' '.join(e[1:])))
