# Collects the system texts (items, magic, skills, special moves, enemies, speaker names, place
# names, menus, battle texts) with their Japanese counterparts, for translation and make_sys.py.
#   python sys_extract.py <us GR3.MDT> <jp GR3.MDT> <EE RAM dump with GR3 loaded> <us mdt dir>
#                         <jp mdt dir> <us FIELD.BIN> <jp FIELD.BIN> <us BATTLE.BIN> <out src.tsv>
# out: kind a b c cat <tab> EN <tab> JP   (kind/a/b/c as in runtime/src/ee/sys_text.cpp; EN in
# readable form: space, \n between lines; cat groups texts that share one translation dictionary)
import glob, os, re, struct, sys
import scnr_text as S
import jp_decode as J

NUL = bytes(1)
BSN = chr(92) + "n"  # line break in the readable form


def fnv32(b):
    h = 0x811C9DC5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


def cstr(d, o):
    return d[o:d.index(b'\0', o)]


def en_text(raw):
    """font-code bytes -> readable; two-byte glyphs (second byte F0..FF) as {hex}"""
    s = ''
    for i, x in enumerate(raw):
        if x >= 0xF0 and i:
            continue
        if i + 1 < len(raw) and raw[i + 1] >= 0xF0:
            s += '{%02x%02x}' % (x, raw[i + 1])
        elif x == 0x80:
            s += ' '
        elif 0x20 <= x < 0x7F:
            s += chr(x)
        elif x == 0x0A:
            s += '\\n'
        else:
            s += '{%02x}' % x
    return s


def jp_text(raw, wide=False):
    m = J.charmap()
    s, i = '', 0
    if wide:
        for k in range(0, len(raw) - 1, 2):
            c = raw[k] | raw[k + 1] << 8
            s += m.get(c + 4, '〓')
        return s
    while i < len(raw):
        b = raw[i]
        if i + 1 < len(raw) and raw[i + 1] >= 0xF0:
            s += m.get(0xD4 + (raw[i + 1] - 0xF0) * 0xD0 + b, '〓')
            i += 2
        elif b == 0x0A:
            s += '\\n'
            i += 1
        else:
            s += m.get(b + 4, '〓')
            i += 1
    return s


def printable(raw, short=False):
    """font-code text: ASCII, space 0x80, two-byte glyphs (second byte F0..FF)"""
    if len(raw) <= 2 and not short or not re.search(rb'[A-Za-z]', raw) and not short:
        return False
    i = 0
    while i < len(raw):
        if i + 1 < len(raw) and raw[i + 1] >= 0xF0:
            i += 2
        elif 0x20 <= raw[i] < 0x7F or raw[i] == 0x80:
            i += 1
        else:
            return False
    return True


def desc_block(d, o):
    """lines of a description: NUL separated, ends with an empty line"""
    lines = []
    while d[o] != 0:
        s = cstr(d, o)
        lines.append(s)
        o += len(s) + 1
    return lines


def chunk7_dir(d):
    n = struct.unpack_from('<I', d, 0x980)[0]
    return [0x980 + o for o, s in (struct.unpack_from('<II', d, 0x984 + 8 * i) for i in range(n))]


def main():
    us_gr3, jp_gr3, dump, us_mdt, jp_mdt, us_field, jp_field, us_battle, out = sys.argv[1:10]
    u = open(us_gr3, 'rb').read()
    j = open(jp_gr3, 'rb').read()
    r = open(dump, 'rb').read()
    base = r.find(u[0x800:0x810]) - 0x800  # GR3 loaded as a whole
    assert r[base + 0x34700:base + 0x34710] == u[0x34700:0x34710], 'GR3 not found in the dump'
    rows = []

    def ptr_fields(lo, hi):
        """pointer fields relocated at load: GR3 offset of field -> GR3 offset of target"""
        res = {}
        for k in range(lo, hi, 4):
            fv = struct.unpack_from('<I', u, k)[0]
            rv = struct.unpack_from('<I', r, base + k)[0]
            if fv != rv and base <= rv < base + len(u):
                res[k] = rv - base
        return res

    # ---- chunk 7: items, equipment, eggs, books, magic, skills; special moves
    ud, jd = chunk7_dir(u), chunk7_dir(j)
    us_sec = sorted(set(o for o in ud if o > 0x980))
    for si, (us0, jp0) in enumerate(zip(ud, jd)):
        if us0 == 0x980 or si + 1 >= len(ud):
            continue
        us1 = min([o for o in us_sec if o > us0] + [0x31b00])
        fields = ptr_fields(us0, us1)
        jfields = {k: jp0 + struct.unpack_from('<I', j, jp0 + (k - us0))[0] for k in fields}
        uts, jts = sorted(set(fields.values())), sorted(set(jfields.values()))

        def block(d, t, starts, lim):
            """lines from t up to the next pointed-to string (descriptions span several lines)"""
            nxt = min([x for x in starts if x > t] + [lim])
            lines = d[t:nxt].split(NUL)
            while lines and not lines[-1]:
                lines.pop()
            return lines
        for k, t in sorted(fields.items()):
            raw = cstr(u, t)
            if not printable(raw):
                continue
            def by_length(lf):
                """lines when the u16 at lf is the description's length (up to the last line's end,
                empty lines included); None otherwise"""
                L = struct.unpack_from('<H', u, lf)[0]
                if not len(raw) <= L < 512 or u[t + L] != 0:
                    return None
                ls = u[t:t + L].split(NUL)
                if not all(printable(x, True) or not x for x in ls) or len(ls) > 4:
                    return None
                return ls, L
            kind = None
            for kd, lf in (('d', k + 4),):
                r_ = by_length(lf)
                if r_ and (kd == 'd' or len(r_[0]) > 1):
                    kind, (lines, ln) = kd, r_
                    break
            if kind is None:
                lines = block(u, t, uts, t + 400)
                if not all(printable(x, True) for x in lines):
                    lines = [raw]
                ln = len(NUL.join(lines))
                kind = 'f' if len(lines) > 1 else ('n' if u[k + 4] == len(raw) else 'g')
            while lines and not lines[-1]:
                lines = lines[:-1]
            jlines = block(j, jfields[k], jts, jfields[k] + 400)[:4] if 0 < jfields[k] < len(j) else []
            if len(lines) == 1:
                jlines = jlines[:1]
            jp = BSN.join(jp_text(x) for x in jlines if x)
            if kind in 'def':
                rows.append((kind, k, t, ln, 'desc', BSN.join(en_text(x) for x in lines), jp))
            else:  # one-line descriptions without a length ("Medal/For casual gambling.") are texts too
                cat = 'desc' if re.match(rb'[A-Za-z]+/', raw) or raw.endswith(b'.') else 'name'
                rows.append((kind, k, t, fnv32(raw), cat, en_text(raw), jp))

    # ---- chunk 17: enemies (171 blocks, each relocated against its own base)
    def c17(d):
        for p, a, sz in S.chunks(d):
            if a == 0xA0000930:
                return p + 0x180
    ub, jb = c17(u), c17(j)
    nb = struct.unpack_from('<I', u, ub)[0]
    for bi in range(nb):
        uo, us_ = struct.unpack_from('<II', u, ub + 4 + 8 * bi)
        jo, js_ = struct.unpack_from('<II', j, jb + 4 + 8 * bi)
        for k, t in sorted(ptr_fields(ub + uo, ub + uo + us_).items()):
            raw = cstr(u, t)
            if not printable(raw) or not re.search(rb'[A-Za-z]{2}', raw) or u[t - 1] != 0:
                continue
            jk = jb + jo + (k - ub - uo)
            jt = jb + jo + struct.unpack_from('<I', j, jk)[0]
            jraw = cstr(j, jt) if 0 < jt < len(j) else b''
            rows.append(('g', k, t, fnv32(raw), 'enemy', en_text(raw), jp_text(jraw)))

    # ---- chunk 9: speaker names (u16, offsets relative to the data start)
    def c9(d):
        for p, a, sz in S.chunks(d):
            if a == 0x03900000:
                return p + 0x180
    for d_, b_ in ((u, c9(u)), (j, c9(j))):
        pass
    ub9, jb9 = c9(u), c9(j)
    n, _, ut, first = struct.unpack_from('<4I', u, ub9)
    jt9 = struct.unpack_from('<I', j, jb9 + 8)[0]
    jfirst = struct.unpack_from('<I', j, jb9 + 12)[0]

    def wstr(d, a):
        e = a
        while d[e] | d[e + 1]:
            e += 2
        return d[a:e]
    us2jp = {}
    for k in range(n):
        ui = struct.unpack_from('<H', u, ub9 + first + 8 * k + 2)[0]
        ji = struct.unpack_from('<H', j, jb9 + jfirst + 8 * k + 2)[0]
        if ui != 0xFFFF and ji != 0xFFFF:
            us2jp.setdefault(ui, ji)
    for ui in range(struct.unpack_from('<I', u, ub9 + 4)[0]):
        f = ub9 + ut + 4 * ui
        off = struct.unpack_from('<I', u, f)[0]
        raw = wstr(u, ub9 + off)
        en = en_text(bytes(raw[i] for i in range(0, len(raw), 2)))
        ji = us2jp.get(ui)
        jp = jp_text(wstr(j, jb9 + struct.unpack_from('<I', j, jb9 + jt9 + 4 * ji)[0]), True) if ji is not None else ''
        rows.append(('w', f, ub9, off, 'speaker', en, jp))

    # ---- place names / prompts: chunk a0001350 of every map (and GR3), u16, offset + length
    def name_table(d):
        for p, a, sz in S.chunks(d):
            if a == 0xA0001350:
                t = p + 0x180
                n, _, lo, oo = struct.unpack_from('<4I', d, t)
                strs, end = [], 0
                for i in range(n):
                    off = struct.unpack_from('<I', d, t + oo + 4 * i)[0]
                    ln = d[t + lo + i]
                    end = max(end, off + 2 * ln)
                    strs.append(d[t + off:t + off + 2 * ln])
                return fnv32(d[t:t + end]), strs
        return None, []
    for f in [us_gr3] + sorted(glob.glob(os.path.join(us_mdt, '*.MDT'))):
        d = open(f, 'rb').read()
        key, strs = name_table(d)
        if key is None:
            continue
        jf = jp_gr3 if f == us_gr3 else os.path.join(jp_mdt, os.path.basename(f))
        jstrs = name_table(open(jf, 'rb').read())[1] if os.path.exists(jf) else []
        for i, raw in enumerate(strs):
            en = en_text(bytes(raw[k] for k in range(0, len(raw), 2)))
            jp = jp_text(jstrs[i], True) if i < len(jstrs) and len(jstrs) == len(strs) else ''
            rows.append(('m', key, i, 0, 'place' if f != us_gr3 else 'menu', en, jp))

    # ---- field menus: string table of FIELD.BIN (384 entries), Japanese table aligned
    def overlay(path):
        d = open(path, 'rb').read()
        return d, struct.unpack_from('<I', d, 8)[0]
    fu, fub = overlay(us_field)
    fj, fjb = overlay(jp_field)
    tu = fu.index(struct.pack('<I', fub + fu.index(b'Top Menu\0')))
    tj = fj.index(struct.pack('<I', fjb + fj.index('トップメニュー'.encode('sjis') + b'\0')))
    for i in range(384):
        pu = struct.unpack_from('<I', fu, tu + 4 * i)[0]
        pj = struct.unpack_from('<I', fj, tj + 4 * i)[0]
        raw = cstr(fu, pu - fub)
        if not re.search(rb'[A-Za-z]', raw):
            continue
        try:
            jp = cstr(fj, pj - fjb).decode('sjis').replace('\n', '\\n')
        except Exception:
            jp = ''
        en = re.sub(rb'[\x81-\x9f\xe0-\xfc].', lambda m: b'{' + m.group().hex().encode() + b'}', raw)
        en = en.decode('latin1').replace('\n', '\\n')
        rows.append(('a', fub + tu + 4 * i, pu, fnv32(raw), 'menu', en, jp))

    # ---- battle overlay: messages and command names (font codes). Strings reached through a pointer
    # in the data are redirected ('b'; a pointer to the first of several lines: 'c'), the others are
    # overwritten in place ('i', room = up to the next string)
    import str_refs
    bd, bbase, bstart = str_refs.load(us_battle)
    lo, hi = bbase + bd.index(b"You're\x80surrounded!!\0"), bbase + bd.index(b'Special\x80Move\0') + 13
    strs = {}
    for mt in re.finditer(rb'[\x20-\x7e\x80]{2,}\x00', bd[lo - bbase:hi - bbase]):
        strs[lo + mt.start()] = mt.group()[:-1]
    dref = {}
    for o in range(0, len(bd) - 3, 4):
        v = struct.unpack_from('<I', bd, o)[0]
        if v in strs:
            dref.setdefault(v, []).append(bbase + o)
    cref = {v for a, v, rg, jt in str_refs.refs(bd, bbase, bstart, set(strs))}
    addrs = sorted(strs)
    skip = set()
    for n, a in enumerate(addrs):
        raw = strs[a]
        if a in skip or not re.search(rb'[a-z]{2}', raw) or re.search(rb'ff[vN]|fff|^CStr', raw) or len(raw) < 4:
            continue
        if a in dref:
            lines = [raw]
            for b2 in addrs[n + 1:]:  # following lines nobody points to belong to the same text
                if b2 in dref or b2 in cref or b2 != a + len(NUL.join(lines)) + 1 or len(lines) == 3:
                    break
                lines.append(strs[b2])
                skip.add(b2)
            kind = 'c' if len(lines) > 1 else 'b'
            for f in dref[a]:
                rows.append((kind, f, a, fnv32(raw), 'battle', BSN.join(en_text(x) for x in lines), ''))
        else:
            nxt = addrs[n + 1] if n + 1 < len(addrs) else a + len(raw) + 1
            rows.append(('i', a, nxt - a, fnv32(raw), 'battle', en_text(raw), ''))

    with open(out, 'w', encoding='utf-8', newline='\n') as fo:
        fo.write('# kind\ta\tb\tc\tcat\tEN\tJP (generated by sys_extract.py)\n')
        for kind, a, b, c, cat, en, jp in rows:
            fo.write('%s\t%x\t%x\t%x\t%s\t%s\t%s\n' % (kind, a, b, c, cat, en, jp))
    from collections import Counter
    print(len(rows), 'texts', dict(Counter(r[4] for r in rows)))


if __name__ == '__main__':
    main()
