"""Build lang/sub_<language>.tsv for the runtime (platform/subtitles.cpp).
usage: make_subs.py <part.tsv> <scenes.tsv> <out.tsv> [scene ...]
scenes.tsv (from stz_map.py): scene, tag, offset = reel time at the start of the voice track.
Only the scenes named on the command line are written (all when none are named).
Output lines: tag (hex) TAB start TAB end TAB text, seconds from the start of the track."""
import csv, re, sys

part, scenes, out = sys.argv[1:4]
only = set(sys.argv[4:])


def sec(tc):
    h, m, s, f = (int(x) for x in tc.split(':'))
    return h * 3600 + m * 60 + s + f / 30


track = {r['scene']: (r['tag'], float(r['offset'])) for r in csv.DictReader(open(scenes, encoding='utf-8'), delimiter='\t')}
rows = [r for r in csv.DictReader(open(part, encoding='utf-8'), delimiter='\t')
        if r['scene'] in track and (not only or r['scene'] in only) and r['start'] and re.search(r'[A-Za-z]{2}', r['text'])]
lines = []
for r in rows:
    tag, off = track[r['scene']]
    a = sec(r['start']) - off
    # script end times mark the end of the recorded loop; give short lines enough reading time
    b = sec(r['end']) - off if r['end'] else a
    b = max(b, a + min(4.0, 1.2 + 0.045 * len(r['text'])))
    lines.append([tag, a, b, r['text']])
lines.sort(key=lambda l: (int(l[0], 16), l[1]))
for cur, nxt in zip(lines, lines[1:]):
    if cur[0] == nxt[0] and cur[2] > nxt[1] - 0.05:
        cur[2] = max(cur[1] + 0.5, nxt[1] - 0.05)     # never overlap the next line
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('# cutscene subtitles: track tag (GR3_STR.STZ), start, end (seconds from the track start), text\n')
    for tag, a, b, t in lines:
        f.write('%s\t%.2f\t%.2f\t%s\n' % (tag, a, b, t))
print(len(lines), 'lines')
