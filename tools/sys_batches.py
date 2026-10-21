# Translation batches for the system texts (see sys_extract.py).
#   python sys_batches.py <src.tsv> <de dir> <out dir> [size]
# Each distinct (category, English text) gets an id (FNV-1a 32 of "cat|EN", 8 hex digits); texts
# already translated in <de dir>/*.tsv ("id<TAB>German") are left out. Batch lines:
#   id  cat  EN  ||  JP   (x count)
import glob, os, sys


def fnv32(s):
    h = 0x811C9DC5
    for x in s.encode('utf-8'):
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


def tid(cat, en):
    return '%08x' % fnv32(cat + '|' + en)


def rows(src):
    for ln in open(src, encoding='utf-8'):
        if ln.startswith('#'):
            continue
        p = ln.rstrip('\n').split('\t')
        if len(p) >= 6:
            yield p[0], p[1], p[2], p[3], p[4], p[5], (p[6] if len(p) > 6 else '')


def done(de_dir):
    d = {}
    for f in sorted(glob.glob(os.path.join(de_dir, '*.tsv'))):
        for ln in open(f, encoding='utf-8'):
            if ln.startswith('#') or '\t' not in ln:
                continue
            k, v = ln.rstrip('\n').split('\t', 1)
            d[k] = v
    return d


if __name__ == '__main__':
    src, de_dir, out = sys.argv[1:4]
    size = int(sys.argv[4]) if len(sys.argv) > 4 else 150
    have = done(de_dir)
    order, info = [], {}
    for kind, a, b, c, cat, en, jp in rows(src):
        k = tid(cat, en)
        if k not in info:
            info[k] = [cat, en, jp, 0]
            order.append(k)
        info[k][3] += 1
        if not info[k][2] and jp:
            info[k][2] = jp
    todo = [k for k in order if k not in have]
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(out, 's*.txt')):
        os.remove(f)
    for n in range(0, len(todo), size):
        with open(os.path.join(out, 's%03d.txt' % (n // size)), 'w', encoding='utf-8', newline='\n') as fo:
            for k in todo[n:n + size]:
                cat, en, jp, cnt = info[k]
                fo.write('%s %-7s %s  ||  %s%s\n' % (k, cat, en, jp, '  (x%d)' % cnt if cnt > 1 else ''))
    print('%d distinct texts, %d translated, %d to do in %d batches' % (len(order), len(order) - len(todo), len(todo), (len(todo) + size - 1) // size))
