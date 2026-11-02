"""Turn the raw OCR table into subtitle lines.
usage: vo_clean.py <raw.tsv> <out.tsv> [fixes.tsv ...]
fixes: <row><TAB><text> replaces the text of a row (empty text = no subtitle), a row number alone = no
subtitle, <row><TAB>@<start> [<end>] replaces the time codes (script errors: end before start, 10:.. for
01:..), "page <n>" leaves out a page of the scan (a page scanned twice)"""
import csv, re, sys
from collections import Counter

FIX, TCFIX, DROP = {}, {}, set()
for fp in sys.argv[3:]:
    for l in open(fp, encoding='utf-8'):
        l = l.rstrip('\n')
        if re.fullmatch(r'page \d+', l.strip()):
            DROP.add(l.split()[1])
        elif '\t' in l:
            k, v = l.split('\t', 1)
            if v.startswith('@'): TCFIX[k] = v[1:].split()
            else: FIX[k] = v.strip()
        elif l.strip().isdigit():
            FIX[l.strip()] = ''           # row number alone: no subtitle

rows = [r for r in csv.DictReader(open(sys.argv[1], encoding='utf-8'), delimiter='\t') if r['page'] not in DROP]

D = '[0-9OoIlT]'
TC = re.compile(r'(%s{2})[:.;,]\s?(%s{2})[:.;,]\s?(%s{2})[:.;,]\s?(%s{2})' % (D, D, D, D))
ALT = re.compile(r'\bA[Ll1][TtY�][^:]{0,30}:\s*')
OK = {'i', 'a', 'oh', 'uh', 'ah', 'hm', 'no', 'ok', 'eh', 'mm', 'ha', 'so', 'go', 'me', 'us', 'it', 'is', 'up',
      'we', 'he', 'do', 'to', 'my', 'hi', 'yo', 'aw', 'um', 'er', 'oi', 'ow', 'on', 'in', 'at', 'of', 'im', 'if',
      'id', 'or', 'an', 'as', 'be', 'by', 'ya'}


def digits(s):
    for a, b in (('O', '0'), ('o', '0'), ('I', '1'), ('l', '1'), ('T', '1')):
        s = s.replace(a, b)
    return s


def tcs(s):
    t = ['%s:%s:%s:%s' % tuple(digits(g) for g in m.groups()) for m in TC.finditer(s)]
    if len(t) < 2:
        # both time codes run together and wrap ("05:00:47:1505:00: | 48:05")
        t2 = tcs(re.sub(r'\s*\|\s*|\s+', '', s)) if re.search(r'\s', s) else t
        if len(t2) > len(t): t = t2
    return t


def sec(x):
    return int(x[0:2]) * 3600 + int(x[3:5]) * 60 + int(x[6:8]) + int(x[9:11]) / 30


def speaker(s):
    # pen marks in the column come out as stray short tokens ("Yuki J", "Alfina o")
    s = re.sub(r'\([^)]*\)', lambda m: m.group(0).replace(' ', '_'), s.replace('/', ' '))
    keep = []
    for tok in s.split():
        if tok.startswith('(') or re.fullmatch(r'[A-Z][a-z\-]{2,}|[A-Z]{3,}|-|with|of|the|and', tok) \
                or (re.fullmatch(r'[A-F]', tok) and keep):
            keep.append(tok.replace('_', ' '))
    return ' '.join(keep)


def junk(w):
    letters = re.sub(r'[^A-Za-z]', '', w)
    return not letters or (len(letters) <= 2 and letters.lower() not in OK)


def clean(t):
    t = t.replace('|', 'I').replace('{', '(').replace('�', '').replace('_', ' ')
    for a, b in (('’', "'"), ('‘', "'"), ('“', '"'), ('”', '"')):
        t = t.replace(a, b)
    t = re.sub(r'\[[^\]]*\]?', ' ', t)                       # [Not in translation.] notes
    t = re.sub(r'\([^)]*\)?', ' ', t)                        # (mns), (clm sigh) ... stage directions
    t = re.sub(r'^[^.!?"]*?\)', ' ', t)                      # direction whose "(" was misread
    t = re.sub(r'-\s*(AA|4A|\^\^?)(?=\s|$)', '--', t)        # "now-^^" = broken off
    t = re.sub(r'(?<!\S)(AA|4A|AN|\^+)(?!\S)', ' ', t)        # ^^ pause mark
    t = re.sub(r'(?<!\S)A(?=\s+(I\b|[A-Z]))', ' ', t)         # ^ pause mark read as "A" before a capital
    t = re.sub(r'(?<=[!?.,\-])\s+[4^]+(?!\S)', ' ', t)        # ^ read as 4
    t = re.sub(r"(?<!\S)['\"](?!\S)", ' ', t)                # ' = short pause mark
    # the later scans print "..." as raised dots, read as ":::", "-:-", "::-" or "---"
    t = re.sub(r'(?<!\d)[\-:;.]*[:;][\-:;.]*(?!\d)|-{3,}', '...', t)
    t = re.sub(r'\.{4,}', '...', t)
    t = re.sub(r'(?<!\S)A(?=A?[A-Z][a-z])|(?<!\S)A\'(?=[A-Z])', '', t)   # ^ glued to the next word ("AAYou")
    t = re.sub(r'(?<=[a-z,\-.])\s*A\s+(?=[a-z])', ' ', t)    # ^ in the middle of a sentence ("are- A fake")
    t = re.sub(r'(?<!\S)4(?!\S)', ' ', t)                    # ^ read as 4
    t = re.sub(r'\s+A\s*$', '', t)
    # words joined by the rule of a struck version ("of-course-not.")
    t = ' '.join(w for w in t.split() if not re.search(r'[a-z]{2,}-[a-z]{2,}-\S', w) or w == 'up-and-up.')
    for a, b in (("!'m", "I'm"), ("'l ", "I "), (' ls ', ' Is '), ("i'll", "I'll"), ('_', ' ')):
        t = t.replace(a, b)
    t = re.sub(r'^ls ', 'Is ', t)
    t = re.sub(r'(?<!\S)l([tf])(?!\S)', r'I\1', t)
    toks = t.split()
    while toks and junk(toks[-1]): toks.pop()
    while toks and junk(toks[0]): toks.pop(0)
    return ' '.join(toks)


# every reel of the script lies within one hour (01:.. for part 1): a misread hour ("93:22:34:00") is that hour
HOUR = Counter(x[:2] for r in rows for x in tcs(r['tc'])).most_common(1)[0][0]

out, scene = [], ''
for i, r in enumerate(rows):
    key = str(i + 1)
    sc = digits(r['scene'].replace(' ', ''))
    m = re.search(r'(\d\d#\d\d[A-Z]?)', sc)
    if m: scene = m.group(1)
    t = tcs(r['tc'])
    lines = [l for l in r['text'].split(' / ') if l.strip()]
    main, alt = [], []
    flags = set(f for f in r['flags'].split(',') if f and f not in ('lowconf', 'bg'))
    if key in TCFIX:
        t = TCFIX[key]
    elif any(x[:2] != HOUR for x in t):
        t = [HOUR + x[2:] for x in t]; flags.add('hourfix')
    cur, struck, prev = main, False, ''
    for l in lines:
        # a struck line, or the continuation of a struck sentence (the line before did not end it)
        if l.startswith('-') or (struck and (l.startswith('?') or not re.search(r'[.!?"]\W*$', prev))):
            struck, prev = True, l
            continue
        struck, prev = False, l
        if l.startswith('?'):
            flags.add('unclear'); l = l[1:]
        mm = ALT.search(l)
        if mm:
            cur.append(l[:mm.start()]); cur = alt; l = l[mm.end():]
        cur.append(l)
    text, alt_text = clean(' '.join(main)), clean(' '.join(alt))
    if any(l.startswith('-') for l in lines):
        # tail of a struck version on the kept line ("... all this. this:")
        toks = text.split()
        if len(toks) > 2 and re.sub(r'\W', '', toks[-1]).lower() in [re.sub(r'\W', '', w).lower() for w in toks[:-1]]:
            text = ' '.join(toks[:-1]); flags.add('tailcut')
    if not re.search(r'[A-Za-z]{2}', text) and alt_text:
        text, alt_text = alt_text, ''
        flags.add('alt-used')
    if key in FIX:
        text, alt_text, flags = FIX[key], '', {'checked'}
    if not t:
        flags.add('notc')
    elif len(t) > 1 and not 0 < sec(t[1]) - sec(t[0]) < 40:
        flags.add('tcbad')
    out.append(dict(i=key, page=r['page'], num=r['num'], scene=scene, start=t[0] if t else '',
                    end=t[1] if len(t) > 1 else '', who=speaker(r['who']), text=text, alt=alt_text,
                    flags=','.join(sorted(flags)), raw=r['text']))


def fmt(x):
    x = round(x * 30)
    return '%02d:%02d:%02d:%02d' % (x // 108000, x // 1800 % 60, x // 30 % 60, x % 30)


# a start time far out of the script order is a misread digit ("02:40:46:24" between 02:20:40 and 02:20:48):
# one digit of the minutes or the tens of seconds changed so that it fits between its neighbours, or no time
timed = [o for o in out if o['start']]
for k, o in enumerate(timed):
    if o['i'] in TCFIX: continue
    near = lambda rng: sorted(sec(x['start']) for x in rng)[len(rng) // 2] if rng else None
    p, q = near(timed[max(0, k - 3):k]), near(timed[k + 1:k + 4])
    t = sec(o['start'])
    if not ((p is not None and t < p - 60) or (q is not None and t > q + 60)): continue
    lo, hi = (p if p is not None else q) - 5, (q if q is not None else p) + 5
    cands = set()
    for pos in (3, 4, 6):
        for dgt in '0123456789':
            c = o['start'][:pos] + dgt + o['start'][pos + 1:]
            if c != o['start'] and int(c[3:5]) < 60 and int(c[6:8]) < 60 and lo <= sec(c) <= hi: cands.add(c)
    flags = set(f for f in o['flags'].split(',') if f)
    if len(cands) == 1:
        c = cands.pop(); d = sec(c) - t
        if o['end'] and 0 < sec(o['end']) - t < 40: o['end'] = fmt(sec(o['end']) + d)   # same misread in both
        o['start'] = c; flags.add('tcorder'); flags.discard('tcbad')
        if o['end'] and not 0 < sec(o['end']) - sec(c) < 40: flags.add('tcbad')
    else:
        o['start'] = o['end'] = ''; flags.add('notc')
    o['flags'] = ','.join(sorted(flags))

with open(sys.argv[2], 'w', encoding='utf-8', newline='') as f:
    w = csv.writer(f, delimiter='\t')
    keys = ['i', 'page', 'num', 'scene', 'start', 'end', 'who', 'text', 'alt', 'flags', 'raw']
    w.writerow(keys)
    for o in out:
        w.writerow([o[k] for k in keys])
spoken = [o for o in out if re.search(r'[A-Za-z]{2}', o['text'])]
print('rows', len(out), 'with spoken text', len(spoken), 'checked', sum('checked' in o['flags'] for o in out))
