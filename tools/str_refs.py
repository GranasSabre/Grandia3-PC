# Finds code references (lui + addiu/ori pairs) to addresses in an overlay / the main executable.
#   python str_refs.py <FIELD.BIN|BATTLE.BIN|FLIGHT.BIN|SLUS_213.34> <addr hex> [...]
# prints: reference address, register, and the next jal target after it
import struct, sys


def load(path):
    d = open(path, 'rb').read()
    if d[:4] in (b'MWo3', b'gccM', b'Mgcc'):  # overlay: load address of file offset 0 at +8
        return d, struct.unpack_from('<I', d, 8)[0], 0x80
    # ELF: first PT_LOAD
    phoff = struct.unpack_from('<I', d, 0x1C)[0]
    off, vaddr = struct.unpack_from('<II', d, phoff + 4)
    return d, vaddr - off, off


def refs(d, base, start, targets):
    out = []
    n = len(d) // 4
    for i in range(start // 4, n):
        w = struct.unpack_from('<I', d, i * 4)[0]
        if w >> 26 != 0x0F:
            continue
        rt, imm = (w >> 16) & 31, w & 0xFFFF
        for j in range(i + 1, min(i + 24, n)):
            x = struct.unpack_from('<I', d, j * 4)[0]
            op = x >> 26
            if op in (0x09, 0x0D) and (x >> 21) & 31 == rt:
                lo = x & 0xFFFF
                v = ((imm << 16) + (lo - 0x10000 if op == 0x09 and lo & 0x8000 else lo)) & 0xFFFFFFFF
                if v in targets:
                    # next jal
                    jt = None
                    for k in range(j, min(j + 16, n)):
                        y = struct.unpack_from('<I', d, k * 4)[0]
                        if y >> 26 == 3:
                            jt = ((y & 0x3FFFFFF) << 2) | ((base + k * 4) & 0xF0000000)
                            break
                    out.append((base + j * 4, v, (x >> 16) & 31, jt))
            if (x >> 26 == 0x0F and (x >> 16) & 31 == rt):
                break
    return out


if __name__ == '__main__':
    d, base, start = load(sys.argv[1])
    t = set(int(a, 16) for a in sys.argv[2:])
    for a, v, r, jt in refs(d, base, start, t):
        print('%08x -> %08x  r%d  next jal %s' % (a, v, r, '%08x' % jt if jt else '-'))
