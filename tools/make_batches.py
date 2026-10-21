# Translation work batches: every distinct English scenario message once (in map order), with the
# Japanese original decoded to text. Messages already translated (keys in <done dir>/*.tsv) are skipped.
#   python make_batches.py <us mdt dir> <jp mdt dir> <done dir> <out dir> [batch size]
# Batch file: one block per message
#   #<key> <map>/<index> [<speaker>] xN (number of occurrences)
#   JP <japanese markup>
#   EN <english markup>
import difflib, glob, os, sys
import scnr_text as S
import jp_decode as J
from make_lang import fnv

us_dir, jp_dir, done_dir, out_dir = sys.argv[1:5]
size = int(sys.argv[5]) if len(sys.argv) > 5 else 120
done = set()
for tf in glob.glob(os.path.join(done_dir, '*.tsv')):
    for ln in open(tf, encoding='utf-8'):
        if len(ln) > 17 and ln[16] == '\t':
            done.add(int(ln[:16], 16))
order, info = [], {}
for f in sorted(glob.glob(os.path.join(us_dir, '*.MDT'))):
    name = os.path.splitext(os.path.basename(f))[0]
    su = S.script(open(f, 'rb').read())
    if not su:
        continue
    jf = os.path.join(jp_dir, os.path.basename(f))
    sj = S.script(open(jf, 'rb').read()) if os.path.exists(jf) else b''
    tu, tj = S.texts(su, False), S.texts(sj, True)
    # pair EN and JP messages; if the counts differ, align them by speaker id sequence
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
        if key in info:
            info[key][4] += 1
            continue
        sp = su[o - 2] | su[o - 1] << 8
        if o >= 15 and su[o - 15] == 0x0D and su[o - 14] == 0x09:
            sp |= 0x10000  # choice menu
        jp = J.decode(tj[jmap[k]][1]) if k in jmap else "(?)"
        info[key] = ['%s/%d' % (name, k), sp, jp, S.to_markup(raw), 1]
        order.append(key)
todo = [k for k in order if k not in done]
os.makedirs(out_dir, exist_ok=True)
for old in glob.glob(os.path.join(out_dir, 'b*.txt')):
    os.remove(old)
for b in range(0, len(todo), size):
    with open(os.path.join(out_dir, 'b%03d.txt' % (b // size)), 'w', encoding='utf-8') as f:
        for key in todo[b:b + size]:
            loc, sp, jp, en, n = info[key]
            menu = ' MENU(one line per option)' if sp & 0x10000 else ''
            f.write('#%016x %s [%02x] x%d%s\nJP %s\nEN %s\n' % (key, loc, sp & 0xFFFF, n, menu, jp, en))
print('%d distinct messages, %d translated, %d to do -> %d batches' % (len(order), len(order) - len(todo), len(todo), (len(todo) + size - 1) // size))
