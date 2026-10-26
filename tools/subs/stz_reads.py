"""Which GR3_STR.STZ entries does the game read, and when? (from grandia3.log with IOP sceCdRead lines)
usage: stz_reads.py <grandia3.log> <GR3_STR.IDX> <stz lsn> [from_sec] [to_sec]"""
import re, struct, sys

log, idxp, base = sys.argv[1], sys.argv[2], int(sys.argv[3])
t0 = float(sys.argv[4]) if len(sys.argv) > 4 else 0
t1 = float(sys.argv[5]) if len(sys.argv) > 5 else 1e9
d = open(idxp, 'rb').read()
ents = []
for i, x in enumerate(struct.unpack('<%dI' % (len(d) // 4), d)):
    tag, sec = x >> 20, x & 0xFFFFF
    if tag != 0xFFF and (sec or i == 0):
        ents.append((sec, tag, i))
ents.sort()
secs = [e[0] for e in ents]

import bisect
cur = None
for line in open(log, encoding='utf-8', errors='replace'):
    m = re.match(r'\[\s*([\d.]+)\]\[cdvd\] IOP sceCdRead lsn=(\d+) n=(\d+)', line)
    if m:
        t, lsn, n = float(m.group(1)), int(m.group(2)), int(m.group(3))
        if not t0 <= t <= t1: continue
        off = lsn - base
        if 0 <= off < 289982:
            k = bisect.bisect_right(secs, off) - 1
            sec, tag, i = ents[k]
            key = (i, tag)
            if key != cur:
                print('%9.3f  STZ entry #%d tag 0x%03x  (+%d sectors into it)' % (t, i, tag, off - sec))
                cur = key
        else:
            if cur != 'other':
                print('%9.3f  other lsn %d' % (t, lsn))
                cur = 'other'
    m = re.match(r'\[\s*([\d.]+)\]\[spu\] kon c0 v0([89])', line)
    if m and t0 <= float(m.group(1)) <= t1 and m.group(2) == '8':
        print('%9.3f  speech voices key on' % float(m.group(1)))
