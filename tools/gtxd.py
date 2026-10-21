# Grandia III GTXD textures inside unpacked MDT files (see mdz_extract.c), and the content keys the
# GPU renderer computes for them (gs_gpu.cpp rep_key / gs_texrep.cpp hash).
#
# GTXD header (offsets from the magic; "base" = magic + 0x10):
#   0x04 u32 size from base       0x12 u16 width   0x14 u16 height   0x17 u8 psm (0x13 T8, 0x14 T4)
#   0x1C u16 flags (high byte: mip levels)        0x1E u16 palette count
#   0x20 u32 texel offset (from base)             0x24 u32 palette offset (from base)
#   0x30 name (32 bytes)
# Texels are stored as a PSMCT32 upload of the same memory (T8 w x h = CT32 w/2 x h/2), palettes
# as 32-bit CLUTs (T8: CSM1 order).
import ctypes, os, struct
import numpy as np
import gsmem

_dll = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'texhash.dll'))
_dll.texhash.restype = ctypes.c_uint64
_dll.texhash.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64]


def texhash(buf, seed):
    b = np.ascontiguousarray(buf)
    return _dll.texhash(b.ctypes.data, b.nbytes, seed & 0xFFFFFFFFFFFFFFFF)


PSMT8, PSMT4 = 0x13, 0x14
_CSM1 = np.array([gsmem.csm1(i) for i in range(256)])


class Tex:
    pass


def parse(d, i):
    t = Tex()
    t.off = i
    t.size = struct.unpack_from('<I', d, i + 4)[0]
    t.w, t.h = struct.unpack_from('<HH', d, i + 0x12)
    t.b16, t.psm = d[i + 0x16], d[i + 0x17]
    t.flags, t.npal = struct.unpack_from('<HH', d, i + 0x1C)
    t.texoff, t.paloff = struct.unpack_from('<II', d, i + 0x20)
    t.name = d[i + 0x30:i + 0x50].split(b'\0')[0].decode('cp932', 'replace')
    return t


def find_all(d):
    out = []
    i = d.find(b'GTXD')
    while i >= 0:
        try:
            t = parse(d, i)
            if t.psm in (PSMT8, PSMT4) and 0 < t.w <= 1024 and 0 < t.h <= 1024 and t.npal >= 1:
                out.append(t)
        except struct.error:
            pass
        i = d.find(b'GTXD', i + 4)
    return out


def _mem_t8(raw, w, h):
    mem = np.zeros(1 << 22, np.uint8)
    cw, ch = max(w // 2, 1), max(h // 2, 1)
    words = np.frombuffer(raw, np.uint32, cw * ch).reshape(ch, cw)
    mem.view(np.uint32)[gsmem.addr32(cw, ch, 0, max(1, cw // 64))] = words
    return mem[gsmem.addr8(w, h, 0, max(2, w // 64))]


def _ct32_shape_t4(w, h):
    """PSMCT32 rectangle the game uploads a T4 w x h texture as: within a page it covers the same
    memory blocks (T4 blocks are 32x16 texels, CT32 blocks 8x8 pixels, arranged differently)"""
    if w >= 128 and h >= 128:
        return w // 2, h // 4
    need = set(gsmem.BT4[(y >> 4) & 7, (x >> 5) & 3] for y in range(0, h, 16) for x in range(0, w, 32))
    n = w * h // 8
    cw = 8
    while cw <= 64:
        ch = n // cw
        if ch >= 8 and cw * ch == n and ch <= 32:
            got = set(gsmem.BT32[(y >> 3) & 3, (x >> 3) & 7] for y in range(0, ch, 8) for x in range(0, cw, 8))
            if got == need:
                return cw, ch
        cw *= 2
    return max(w // 2, 1), max(h // 4, 1)


def _mem_t4(raw, w, h, variant):
    mem = np.zeros(1 << 22, np.uint8)
    if variant == 'ct32':
        cw, ch = _ct32_shape_t4(w, h)
        words = np.frombuffer(raw, np.uint32, cw * ch).reshape(ch, cw)
        mem.view(np.uint32)[gsmem.addr32(cw, ch, 0, max(1, cw // 64))] = words
    else:  # linear 4-bit rows
        b = np.frombuffer(raw, np.uint8, w * h // 2)
        nib = np.empty(w * h, np.uint8)
        nib[0::2] = b & 15
        nib[1::2] = b >> 4
        a = gsmem.addr4(w, h, 0, max(2, w // 64)).ravel()
        cur = mem[a >> 1]
        mem[a >> 1] = np.where(a & 1, (cur & 15) | (nib << 4), (cur & 0xF0) | nib)
        return _read4(mem, w, h)
    return _read4(mem, w, h)


def _read4(mem, w, h):
    a = gsmem.addr4(w, h, 0, max(2, w // 64))
    return (mem[a >> 1] >> ((a & 1) * 4)) & 15


def texels(d, t, t4_variant='ct32'):
    base = t.off + 0x10
    if t.psm == PSMT8:
        return _mem_t8(d[base + t.texoff:base + t.texoff + t.w * t.h], t.w, t.h).astype(np.uint8)
    return _mem_t4(d[base + t.texoff:base + t.texoff + t.w * t.h // 2], t.w, t.h, t4_variant).astype(np.uint8)


def palettes(d, t):
    base = t.off + 0x10
    n = 256 if t.psm == PSMT8 else 16
    out = []
    for p in range(t.npal):
        e = np.frombuffer(d, np.uint32, n, base + t.paloff + p * n * 4)
        out.append(e[_CSM1] if n == 256 else e.copy())
    return out


def key(tex, pal, psm, w, h):
    """renderer key of decoded texels (one byte per texel) shown with palette entries pal"""
    k = texhash(tex.astype(np.uint8), psm | w << 8 | h << 24)
    return texhash(pal.astype(np.uint32), k ^ 0x32)


def rgba(tex, pal):
    """PNG pixels like the runtime dump (alpha 0..128 -> 0..255)"""
    c = pal[tex.astype(np.int64)]
    a = (c >> 24) & 0xFF
    a = np.where(a >= 128, 255, (a * 255 + 64) // 128)
    out = np.empty(tex.shape + (4,), np.uint8)
    out[..., 0] = c & 0xFF
    out[..., 1] = (c >> 8) & 0xFF
    out[..., 2] = (c >> 16) & 0xFF
    out[..., 3] = a
    return out
