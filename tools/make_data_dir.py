"""Unpack a PS2 disc image into a data directory the port can run from without the image.

Usage: python make_data_dir.py <disc.iso> <outdir> [--verify] [--index NAME --subdir DIR]

--index/--subdir add another disc to an existing data directory: files identical to one already
there (same path and content) are shared, the others go to <outdir>/<DIR>/; the index is
<outdir>/NAME (default disc.idx).

Writes every ISO9660 file to <outdir>/<path> and <outdir>/disc.idx, which lists the files with
their sector positions and holds the sectors that belong to no file (volume descriptors, path
tables, directories; all-zero sectors are left out). The runtime serves sector reads from these.
--verify reads every sector both ways and compares.
"""
import os
import struct
import sys

SECTOR = 2048


def walk(f):
    def rd(lsn, n=1):
        f.seek(lsn * SECTOR)
        return f.read(n * SECTOR)

    pvd = rd(16)
    assert pvd[1:6] == b'CD001', 'not an ISO9660 image'
    total, = struct.unpack_from('<I', pvd, 80)
    rlsn, = struct.unpack_from('<I', pvd, 156 + 2)
    rsize, = struct.unpack_from('<I', pvd, 156 + 10)
    files = []

    def scan(lsn, size, prefix, depth):
        d = rd(lsn, (size + SECTOR - 1) // SECTOR)
        off = 0
        while off < size:
            ln = d[off]
            if ln == 0:
                off = (off // SECTOR + 1) * SECTOR
                continue
            elsn, = struct.unpack_from('<I', d, off + 2)
            esz, = struct.unpack_from('<I', d, off + 10)
            fl = d[off + 25]
            nl = d[off + 32]
            name = d[off + 33:off + 33 + nl]
            off += ln
            if nl == 1 and name[0] in (0, 1):
                continue
            name = name.decode('latin1').split(';')[0]
            path = (prefix + '/' + name).lstrip('/')
            if fl & 2:
                if depth < 8:
                    scan(elsn, esz, path, depth + 1)
            else:
                files.append((elsn, esz, path))

    scan(rlsn, rsize, '', 0)
    files.sort()
    return total, files


def opt(name, default=None):
    if name in sys.argv:
        return sys.argv[sys.argv.index(name) + 1]
    return default


def same_content(path, f, lsn, size):
    if not os.path.exists(path) or os.path.getsize(path) != size:
        return False
    f.seek(lsn * SECTOR)
    with open(path, 'rb') as o:
        left = size
        while left:
            n = min(left, 8 << 20)
            if f.read(n) != o.read(n):
                return False
            left -= n
    return True


def main():
    iso, out = sys.argv[1], sys.argv[2]
    index = opt('--index', 'disc.idx')
    subdir = opt('--subdir')
    if subdir is None and index != 'disc.idx':
        raise SystemExit('an additional disc needs --subdir')
    f = open(iso, 'rb')
    total = os.path.getsize(iso) // SECTOR
    _, files = walk(f)
    covered = bytearray(total)
    for lsn, size, _ in files:
        n = (size + SECTOR - 1) // SECTOR
        for s in range(lsn, min(total, lsn + n)):
            covered[s] = 1
    os.makedirs(out, exist_ok=True)
    done = 0
    shared = 0
    stored = []  # path of each file relative to out
    for lsn, size, path in files:
        rel = path
        if subdir:
            if same_content(os.path.join(out, path.replace('/', os.sep)), f, lsn, size):
                stored.append(rel)
                shared += 1
                continue
            rel = subdir + '/' + path
        stored.append(rel)
        dst = os.path.join(out, rel.replace('/', os.sep))
        os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
        if os.path.exists(dst) and os.path.getsize(dst) == size:
            continue
        f.seek(lsn * SECTOR)
        with open(dst + '.part', 'wb') as o:
            left = size
            while left:
                chunk = f.read(min(left, 8 << 20))
                o.write(chunk)
                left -= len(chunk)
        os.replace(dst + '.part', dst)
        done += size
    # sectors outside files: keep the non-zero ones
    sys_secs = []
    zero = bytes(SECTOR)
    for s in range(total):
        if covered[s]:
            continue
        f.seek(s * SECTOR)
        d = f.read(SECTOR)
        if d != zero:
            sys_secs.append((s, d))
    with open(os.path.join(out, index), 'wb') as o:
        o.write(b'G3DISC1\0')
        o.write(struct.pack('<III', total, len(files), len(sys_secs)))
        for (lsn, size, path), rel in zip(files, stored):
            p = rel.encode('latin1')
            o.write(struct.pack('<IIH', lsn, size, len(p)))
            o.write(p)
        for s, _ in sys_secs:
            o.write(struct.pack('<I', s))
        for _, d in sys_secs:
            o.write(d)
    print(f'{len(files)} files ({done >> 20} MB written, {shared} shared), {len(sys_secs)} system sectors, {total} sectors')

    if '--verify' in sys.argv:
        import bisect
        starts = [x[0] for x in files]
        sysmap = dict(sys_secs)
        handles = {}
        bad = 0
        for s in range(total):
            f.seek(s * SECTOR)
            ref = f.read(SECTOR)
            i = bisect.bisect_right(starts, s) - 1
            got = None
            if i >= 0:
                lsn, size, _ = files[i]
                path = stored[i]
                if s < lsn + (size + SECTOR - 1) // SECTOR:
                    h = handles.get(i)
                    if h is None:
                        if len(handles) > 64:
                            for k in list(handles)[:32]:
                                handles.pop(k).close()
                        h = handles[i] = open(os.path.join(out, path.replace('/', os.sep)), 'rb')
                    h.seek((s - lsn) * SECTOR)
                    got = h.read(SECTOR).ljust(SECTOR, b'\0')
            if got is None:
                got = sysmap.get(s, zero)
            if got != ref:
                bad += 1
                if bad < 10:
                    print('mismatch at sector', s)
        print('verify:', 'OK' if bad == 0 else f'{bad} sectors differ')


if __name__ == '__main__':
    main()
