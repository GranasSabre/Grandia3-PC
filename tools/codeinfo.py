"""Code module loading and function discovery for stripped PS2 binaries.

A Module is a contiguous code+data image loaded at a fixed address
(main ELF, an overlay, or an IRX after relocation).
"""
import struct
from r5900 import decode


class Module:
    def __init__(self, name, base, data, code_end, data_end=None, bss_end=None, code_start=None):
        self.name = name
        self.base = base
        self.data = data            # bytes of the whole image (code+data)
        self.code_start = code_start if code_start is not None else base
        self.code_end = code_end    # absolute address where code stops
        self.data_end = data_end or base + len(data)
        self.bss_end = bss_end or self.data_end
        n = (code_end - base) // 4
        self.words = struct.unpack_from('<%dI' % n, data, 0)
        self.insns = [decode(w, base + 4 * k) for k, w in enumerate(self.words)]

    def in_code(self, a):
        return self.code_start <= a < self.code_end

    def insn(self, a):
        return self.insns[(a - self.base) >> 2]

    def word(self, a):
        off = a - self.base
        if 0 <= off < len(self.data) - 3:
            return struct.unpack_from('<I', self.data, off)[0]
        return None


def load_main_elf(path, code_end):
    d = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', d, 28)[0]
    phnum = struct.unpack_from('<H', d, 44)[0]
    segs = [struct.unpack_from('<8I', d, phoff + i * 32) for i in range(phnum)]
    p = segs[0]
    off, vaddr, filesz, memsz = p[1], p[2], p[4], p[5]
    data = d[off:off + filesz]
    return Module('main', vaddr, data, code_end, vaddr + filesz, vaddr + memsz)


def extend_code_end(img, base, text_end, window=0x400):
    """Code sometimes continues a little past the header's text size; extend to the last jr ra."""
    from r5900 import decode
    best = text_end
    a = text_end
    while a < text_end + window and a - base + 4 <= len(img):
        w = struct.unpack_from('<I', img, a - base)[0]
        i = decode(w, a)
        if i.kind == 'invalid':
            break
        if w == 0x03E00008:
            best = a + 8
        a += 4
    return best


def load_overlay(path, name=None, offset=0):
    """Overlay files are loaded whole (including the 0x80-byte header) at the load address."""
    d = open(path, 'rb').read()[offset:]
    magic, ver, load, text, data, bss, a6, a7 = struct.unpack_from('<4s7I', d, 0)
    img = d[:0x80 + text + data]
    code_end = extend_code_end(img, load, load + 0x80 + text)
    return Module(name or path, load, img, code_end, load + 0x80 + text + data, load + 0x80 + text + data + bss,
                  code_start=load + 0x80)


class Function:
    __slots__ = ('start', 'end', 'labels', 'switches', 'tailcalls', 'name', 'calls', 'flags')

    def __init__(self, start, end):
        self.start = start
        self.end = end
        self.labels = set()
        self.switches = {}     # pc of jr -> list of targets
        self.tailcalls = set()
        self.calls = set()
        self.name = 'f_%08x' % start
        self.flags = set()


def find_jump_table(mod, jr_pc, window=40):
    """Detect a switch jump table feeding 'jr reg' at jr_pc.

    Runs a tiny symbolic evaluation over the instructions preceding the jr:
    registers are tracked as ('const', v), ('idx', base) meaning base + scaled index,
    or ('tbl', addr) meaning a word loaded from table at addr+scaled index.
    Returns (table_addr, count_or_None) or None.
    """
    start = max(mod.base, jr_pc - window * 4)
    st = {}
    count = None
    for a in range(start, jr_pc, 4):
        i = mod.insn(a)
        op = i.op
        if op == 'lui':
            st[i.rt] = ('const', (i.uimm << 16) & 0xFFFFFFFF)
        elif op in ('addiu', 'daddiu'):
            s = st.get(i.rs)
            if s and s[0] == 'const':
                st[i.rt] = ('const', (s[1] + i.imm) & 0xFFFFFFFF)
            elif s and s[0] == 'idx':
                st[i.rt] = ('idx', (s[1] + i.imm) & 0xFFFFFFFF)
            else:
                st.pop(i.rt, None)
        elif op == 'ori':
            s = st.get(i.rs)
            if s and s[0] == 'const':
                st[i.rt] = ('const', s[1] | i.uimm)
            else:
                st.pop(i.rt, None)
        elif op in ('addu', 'daddu'):
            a1 = st.get(i.rs)
            a2 = st.get(i.rt)
            if a1 and a1[0] == 'const' and not (a2 and a2[0] == 'const'):
                st[i.rd] = ('idx', a1[1])
            elif a2 and a2[0] == 'const' and not (a1 and a1[0] == 'const'):
                st[i.rd] = ('idx', a2[1])
            else:
                st.pop(i.rd, None)
        elif op == 'lw':
            s = st.get(i.rs)
            if s and s[0] == 'idx':
                st[i.rt] = ('tbl', (s[1] + i.imm) & 0xFFFFFFFF)
            else:
                st.pop(i.rt, None)
        elif op == 'sltiu':
            count = i.imm & 0xFFFF
            st.pop(i.rt, None)
        else:
            if i.kind in ('alu', 'load', 'mmi', 'fpu', 'cop2') and op not in ('sw', 'sd'):
                # generic destination clobber
                for r in (i.rd, i.rt):
                    pass
                if op in ('sll', 'srl', 'sra', 'and', 'or', 'xor', 'nor', 'subu', 'dsubu', 'slt', 'sltu',
                          'andi', 'xori', 'slti', 'dsll', 'dsrl', 'dsra', 'dsll32', 'dsrl32', 'dsra32',
                          'sllv', 'srlv', 'srav', 'mfhi', 'mflo', 'movz', 'movn'):
                    st.pop(i.rd if i.op not in ('andi', 'xori', 'slti') else i.rt, None)
                elif i.kind == 'load':
                    st.pop(i.rt, None)
    jr = mod.insn(jr_pc)
    s = st.get(jr.rs)
    if s and s[0] == 'tbl':
        return s[1], count
    return None


def discover_functions(mod, extra_starts=(), data_ptr_scan=True, other_mods=()):
    """Partition module code into functions."""
    starts = set(extra_starts)
    # 1. jal targets (from this module and others)
    for m in (mod,) + tuple(other_mods):
        for i in m.insns:
            if i.kind == 'jal' and mod.in_code(i.target):
                starts.add(i.target)
    # 2. jump tables: collect label targets (not function starts)
    jt_targets = set()
    switches = {}
    for i in mod.insns:
        if i.kind == 'jr' and i.rs != 31:
            jt = find_jump_table(mod, i.pc)
            if jt:
                table, count = jt
                tgts = []
                k = 0
                while True:
                    if count is not None and k >= count:
                        break
                    w = mod.word(table + 4 * k)
                    if w is None or not mod.in_code(w) or (w & 3):
                        break
                    tgts.append(w)
                    k += 1
                    if count is None and k > 512:
                        break
                if tgts:
                    switches[i.pc] = (table, tgts)
                    jt_targets.update(tgts)
    # 3. data pointers into code (function pointer tables)
    data_ptrs = set()
    if data_ptr_scan:
        off0 = mod.code_end - mod.base
        for off in range(off0, len(mod.data) - 3, 4):
            w = struct.unpack_from('<I', mod.data, off)[0]
            if mod.in_code(w) and (w & 3) == 0 and w not in jt_targets:
                data_ptrs.add(w)
        for m in other_mods:
            off0 = m.code_end - m.base
            for off in range(off0, len(m.data) - 3, 4):
                w = struct.unpack_from('<I', m.data, off)[0]
                if mod.in_code(w) and (w & 3) == 0:
                    data_ptrs.add(w)
    # also lui/addiu pairs in code that form code addresses (function pointers loaded in code)
    code_ptrs = set()
    for k in range(len(mod.insns) - 1):
        i = mod.insns[k]
        if i.op == 'lui':
            hi = i.uimm << 16
            for j in range(k + 1, min(k + 8, len(mod.insns))):
                n = mod.insns[j]
                if n.op in ('addiu', 'ori') and n.rs == i.rt:
                    a = (hi + (n.imm if n.op == 'addiu' else n.uimm)) & 0xFFFFFFFF
                    if mod.in_code(a) and (a & 3) == 0 and a not in jt_targets:
                        code_ptrs.add(a)
                    break
                if n.rt == i.rt and n.kind in ('alu', 'load'):
                    break
    candidate_ptrs = data_ptrs | code_ptrs

    def follows_boundary(a):
        """True if address a looks like the beginning of a function (after padding or a flow end)."""
        if a == mod.code_start:
            return True
        # a terminator (jr / j / b) at t, delay slot at t+4, only zero words in [t+8, a)
        t = a - 8
        while t >= mod.code_start:
            i = mod.insn(t)
            if (i.kind == 'jr' and i.rs == 31) or i.kind == 'jump':
                return True
            if mod.word(t + 4) != 0:
                return False
            t -= 4
        return False

    all_starts = set(s for s in starts if mod.in_code(s))
    for p in candidate_ptrs:
        if follows_boundary(p) and mod.word(p) != 0:
            all_starts.add(p)

    def extent(s, limit):
        """Linear sweep from s: end = first flow-terminating point past all forward branch targets."""
        a = s
        horizon = s
        while a < limit:
            i = mod.insn(a)
            k = i.kind
            if k in ('branch', 'branchl'):
                if s <= i.target < limit:
                    horizon = max(horizon, i.target)
                a += 8
                if i.op == 'beq' and i.rs == 0 and i.rt == 0 and horizon < a:
                    return a
                continue
            if k == 'jump':
                # a jump at the function start, or one followed by padding, is a tail call (thunk)
                followed_by_pad = mod.word(a + 8) == 0 if a + 8 < mod.code_end else True
                tail = (a == s) or (followed_by_pad and i.target > a + 8)
                if not tail and s <= i.target < limit and i.target not in all_starts:
                    horizon = max(horizon, i.target)
                a += 8
                if horizon < a:
                    return a
                continue
            if k == 'jr':
                if a in switches:
                    for t in switches[a][1]:
                        if s <= t < limit:
                            horizon = max(horizon, t)
                a += 8
                if horizon < a:
                    return a
                continue
            if k in ('jal', 'jalr'):
                a += 8
                continue
            a += 4
        return limit

    a = mod.code_start
    while a < mod.code_end and mod.word(a) == 0:
        a += 4
    if a < mod.code_end:
        all_starts.add(a)
    changed = True
    while changed:
        changed = False
        ss = sorted(all_starts)
        for idx, s in enumerate(ss):
            limit = ss[idx + 1] if idx + 1 < len(ss) else mod.code_end
            end = extent(s, limit)
            # j / branch targets outside [s,end) must be function starts
            for a in range(s, end, 4):
                i = mod.insn(a)
                if i.kind in ('jump', 'branch', 'branchl') and mod.in_code(i.target) and not (s <= i.target < end):
                    if i.target not in all_starts:
                        all_starts.add(i.target)
                        changed = True
            # code after the end (skip padding) starts a new function
            a = end
            while a < limit and mod.word(a) == 0:
                a += 4
            if a < limit and a not in all_starts:
                all_starts.add(a)
                changed = True
    ss = sorted(all_starts)
    funcs = []
    for idx, s in enumerate(ss):
        limit = ss[idx + 1] if idx + 1 < len(ss) else mod.code_end
        end = extent(s, limit)
        f = Function(s, end)
        for a in range(s, end, 4):
            i = mod.insn(a)
            if i.kind in ('branch', 'branchl', 'jump') and s <= i.target < end:
                f.labels.add(i.target)
            if a in switches:
                tg = switches[a][1]
                f.switches[a] = tg
                for t in tg:
                    if s <= t < end:
                        f.labels.add(t)
        funcs.append(f)
    return funcs, switches, candidate_ptrs, jt_targets
