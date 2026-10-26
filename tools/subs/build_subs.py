"""One run from the cleaned ADR script to lang/sub_<language>.tsv - no tests in the game needed.

usage: build_subs.py <part.tsv> <US MUSIC dir> <JP MUSIC dir> <US mdt dir> <first voice tag hex> <out.tsv> <report.txt>

1. Voice tracks = GR3_STR.STZ entries whose US and JP data differ, in tag order from the first tag on.
   Their speech activity (US minus JP removes music and effects) is cached next to the report.
2. Every track is scored against the whole script reel at 10 ms steps (FFT cross-correlation with the
   line intervals). A dynamic programme picks one offset per track so that the tracks follow each other
   in the reel without overlapping (tracks that fit nowhere are skipped).
3. Every script line goes to the track that covers its start time.
4. Only cutscenes without text boxes: a track whose lines are found in the game's own scenario texts
   (they appear in dialogue windows) is left out.
The report lists every track with offset, score and margin, flags uncertain tracks and lines, and the
script lines that no track covers."""
import csv, os, re, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location('al', os.path.join(HERE, 'stz_align_lib.py'))
al = importlib.util.module_from_spec(spec); spec.loader.exec_module(al)
import scnr_text

part, usd, jpd, mdtd, first, outp, repp = sys.argv[1:8]
cache = os.path.join(os.path.dirname(os.path.abspath(repp)), 'cache')
os.makedirs(cache, exist_ok=True)
STEP = 100   # activity samples per second
GAIN = 0.12  # a track placement must score above this to count
MIN_LEN = 8  # cutscene tracks are long; shorter entries are single shouts that fit anywhere


def sec(tc):
    h, m, s, f = (int(x) for x in tc.split(':'))
    return h * 3600 + m * 60 + s + f / 30


def tc(x):
    return '%02d:%02d:%05.2f' % (x // 3600, x % 3600 // 60, x % 60)


# ---- script lines
rows = [r for r in csv.DictReader(open(part, encoding='utf-8'), delimiter='\t') if r['start']]
iv = []          # every timed row (efforts are audible too): (a, b)
spoken = []      # rows with text
for r in rows:
    a = sec(r['start'])
    b = sec(r['end']) if r['end'] else a + 1.0
    if not 0 < b - a < 40: b = a + 1.0
    iv.append((a, b))
    if re.search(r'[A-Za-z]{2}', r['text']):
        spoken.append(dict(i=r['i'], scene=r['scene'], a=a, b=b, end_given=bool(r['end']), text=r['text']))
t0 = min(a for a, b in iv) - 120
t1 = max(b for a, b in iv) + 120
n = int((t1 - t0) * STEP)
T = np.zeros(n)
for a, b in iv:
    T[int((a - t0) * STEP):int((b - t0) * STEP)] = 1
cumT = np.concatenate([[0], np.cumsum(T)])

# ---- voice tracks and their activity
eu = al.entries(os.path.join(usd, 'GR3_STR.IDX')); ej = al.entries(os.path.join(jpd, 'GR3_STR.IDX'))
su, sj = os.path.join(usd, 'GR3_STR.STZ'), os.path.join(jpd, 'GR3_STR.STZ')


def data(stz, e, tag):
    sec_, nxt = e[tag]
    f = open(stz, 'rb'); f.seek(sec_ * 2048); hdr = f.read(0x800)
    return f.read(struct.unpack_from('<I', hdr, 24)[0])


def level(name, pcm, jp, rate):
    """speech level per 10 ms, 0..1 (US minus JP when both have the same length), cached"""
    how = 'us-jp' if jp is not None and abs(len(jp) - len(pcm)) < rate // 10 else 'us'
    x = pcm.astype(np.float64)
    if how == 'us-jp':
        m = min(len(x), len(jp)); x = x[:m]; y = jp[:m].astype(np.float64)
        seg = slice(2000, min(m, rate * 10) - 2000)
        lag = max(range(-300, 301), key=lambda k: np.dot(x[seg], np.roll(y, k)[seg]))
        yl = np.roll(y, lag)
        corr = np.dot(x[seg], yl[seg]) / np.sqrt(np.dot(x[seg], x[seg]) * np.dot(yl[seg], yl[seg]) + 1)
        if corr < 0.4: how = 'us-nomatch'      # the Japanese version has its own mix: no reference
        else: x = x - yl
    win = rate // STEP
    k = len(x) // win
    db = 20 * np.log10(np.sqrt((x[:k * win].reshape(k, win) ** 2).mean(axis=1)) + 1)
    lo, hi = np.percentile(db, [10, 90])
    act = np.clip((db - lo) / max(hi - lo, 1), 0, 1)
    np.savez(os.path.join(cache, name + '.npz'), act=act, how=how)
    return act, how


def cached(name):
    p = os.path.join(cache, name + '.npz')
    if not os.path.exists(p): return None
    z = np.load(p)
    return z['act'], str(z['how'])


def activity(tag):
    if isinstance(tag, str): return movie_activity(tag)
    name = '%03x_c' % tag
    r = cached(name)
    if r: return r
    rate, pcm = al.track(su, eu, tag)
    jp = al.track(sj, ej, tag)[1] if tag in ej else None
    return level(name, pcm, jp, rate)


import mov_audio
mov_us, mov_jp = os.path.join(usd, '..', 'MOVIE'), os.path.join(jpd, '..', 'MOVIE')


def movie_left(path):
    rate, hdr, a = mov_audio.audio(path)
    return rate, al.decode_channel(b''.join(a[i:i + 0x800] for i in range(0, len(a), 0x1000)))


def movie_activity(name):
    r = cached('mov2_' + name)
    if r: return r
    rate, pcm = movie_left(os.path.join(mov_us, name + '.MOV'))
    jp = movie_left(os.path.join(mov_jp, name + '.MOV'))[1] if os.path.exists(os.path.join(mov_jp, name + '.MOV')) else None
    return level('mov2_' + name, pcm, jp, rate)


def scores(act):
    """Pearson correlation of the speech level with the line intervals T for every reel offset: lines
    must fall on speech and the pauses between them on quiet stretches (T is 0/1, so sum T^2 = sum T)"""
    L = len(act)
    m = 1 << int(np.ceil(np.log2(n + L)))
    C = np.fft.irfft(np.fft.rfft(T, m) * np.conj(np.fft.rfft(act, m)), m)[:n - L + 1]
    tw = cumT[L:n + 1] - cumT[:n - L + 1]
    mt = tw / L
    st = np.sqrt(np.maximum(mt - mt * mt, 1e-9))
    return np.where(tw > 0, (C / L - mt * act.mean()) / (st * act.std() + 1e-9), -1)


def candidates(score):
    """best local maxima at least 1 s apart"""
    cand = []
    for o in np.argsort(score)[::-1]:
        if score[o] < GAIN or len(cand) >= 6: break
        if all(abs(o - c) > STEP for c, s in cand): cand.append((int(o), float(score[o])))
    return cand


def score_at(act, o):
    """the Pearson score of one track at reel index o"""
    L = len(act)
    if o < 0 or o + L > n: return -1.0
    w = T[o:o + L]
    if w.sum() == 0: return -1.0
    return float(np.corrcoef(w, act)[0, 1])


# movies: lengths only (their Japanese sound is often a different mix, so US minus JP does not help)
movies = []
for fn in sorted(os.listdir(mov_us)):
    if fn.upper().endswith('.MOV'):
        h = open(os.path.join(mov_us, fn), 'rb').read(64)
        movies.append(dict(tag=fn[:-4].upper(), dur=struct.unpack_from('<I', h, 32)[0] / 60, how='movie', cand=[]))


def hdr_len(tag):
    f = open(su, 'rb'); f.seek(eu[tag][0] * 2048)
    return struct.unpack_from('<I', f.read(64), 32)[0] / 60   # length in 60 Hz frames


tags = [t for t in sorted(eu) if int(first, 16) <= t < 0x800 and hdr_len(t) >= MIN_LEN
        and (t not in ej or data(su, eu, t) != data(sj, ej, t))]
# only as many tracks as this part of the script can hold (the reel also has pauses between scenes)
total, keep = 0.0, []
for t in tags:
    total += hdr_len(t)
    if total > 1.3 * (t1 - t0): break
    keep.append(t)
tags = keep
tracks = []
for k, tag in enumerate(tags):
    act, how = activity(tag)
    L = len(act)
    if L >= n or act.sum() == 0: continue
    score = scores(act)
    tracks.append(dict(tag=tag, dur=L / STEP, how=how, cand=candidates(score), best=float(score.max())))
print(len(tracks), 'tracks scored')

# ---- dynamic programme: one candidate (or none) per track, increasing and non-overlapping
NEG = -1e9
best = []   # per track: list of (total, prev (track, cand) or None) for each candidate
for k, tr in enumerate(tracks):
    row = []
    for o, s in tr['cand']:
        start = t0 + o / STEP
        top, arg = s - GAIN, None
        for j in range(k):
            pj = tracks[j]
            for c, (oj, sj_) in enumerate(pj['cand']):
                if t0 + oj / STEP + pj['dur'] - 2.0 <= start and best[j][c][0] + s - GAIN > top:
                    top, arg = best[j][c][0] + s - GAIN, (j, c)
        row.append((top, arg))
    best.append(row)
end = max(((best[k][c][0], (k, c)) for k in range(len(tracks)) for c in range(len(tracks[k]['cand']))), default=(0, None))[1]
chosen = {}
while end:
    k, c = end
    chosen[k] = c
    end = best[k][c][1]

# ---- texts of the game's own dialogue windows (US scenario scripts)
norm = lambda s: re.sub(r'[^a-z]', '', s.lower())
game = []
for fn in os.listdir(mdtd):
    d = open(os.path.join(mdtd, fn), 'rb').read()
    s = scnr_text.script(d)
    if s:
        game += [norm(scnr_text.dec_en(raw)) for off, raw in scnr_text.texts(s, False)]
game_all = '|'.join(game)

# ---- chain: the studio reel is the scene videos one after another with 10 s between them
GAP = 10.0
TOL = 0.8      # measured pauses: 9.75 .. 10.64 s
scene_of = {}
for r in rows:
    if r['start']:
        a = sec(r['start'])
        b = sec(r['end']) if r['end'] else a + 1
        if not 0 < b - a < 40: b = a + 1           # misread end time
        lo, hi = scene_of.get(r['scene'] or 'OPEN', (1e9, 0))
        scene_of[r['scene'] or 'OPEN'] = (min(lo, a), max(hi, b))


def fits(st, d):
    """the item [st, st+d] holds spoken lines, cuts no scene of the script in two, and the 10 s pauses
    before and after it are free of lines"""
    if not any(st <= l['a'] < st + d for l in spoken): return False
    if any(st - GAP + 0.5 < a < st - 0.3 or st + d + 0.3 < a < st + d + GAP - 0.5 for a, b in iv): return False
    for lo, hi in scene_of.values():
        inside = st - 0.5 <= lo <= st + d + 0.5 or st - 0.5 <= hi <= st + d + 0.5
        if inside and not (st - 0.5 <= lo and hi <= st + d + 0.5): return False
    return True


# seeds: tracks the programme above placed with a clear score
items = []     # dict(st, tr, s, why)
for k in sorted(chosen):
    tr = tracks[k]; o, s = tr['cand'][chosen[k]]
    others = [sc for oc, sc in tr['cand'] if abs(oc - o) > STEP]
    if s >= 0.5 and s - (max(others) if others else 0) >= 0.05 and fits(t0 + o / STEP, tr['dur']):
        items.append(dict(st=t0 + o / STEP, tr=tr, s=s, why='score'))


def free(st, d):
    """no overlap, and the distance to every placed neighbour is the 10 s pause or room for another
    video in between (at least two pauses and a short scene)"""
    for it in items:
        g = it['st'] - (st + d) if it['st'] >= st else st - (it['st'] + it['tr']['dur'])
        if g < GAP - TOL or (GAP + TOL < g < 2 * GAP + MIN_LEN): return False
    return True


def order_ok(tag, st):
    """Movies come in number order, skipping at most a few that are not in this reel. (Archive tracks
    are only roughly in tag order - 0x5c plays between 0x5a and 0x5b - and their sound decides.)"""
    if isinstance(tag, int): return True
    num = lambda t: int(re.sub(r'\D', '', t)) if isinstance(t, str) else None
    me = num(tag)
    before = [num(it['tr']['tag']) for it in items if it['st'] < st and num(it['tr']['tag']) is not None]
    after = [num(it['tr']['tag']) for it in items if it['st'] > st and num(it['tr']['tag']) is not None]
    return me > max(before, default=0) and me <= max(before, default=0) + 3 and me < min(after, default=1000)


def need(tag, st):
    """score a track needs here: tracks come roughly in tag order, so one whose tag is far from the
    placed tracks around it needs much stronger sound evidence"""
    trk = sorted((it['st'], it['tr']['tag']) for it in items if isinstance(it['tr']['tag'], int))
    before = [t for s, t in trk if s < st][-1:]
    after = [t for s, t in trk if s > st][:1]
    lo = (before[0] if before else 0) - 4
    hi = (after[0] if after else 0x800) + 4
    return 0.4 if lo <= tag <= hi else 0.6


def try_place(st, forward, prev_item):
    """the next item after (forward) or before prev_item, starting (ending) at st"""
    used = {id(it['tr']) for it in items}
    best = None
    for tr in tracks:
        if id(tr) in used: continue
        s0 = st if forward else st - tr['dur']
        if not order_ok(tr['tag'], s0): continue
        act, how = activity(tr['tag'])
        o0 = int(round((s0 - t0) * STEP))
        sc, o = max((score_at(act, o0 + d), o0 + d) for d in range(-60, 61, 2))
        s1 = t0 + o / STEP
        if sc >= need(tr['tag'], s1) and fits(s1, tr['dur']) and free(s1, tr['dur']) and (best is None or sc > best['s']):
            best = dict(st=s1, tr=tr, s=sc, why='chain+score')
    if best: return best
    # Movies: even where both versions share the mix, too much music stays in US minus JP, so only the
    # 10 s chain, the scene borders and the number order speak for them; the lowest fitting number wins
    for mv in movies:
        if id(mv) in used: continue
        s1 = st if forward else st - mv['dur']
        if order_ok(mv['tag'], s1) and fits(s1, mv['dur']) and free(s1, mv['dur']):
            return dict(st=s1, tr=mv, s=0.25, why='chain')
    return None


# best first: of all possible extensions the one with the strongest evidence goes in, then again
while True:
    cands = []
    for it in items:
        for forward in (True, False):
            st = it['st'] + it['tr']['dur'] + GAP if forward else it['st'] - GAP
            if any(abs((o['st'] if forward else o['st'] + o['tr']['dur']) - st) < TOL for o in items): continue
            c = try_place(st, forward, it)
            if c: cands.append(c)
    if not cands: break
    items.append(max(cands, key=lambda c: (c['s'], -c['st'])))   # ties: earliest first
items.sort(key=lambda x: x['st'])
# ---- close gaps: a stretch between two placed items that still holds lines must be one or two videos
# that fit exactly, pauses included. Any archive entry may fill it, also one that is the same in both
# versions (then only music is in it and the voices come another way; no sound check is possible, so it
# must fit within 0.15 s).
ALL = {t: hdr_len(t) for t in eu if t < 0x800 and hdr_len(t) >= MIN_LEN}


def evidence(tag, st):
    """sound score of an archive entry at st, or None when US and JP are identical"""
    if tag in ej and data(su, eu, tag) == data(sj, ej, tag): return None
    act, how = activity(tag)
    o0 = int(round((st - t0) * STEP))
    sc = [score_at(act, o0 + d) for d in range(-80, 81, 2)]
    sc = [x for x in sc if x == x]
    return max(sc) if sc else None


def piece_ok(tag, st, d, err):
    if not fits(st, d): return None
    e = evidence(tag, st)
    if e is None: return 0.3 if err <= 0.15 else None
    return e if e >= 0.3 else None


items.sort(key=lambda x: x['st'])
used_tags = {it['tr']['tag'] for it in items}
for a, b in list(zip(items, items[1:])):
    lo, hi = a['st'] + a['tr']['dur'] + GAP, b['st'] - GAP
    if hi - lo < MIN_LEN or not any(lo <= l['a'] < hi for l in spoken): continue
    best = None
    for t, d in ALL.items():                         # one video
        err = abs(d - (hi - lo))
        if t not in used_tags and err <= TOL:
            s = piece_ok(t, lo, d, err)
            if s is not None and (best is None or s > best[0]): best = (s, [(t, lo, d)])
    for t1, d1 in ALL.items():                       # two videos with a pause between them
        for t2, d2 in ALL.items():
            err = abs(d1 + GAP + d2 - (hi - lo))
            if t1 == t2 or t1 in used_tags or t2 in used_tags or err > TOL: continue
            s1 = piece_ok(t1, lo, d1, err)
            s2 = piece_ok(t2, lo + d1 + GAP, d2, err) if s1 is not None else None
            if s2 is not None and (best is None or min(s1, s2) > best[0]): best = (min(s1, s2), [(t1, lo, d1), (t2, lo + d1 + GAP, d2)])
    if best:
        for t, st, d in best[1]:
            items.append(dict(st=st, tr=dict(tag=t, dur=d, how='gap', cand=[]), s=best[0], why='gap'))
            used_tags.add(t)
items.sort(key=lambda x: x['st'])

# confirmed: linked to a neighbour by the 10 s gap
chain = set()
for i, it in enumerate(items):
    if i and abs(it['st'] - (items[i - 1]['st'] + items[i - 1]['tr']['dur']) - GAP) <= TOL: chain.update((i, i - 1))
placed = [(it['st'], it['tr'], it['s'], 0.0) for it in items]


def key(tag):
    """runtime key: archive tag, or 0x1000 + movie number for MOVIE\\GRMnn.MOV"""
    return tag if isinstance(tag, int) else 0x1000 + int(re.sub(r'\D', '', tag))


out, rep, covered = [], [], set()
for pi, (start, tr, s, margin) in enumerate(placed):
    ls = [l for l in spoken if start <= l['a'] < start + tr['dur']]
    long_ = [l for l in ls if len(norm(l['text'])) >= 15]
    boxed = [l for l in long_ if norm(l['text']) in game_all]
    flags = ['CHAIN' if pi in chain else 'not confirmed (left out)']
    if tr['how'] == 'us': flags.append('no-JP-reference')
    drop = long_ and len(boxed) * 2 >= len(long_)
    if drop: flags.append('TEXTBOX (left out)')
    scenes = sorted({l['scene'] for l in ls})
    rep.append('%-5s  %6.1f s  reel %s  score %.2f  margin %.2f  %2d lines  %-14s %s'
               % (tr['tag'] if isinstance(tr['tag'], str) else '%03x' % tr['tag'], tr['dur'], tc(start), s, margin,
                  len(ls), ','.join(scenes), ' '.join(flags)))
    if drop or pi not in chain: continue
    for l in ls: covered.add(l['i'])
    act, how = activity(tr['tag'])
    ls.sort(key=lambda l: l['a'])
    for j, l in enumerate(ls):
        a = l['a'] - start
        b = max(l['b'] - start, a + min(4.0, 1.2 + 0.045 * len(l['text'])))
        if j + 1 < len(ls): b = min(b, max(a + 0.5, ls[j + 1]['a'] - start - 0.05))
        seg = act[int(l['a'] - start) * STEP if False else int(a * STEP):int((l['b'] - start) * STEP) or int(a * STEP) + STEP]
        hit = float(seg.mean()) if len(seg) else 0
        if hit < 0.25: rep.append('       line %s: little speech under it (%.0f%%): %s' % (l['i'], hit * 100, l['text'][:60]))
        out.append((key(tr['tag']), a, b, l['text']))
rep.append('')
rep.append('script lines no track covers:')
rep += ['  %s %s %s %s' % (l['i'], l['scene'], tc(l['a']), l['text'][:70]) for l in spoken if l['i'] not in covered]

with open(outp, 'w', encoding='utf-8', newline='\n') as f:
    f.write('# cutscene subtitles: track tag (GR3_STR.STZ), start, end (seconds from the track start), text\n')
    for tag, a, b, t in out:
        f.write('%x\t%.2f\t%.2f\t%s\n' % (tag, a, b, t))
open(repp, 'w', encoding='utf-8').write('\n'.join(rep) + '\n')
print('%d subtitle lines in %d tracks, %d of %d script lines without subtitle, %d placements not confirmed'
      % (len(out), len(placed), len(spoken) - len(covered), len(spoken), sum('not confirmed' in r for r in rep)))
