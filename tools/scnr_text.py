# Texts of the scenario scripts (MDT chunk loaded to 0x03810000, "ScnrScript") in both versions.
# Script text bytes are font codes + 0x20: English ASCII + 0x20 (space 0xA0), Japanese single codes
# + 0x20 and two-byte codes (b + 0x20, F0..FF); 0x08 = line break, 0x0D ends a text.
import struct

SCRIPT_DST = 0x03810000


def chunks(d):
    out, p = [], 0x80
    while p + 8 <= len(d):
        a, sz = struct.unpack_from('<II', d, p)
        if sz == 0 or p + sz > len(d) + 0x1000:
            break
        out.append((p, a, sz))
        p += sz
    return out


def script(d):
    for p, a, sz in chunks(d):
        if a == SCRIPT_DST:
            return d[p:p + sz]
    return b''


def end(s, i):
    """offset of the 0x0D terminator; control arguments are skipped (a speaker id can be 0x0D)"""
    n = len(s)
    while i < n:
        b = s[i]
        if b == 0x0D:
            return i
        i += 1 + (CTRL_ARGS.get(b, 0) if b < 0x20 else 0)
    return -1


def texts(s, jp):
    """(offset, raw bytes) of every message, in script order. A message starts after the message
    opcode's header  <x u16> <y u16> <w u16> <h u16> <style> 0E 00 <speaker u16>  and runs to the
    0x0D terminator; pages are separated by 07 0E 00 <speaker u16>."""
    out = []
    k = s.find(b'\x0e\x00', 9)
    while k >= 0:
        x = s[k - 9] | s[k - 8] << 8
        y = s[k - 7] | s[k - 6] << 8
        w = s[k - 5] | s[k - 4] << 8
        h = s[k - 3] | s[k - 2] << 8
        ok = (x == 0xFFFF or x < 640) and (y == 0xFFFF or y < 480) and 0 < w < 1000 and 0 < h < 1000
        if ok:
            j = end(s, k + 4)
            if j > 0 and j - (k + 4) < 4096:
                out.append((k + 4, s[k + 4:j]))
                k = s.find(b'\x0e\x00', j)
                continue
        k = s.find(b'\x0e\x00', k + 1)
    return out


# control sequences inside message text: code -> number of argument bytes
CTRL_ARGS = {0x0E: 3, 0x13: 2, 0x12: 1}


def tokens(raw):
    """[(kind, bytes)]: 't' printable text (incl. 0x08 line breaks), 'c' control sequence or special
    two-byte character (kept verbatim by translations)"""
    out, i, n = [], 0, len(raw)
    while i < n:
        b = raw[i]
        if b < 0x20 and b != 0x08:  # control codes first: 13 FF xx is a pause, not a symbol
            k = 1 + CTRL_ARGS.get(b, 0)
            out.append(('c', raw[i:i + k]))
            i += k
        elif i + 1 < n and raw[i + 1] >= 0xF0:
            out.append(('c', raw[i:i + 2]))
            i += 2
        elif b < 0x40 and b != 0x08:
            k = 1 + CTRL_ARGS.get(b, 0)
            out.append(('c', raw[i:i + k]))
            i += k
        else:
            if out and out[-1][0] == 't':
                out[-1] = ('t', out[-1][1] + bytes([b]))
            else:
                out.append(('t', bytes([b])))
            i += 1
    return out


def to_markup(raw):
    """English message as editable text: \\n line break, {hex} control sequence"""
    s = ''
    for kind, b in tokens(raw):
        if kind == 'c':
            s += '{' + b.hex() + '}'
        else:
            s += ''.join('\\n' if x == 8 else (' ' if x == 0xA0 else chr(x - 0x20)) for x in b)
    return s


def dec_en(raw):
    """visible text; control bytes shown as {xx}"""
    return ''.join('\n' if b == 8 else (' ' if b == 0xA0 else (chr(b - 0x20) if b >= 0x40 else '{%02x}' % b)) for b in raw)


def glyphs_jp(raw):
    """glyph indices (-1 = line break)"""
    out, i = [], 0
    while i < len(raw):
        b = raw[i]
        if i + 1 < len(raw) and raw[i + 1] >= 0xF0:
            out.append(0xD4 + (raw[i + 1] - 0xF0) * 0xD0 + b - 0x20)
            i += 2
            continue
        out.append(-1 if b == 8 else b + 4 - 0x20)
        i += 1
    return out
