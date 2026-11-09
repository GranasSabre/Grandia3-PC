"""Snap subtitle lines to the words actually spoken (whisper word timestamps), after build_subs.py.

usage: snap_subs.py <sub.tsv> <words.tsv> <out.tsv> <report.txt> [--override=<tsv>] [--fix=<tsv>]
words.tsv: key hex, start, end, word (one row per word, keys as in sub.tsv: tag or 0x1000 + movie number).
--override: lines "key start end text" that replace all lines of their key (scenes whose recording differs
from the ADR script, or that are not in it). --fix: lines "ADR text <TAB> subtitle text" (empty: drop the
line) for script notes the OCR took for dialogue and garbled lines. Left-over pause marks (A, AA, AAA
after punctuation) and stray quotes are removed from every line.

A line moves to the place where its words are heard when at least 60 % of its words (and two or more)
are found in order near its script time (plus the shift of the line before), after the words of the line
before, and the shift is over 0.5 s. A match far off from the matches around it is dropped (the same words
in another line). Lines without a match get the shift interpolated between the matched lines around them.
Archive tracks were placed by their sound and keep their script times except for single matched lines
within 2.5 s; movies (placed by order only) follow their words fully; a movie with a single heard line
shifts by it only if it is under 3 s. Movies run at the same speed in the game as in the ADR reel (the
words of the first transcript seemed to drift by 8 %, that was the old mov_audio.py: asr_remap.py fixed
them)."""
import difflib, re, statistics, sys

subp, wordp, outp, repp = [a for a in sys.argv[1:] if not a.startswith('--')][:4]
over = [a[11:] for a in sys.argv[1:] if a.startswith('--override=')]
fixes = {}
for p in [a[6:] for a in sys.argv[1:] if a.startswith('--fix=')]:
    for l in open(p, encoding='utf-8'):
        if l.startswith('#') or '\t' not in l: continue
        a, b = l.rstrip('\n').split('\t', 1)
        fixes[a] = b
SEARCH = 6.0   # seconds around the script time (plus the shift of the line before)
SEARCH_MOVIE = 9.0
MIN_SHIFT = 0.5
TRACK_MAX = 2.5  # largest fix of a line in an archive track (their placement has sound evidence)


def norm(t):
    return re.findall(r"[a-z0-9]+", t.lower().replace("'", ''))


def clean(t):
    """pause marks the OCR read as A/AA/AAA, CSV-doubled and unpaired quotes"""
    t = fixes.get(t, t)
    if t.startswith('"') and t.endswith('"') and '""' in t: t = t[1:-1].replace('""', '"')
    t = re.sub(r'(?<=[.!?,])A{1,3}-?(?=\s|$)', '', t)
    t = re.sub(r'(?<=[.!?,])\s*A{2,3}-?(?=\s|$)', '', t)
    t = re.sub(r'\s+A$', '', t)
    t = re.sub(r'(?<=[.!?,])A{1,3}-(?=\S)', ' ', t)
    t = re.sub(r'^A{2,3}\s+', '', t)
    t = re.sub(r'\s+A{2,3}(?=\s|$)', '', t)
    t = re.sub(r'\bA(?=[A-Z][a-z]|")', '', t)
    if t.count('"') % 2: t = t.replace('"', '')
    t = re.sub(r'\s{2,}', ' ', t).strip()
    return fixes.get(t, t)   # fixes may also name the cleaned text


def same(x, y):
    return x == y or (len(y) >= 4 and difflib.SequenceMatcher(None, x, y).ratio() >= 0.7)


lines = {}
for l in open(subp, encoding='utf-8'):
    if l.startswith('#') or not l.strip(): continue
    k, a, b, t = l.rstrip('\n').split('\t', 3)
    t = clean(t)
    if t: lines.setdefault(int(k, 16), []).append([float(a), float(b), t])
words = {}
for l in open(wordp, encoding='utf-8'):
    k, a, b, w = l.rstrip('\n').split('\t', 3)
    n = norm(w)
    if n: words.setdefault(int(k, 16), []).append((float(a), float(b), n[0]))
for p in over:
    rows = {}
    for l in open(p, encoding='utf-8'):
        if l.startswith('#') or not l.strip(): continue
        k, a, b, t = l.rstrip('\n').split('\t', 3)
        rows.setdefault(int(k, 16), []).append([float(a), float(b), t])
    lines.update(rows)


def match(ws, toks, a, lo, search):
    """best in-order match of toks in ws (word index >= lo) starting near a:
    (score, first word index, last word index, first tok)"""
    best = None
    for i in range(lo, len(ws)):
        s = ws[i][0]
        if s < a - search: continue
        if s > a + search: break
        for j0, t0 in enumerate(toks[:2]):
            if not same(ws[i][2], t0): continue
            hit, last, j, wi = 1, i, j0 + 1, i + 1
            limit = s + 1.0 + 0.6 * len(toks)
            while j < len(toks) and wi < len(ws) and ws[wi][0] <= limit:
                if same(ws[wi][2], toks[j]): hit, last, j = hit + 1, wi, j + 1
                elif j + 1 < len(toks) and same(ws[wi][2], toks[j + 1]): hit, last, j = hit + 1, wi, j + 2
                wi += 1
            sc = (hit / len(toks), -abs(s - a))
            if best is None or sc > best[0]: best = (sc, i, last, j0)
    return best


def line_fit(pts):
    """least squares shift = a + b * t (b = 0 for a single point)"""
    if len(pts) < 2: return (pts[0][1] if pts else 0.0), 0.0
    n = len(pts)
    mt = sum(p[0] for p in pts) / n
    ms = sum(p[1] for p in pts) / n
    var = sum((p[0] - mt) ** 2 for p in pts)
    b = sum((p[0] - mt) * (p[1] - ms) for p in pts) / var if var > 1e-9 else 0.0
    return ms - b * mt, b


def interp(t, pts):
    """shift at script time t from the matched lines (time, shift): linear between neighbours, the line
    through all of them outside"""
    before = [p for p in pts if p[0] <= t]
    after = [p for p in pts if p[0] > t]
    if before and after:
        (t0, s0), (t1, s1) = before[-1], after[0]
        return s0 + (s1 - s0) * (t - t0) / (t1 - t0) if t1 > t0 else s0
    a, b = line_fit(pts)
    edge = before[-1] if before else after[0]
    return edge[1] + b * (t - edge[0])


out, rep, total_moved = [], [], 0
for k in sorted(lines):
    ls = sorted(lines[k])
    ws = words.get(k, [])
    found = {}               # line index -> (start, end) where its words are heard
    lo, last_shift = 0, 0.0
    for n, ln in enumerate(ls):
        toks = [w for w in norm(ln[2]) if len(w) >= 2]
        if len(toks) < 2 or not ws: continue
        m = match(ws, toks, ln[0] + last_shift, lo, SEARCH_MOVIE if k >= 0x1000 else SEARCH)
        if not m or m[0][0] < 0.6 or m[0][0] * len(toks) < 2: continue
        (sc, _), i, last, j0 = m
        st = ws[i][0] - 0.3 * j0
        found[n] = (st, max(ws[last][1] + 0.25, st + 0.8))
        lo, last_shift = last + 1, st - ln[0]
    # a match far off from the matches around it is another line with the same words; in a movie it is
    # measured against the line through the other matches
    movie = k >= 0x1000
    if movie:
        while len(found) >= 3:
            ts = sorted(found)
            res = {}
            for n in ts:
                o = [(ls[m][0], found[m][0] - ls[m][0]) for m in ts if m != n]
                a, b = line_fit(o)
                res[n] = abs(found[n][0] - ls[n][0] - (a + b * ls[n][0]))
            worst = max(res, key=res.get)
            if res[worst] <= 6.0: break
            del found[worst]
    else:
        shift = {n: found[n][0] - ls[n][0] for n in found}
        keys = sorted(shift)
        for x, n in enumerate(keys):
            nb = [shift[m] for m in keys[max(0, x - 3):x] + keys[x + 1:x + 4]]
            if len(nb) >= 2 and abs(shift[n] - statistics.median(nb)) > 3.0: del found[n]
    if not movie:
        # archive tracks were placed by their sound, their script times hold: small fixes of single lines only
        for n in list(found):
            if abs(found[n][0] - ls[n][0]) > TRACK_MAX: del found[n]
    if movie and len(found) == 1:
        # one heard line only: a shift of the whole movie if it is small, otherwise not to be trusted
        n = next(iter(found))
        if abs(found[n][0] - ls[n][0]) > 3.0: del found[n]
    pts = [(ls[n][0], found[n][0] - ls[n][0]) for n in sorted(found)]
    moved = []
    for n, ln in enumerate(ls):
        if n in found:
            st, en = found[n]
        elif movie and pts:
            d = interp(ln[0], pts)
            st, en = ln[0] + d, ln[1] + d
        else:
            continue
        if abs(st - ln[0]) > MIN_SHIFT:
            moved.append((ln[0], st, ln[2], n in found))
            ln[0], ln[1] = st, max(en, st + (ln[1] - ln[0]))
    ls.sort()
    for n, ln in enumerate(ls):
        ln[0] = max(ln[0], 0.0)
        if n + 1 < len(ls) and ln[1] > ls[n + 1][0] - 0.05: ln[1] = max(ln[0] + 0.5, ls[n + 1][0] - 0.05)
        out.append((k, ln[0], ln[1], ln[2]))
    name = 'GRM%02d' % (k - 0x1000) if k >= 0x1000 else '%03x' % k
    total_moved += len(moved)
    sh = [p[1] for p in pts]
    rep.append('%-6s %3d lines, %3d matched, %3d moved, shift %+.2f..%+.2f s%s' % (
        name, len(ls), len(found), len(moved), min(sh) if sh else 0, max(sh) if sh else 0, '' if ws else '  (no transcript)'))
    for a, st, t, m in moved:
        if abs(st - a) > 2.0: rep.append('       %7.2f -> %7.2f %s %s' % (a, st, 'heard ' if m else 'interp', t[:70]))
with open(outp, 'w', encoding='utf-8', newline='\n') as f:
    f.write('# cutscene subtitles: track tag (GR3_STR.STZ), start, end (seconds from the track start), text\n')
    for k, a, b, t in out:
        f.write('%x\t%.2f\t%.2f\t%s\n' % (k, a, b, t))
open(repp, 'w', encoding='utf-8').write('\n'.join(rep) + '\n')
print('%d lines in %d tracks, %d moved to their words' % (len(out), len(lines), total_moved))
