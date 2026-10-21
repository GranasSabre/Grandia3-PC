"""Map native profiler samples (perf_samples.txt from --perf) to symbols using the lld map file.

Usage: python perfmap.py <Grandia3.map> <perf_samples.txt> [top_n]
Prints the hottest symbols and the hottest object files.
"""
import bisect
import collections
import re
import sys


def load_map(path):
    syms = []   # (rva, name)
    objs = []   # (rva, size, object)
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'^([0-9a-f]{8}) ([0-9a-f]{8}) +(\d+)( +)(.*)$', line.rstrip('\n'))
        if not m:
            continue
        addr, size, indent, rest = int(m.group(1), 16), int(m.group(2), 16), len(m.group(4)), m.group(5)
        if indent >= 16:          # symbol line (deepest indentation)
            syms.append((addr, rest.strip()))
        elif indent >= 8:         # input section of an object file
            objs.append((addr, size, rest.strip()))
    syms.sort()
    objs.sort()
    return syms, objs


def main():
    syms, objs = load_map(sys.argv[1])
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    sym_addrs = [a for a, _ in syms]
    obj_addrs = [a for a, _, _ in objs]
    by_sym = collections.Counter()
    by_obj = collections.Counter()
    total = 0
    for line in open(sys.argv[2]):
        rva, cnt = line.split()
        rva, cnt = int(rva, 16), int(cnt)
        total += cnt
        if rva == (1 << 64) - 1:
            by_sym['[system, no caller found]'] += cnt
            continue
        ext = rva >> 63
        rva &= (1 << 63) - 1
        i = bisect.bisect_right(sym_addrs, rva) - 1
        name = syms[i][1] if i >= 0 else '?'
        by_sym[('[in Windows, called from] ' if ext else '') + name] += cnt
        j = bisect.bisect_right(obj_addrs, rva) - 1
        name = '?'
        if j >= 0 and rva < objs[j][0] + objs[j][1]:
            name = objs[j][2].split('/')[-1]
        by_obj[name] += cnt
    print(f'{total} samples')
    print('--- symbols ---')
    for name, c in by_sym.most_common(top):
        print(f'{100.0 * c / total:6.2f}%  {c:7d}  {name}')
    if '--lines' in sys.argv:
        # hottest individual addresses -> source lines (needs a --symbols build)
        import subprocess, os
        exe = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), 'Grandia3.exe')
        tc = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'toolchain', 'llvm-mingw-20261006-ucrt-x86_64', 'bin', 'addr2line.exe')
        hot = collections.Counter()
        for line in open(sys.argv[2]):
            rva, cnt = line.split()
            rva = int(rva, 16)
            if rva >> 63:
                continue
            hot[rva] += int(cnt)
        top_addrs = [a for a, _ in hot.most_common(60)]
        base = 0x140000000
        print('--- hottest addresses (innermost inlined frame first) ---')
        for a in top_addrs:
            out = subprocess.run([tc, '-e', exe, '-f', '-C', '-i', '%x' % (base + a)],
                                 capture_output=True, text=True).stdout.splitlines()
            frames = [f'{out[k + 1].split("/")[-1]} {out[k][:40]}' for k in range(0, len(out) - 1, 2)]
            print(f'{100.0 * hot[a] / total:6.2f}%  {a:08x}  ' + ' <- '.join(frames[:3]))
    print('--- objects ---')
    for name, c in by_obj.most_common(15):
        print(f'{100.0 * c / total:6.2f}%  {c:7d}  {name}')


if __name__ == '__main__':
    main()
