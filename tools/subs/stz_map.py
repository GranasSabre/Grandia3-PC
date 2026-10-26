"""Map the scenes of an ADR script part to GR3_STR.STZ voice tracks, in order, and align them.
usage: stz_map.py <US dir with GR3_STR.IDX/.STZ> <JP dir> <part.tsv> <first voice tag hex> <out.tsv>
Voice tracks = entries whose US and JP data differ. For every scene (in script order) the next few
voice tracks are tried; the best alignment score wins. Output: scene, tag, offset (reel seconds at the
track start), score, runner-up score."""
import csv, os, struct, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
spec = importlib.util.spec_from_file_location('al', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'stz_align_lib.py'))
al = importlib.util.module_from_spec(spec); spec.loader.exec_module(al)

us, jp, part, first, outp = sys.argv[1:6]
eu = al.entries(os.path.join(us, 'GR3_STR.IDX')); ej = al.entries(os.path.join(jp, 'GR3_STR.IDX'))
su, sj = os.path.join(us, 'GR3_STR.STZ'), os.path.join(jp, 'GR3_STR.STZ')


def blob(stz, e, tag):
    sec, nxt = e[tag]
    f = open(stz, 'rb'); f.seek(sec * 2048); hdr = f.read(0x800)
    return f.read(struct.unpack_from('<I', hdr, 24)[0])


voice = [t for t in sorted(eu) if t < 0x800 and t in ej and blob(su, eu, t) != blob(sj, ej, t)]
scenes = []
for r in csv.DictReader(open(part, encoding='utf-8'), delimiter='\t'):
    if r['scene'] and r['scene'] not in scenes: scenes.append(r['scene'])
cache = {}


def act(tag):
    if tag not in cache:
        rate, pcm = al.track(su, eu, tag)
        cache[tag] = al.activity(pcm, rate, al.track(sj, ej, tag)[1])
    return cache[tag]


pos = voice.index(int(first, 16))
rows = []
for sc in scenes:
    ls = al.lines(part, sc)
    if not ls: continue
    span = max(b for a, b, t in ls) - min(a for a, b, t in ls)
    cands = []
    for tag in voice[pos:pos + 4]:
        a = act(tag)
        if span > len(a) / 100 + 2: continue          # the lines do not fit into this track
        (s1, o1), (s2, o2) = al.align(a, ls)
        cands.append((s1, tag, o1, s2))
    if not cands:
        print('%-8s no fitting track' % sc); continue
    s1, tag, o1, s2 = max(cands)
    pos = voice.index(tag) + 1
    rows.append((sc, '%x' % tag, '%.2f' % o1, '%.2f' % s1, '%.2f' % s2))
    print('%-8s tag %3x  %5.1f s  offset %.2f  score %.2f (runner-up %.2f)  lines %d'
          % (sc, tag, len(act(tag)) / 100, o1, s1, s2, len(ls)), flush=True)
with open(outp, 'w', encoding='utf-8', newline='') as f:
    w = csv.writer(f, delimiter='\t'); w.writerow(['scene', 'tag', 'offset', 'score', 'runner_up'])
    w.writerows(rows)
