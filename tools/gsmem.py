# GS local memory addressing (swizzle) in numpy, tables parsed from runtime/src/gs/gs_mem.cpp.
import os, re
import numpy as np

_src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runtime', 'src', 'gs', 'gs_mem.cpp')).read()


def _table(name):
    m = re.search(r'%s\[(\d+)\]\[(\d+)\]\s*=\s*\{(.*?)\};' % name, _src, re.S)
    rows, cols = int(m.group(1)), int(m.group(2))
    vals = [int(v) for v in re.findall(r'\d+', m.group(3))]
    return np.array(vals, np.int64).reshape(rows, cols)


BT32, BT16, BT8, BT4 = _table('blockTable32'), _table('blockTable16'), _table('blockTable8'), _table('blockTable4')
CT32, CT16, CT8, CT4 = _table('columnTable32'), _table('columnTable16'), _table('columnTable8'), _table('columnTable4')


def _grid(w, h):
    y, x = np.mgrid[0:h, 0:w]
    return x.astype(np.int64), y.astype(np.int64)


def addr32(w, h, bp, bw):  # word addresses of a w x h PSMCT32 rectangle
    x, y = _grid(w, h)
    page = (x >> 6) + (y >> 5) * bw
    block = bp + page * 32 + BT32[(y >> 3) & 3, (x >> 3) & 7]
    return block * 64 + CT32[y & 7, x & 7]


def addr16(w, h, bp, bw):  # halfword addresses (PSMCT16)
    x, y = _grid(w, h)
    page = (x >> 6) + (y >> 6) * bw
    block = bp + page * 32 + BT16[(y >> 3) & 7, (x >> 4) & 3]
    return block * 128 + CT16[y & 7, x & 15]


def addr8(w, h, bp, bw):  # byte addresses (PSMT8); bw in units of 64 texels
    x, y = _grid(w, h)
    page = (x >> 7) + (y >> 6) * (bw >> 1)
    block = bp + page * 32 + BT8[(y >> 4) & 3, (x >> 4) & 7]
    return block * 256 + CT8[y & 15, x & 15]


def addr4(w, h, bp, bw):  # nibble addresses (PSMT4)
    x, y = _grid(w, h)
    page = (x >> 7) + (y >> 7) * (bw >> 1)
    block = bp + page * 32 + BT4[(y >> 4) & 7, (x >> 5) & 3]
    return block * 512 + CT4[y & 15, x & 31]


def csm1(i):
    """CLUT entry position of index i in a CSM1 (swizzled) 256-entry palette"""
    return (i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1)
