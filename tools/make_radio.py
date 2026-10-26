# Builds lang/de_radio.tsv for the flight radio messages (see runtime/src/ee/sys_text.cpp, radio).
# The texts sit inline in the flight script, so a translation replaces the English bytes in place:
# it must not be longer than the original; shorter ones are padded with spaces at line ends.
#   python make_radio.py <src.tsv from radio_extract.py> <de.tsv> <out.tsv> <GR3.MDT>
# Checks: byte length, line count, line width (font pixels) against the widest English line.
# Output lines: <English bytes hex><TAB><German bytes hex, same length>.
import sys
from make_sys import width
from make_lang import font_widths

BSN = chr(92) + 'n'
from letters import CODE as DE, normalize


def encode(s):
    out, i, s = bytearray(), 0, normalize(s)
    while i < len(s):
        if s.startswith(BSN, i):
            out.append(8); i += 2; continue
        if s[i] == '{':
            e = s.index('}', i)
            out += bytes.fromhex(s[i + 1:e]); i = e + 1; continue
        c = s[i]
        if c == ' ': out.append(0x80)
        elif c in DE: out.append(DE[c])
        elif c in '„“”': out.append(ord('"'))
        elif c in '–—': out.append(ord('-'))
        elif ord(c) < 0x7f: out.append(ord(c))
        else: raise ValueError('character %r' % c)
        i += 1
    return bytes(out)


def main():
    src, de, out, gr3 = sys.argv[1:5]
    wt = font_widths(gr3)
    rows = [l.rstrip('\n').split('\t') for l in open(src, encoding='utf-8')]
    tr = {}
    for ln in open(de, encoding='utf-8'):
        if ln.startswith('#') or '\t' not in ln:
            continue
        n, t = ln.rstrip('\n').split('\t', 1)
        tr[int(n)] = t
    space = wt[(0x20 - 0x20) * 2 + 1] + 1
    maxw = max(width(l, wt) for r in rows for l in r[4].split(BSN))
    first = {}
    for r in rows:
        first.setdefault(r[4], int(r[0]))
    warn, done, res = 0, set(), []
    for r in rows:
        en = r[4]
        if en in done:
            continue
        done.add(en)
        n = first[en]
        if n not in tr:
            print('untranslated #%d: %s' % (n, en[:60])); warn += 1
            continue
        t = tr[n]
        eb, db = encode(en), encode(t)
        lines = t.split(BSN)
        msgs = []
        if len(db) > len(eb):
            msgs.append('%d bytes > %d' % (len(db), len(eb)))
        if len(lines) > len(en.split(BSN)):
            msgs.append('%d lines > %d' % (len(lines), len(en.split(BSN))))
        if not t.endswith(BSN):
            msgs.append('no final line break')
        for l in lines:
            if width(l, wt) > maxw:
                msgs.append('%d px: %s' % (width(l, wt), l))
        if en.count('{') != t.count('{'):
            msgs.append('control codes differ')
        if msgs:
            print('#%d: %s  [%s]' % (n, '; '.join(msgs), t)); warn += 1
            continue
        # pad: spaces at the ends of the text lines (speaker line and the empty last line excluded)
        pad = len(eb) - len(db)
        body = lines[1:-1]
        while pad > 0 and body:
            i = min(range(len(body)), key=lambda k: width(body[k], wt))
            if width(body[i], wt) + space > maxw:
                break
            body[i] += ' '
            pad -= 1
        if pad > 0 and body:
            body[-1] += ' ' * pad
        db = encode(BSN.join([lines[0]] + body + [lines[-1]]))
        assert len(db) == len(eb), (n, len(db), len(eb))
        res.append((eb, db))
    with open(out, 'w', encoding='utf-8', newline='\n') as fo:
        fo.write('# flight radio messages: English bytes, German bytes (same length); make_radio.py\n')
        for eb, db in res:
            fo.write('%s\t%s\n' % (eb.hex(), db.hex()))
    print('%d messages -> %s (%d warnings, widest English line %d px)' % (len(res), out, warn, maxw))


if __name__ == '__main__':
    main()
