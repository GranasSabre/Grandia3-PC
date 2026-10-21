"""Minimal VU micro-mode disassembler.

Usage: python vudis.py <elf> <vif_packet_addr_hex> <qwc_hex> [start_hex end_hex]
Extracts MPG uploads from a VIF packet in the main ELF and disassembles VU1 micro memory.
"""
import struct
import sys

UP1 = {}
for i, n in enumerate(['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL']):
    for b, c in enumerate('xyzw'):
        UP1[i * 4 + b] = n + c
UP1.update({0x1C: 'MULq', 0x1D: 'MAXi', 0x1E: 'MULi', 0x1F: 'MINIi', 0x20: 'ADDq', 0x21: 'MADDq', 0x22: 'ADDi',
            0x23: 'MADDi', 0x24: 'SUBq', 0x25: 'MSUBq', 0x26: 'SUBi', 0x27: 'MSUBi', 0x28: 'ADD', 0x29: 'MADD',
            0x2A: 'MUL', 0x2B: 'MAX', 0x2C: 'SUB', 0x2D: 'MSUB', 0x2E: 'OPMSUB', 0x2F: 'MINI'})
UP2 = {}
for i, n in enumerate(['ADDA', 'SUBA', 'MADDA', 'MSUBA']):
    for b, c in enumerate('xyzw'):
        UP2[i * 4 + b] = n + c
for b, c in enumerate('xyzw'):
    UP2[0x18 + b] = 'MULA' + c
UP2.update({0x10: 'ITOF0', 0x11: 'ITOF4', 0x12: 'ITOF12', 0x13: 'ITOF15', 0x14: 'FTOI0', 0x15: 'FTOI4',
            0x16: 'FTOI12', 0x17: 'FTOI15', 0x1C: 'MULAq', 0x1D: 'ABS', 0x1E: 'MULAi', 0x1F: 'CLIPw',
            0x20: 'ADDAq', 0x21: 'MADDAq', 0x22: 'ADDAi', 0x23: 'MADDAi', 0x24: 'SUBAq', 0x25: 'MSUBAq',
            0x26: 'SUBAi', 0x27: 'MSUBAi', 0x28: 'ADDA', 0x29: 'MADDA', 0x2A: 'MULA', 0x2C: 'SUBA', 0x2D: 'MSUBA',
            0x2E: 'OPMULA', 0x2F: 'NOP'})
LO = {0x00: 'LQ', 0x01: 'SQ', 0x04: 'ILW', 0x05: 'ISW', 0x08: 'IADDIU', 0x09: 'ISUBIU', 0x10: 'FCEQ', 0x11: 'FCSET',
      0x12: 'FCAND', 0x13: 'FCOR', 0x14: 'FSEQ', 0x15: 'FSSET', 0x16: 'FSAND', 0x17: 'FSOR', 0x18: 'FMEQ',
      0x1A: 'FMAND', 0x1B: 'FMOR', 0x1C: 'FCGET', 0x20: 'B', 0x21: 'BAL', 0x24: 'JR', 0x25: 'JALR', 0x28: 'IBEQ',
      0x29: 'IBNE', 0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}
LO40 = {0x30: 'IADD', 0x31: 'ISUB', 0x32: 'IADDI', 0x34: 'IAND', 0x35: 'IOR'}
LO40X = {0x30: 'MOVE', 0x31: 'MR32', 0x34: 'LQI', 0x35: 'SQI', 0x36: 'LQD', 0x37: 'SQD', 0x38: 'DIV', 0x39: 'SQRT',
         0x3A: 'RSQRT', 0x3B: 'WAITQ', 0x3C: 'MTIR', 0x3D: 'MFIR', 0x3E: 'ILWR', 0x3F: 'ISWR', 0x40: 'RNEXT',
         0x41: 'RGET', 0x42: 'RINIT', 0x43: 'RXOR', 0x64: 'MFP', 0x68: 'XTOP', 0x69: 'XITOP', 0x6C: 'XGKICK',
         0x70: 'ESADD', 0x71: 'ERSADD', 0x72: 'ELENG', 0x73: 'ERLENG', 0x74: 'EATANxy', 0x75: 'EATANxz',
         0x76: 'ESUM', 0x78: 'ESQRT', 0x79: 'ERSQRT', 0x7A: 'ERCPR', 0x7B: 'WAITP', 0x7C: 'ESIN', 0x7D: 'EATAN',
         0x7E: 'EEXP'}


def dest(w):
    d = (w >> 21) & 15
    return ''.join(c for c, b in zip('xyzw', (8, 4, 2, 1)) if d & b)


def dis_upper(u):
    f = u & 63
    if f < 0x3C:
        n = UP1.get(f, '???')
    else:
        n = UP2.get((u & 3) | ((u >> 4) & 0x7C), '???')
    flags = ''.join(c for c, b in zip('IEMDT', (31, 30, 29, 28, 27)) if (u >> b) & 1)
    return f'{n}.{dest(u)} vf{(u >> 6) & 31},vf{(u >> 11) & 31},vf{(u >> 16) & 31} [{flags}]'


def dis_lower(l, pc):
    op = l >> 25
    imm11 = l & 0x7FF
    if imm11 & 0x400:
        imm11 -= 0x800
    it, is_, id_ = (l >> 16) & 31, (l >> 11) & 31, (l >> 6) & 31
    if op == 0x40:
        f = l & 63
        if f in LO40:
            return f'{LO40[f]} vi{id_},vi{is_},vi{it}' if f != 0x32 else f'IADDI vi{it},vi{is_},{((l >> 6) & 31) - (32 if (l >> 10) & 1 else 0)}'
        idx = (l & 3) | ((l >> 4) & 0x7C)
        n = LO40X.get(idx, f'??? {l:08x}')
        return f'{n}.{dest(l)} ft{it} fs{is_}'
    n = LO.get(op)
    if n is None:
        return f'??? {l:08x}'
    if n in ('B', 'BAL', 'IBEQ', 'IBNE', 'IBLTZ', 'IBGTZ', 'IBLEZ', 'IBGEZ'):
        return f'{n} vi{it},vi{is_} -> {pc + 8 + imm11 * 8:04x}'
    if n in ('LQ', 'SQ', 'ILW', 'ISW'):
        return f'{n}.{dest(l)} f/vi{it}, {imm11}(vi{is_})'
    if n in ('IADDIU', 'ISUBIU'):
        return f'{n} vi{it},vi{is_},{(l & 0x7FF) | ((l >> 10) & 0x7800)}'
    return f'{n} vi{it} vi{is_} imm={l & 0xFFFFFF:x}'


def main_raw():
    """vudis.py --raw <micro.bin> [start_hex end_hex]: disassemble a micro memory dump"""
    m = open(sys.argv[2], 'rb').read()
    a0 = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0
    a1 = int(sys.argv[4], 16) if len(sys.argv) > 4 else len(m)
    for pc in range(a0, a1, 8):
        lo, up = struct.unpack_from('<II', m, pc)
        print(f"{pc:04x}: {dis_upper(up):32s} {dis_lower(lo, pc)}")


def main():
    if sys.argv[1] == '--raw':
        return main_raw()
    d = open(sys.argv[1], 'rb').read()
    pkt = int(sys.argv[2], 16)
    qwc = int(sys.argv[3], 16)
    off = pkt - 0x100000 + 0x100
    words = struct.unpack_from('<%dI' % (qwc * 4), d, off)
    micro = bytearray(16384)
    i = 0
    while i < len(words):
        code = words[i]
        i += 1
        cmd = (code >> 24) & 0x7F
        num = (code >> 16) & 0xFF
        imm = code & 0xFFFF
        if cmd == 0x4A:
            n = (num or 256) * 2
            addr = imm * 8
            for k in range(n):
                struct.pack_into('<I', micro, (addr + k * 4) % 16384, words[i + k])
            i += n
        elif cmd >= 0x60:
            vn, vl = (cmd >> 2) & 3, cmd & 3
            bits = (32 >> vl) * (vn + 1) * (num or 256)
            i += (bits + 31) // 32
        elif cmd in (0x20,):
            i += 1
        elif cmd in (0x30, 0x31):
            i += 4
        elif cmd in (0x50, 0x51):
            i += (imm or 65536) * 4
    a0 = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
    a1 = int(sys.argv[5], 16) if len(sys.argv) > 5 else 0x400
    for pc in range(a0, a1, 8):
        lo, up = struct.unpack_from('<II', micro, pc)
        lower = f'LOI {lo:08x}' if (up >> 31) & 1 else dis_lower(lo, pc)
        print(f'{pc:04x}: {dis_upper(up):40s} | {lower}')


if __name__ == '__main__':
    main()
