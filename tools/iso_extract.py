"""Minimal ISO9660 lister/extractor for PS2 DVD images.

Usage:
  python iso_extract.py <iso> list
  python iso_extract.py <iso> extract <outdir>
"""
import os
import struct
import sys

SECTOR = 2048


def read_dir(f, lba, size, path=""):
    f.seek(lba * SECTOR)
    data = f.read(size)
    off = 0
    entries = []
    while off < len(data):
        ln = data[off]
        if ln == 0:
            # advance to next sector
            off = (off // SECTOR + 1) * SECTOR
            continue
        rec = data[off:off + ln]
        elba = struct.unpack_from('<I', rec, 2)[0]
        esize = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        off += ln
        if name in (b'\x00', b'\x01'):
            continue
        name = name.decode('ascii', 'replace')
        if ';' in name:
            name = name.split(';')[0]
        full = path + "/" + name
        if flags & 2:
            entries.append((full, elba, esize, True))
            entries.extend(read_dir(f, elba, esize, full))
        else:
            entries.append((full, elba, esize, False))
    return entries


def main():
    iso = sys.argv[1]
    cmd = sys.argv[2]
    with open(iso, 'rb') as f:
        f.seek(16 * SECTOR)
        pvd = f.read(SECTOR)
        root = pvd[156:156 + 34]
        rlba = struct.unpack_from('<I', root, 2)[0]
        rsize = struct.unpack_from('<I', root, 10)[0]
        entries = read_dir(f, rlba, rsize)
        if cmd == 'list':
            for p, lba, size, isdir in entries:
                print(f"{'D' if isdir else 'F'} {lba:8d} {size:12d} {p}")
        elif cmd == 'extract':
            out = sys.argv[3]
            for p, lba, size, isdir in entries:
                dst = os.path.join(out, p.lstrip('/'))
                if isdir:
                    os.makedirs(dst, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                f.seek(lba * SECTOR)
                remaining = size
                with open(dst, 'wb') as o:
                    while remaining:
                        chunk = f.read(min(remaining, 1 << 22))
                        o.write(chunk)
                        remaining -= len(chunk)
                print(p, size)


if __name__ == '__main__':
    main()
