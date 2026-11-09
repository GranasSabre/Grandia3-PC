# asr_remap.py <asr_words.tsv> <out.tsv> <extracted disc dir with disc1/ and disc2/>: movie word times from
# the old (wrong) sound extraction to the real movie time. Run once, on the first whisper transcript. The old
# extraction took every sector outside the MPEG pack runs as audio and decoded every
# other 0x800 chunk as the left channel; its sample k*3584 + i is file sector old[2k] at sample i.
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mov_audio
G = sys.argv[3] + '/'
CH = 3584  # samples per 0x800 chunk


def old_sectors(d):
    want = struct.unpack_from('<I', d, 24)[0]
    out = []
    p = 0x800
    while p < len(d) and len(out) * 0x800 < want:
        if d[p:p + 4] == b'\x00\x00\x01\xba':
            p = (mov_audio.pack_end(d, p) + 0x7FF) & ~0x7FF
        else:
            out.append(p // 0x800); p += 0x800
    return out


maps = {}
def mapper(key):
    if key not in maps:
        n = key - 0x1000
        d = open(G + 'disc%d/MOVIE/GRM%02d.MOV' % (2 if n >= 50 else 1, n), 'rb').read()
        old = old_sectors(d)
        new = {s: j for j, s in enumerate(mov_audio.audio_sectors(d))}
        rate = struct.unpack_from('<I', d, 4)[0]
        maps[key] = (old, new, rate)
    return maps[key]


def remap(key, t, side):
    """real time of old time t; side +1 moves out of a video chunk forwards, -1 backwards"""
    old, new, rate = mapper(key)
    x = t * rate
    k = int(x // CH)
    for step in range(12):
        kk = k + side * step
        if 0 <= 2 * kk < len(old) and old[2 * kk] in new:
            j = new[old[2 * kk]] // 2
            if step == 0: return (j * CH + x - k * CH) / rate
            return (j * CH + (0 if side > 0 else CH)) / rate
    return None


rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1], encoding='utf-8') if l.strip()]
out, dropped, moved = [], 0, {}
for k, a, b, w in rows:
    key = int(k, 16)
    if key < 0x1000: out.append((k, a, b, w)); continue
    na, nb = remap(key, float(a), +1), remap(key, float(b), -1)
    if na is None or nb is None or nb < na: dropped += 1; continue
    moved.setdefault(k, []).append(float(a) - na)
    out.append((k, '%.2f' % na, '%.2f' % nb, w))
with open(sys.argv[2], 'w', encoding='utf-8', newline='\n') as f:
    for r in out: f.write('\t'.join(r) + '\n')
print('dropped', dropped)
for k, v in sorted(moved.items()): print('%s: %d words, earlier by %.2f .. %.2f s' % (k, len(v), min(v), max(v)))
