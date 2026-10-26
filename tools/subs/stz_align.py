"""Align ADR script lines (reel timecodes) with a GR3_STR.STZ speech track.
usage: stz_align.py <GR3_STR.IDX> <GR3_STR.STZ> <part.tsv> <scene id> <tag hex> [more scene:tag pairs]
Decodes the track (PS-ADPCM, stereo, 0x800 interleave after a 0x800 header), builds a 10 ms speech
activity curve and finds the offset (reel time at the start of the track) where the script lines fall on
speech. Prints offset, score and the margin to the next best offset."""
import csv, struct, sys
import numpy as np

F0 = (0, 60, 115, 98, 122)
F1 = (0, 0, -52, -55, -60)


def entries(idxp):
    d = open(idxp, 'rb').read()
    v = struct.unpack('<%dI' % (len(d) // 4), d)
    e = sorted((x & 0xFFFFF, x >> 20) for i, x in enumerate(v) if (x >> 20) != 0xFFF and ((x & 0xFFFFF) or i == 0))
    return {tag: (sec, (e[k + 1][0] if k + 1 < len(e) else None)) for k, (sec, tag) in enumerate(e)}


def decode_channel(buf):
    out = np.zeros(len(buf) // 16 * 28, np.int32)
    s1 = s2 = 0
    o = 0
    for b in range(0, len(buf) - 15, 16):
        shift, filt = buf[b] & 15, min(buf[b] >> 4, 4)
        f0, f1 = F0[filt], F1[filt]
        for j in range(14):
            byte = buf[b + 2 + j]
            for nib in (byte & 15, byte >> 4):
                s = (nib << 12) & 0xFFFF
                if s & 0x8000: s -= 0x10000
                s = (s >> shift) + ((s1 * f0 + s2 * f1 + 32) >> 6)
                s = max(-32768, min(32767, s))
                out[o] = s; o += 1
                s2, s1 = s1, s
    return out


def track(stz, ents, tag):
    if tag not in ents: return None, None
    sec, nxt = ents[tag]
    f = open(stz, 'rb')
    f.seek(sec * 2048)
    hdr = f.read(0x800)
    rate, data, samples = struct.unpack_from('<I', hdr, 4)[0], struct.unpack_from('<I', hdr, 24)[0], struct.unpack_from('<I', hdr, 28)[0]
    raw = f.read(data)
    left = b''.join(raw[i:i + 0x800] for i in range(0, len(raw), 0x1000))
    pcm = decode_channel(left)[:samples]
    return rate, pcm


def activity(pcm, rate, jp=None):
    # The tracks are full mixes (music, effects, voice). The Japanese track has the same mix with other
    # voices, so US minus JP leaves mostly the voices of both - which are timed to the same picture.
    x = pcm.astype(np.float64)
    if jp is not None and abs(len(jp) - len(pcm)) < rate // 10:
        n = min(len(x), len(jp)); y = jp[:n].astype(np.float64); x = x[:n]
        seg = slice(2000, min(n, 240000) - 2000)
        lag = max(range(-200, 201), key=lambda k: np.dot(x[seg], np.roll(y, k)[seg]))
        x = x - np.roll(y, lag)
    win = rate // 100
    n = len(x) // win
    db = 20 * np.log10(np.sqrt((x[:n * win].reshape(n, win) ** 2).mean(axis=1)) + 1)
    if jp is not None:
        return (db > np.percentile(db, 10) + 18).astype(np.float64)
    return (db > np.percentile(db, 10) + 15).astype(np.float64)


def sec_of(tc):
    h, m, s, f = (int(x) for x in tc.split(':'))
    return h * 3600 + m * 60 + s + f / 30


def lines(part, scene):
    out = []
    for r in csv.DictReader(open(part, encoding='utf-8'), delimiter='\t'):
        if r['scene'] == scene and r['start']:
            a = sec_of(r['start'])
            b = sec_of(r['end']) if r['end'] else a + 1.0
            out.append((a, b, r['text']))
    return out


def align(act, ls):
    n = len(act)
    first = min(a for a, b, t in ls)
    best = []
    # offset = reel time at track start; search from 60 s before the first line to the first line
    for k in range(0, 6000):
        o = first - k / 100
        tgt = np.zeros(n)
        for a, b, t in ls:
            i, j = int((a - o) * 100), int((b - o) * 100)
            if j <= 0 or i >= n: continue
            tgt[max(0, i):min(n, j)] = 1
        if not tgt.any(): continue
        hit = (tgt * act).sum() / tgt.sum()                   # share of line time that is speech
        miss = ((1 - tgt) * act).sum() / max(1, act.sum())    # share of speech outside any line
        best.append((hit - miss, o))
    best.sort(reverse=True)
    top = best[0]
    second = next((s for s in best if abs(s[1] - top[1]) > 1.0), (0, 0))
    return top, second


args = sys.argv[1:]
jp = None
if args[0] == '--jp':
    jp = (entries(args[1]), args[2]); args = args[3:]
idxp, stz, part = args[:3]
ents = entries(idxp)
for pair in args[3:]:
    scene, tag = pair.split(':')
    rate, pcm = track(stz, ents, int(tag, 16))
    jpcm = track(jp[1], jp[0], int(tag, 16))[1] if jp else None
    act = activity(pcm, rate, jpcm)
    ls = lines(part, scene)
    (s1, o1), (s2, o2) = align(act, ls)
    h = int(o1 // 3600); m = int(o1 % 3600 // 60); s = o1 % 60
    print('%-7s tag %s: %5.1f s audio, %2d lines, speech %3.0f%%, offset %02d:%02d:%05.2f  score %.2f (next best %.2f)'
          % (scene, tag, len(pcm) / rate, len(ls), act.mean() * 100, h, m, s, s1, s2))
