"""VU1 microprogram ahead-of-time translator.

Usage: python vu_recomp.py <image.bin> <out.cpp>

Translates a 16 KB VU1 micro memory image (dumped with --vu-dump) into a C++ function that executes
it with the same semantics as the interpreter in runtime/src/vu/vu.cpp:
  - one label per instruction, register numbers and masks are constants
  - branch delay slots and end (E) bits resolved statically, JR/JALR through a dispatch switch
  - MAC/clip flags through the shared FIFO model (4 cycle latency), committed lazily before reads
  - Q/P latencies, the conditional-branch VI hazard (history of the last 4 VI writes)
The generated function registers itself under the FNV-1a hash of the image.
"""
import struct
import sys

# ---------------------------------------------------------------------------
# decoding (mirrors decode_upper / decode_lower in vu.cpp)
# ---------------------------------------------------------------------------
T1 = (['ADDbc'] * 4 + ['SUBbc'] * 4 + ['MADDbc'] * 4 + ['MSUBbc'] * 4 + ['MAXbc'] * 4 + ['MINIbc'] * 4 +
      ['MULbc'] * 4 + ['MULq', 'MAXi', 'MULi', 'MINIi', 'ADDq', 'MADDq', 'ADDi', 'MADDi', 'SUBq', 'MSUBq', 'SUBi',
                       'MSUBi', 'ADD', 'MADD', 'MUL', 'MAX', 'SUB', 'MSUB', 'OPMSUB', 'MINI'] + ['INVALID'] * 12)
assert len(T1) == 0x3C
T2 = {}
for i, n in enumerate(['ADDAbc', 'SUBAbc', 'MADDAbc', 'MSUBAbc']):
    for b in range(4):
        T2[i * 4 + b] = n
T2.update({0x10: 'ITOF0', 0x11: 'ITOF4', 0x12: 'ITOF12', 0x13: 'ITOF15', 0x14: 'FTOI0', 0x15: 'FTOI4', 0x16: 'FTOI12',
           0x17: 'FTOI15', 0x18: 'MULAbc', 0x19: 'MULAbc', 0x1A: 'MULAbc', 0x1B: 'MULAbc', 0x1C: 'MULAq', 0x1D: 'ABS',
           0x1E: 'MULAi', 0x1F: 'CLIP', 0x20: 'ADDAq', 0x21: 'MADDAq', 0x22: 'ADDAi', 0x23: 'MADDAi', 0x24: 'SUBAq',
           0x25: 'MSUBAq', 0x26: 'SUBAi', 0x27: 'MSUBAi', 0x28: 'ADDA', 0x29: 'MADDA', 0x2A: 'MULA', 0x2C: 'SUBA',
           0x2D: 'MSUBA', 0x2E: 'OPMULA', 0x2F: 'NOP'})

LOPS = {0x00: 'LQ', 0x01: 'SQ', 0x04: 'ILW', 0x05: 'ISW', 0x08: 'IADDIU', 0x09: 'ISUBIU', 0x10: 'FCEQ', 0x11: 'FCSET',
        0x12: 'FCAND', 0x13: 'FCOR', 0x14: 'FSEQ', 0x15: 'FSSET', 0x16: 'FSAND', 0x17: 'FSOR', 0x18: 'FMEQ',
        0x1A: 'FMAND', 0x1B: 'FMOR', 0x1C: 'FCGET', 0x20: 'B', 0x21: 'BAL', 0x24: 'JR', 0x25: 'JALR', 0x28: 'IBEQ',
        0x29: 'IBNE', 0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}
L40 = {0x30: 'IADD', 0x31: 'ISUB', 0x32: 'IADDI', 0x34: 'IAND', 0x35: 'IOR'}
L40X = {0x30: 'MOVE', 0x31: 'MR32', 0x34: 'LQI', 0x35: 'SQI', 0x36: 'LQD', 0x37: 'SQD', 0x38: 'DIV', 0x39: 'SQRT',
        0x3A: 'RSQRT', 0x3B: 'WAITQ', 0x3C: 'MTIR', 0x3D: 'MFIR', 0x3E: 'ILWR', 0x3F: 'ISWR', 0x40: 'RNEXT',
        0x41: 'RGET', 0x42: 'RINIT', 0x43: 'RXOR', 0x64: 'MFP', 0x68: 'XTOP', 0x69: 'XITOP', 0x6C: 'XGKICK',
        0x70: 'ESADD', 0x71: 'ERSADD', 0x72: 'ELENG', 0x73: 'ERLENG', 0x74: 'EATANxy', 0x75: 'EATANxz', 0x76: 'ESUM',
        0x78: 'ESQRT', 0x79: 'ERSQRT', 0x7A: 'ERCPR', 0x7B: 'WAITP', 0x7C: 'ESIN', 0x7D: 'EATAN', 0x7E: 'EEXP',
        0x2F: 'NOP', 0x2C: 'NOP'}


def decode_upper(u):
    f = u & 63
    if f < 0x3C:
        return T1[f]
    return T2.get((u & 3) | ((u >> 4) & 0x7C), 'INVALID')


def decode_lower(l):
    op = l >> 25
    if op != 0x40:
        return LOPS.get(op, 'INVALID')
    f = l & 63
    if f in L40:
        return L40[f]
    if f < 0x3C:
        return 'INVALID'
    n = L40X.get((l & 3) | ((l >> 4) & 0x7C))
    if n:
        return n
    return 'NOP' if l == 0x8000033C else 'INVALID'


BRANCHES = {'B', 'BAL', 'JR', 'JALR', 'IBEQ', 'IBNE', 'IBLTZ', 'IBGTZ', 'IBLEZ', 'IBGEZ'}
COND = {'IBEQ': '==', 'IBNE': '!='}
COND1 = {'IBLTZ': '< 0', 'IBGTZ': '> 0', 'IBLEZ': '<= 0', 'IBGEZ': '>= 0'}
FLAG_KIND = {'FSEQ', 'FSAND', 'FSOR', 'FMEQ', 'FMAND', 'FMOR', 'FCEQ', 'FCAND', 'FCOR', 'FCGET'}
LOAD_KIND = {'ILW', 'ILWR'}

DEST = lambda x: (x >> 21) & 15
FT = lambda x: (x >> 16) & 31
FS = lambda x: (x >> 11) & 31
FD = lambda x: (x >> 6) & 31


class Ins:
    def __init__(self, pc, lower, upper):
        self.pc, self.lower, self.upper = pc, lower, upper
        self.ibit = (upper >> 31) & 1
        self.ebit = (upper >> 30) & 1
        self.uop = decode_upper(upper)
        self.lop = 'NOP' if self.ibit else decode_lower(lower)
        if not self.ibit and lower == 0x8000033C:
            self.lop = 'NOP'


SIZE = 16384
MASK = SIZE - 8


def imm11(l):
    v = l & 0x7FF
    return v - 0x800 if v & 0x400 else v


def target_of(ins):
    return (ins.pc + 8 + imm11(ins.lower) * 8) & MASK


# ---------------------------------------------------------------------------
# code generation
# ---------------------------------------------------------------------------
REV4 = [0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15]


def blend(old, new, d):
    """old with the lanes selected by the VU dest mask d (x = bit 3) replaced by new"""
    if d == 15:
        return new
    if d == 0:
        return old
    return f'_mm_blend_ps({old}, {new}, {REV4[d]})'


# Analysis results (filled by analyze()): MAC flag pushes that no flag read can observe, and
# conditional branches whose VI operands cannot be affected by the branch hazard.
DEAD_MAC = set()      # (pc, ctx) of producers whose MAC push is skipped
PLAIN_BVI = set()     # (pc, reg) of conditional branches reading plain v.vi[reg]
USE_HIST = True       # VI write history needed at all
# Saturated shadows: local sh<r>/shacc hold vsat(v.vf[r]) / vsat(v.acc), updated at every write, so
# FMAC operands need no saturation (and stay in host registers). VALID[node] = bit mask of the
# shadows that are current when the node executes (bit 32 = ACC); dispatch entries start with none.
ACC_BIT = 32
VALID = {}
ENTRIES = set()       # pcs the dispatch switch may enter (others fall back to the interpreter)
FMAC_ACC = {'ADDA', 'SUBA', 'MADDA', 'MSUBA', 'MULA'}


def upper_base(op):
    for b_ in ('ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL', 'ADDA', 'SUBA', 'MADDA', 'MSUBA', 'MULA'):
        for suf in ('bc', 'q', 'i', ''):
            if op == b_ + suf:
                return b_
    return op


def upper_writes(x):
    """VF registers (ACC_BIT for ACC) written by the upper instruction of x"""
    op, u = x.uop, x.upper
    if op in ('NOP', 'INVALID', 'CLIP'):
        return []
    if op.startswith('ITOF') or op.startswith('FTOI') or op == 'ABS':
        return [FT(u)] if FT(u) else []
    if op == 'OPMULA' or upper_base(op) in FMAC_ACC:
        return [ACC_BIT]
    return [FD(u)] if FD(u) else []


def lower_writes(x):
    if x.ibit:
        return []
    ft = FT(x.lower)
    if x.lop in ('LQ', 'LQI', 'LQD', 'MOVE', 'MR32', 'MFIR', 'RNEXT', 'RGET', 'MFP') and ft:
        return [ft]
    return []


def shadow(valid, r):
    """saturated operand: the shadow if current, else saturate the register now"""
    return f'sh{r}' if (valid >> r) & 1 else f'vsat(v.vf[{r}].v)'


def gen_upper(ins, ctx=None, valid=1):
    """returns (pre_code, commit_code). pre runs before the lower op, commit after it."""
    u = ins.upper
    op = ins.uop
    if op in ('NOP', 'INVALID'):
        return '', ''
    d = DEST(u)
    ft, fs, fd, bc = FT(u), FS(u), FD(u), u & 3
    pre = []
    commit = []
    lw_regs = set(lower_writes(ins))
    S = shadow(valid, fs)
    T = shadow(valid, ft)
    A = 'shacc' if (valid >> ACC_BIT) & 1 else 'vsat(v.acc.v)'
    if (valid >> ft) & 1:
        B = f'_mm_shuffle_ps(sh{ft}, sh{ft}, {bc * 0x55})'
    else:
        B = f'_mm_set1_ps(inf(v.vf[{ft}].f[{bc}]))'

    def shadow_fd(reg, mask, sat_new):
        """shadow of VF reg after its write uw = blend(old, ur, mask); sat_new: ur is saturated"""
        if mask == 15 and sat_new:
            return f'sh{reg} = uw;'
        if sat_new and (valid >> reg) & 1 and reg not in lw_regs:
            return f'sh{reg} = {blend(f"sh{reg}", "ur", mask)};'
        return f'sh{reg} = vsat(uw);'

    def shadow_acc(mask):
        if mask == 15:
            return 'shacc = ua;'
        if (valid >> ACC_BIT) & 1:
            return f'shacc = {blend("shacc", "ur", mask)};'
        return 'shacc = vsat(ua);'
    I = '_mm_set1_ps(inf(v.ireg))'
    Q = '_mm_set1_ps(inf(v.q))'
    uses_q = op in ('MULq', 'ADDq', 'MADDq', 'SUBq', 'MSUBq', 'MULAq', 'ADDAq', 'MADDAq', 'SUBAq', 'MSUBAq')
    if uses_q:
        pre.append('aot_qp(v, n);')

    def to_fd(expr, flags):
        pre.append(f'__m128 ur = vsat({expr});')
        if flags and (ins.pc, ctx) not in DEAD_MAC:
            pre.append(f'aot_push_mac(v, n, mac_of(ur, {d}));')
        if fd:
            pre.append(f'__m128 uw = {blend(f"v.vf[{fd}].v", "ur", d)};')
            commit.append(f'v.vf[{fd}].v = uw;')
            commit.append(shadow_fd(fd, d, True))

    def to_acc(expr):
        pre.append(f'__m128 ur = vsat({expr});')
        if (ins.pc, ctx) not in DEAD_MAC:
            pre.append(f'aot_push_mac(v, n, mac_of(ur, {d}));')
        pre.append(f'__m128 ua = {blend("v.acc.v", "ur", d)};')
        commit.append('v.acc.v = ua;')
        commit.append(shadow_acc(d))

    two = {'ADD': '_mm_add_ps', 'SUB': '_mm_sub_ps', 'MUL': '_mm_mul_ps', 'MAX': '_mm_max_ps', 'MINI': '_mm_min_ps'}
    noflag = {'MAX', 'MINI'}
    # FMAC forms: <base><operand> with operand bc (broadcast), q, i or none (vector)
    FMAC_BASES = ('ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL', 'ADDA', 'SUBA', 'MADDA', 'MSUBA', 'MULA')
    base, other = op, T
    for b_ in FMAC_BASES:
        for suf, src in (('bc', B), ('q', Q), ('i', I), ('', T)):
            if op == b_ + suf:
                base, other = b_, src
    if base in ('ITOF0', 'ITOF4', 'ITOF12', 'ITOF15'):
        sc = {'ITOF0': '1.0f', 'ITOF4': '1.0f / 16', 'ITOF12': '1.0f / 4096', 'ITOF15': '1.0f / 32768'}[base]
        if ft:
            pre.append(f'__m128 uw = {blend(f"v.vf[{ft}].v", f"_mm_mul_ps(_mm_cvtepi32_ps(v.vf[{fs}].vi), _mm_set1_ps({sc}))", d)};')
            commit.append(f'v.vf[{ft}].v = uw;')
            commit.append(f'sh{ft} = uw;' if d == 15 and ft not in lw_regs else f'sh{ft} = vsat(uw);')  # converted ints are finite
    elif base in ('FTOI0', 'FTOI4', 'FTOI12', 'FTOI15'):
        sc = {'FTOI0': '1.0f', 'FTOI4': '16.0f', 'FTOI12': '4096.0f', 'FTOI15': '32768.0f'}[base]
        if ft:
            pre.append(f'__m128 uw = {blend(f"v.vf[{ft}].v", f"aot_ftoi({S}, {sc})", d)};')
            commit.append(f'v.vf[{ft}].v = uw;')
            commit.append(f'sh{ft} = vsat(uw);')  # integer bits
    elif base == 'ABS':
        if ft:
            pre.append(f'__m128 uw = {blend(f"v.vf[{ft}].v", f"_mm_and_ps({S}, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)))", d)};')
            commit.append(f'v.vf[{ft}].v = uw;')
            commit.append(f'sh{ft} = uw;' if d == 15 and ft not in lw_regs else f'sh{ft} = vsat(uw);')
    elif base == 'CLIP':
        pre.append(f'aot_clip(v, n, v.vf[{fs}], v.vf[{ft}]);')
    elif base in ('OPMSUB', 'OPMULA'):
        p = f'aot_outer({S}, {T})'
        if base == 'OPMSUB':
            pre.append(f'__m128 ur = vsat(_mm_sub_ps({A}, {p}));')
            if (ins.pc, ctx) not in DEAD_MAC:
                pre.append('aot_push_mac(v, n, mac_of(ur, 14));')
            if fd:
                pre.append(f'__m128 uw = {blend(f"v.vf[{fd}].v", "ur", 14)};')
                commit.append(f'v.vf[{fd}].v = uw;')
                commit.append(shadow_fd(fd, 14, True))
        else:
            pre.append(f'__m128 ur = vsat({p});')
            if (ins.pc, ctx) not in DEAD_MAC:
                pre.append('aot_push_mac(v, n, mac_of(ur, 14));')
            pre.append(f'__m128 ua = {blend("v.acc.v", "ur", 14)};')
            commit.append('v.acc.v = ua;')
            commit.append(shadow_acc(14))
    else:
        acc = base.endswith('A')
        b2 = base[:-1] if acc else base
        if b2 in ('ADD', 'SUB', 'MUL', 'MAX', 'MINI'):
            expr = f'{two[b2]}({S}, {other})'
        elif b2 == 'MADD':
            expr = f'_mm_add_ps({A}, _mm_mul_ps({S}, {other}))'
        elif b2 == 'MSUB':
            expr = f'_mm_sub_ps({A}, _mm_mul_ps({S}, {other}))'
        else:
            raise ValueError('unhandled upper op ' + op)
        if acc:
            to_acc(expr)
        else:
            to_fd(expr, b2 not in noflag)
    return '\n'.join(pre), '\n'.join(commit)


def vi_write(reg, expr, kind):
    """VI write with hazard record; returns code that sets 'hz' (new history entry)."""
    if reg == 0:
        return f'(void)({expr});'
    if not USE_HIST:
        return f'v.vi[{reg}] = ({expr}) & 0xFFFF;'
    return f'{{ u32 nv_ = ({expr}) & 0xFFFF; hz = HZ({reg}, {kind}, v.vi[{reg}]); v.vi[{reg}] = nv_; }}'


def gen_lower(ins, valid=1):
    """returns (code, commit_code, control) where control describes a branch:
       None, ('static', taken_expr, target) or ('dynamic', target_expr)"""
    l = ins.lower
    op = ins.lop
    if op == 'NOP' or ins.ibit:
        return '', '', None
    d = DEST(l)
    it, is_, id_ = FT(l) & 15, FS(l) & 15, FD(l) & 15
    ft, fs = FT(l), FS(l)
    i11 = imm11(l)
    kind = 'VIW_LOAD' if op in LOAD_KIND else 'VIW_FLAG' if op in FLAG_KIND else 'VIW_ALU'
    code, commit, ctl = [], [], None
    imm15 = (l & 0x7FF) | ((l >> 10) & 0x7800)
    imm12 = ((l >> 10) & 0x800) | (l & 0x7FF)
    if op in ('LQ', 'LQI', 'LQD'):
        if op == 'LQD':
            code.append(vi_write(is_, f'v.vi[{is_}] - 1', kind))
        addr = f'v.vi[{is_}] + ({i11})' if op == 'LQ' else f'v.vi[{is_}]'
        if ft:
            code.append(f'__m128 lw = {blend(f"v.vf[{ft}].v", f"_mm_loadu_ps(memq(v, {addr})->f)", d)};')
            commit.append(f'v.vf[{ft}].v = lw;')
        if op == 'LQI':
            code.append(vi_write(is_, f'v.vi[{is_}] + 1', kind))
    elif op in ('SQ', 'SQI', 'SQD'):
        if op == 'SQD':
            code.append(vi_write(it, f'v.vi[{it}] - 1', kind))
        addr = f'v.vi[{it}] + ({i11})' if op == 'SQ' else f'v.vi[{it}]'
        code.append(f'{{ vf128* m_ = memq(v, {addr}); _mm_storeu_ps(m_->f, {blend("_mm_loadu_ps(m_->f)", f"v.vf[{fs}].v", d)}); }}')
        if op == 'SQI':
            code.append(vi_write(it, f'v.vi[{it}] + 1', kind))
    elif op in ('ILW', 'ILWR'):
        k = 0 if d & 8 else 1 if d & 4 else 2 if d & 2 else 3
        addr = f'v.vi[{is_}] + ({i11})' if op == 'ILW' else f'v.vi[{is_}]'
        code.append(vi_write(it, f'memq(v, {addr})->u[{k}]', kind))
    elif op in ('ISW', 'ISWR'):
        addr = f'v.vi[{is_}] + ({i11})' if op == 'ISW' else f'v.vi[{is_}]'
        stores = ' '.join(f'm_->u[{k}] = v.vi[{it}] & 0xFFFF;' for k in range(4) if d & (8 >> k))
        code.append(f'{{ vf128* m_ = memq(v, {addr}); {stores} }}')
    elif op == 'IADDIU':
        code.append(vi_write(it, f'v.vi[{is_}] + {imm15}', kind))
    elif op == 'ISUBIU':
        code.append(vi_write(it, f'v.vi[{is_}] - {imm15}', kind))
    elif op == 'IADD':
        code.append(vi_write(id_, f'v.vi[{is_}] + v.vi[{it}]', kind))
    elif op == 'ISUB':
        code.append(vi_write(id_, f'v.vi[{is_}] - v.vi[{it}]', kind))
    elif op == 'IADDI':
        imm5 = (l >> 6) & 31
        if imm5 & 16:
            imm5 -= 32
        code.append(vi_write(it, f'v.vi[{is_}] + ({imm5})', kind))
    elif op == 'IAND':
        code.append(vi_write(id_, f'v.vi[{is_}] & v.vi[{it}]', kind))
    elif op == 'IOR':
        code.append(vi_write(id_, f'v.vi[{is_}] | v.vi[{it}]', kind))
    elif op in ('FCEQ', 'FCAND', 'FCOR', 'FCGET', 'FCSET', 'FSEQ', 'FSSET', 'FSAND', 'FSOR', 'FMEQ', 'FMAND', 'FMOR'):
        code.append('aot_sync(v, n);')
        imm24 = l & 0xFFFFFF
        if op == 'FCEQ':
            code.append(vi_write(1, f'(v.clip & 0xFFFFFF) == 0x{imm24:x}u', kind))
        elif op == 'FCSET':
            code.append(f'v.clip = v.clip_issued = 0x{imm24:x}u;')
        elif op == 'FCAND':
            code.append(vi_write(1, f'(v.clip & 0x{imm24:x}u) != 0', kind))
        elif op == 'FCOR':
            code.append(vi_write(1, f'((v.clip | 0x{imm24:x}u) & 0xFFFFFF) == 0xFFFFFF', kind))
        elif op == 'FCGET':
            code.append(vi_write(it, 'v.clip & 0xFFF', kind))
        elif op == 'FSEQ':
            code.append(vi_write(it, f'((v.status | v.status_sticky) & 0xFFF) == {imm12}u', kind))
        elif op == 'FSSET':
            code.append(f'v.status_sticky = {imm12 & 0xFC0}u;')
        elif op == 'FSAND':
            code.append(vi_write(it, f'(v.status | v.status_sticky) & {imm12}u', kind))
        elif op == 'FSOR':
            code.append(vi_write(it, f'(v.status | v.status_sticky) | {imm12}u', kind))
        elif op == 'FMEQ':
            code.append(vi_write(it, f'(v.mac & 0xFFFF) == (v.vi[{is_}] & 0xFFFF)', kind))
        elif op == 'FMAND':
            code.append(vi_write(it, f'v.mac & v.vi[{is_}]', kind))
        elif op == 'FMOR':
            code.append(vi_write(it, f'v.mac | v.vi[{is_}]', kind))
    elif op == 'B':
        ctl = ('static', 'true', target_of(ins))
    elif op == 'BAL':
        code.append(vi_write(it, f'{(ins.pc + 16) // 8}', kind))
        ctl = ('static', 'true', target_of(ins))
    elif op == 'JR':
        code.append(f'u32 jt_ = (v.vi[{is_}] * 8) & {MASK};')
        ctl = ('dynamic', 'jt_')
    elif op == 'JALR':
        code.append(f'u32 jt_ = (v.vi[{is_}] * 8) & {MASK};')
        code.append(vi_write(it, f'{(ins.pc + 16) // 8}', kind))
        ctl = ('dynamic', 'jt_')
    elif op in COND or op in COND1:
        def bvi(r):
            if r == 0:
                return '0'
            if (ins.pc, r) in PLAIN_BVI:
                return f'v.vi[{r}]'
            return f'aot_bvi(v, {r}, h0, h1, h2, h3)'
        a = f'(s16){bvi(is_)}'
        if op in COND:
            b = f'(s16){bvi(it)}'
            cond = f'{a} {COND[op]} {b}'
        else:
            cond = f'{a} {COND1[op]}'
        code.append(f'bool tk_ = {cond};')
        ctl = ('static', 'tk_', target_of(ins))
    elif op == 'MOVE':
        if ft:
            code.append(f'__m128 lw = {blend(f"v.vf[{ft}].v", f"v.vf[{fs}].v", d)};')
            commit.append(f'v.vf[{ft}].v = lw;')
    elif op == 'MR32':
        if ft:
            code.append(f'__m128 lw = {blend(f"v.vf[{ft}].v", f"_mm_shuffle_ps(v.vf[{fs}].v, v.vf[{fs}].v, _MM_SHUFFLE(0, 3, 2, 1))", d)};')
            commit.append(f'v.vf[{ft}].v = lw;')
    elif op in ('DIV', 'SQRT', 'RSQRT'):
        fsf, ftf = (l >> 21) & 3, (l >> 23) & 3
        code.append(f'aot_qp(v, n); aot_div(v, n, {"0" if op == "DIV" else "1" if op == "SQRT" else "2"}, v.vf[{fs}].f[{fsf}], v.vf[{ft}].f[{ftf}]);')
    elif op == 'WAITQ':
        code.append('aot_qp(v, n); if (v.q_busy) { v.q = v.q_pending; v.q_busy = false; }')
    elif op == 'MTIR':
        fsf = (l >> 21) & 3
        code.append(vi_write(it, f'v.vf[{fs}].u[{fsf}] & 0xFFFF', kind))
    elif op == 'MFIR':
        if ft:
            code.append(f'__m128 lw = {blend(f"v.vf[{ft}].v", f"_mm_castsi128_ps(_mm_set1_epi32((s32)(s16)v.vi[{is_}]))", d)};')
            commit.append(f'v.vf[{ft}].v = lw;')
    elif op in ('RNEXT', 'RGET'):
        if op == 'RNEXT':
            code.append('{ u32 x_ = v.r & 0x7FFFFF; int b_ = ((x_ >> 4) ^ (x_ >> 22)) & 1; v.r = ((x_ << 1) | b_) & 0x7FFFFF; }')
        if ft:
            code.append(f'__m128 lw = {blend(f"v.vf[{ft}].v", "_mm_castsi128_ps(_mm_set1_epi32((int)(0x3F800000u | (v.r & 0x7FFFFF))))", d)};')
            commit.append(f'v.vf[{ft}].v = lw;')
    elif op == 'RINIT':
        code.append(f'v.r = v.vf[{fs}].u[{(l >> 21) & 3}] & 0x7FFFFF;')
    elif op == 'RXOR':
        code.append(f'v.r = (v.r ^ v.vf[{fs}].u[{(l >> 21) & 3}]) & 0x7FFFFF;')
    elif op == 'MFP':
        code.append('aot_qp(v, n);')
        if ft:
            code.append(f'__m128 lw = {blend(f"v.vf[{ft}].v", "_mm_set1_ps(v.p)", d)};')
            commit.append(f'v.vf[{ft}].v = lw;')
    elif op == 'XTOP':
        code.append(vi_write(it, 'v.top', kind))
    elif op == 'XITOP':
        code.append(vi_write(it, 'v.itop', kind))
    elif op == 'XGKICK':
        code.append(f'xgkick(v, v.vi[{is_}]);')
    elif op == 'WAITP':
        code.append('aot_qp(v, n); if (v.p_busy) { v.p = v.p_pending; v.p_busy = false; }')
    elif op in ('ESADD', 'ERSADD', 'ELENG', 'ERLENG', 'ESUM', 'ESQRT', 'ERSQRT', 'ERCPR', 'ESIN', 'EATAN', 'EEXP', 'EATANxy', 'EATANxz'):
        code.append(f'aot_qp(v, n); aot_efu(v, n, EFU_{op}, v.vf[{fs}], {(l >> 21) & 3});')
    elif op == 'INVALID':
        code.append(f'/* invalid lower op {l:08x} */')
    else:
        raise ValueError('unhandled lower op ' + op)
    # saturated shadow of a written VF register (see VALID)
    if ft and op in ('LQ', 'LQI', 'LQD', 'MOVE', 'MR32', 'MFIR', 'RNEXT', 'RGET', 'MFP'):
        if op == 'MOVE' and d == 15 and (valid >> fs) & 1:
            commit.append(f'sh{ft} = sh{fs};')
        elif op == 'MR32' and d == 15 and (valid >> fs) & 1:
            commit.append(f'sh{ft} = _mm_shuffle_ps(sh{fs}, sh{fs}, _MM_SHUFFLE(0, 3, 2, 1));')
        elif op in ('MFP', 'RNEXT', 'RGET') and d == 15:
            commit.append(f'sh{ft} = lw;')  # P is clamped, R values lie in [1, 2)
        else:
            commit.append(f'sh{ft} = vsat(lw);')
    return '\n'.join(code), '\n'.join(commit), ctl


def gen_ins(ins, label=True, ctx=None):
    """code for one instruction (without control transfer); returns (code, control)"""
    valid = VALID.get((ins.pc, ctx), 1)
    up_pre, up_commit = gen_upper(ins, ctx, valid)
    lo, lo_commit, ctl = gen_lower(ins, valid)
    out = []
    out.append('++n; hz = HZ_NONE;' if USE_HIST else '++n;')
    if up_pre:
        out.append(up_pre)
    if ins.ibit:
        out.append(f'{{ u32 i_ = 0x{ins.lower:08x}u; memcpy(&v.ireg, &i_, 4); }}')
    if lo:
        out.append(lo)
    if lo_commit:
        out.append(lo_commit)
    if up_commit:
        out.append(up_commit)
    if USE_HIST:
        out.append('h3 = h2; h2 = h1; h1 = h0; h0 = hz;')
    return '{\n' + '\n'.join(out) + '\n}', ctl



# ---------------------------------------------------------------------------
# control flow analysis
# ---------------------------------------------------------------------------
# A node is an instruction executed in a context: None (normal), ('ds', b) as the delay slot of the
# branch at b, ('end', e) as the last instruction after the E bit at e. Successors are nodes or the
# markers 'END' (program stops, pending flags are committed) and 'DYN' (JR/JALR: unknown target).
NO_MAC = {'NOP', 'INVALID', 'ITOF0', 'ITOF4', 'ITOF12', 'ITOF15', 'FTOI0', 'FTOI4', 'FTOI12', 'FTOI15', 'ABS', 'CLIP',
          'MAX', 'MAXbc', 'MAXi', 'MINI', 'MINIbc', 'MINIi'}
MAC_READERS = {'FMAND', 'FMEQ', 'FMOR'}
STATUS_USERS = {'FSAND', 'FSEQ', 'FSOR', 'FSSET'}


def pushes_mac(x):
    return x.uop not in NO_MAC


def branch_targets(x):
    """static targets of the branch x (list), or None for JR/JALR"""
    if x.lop in ('JR', 'JALR'):
        return None
    if x.lop in ('B', 'BAL'):
        return [target_of(x)]
    return [target_of(x), (x.pc + 16) & MASK]


def successors(by_pc, node):
    pc, ctx = node
    x = by_pc[pc]
    if ctx is not None and ctx[0] == 'end':
        return ['END']
    if ctx is not None and ctx[0] == 'ds':
        b = by_pc[ctx[1]]
        if b.ebit:
            return ['END']
        t = branch_targets(b)
        return ['DYN'] if t is None else [(a, None) for a in t]
    if x.lop in BRANCHES:
        return [((pc + 8) & MASK, ('ds', pc))]
    if x.ebit:
        return [((pc + 8) & MASK, ('end', pc))]
    return [((pc + 8) & MASK, None)]


def vi_writes(x):
    """(reg, kind) of the VI register written by the lower instruction of x"""
    if x.ibit or x.lop in ('NOP', 'INVALID'):
        return []
    l = x.lower
    it, is_, id_ = FT(l) & 15, FS(l) & 15, FD(l) & 15
    op = x.lop
    kind = 'LOAD' if op in LOAD_KIND else 'FLAG' if op in FLAG_KIND else 'ALU'
    reg = None
    if op in ('LQI', 'LQD'):
        reg = is_
    elif op in ('SQI', 'SQD', 'ILW', 'ILWR', 'IADDIU', 'ISUBIU', 'IADDI', 'BAL', 'JALR', 'MTIR', 'XTOP', 'XITOP',
                'FCGET', 'FSEQ', 'FSAND', 'FSOR', 'FMEQ', 'FMAND', 'FMOR'):
        reg = it
    elif op in ('IADD', 'ISUB', 'IAND', 'IOR'):
        reg = id_
    elif op in ('FCEQ', 'FCAND', 'FCOR'):
        reg = 1
    return [(reg, kind)] if reg else []


def analyze(ins, by_pc, bad):
    global USE_HIST
    nodes = [(x.pc, None) for x in ins if x.pc not in bad]
    for x in ins:
        if x.pc in bad:
            continue
        if x.lop in BRANCHES:
            nodes.append(((x.pc + 8) & MASK, ('ds', x.pc)))
        elif x.ebit:
            nodes.append(((x.pc + 8) & MASK, ('end', x.pc)))
    succ = {n: successors(by_pc, n) for n in nodes}

    # MAC flag liveness: a push is observable if some path reaches a MAC read at least 3 instructions
    # later before a later push became visible (3 instructions after it), the program end while it is
    # the latest push (flush commits everything), or an unknown jump target.
    status_used = any(x.lop in STATUS_USERS and not x.ibit for x in ins if x.pc not in bad)
    dead = 0
    total = 0
    for n in nodes:
        x = by_pc[n[0]]
        if not pushes_mac(x):
            continue
        total += 1
        if status_used:
            continue
        live = False
        stack = [(m, 0, None) for m in succ[n]]
        seen = set()
        while stack and not live:
            m, dp, kf = stack.pop()
            if m == 'END':
                live = kf is None
                continue
            if m == 'DYN' or m not in succ:
                live = True
                continue
            key = (m, dp, kf)
            if key in seen:
                continue
            seen.add(key)
            y = by_pc[m[0]]
            if not y.ibit and y.lop in MAC_READERS and dp >= 3:
                live = True
                continue
            nkf = 0 if (kf is None and pushes_mac(y)) else (kf + 1 if kf is not None else None)
            if nkf is not None and nkf >= 3:
                continue
            for q in succ[m]:
                stack.append((q, min(dp + 1, 3), nkf))
        if not live:
            DEAD_MAC.add(n)
            dead += 1

    # branch hazard: a conditional branch reads an old VI value only if the instruction executed right
    # before it wrote that register with an ALU-type write
    preds = {n: [] for n in nodes}
    dyn_sources = []
    for n, ss in succ.items():
        for m in ss:
            if m == 'DYN':
                dyn_sources.append(n)
            elif m != 'END' and m in preds:
                preds[m].append(n)
    hist_needed = 0
    plain = 0
    for x in ins:
        if x.pc in bad or x.ibit or not (x.lop in COND or x.lop in COND1):
            continue
        regs = [FS(x.lower) & 15]
        if x.lop in COND:
            regs.append(FT(x.lower) & 15)
        bnodes = [n for n in nodes if n[0] == x.pc]
        pre = []
        for bn in bnodes:
            pre += preds[bn]
            if bn[1] is None:
                pre += dyn_sources
        for r in regs:
            if r == 0:
                continue
            if any((r, 'ALU') in vi_writes(by_pc[q[0]]) for q in pre):
                hist_needed += 1
            else:
                PLAIN_BVI.add((x.pc, r))
                plain += 1
    USE_HIST = hist_needed > 0

    # Dispatch entries: the program's entry table (no FMAC work happens there; the runtime entries
    # seen all lie below 0x300), return points of BAL/JALR (JR targets), continuations after an
    # E bit, and JALR targets loaded as constants (IADDIU vi, vi0, imm right before the JALR).
    unresolved = 0
    for x in ins:
        if x.pc in bad:
            continue
        if x.pc < 0x300:
            ENTRIES.add(x.pc)
        if x.lop in ('BAL', 'JALR') or x.ebit:
            ENTRIES.add((x.pc + 16) & MASK)
        if x.lop == 'JALR':
            reg = FS(x.lower) & 15
            found = None
            for k in range(1, 16):
                y = by_pc[(x.pc - 8 * k) & MASK]
                if y.lop in BRANCHES:
                    break
                if any(r == reg for r, _ in vi_writes(y)):
                    if y.lop == 'IADDIU' and (FS(y.lower) & 15) == 0:
                        found = (((y.lower & 0x7FF) | ((y.lower >> 10) & 0x7800)) * 8) & MASK
                    break
            if found is None:
                unresolved += 1
            else:
                ENTRIES.add(found)
    # shadow validity: forward "must" analysis over the instruction graph (writes make shadows valid)
    ALL = (1 << 33) - 1
    writes = {}
    for n in nodes:
        x = by_pc[n[0]]
        m = 0
        for r in upper_writes(x) + lower_writes(x):
            m |= 1 << r
        writes[n] = m
    base_in = {n: (1 if (n[1] is None and n[0] in ENTRIES) else ALL) for n in nodes}
    vin = dict(base_in)
    work = list(nodes)
    inwork = set(work)
    while work:
        n = work.pop()
        inwork.discard(n)
        out = vin[n] | writes[n]
        for m in succ[n]:
            if m in ('END', 'DYN') or m not in vin:
                continue
            nv = vin[m] & out
            if nv != vin[m]:
                vin[m] = nv
                if m not in inwork:
                    work.append(m)
                    inwork.add(m)
    VALID.update(vin)
    return (f'MAC pushes {total}, dead {dead}; branch VI reads plain {plain}, with hazard {hist_needed}; '
            f'dispatch entries {len(ENTRIES)}, unresolved JALR {unresolved}')


def main():
    data = open(sys.argv[1], 'rb').read()
    assert len(data) == SIZE
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    ins = []
    for i in range(SIZE // 8):
        lo, up = struct.unpack_from('<II', data, i * 8)
        ins.append(Ins(i * 8, lo, up))
    by_pc = {x.pc: x for x in ins}

    unsupported = []
    for x in ins:
        if x.lop in BRANCHES:
            ds = by_pc[(x.pc + 8) & MASK]
            if ds.lop in BRANCHES:
                unsupported.append(f'branch in delay slot at {x.pc:04x}')
            if ds.ebit:
                unsupported.append(f'end bit in delay slot of branch at {x.pc:04x}')
        if x.ebit:
            nx = by_pc[(x.pc + 8) & MASK]
            if nx.ebit:
                unsupported.append(f'consecutive end bits at {x.pc:04x}')
    # these patterns can only occur in unreachable data; their labels fall back to the interpreter
    bad = set()
    for msg in unsupported:
        bad.add(int(msg.split()[-1], 16))

    info = analyze(ins, by_pc, bad)
    print(info)
    out = []
    w = out.append
    w('// Generated by tools/vu_recomp.py - do not edit.')
    w(f'// {info}')
    w(f'// VU1 micro image {h:016x}')
    w('#include "vu/vu_aot.h"')
    w('namespace vu {')
    w(f'static bool aot_{h:016x}(VUState& v) {{')
    w('    u64 n = v.cycle;')
    w('    const u64 n0 = n;')
    w('    u32 h0 = HZ_NONE, h1 = HZ_NONE, h2 = HZ_NONE, h3 = HZ_NONE, hz = HZ_NONE;')
    w('    u64 loops = 0;')
    w('    u32 pc = v.pc;')
    w('    (void)hz;')
    w('    __m128 sh0 = v.vf[0].v, ' + ', '.join(f'sh{r} = sh0' for r in range(1, 32)) + ', shacc = sh0;')
    w('dispatch:')
    w('    switch (pc) {')
    for x in ins:
        if x.pc in bad or x.pc not in ENTRIES:
            continue
        w(f'    case 0x{x.pc:04x}: goto L{x.pc:04x};')
    # other targets (no dispatch entry): the interpreter continues from there
    w('    default: v.pc = pc; v.cycle = n; g_vu_instrs += n - n0; return false;')
    w('    }')

    def emit_transfer(ctl, after_pc, from_pc):
        """control transfer after the delay slot of a branch at from_pc"""
        lines = []
        nxt = (after_pc) & MASK
        if ctl[0] == 'static':
            tgt = ctl[2]
            back = tgt <= from_pc
            guard = 'if (++loops > 50000000) { pc = 0x%04x; goto done; } ' % tgt if back else ''
            if ctl[1] == 'true':
                lines.append(f'{guard}goto L{tgt:04x};')
            else:
                lines.append(f'if ({ctl[1]}) {{ {guard}goto L{tgt:04x}; }}')
                lines.append(f'goto L{nxt:04x};')
        else:
            lines.append(f'pc = {ctl[1]}; if (++loops > 50000000) goto done; goto dispatch;')
        return lines

    for x in ins:
        if x.pc in bad:
            continue
        w(f'L{x.pc:04x}: // {x.uop} / {x.lop}{" [E]" if x.ebit else ""}{" [I]" if x.ibit else ""}')
        code, ctl = gen_ins(x)
        nx = by_pc[(x.pc + 8) & MASK]
        if ctl is None and not x.ebit:
            w(code)
            continue
        # needs a block scope so branch locals (tk_, jt_) are visible for the transfer
        w('{')
        w(code[1:-1])  # strip braces: keep locals in this scope
        if ctl is not None:
            ds_code, ds_ctl = gen_ins(nx, ctx=('ds', x.pc))
            w(ds_code)
            if x.ebit:
                # branch with end bit: delay slot, then stop at the branch destination
                if ctl[0] == 'static':
                    tgt = ctl[2]
                    if ctl[1] == 'true':
                        w(f'pc = 0x{tgt:04x}; goto done;')
                    else:
                        w(f'pc = {ctl[1]} ? 0x{tgt:04x} : 0x{(x.pc + 16) & MASK:04x}; goto done;')
                else:
                    w(f'pc = {ctl[1]}; goto done;')
            elif nx.ebit:
                raise ValueError('unexpected')
            else:
                for line in emit_transfer(ctl, x.pc + 16, x.pc):
                    w(line)
        else:
            # end bit: one more instruction, then stop
            es_code, es_ctl = gen_ins(nx, ctx=('end', x.pc))
            w(es_code)
            w(f'pc = 0x{(x.pc + 16) & MASK:04x}; goto done;')
        w('}')
    w('done:')
    w('    v.pc = pc;')
    w('    g_vu_instrs += n - n0;')
    w('    v.cycle = n;')
    w('    flush_pipeline(v);')
    w('    return true;')
    w('}')
    w(f'static int reg_ = register_aot(0x{h:016x}ull, aot_{h:016x});')
    w('}  // namespace vu')
    open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
    print(f'image {h:016x}: {len(ins)} instructions, {len(bad)} unsupported labels -> {sys.argv[2]}')
    for msg in unsupported[:20]:
        print('  note:', msg)


if __name__ == '__main__':
    main()
