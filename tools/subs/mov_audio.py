"""Audio of MOVIE\\GRMxx.MOV: a 0x800 header (u32 channels, rate, interleave, ..., +24 audio bytes,
+28 samples per channel, +32 length in 60 Hz frames), then 0x800 sectors that are either MPEG-PS video
(a run starting with a pack header 00 00 01 BA) or PS-ADPCM audio, left and right chunks of 0x800 in turn."""
import struct


def pack_end(d, p):
    """end of the MPEG-PS pack run starting at p (packs and PES packets until a non start code)"""
    while p + 4 <= len(d) and d[p:p + 3] == b'\x00\x00\x01':
        sid = d[p + 3]
        if sid == 0xBA:
            p += 14 + (d[p + 13] & 7)
        elif sid == 0xB9:
            p += 4
        else:
            p += 6 + struct.unpack_from('>H', d, p + 4)[0]
    return p


def audio(path):
    """(rate, header, audio bytes in stream order)"""
    d = open(path, 'rb').read()
    hdr = d[:0x800]
    rate, want = struct.unpack_from('<I', hdr, 4)[0], struct.unpack_from('<I', hdr, 24)[0]
    out = bytearray()
    p = 0x800
    while p < len(d) and len(out) < want:
        if d[p:p + 4] == b'\x00\x00\x01\xba':
            e = pack_end(d, p)
            p = (e + 0x7FF) & ~0x7FF
        else:
            out += d[p:p + 0x800]
            p += 0x800
    return rate, hdr, bytes(out[:want])


if __name__ == '__main__':
    import sys
    for f in sys.argv[1:]:
        rate, hdr, a = audio(f)
        want = struct.unpack_from('<I', hdr, 24)[0]
        print('%s: rate %d, audio %d of %d bytes, %.1f s' % (f, rate, len(a), want, len(a) / 2 / 16 * 28 / rate))
