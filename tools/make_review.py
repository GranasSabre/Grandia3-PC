# Proofreading batches: translated messages in map (scene) order with JP, EN and the current German text.
#   python make_review.py <us mdt dir> <jp mdt dir> <de dir> <out dir> <filter> [batch size]
# filter: "pause" = messages with timed pause codes {13ff..} (voiced cutscenes), "all" = everything,
#         or a regular expression matched against the German text.
# Batch file r000.txt...: one block per message
#   #<key> <map>/<index> [<speaker>] <de file>
#   JP / EN / DE lines
# Corrections go into a TSV (key<TAB>text) and are applied with apply_fix.py.
import difflib, glob, os, re, sys
import scnr_text as S
import jp_decode as J
from make_lang import fnv

us_dir, jp_dir, de_dir, out_dir, flt = sys.argv[1:6]
size = int(sys.argv[6]) if len(sys.argv) > 6 else 100
de = {}
for tf in sorted(glob.glob(os.path.join(de_dir, '*.tsv'))):
    for ln in open(tf, encoding='utf-8'):
        ln = ln.rstrip('\n')
        if len(ln) > 17 and ln[16] == '\t':
            de[int(ln[:16], 16)] = (ln[17:], os.path.basename(tf))
if flt == 'pause':
    keep = lambda t: '{13ff' in t
elif flt == 'all':
    keep = lambda t: True
else:
    rx = re.compile(flt)
    keep = lambda t: rx.search(t) is not None
seen, blocks = set(), []
for f in sorted(glob.glob(os.path.join(us_dir, '*.MDT'))):
    name = os.path.splitext(os.path.basename(f))[0]
    su = S.script(open(f, 'rb').read())
    if not su:
        continue
    jf = os.path.join(jp_dir, os.path.basename(f))
    sj = S.script(open(jf, 'rb').read()) if os.path.exists(jf) else b''
    tu, tj = S.texts(su, False), S.texts(sj, True)
    jmap = {k: k for k in range(len(tu))} if len(tu) == len(tj) else {}
    if not jmap and tj:
        spk = lambda s, t: [s[o - 2] | s[o - 1] << 8 for o, _ in t]
        sm = difflib.SequenceMatcher(None, spk(su, tu), spk(sj, tj), autojunk=False)
        for op, a0, a1, b0, b1 in sm.get_opcodes():
            if op == 'equal' or (op == 'replace' and a1 - a0 == b1 - b0):
                for d in range(a1 - a0):
                    jmap[a0 + d] = b0 + d
    for k, (o, raw) in enumerate(tu):
        key = fnv(raw)
        if key in seen or key not in de or not keep(de[key][0]):
            continue
        seen.add(key)
        sp = su[o - 2] | su[o - 1] << 8
        jp = J.decode(tj[jmap[k]][1]) if k in jmap else "(?)"
        txt, src = de[key]
        blocks.append('#%016x %s/%d [%02x] %s\nJP %s\nEN %s\nDE %s\n' % (key, name, k, sp, src, jp, S.to_markup(raw), txt))
os.makedirs(out_dir, exist_ok=True)
for old in glob.glob(os.path.join(out_dir, 'r*.txt')):
    os.remove(old)
for b in range(0, len(blocks), size):
    with open(os.path.join(out_dir, 'r%03d.txt' % (b // size)), 'w', encoding='utf-8') as f:
        f.write(''.join(blocks[b:b + size]))
print('%d messages -> %d review batches in %s' % (len(blocks), (len(blocks) + size - 1) // size, out_dir))
