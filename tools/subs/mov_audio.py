"""Audio of MOVIE\\GRMxx.MOV: a 0x800 header (u32 channels, rate, interleave, ..., +24 audio bytes,
+28 samples per channel, +32 length in 60 Hz frames), then 0x800 sectors of MPEG-PS video with runs of
PS-ADPCM audio sectors between them (left and right chunks of 0x800 in turn, 24-33 sectors per run).
The video is not cut at pack boundaries: a video stretch may start without a pack header and a pack may
run on into the next audio run. So the audio is found by its content, as runs of sectors whose 128 ADPCM
frame headers are all valid; all-zero sectors are video fill, not silence (a recording of the game shows the
sound without them). (Taking every sector outside the pack runs as audio, as before 2026-11-03,
mixed video sectors into the sound and lost audio ones: the decoded sound ran about 8 % long and its
end was cut off. Checked against a recording of the game: now sample exact.)"""
import struct
import numpy as np

MIN_RUN = 8   # audio runs are 24-33 sectors (64 at the start)


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


def audio_sectors(d):
    """file sector numbers of the audio, in stream order"""
    n = len(d) // 0x800
    s = np.frombuffer(d, np.uint8, n * 0x800).reshape(n, 128, 16)
    b0, b1 = s[:, :, 0], s[:, :, 1]
    ok = ((b0 >> 4 <= 4) & (b0 & 15 <= 12) & np.isin(b1, [0, 1, 2, 3, 4, 6, 7])).all(axis=1)
    ok &= s.reshape(n, -1).any(axis=1)
    ok[0] = False
    out = []
    i = 1
    while i < n:
        j = i
        while j < n and ok[j]: j += 1
        if j - i >= MIN_RUN: out += range(i, j)
        i = max(j, i + 1)
    return out


def audio(path):
    """(rate, header, audio bytes in stream order)"""
    d = open(path, 'rb').read()
    hdr = d[:0x800]
    rate, want = struct.unpack_from('<I', hdr, 4)[0], struct.unpack_from('<I', hdr, 24)[0]
    out = b''.join(d[k * 0x800:(k + 1) * 0x800] for k in audio_sectors(d))
    return rate, hdr, out[:want]


def video(d):
    """the MPEG-PS video stream: every sector after the header that is not audio"""
    a = set(audio_sectors(d))
    return b''.join(d[k * 0x800:(k + 1) * 0x800] for k in range(1, len(d) // 0x800) if k not in a)


if __name__ == '__main__':
    import sys
    for f in sys.argv[1:]:
        rate, hdr, a = audio(f)
        want = struct.unpack_from('<I', hdr, 24)[0]
        print('%s: rate %d, audio %d of %d bytes, %.1f s' % (f, rate, len(a), want, len(a) / 2 / 16 * 28 / rate))
