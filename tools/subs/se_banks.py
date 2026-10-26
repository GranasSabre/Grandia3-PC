"""Sound banks MUSIC\\6xxx.SE: header (u32 ?, u32 0, u32 count, count sound ids), at 0x800 a table of
count 16-byte entries (u32 offset from 0x800, u16 pitch, ...), then the PS-ADPCM samples.
Voice samples are those that differ between the US and the JP bank."""
import os, struct


def parse(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 8)[0]
    if not 0 < n < 500 or len(d) < 0x800 + 16 * n: return None
    ids = list(struct.unpack_from('<%dI' % n, d, 12))
    ent = [struct.unpack_from('<IH', d, 0x800 + 16 * i) for i in range(n)]
    out = []
    for i, (off, pitch) in enumerate(ent):
        a = 0x800 + off
        b = 0x800 + ent[i + 1][0] if i + 1 < n else len(d)
        rate = pitch * 48000 / 4096 if pitch else 22050
        out.append(dict(index=i, id=ids[i], start=a, size=max(0, b - a), rate=rate,
                        dur=max(0, b - a) / 16 * 28 / rate, data=d[a:b]))
    return out


def voice_samples(us_dir, jp_dir):
    """{bank number: [samples]} with 'voice' True where the JP bank has a different sample"""
    banks = {}
    for fn in sorted(os.listdir(us_dir)):
        if not fn.endswith('.SE'): continue
        u = parse(os.path.join(us_dir, fn))
        j = parse(os.path.join(jp_dir, fn)) if os.path.exists(os.path.join(jp_dir, fn)) else None
        if not u: continue
        jd = {s['index']: s for s in (j or [])}
        for s in u:
            o = jd.get(s['index'])
            s['voice'] = o is None or o['data'] != s['data']
            s['jp_dur'] = o['dur'] if o else 0
        banks[int(fn[:-3])] = u
    return banks


if __name__ == '__main__':
    import sys
    b = voice_samples(sys.argv[1], sys.argv[2])
    nv = sum(s['voice'] for v in b.values() for s in v)
    print(len(b), 'banks,', sum(len(v) for v in b.values()), 'samples,', nv, 'voice samples')
    for k in sorted(b)[:int(sys.argv[3]) if len(sys.argv) > 3 else 12]:
        v = [s for s in b[k] if s['voice']]
        print(k, len(b[k]), 'samples', len(v), 'voice:', ' '.join('%.1f' % s['dur'] for s in v[:40]))
