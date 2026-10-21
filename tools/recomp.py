"""R5900 -> C static recompiler.

Generates one C function per guest function. Generated code relies on
runtime/include/ee_recomp.h for the context struct, memory access and
instruction helpers.
"""
import os
import struct
from r5900 import GPR, disasm, decode

FUNCS_PER_FILE = 150


def gpr(n):
    return f'c->r[{n}]'


def U64(n):
    return '0ull' if n == 0 else f'c->r[{n}].ud[0]'


def S64(n):
    return '0ll' if n == 0 else f'c->r[{n}].sd[0]'


def U32(n):
    return '0u' if n == 0 else f'c->r[{n}].uw[0]'


def S32(n):
    return '0' if n == 0 else f'c->r[{n}].sw[0]'


def set32(n, expr):
    if n == 0:
        return f'(void)({expr});'
    return f'c->r[{n}].sd[0] = (int64_t)(int32_t)({expr});'


def set64(n, expr):
    if n == 0:
        return f'(void)({expr});'
    return f'c->r[{n}].ud[0] = (uint64_t)({expr});'


def hx(v):
    return f'0x{v & 0xFFFFFFFF:08X}u'


class FuncResolver:
    """Resolves call targets to C symbol names."""

    def __init__(self):
        self.local = {}      # addr -> name (functions in modules compiled together/linked statically)
        self.hle = {}        # addr -> native function name (replaces guest function)
        self.hooks = {}      # addr -> native function called instead (it calls the recompiled body itself)

    def name_for(self, addr):
        """symbol that callers (and the function table) use"""
        if addr in self.hle:
            return self.hle[addr]
        if addr in self.hooks:
            return self.hooks[addr]
        return self.local.get(addr)

    def body_name(self, addr, prefix):
        """symbol of the recompiled function body"""
        if addr in self.hle:
            return self.hle[addr]
        return self.local.get(addr) or f'{prefix}_{addr:08X}'


class Recompiler:
    def __init__(self, mod, funcs, resolver, prefix, config=None):
        self.mod = mod
        self.funcs = funcs
        self.resolver = resolver
        self.prefix = prefix
        self.config = config or {}
        self.func_by_start = {f.start: f for f in funcs}
        self.warnings = []
        # optional instruction patches of this module: addr -> [(flag index, replacement word)], several
        # variants of one address are tried highest index first (e.g. 16:10 before 16:9 widescreen)
        self.patches = {}
        for pname, pd in self.config.get('patches', {}).items():
            if pd.get('module', 'main') != getattr(mod, 'name', 'main') and not (pd.get('module') == 'main' and prefix == 'ee'):
                continue
            for a, w in pd['words'].items():
                addr = int(a, 16)
                exp = pd.get('expect', {}).get(a)
                if exp is not None and mod.word(addr) != int(exp, 16):
                    raise ValueError(f'patch {pname}: {addr:08x} holds {mod.word(addr):08x}, expected {exp}')
                self.patches.setdefault(addr, []).append((pd['index'], int(w, 16)))
        for v in self.patches.values():
            v.sort(key=lambda e: -e[0])
        # busy-wait loops (config "idle_loops", by loop start): the backward branch lets the host
        # thread sleep until the next event instead of spinning (ee_idle_loop)
        self.idle_loops = set()
        for a, d in self.config.get('idle_loops', {}).items():
            if d.get('module', 'main') == getattr(mod, 'name', 'main') or (d.get('module', 'main') == 'main' and prefix == 'ee'):
                self.idle_loops.add(int(a, 16))

    # ------------------------------------------------------------------
    def addr_expr(self, i):
        if i.rs == 0:
            return hx(i.imm)
        if i.imm == 0:
            return f'{U32(i.rs)}'
        return f'({U32(i.rs)} + {hx(i.imm)})'

    def call_code(self, target, f):
        name = self.resolver.name_for(target)
        if name:
            return f'{name}(c);'
        return f'ee_call(c, {hx(target)});'

    # ------------------------------------------------------------------
    def emit_insn(self, i, f):
        """Emit C code for a non-control-flow instruction. Returns list of lines."""
        op = i.op
        rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
        imm = i.imm
        L = []
        a = L.append
        if op == 'nop' or op in ('sync', 'cache', 'pref'):
            return L
        # ---- ALU immediate
        if op in ('addi', 'addiu'):
            a(set32(rt, f'{U32(rs)} + {hx(imm)}'))
        elif op in ('daddi', 'daddiu'):
            a(set64(rt, f'{U64(rs)} + (uint64_t)(int64_t){imm}'))
        elif op == 'slti':
            a(set64(rt, f'{S64(rs)} < {imm}ll'))
        elif op == 'sltiu':
            a(set64(rt, f'{U64(rs)} < (uint64_t)(int64_t){imm}'))
        elif op == 'andi':
            a(set64(rt, f'{U64(rs)} & {i.uimm}ull'))
        elif op == 'ori':
            a(set64(rt, f'{U64(rs)} | {i.uimm}ull'))
        elif op == 'xori':
            a(set64(rt, f'{U64(rs)} ^ {i.uimm}ull'))
        elif op == 'lui':
            a(set64(rt, f'(int64_t)(int32_t){hx(i.uimm << 16)}'))
        # ---- ALU register
        elif op in ('add', 'addu'):
            a(set32(rd, f'{U32(rs)} + {U32(rt)}'))
        elif op in ('sub', 'subu'):
            a(set32(rd, f'{U32(rs)} - {U32(rt)}'))
        elif op in ('dadd', 'daddu'):
            a(set64(rd, f'{U64(rs)} + {U64(rt)}'))
        elif op in ('dsub', 'dsubu'):
            a(set64(rd, f'{U64(rs)} - {U64(rt)}'))
        elif op == 'and':
            a(set64(rd, f'{U64(rs)} & {U64(rt)}'))
        elif op == 'or':
            a(set64(rd, f'{U64(rs)} | {U64(rt)}'))
        elif op == 'xor':
            a(set64(rd, f'{U64(rs)} ^ {U64(rt)}'))
        elif op == 'nor':
            a(set64(rd, f'~({U64(rs)} | {U64(rt)})'))
        elif op == 'slt':
            a(set64(rd, f'{S64(rs)} < {S64(rt)}'))
        elif op == 'sltu':
            a(set64(rd, f'{U64(rs)} < {U64(rt)}'))
        elif op == 'movz':
            if rd:
                a(f'if ({U64(rt)} == 0) {U64(rd)} = {U64(rs)};')
        elif op == 'movn':
            if rd:
                a(f'if ({U64(rt)} != 0) {U64(rd)} = {U64(rs)};')
        # ---- shifts
        elif op == 'sll':
            a(set32(rd, f'{U32(rt)} << {sa}'))
        elif op == 'srl':
            a(set32(rd, f'{U32(rt)} >> {sa}'))
        elif op == 'sra':
            a(set32(rd, f'{S32(rt)} >> {sa}'))
        elif op == 'sllv':
            a(set32(rd, f'{U32(rt)} << ({U32(rs)} & 31)'))
        elif op == 'srlv':
            a(set32(rd, f'{U32(rt)} >> ({U32(rs)} & 31)'))
        elif op == 'srav':
            a(set32(rd, f'{S32(rt)} >> ({U32(rs)} & 31)'))
        elif op == 'dsll':
            a(set64(rd, f'{U64(rt)} << {sa}'))
        elif op == 'dsrl':
            a(set64(rd, f'{U64(rt)} >> {sa}'))
        elif op == 'dsra':
            a(set64(rd, f'{S64(rt)} >> {sa}'))
        elif op == 'dsll32':
            a(set64(rd, f'{U64(rt)} << {sa + 32}'))
        elif op == 'dsrl32':
            a(set64(rd, f'{U64(rt)} >> {sa + 32}'))
        elif op == 'dsra32':
            a(set64(rd, f'{S64(rt)} >> {sa + 32}'))
        elif op == 'dsllv':
            a(set64(rd, f'{U64(rt)} << ({U32(rs)} & 63)'))
        elif op == 'dsrlv':
            a(set64(rd, f'{U64(rt)} >> ({U32(rs)} & 63)'))
        elif op == 'dsrav':
            a(set64(rd, f'{S64(rt)} >> ({U32(rs)} & 63)'))
        # ---- mult/div
        elif op in ('mult', 'multu', 'mult1', 'multu1', 'madd', 'maddu', 'madd1', 'maddu1',
                    'div', 'divu', 'div1', 'divu1'):
            a(f'ee_{op}(c, {rd}, {rs}, {rt});')
        elif op == 'mfhi':
            a(set64(rd, 'c->hi.ud[0]'))
        elif op == 'mflo':
            a(set64(rd, 'c->lo.ud[0]'))
        elif op == 'mfhi1':
            a(set64(rd, 'c->hi.ud[1]'))
        elif op == 'mflo1':
            a(set64(rd, 'c->lo.ud[1]'))
        elif op == 'mthi':
            a(f'c->hi.ud[0] = {U64(rs)};')
        elif op == 'mtlo':
            a(f'c->lo.ud[0] = {U64(rs)};')
        elif op == 'mthi1':
            a(f'c->hi.ud[1] = {U64(rs)};')
        elif op == 'mtlo1':
            a(f'c->lo.ud[1] = {U64(rs)};')
        elif op == 'mfsa':
            a(set64(rd, 'c->sa'))
        elif op == 'mtsa':
            a(f'c->sa = {U32(rs)} & 0xF;')
        elif op == 'mtsab':
            a(f'c->sa = (({U32(rs)} & 0xF) ^ ({imm} & 0xF));')
        elif op == 'mtsah':
            a(f'c->sa = ((({U32(rs)} & 0x7) ^ ({imm} & 0x7)) << 1);')
        # ---- loads
        elif op in ('lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'ld'):
            addr = self.addr_expr(i)
            if op == 'lb':
                e = f'(int64_t)(int8_t)ee_rd8({addr})'
            elif op == 'lbu':
                e = f'(uint64_t)ee_rd8({addr})'
            elif op == 'lh':
                e = f'(int64_t)(int16_t)ee_rd16({addr})'
            elif op == 'lhu':
                e = f'(uint64_t)ee_rd16({addr})'
            elif op == 'lw':
                e = f'(int64_t)(int32_t)ee_rd32({addr})'
            elif op == 'lwu':
                e = f'(uint64_t)ee_rd32({addr})'
            else:
                e = f'ee_rd64({addr})'
            a(set64(rt, e))
        elif op == 'lq':
            addr = self.addr_expr(i)
            if rt:
                a(f'ee_rd128({addr}, &c->r[{rt}]);')
            else:
                a(f'ee_rd128({addr}, &c->scratch);')
        elif op in ('lwl', 'lwr', 'ldl', 'ldr'):
            a(f'ee_{op}(c, {rt}, {self.addr_expr(i)});')
        elif op == 'lwc1':
            a(f'c->fpr[{rt}].u = ee_rd32({self.addr_expr(i)});')
        elif op == 'lqc2':
            if rt:
                a(f'ee_rd128({self.addr_expr(i)}, (r128*)&c->vf[{rt}]);')
            else:
                a(f'ee_rd128({self.addr_expr(i)}, &c->scratch);')
        # ---- stores
        elif op == 'sb':
            a(f'ee_wr8({self.addr_expr(i)}, (uint8_t){U32(rt)});')
        elif op == 'sh':
            a(f'ee_wr16({self.addr_expr(i)}, (uint16_t){U32(rt)});')
        elif op == 'sw':
            a(f'ee_wr32({self.addr_expr(i)}, {U32(rt)});')
        elif op == 'sd':
            a(f'ee_wr64({self.addr_expr(i)}, {U64(rt)});')
        elif op == 'sq':
            a(f'ee_wr128({self.addr_expr(i)}, &c->r[{rt}]);')
        elif op in ('swl', 'swr', 'sdl', 'sdr'):
            a(f'ee_{op}(c, {rt}, {self.addr_expr(i)});')
        elif op == 'swc1':
            a(f'ee_wr32({self.addr_expr(i)}, c->fpr[{rt}].u);')
        elif op == 'sqc2':
            a(f'ee_wr128({self.addr_expr(i)}, (r128*)&c->vf[{rt}]);')
        # ---- system
        elif op == 'syscall':
            a(f'ee_syscall(c, {hx(i.pc)});')
        elif op == 'break':
            a(f'ee_break(c, {hx(i.pc)});')
        elif op in ('tge', 'tgeu', 'tlt', 'tltu', 'teq', 'tne', 'tgei', 'tgeiu', 'tlti', 'tltiu', 'teqi', 'tnei'):
            a(f'/* trap {op} ignored */')
        # ---- COP0
        elif op == 'mfc0':
            a(set32(rt, f'ee_mfc0(c, {rd})'))
        elif op == 'mtc0':
            a(f'ee_mtc0(c, {rd}, {U32(rt)});')
        elif op == 'ei':
            a('ee_ei(c);')
        elif op == 'di':
            a('ee_di(c);')
        elif op == 'eret':
            a(f'ee_eret(c, {hx(i.pc)}); return;')
        elif op in ('tlbr', 'tlbwi', 'tlbwr', 'tlbp'):
            a(f'/* {op} ignored */')
        # ---- FPU
        elif i.kind == 'fpu':
            a(self.emit_fpu(i))
        elif i.kind == 'cop2':
            if op == 'qmfc2':
                if rt:
                    a(f'c->r[{rt}].v = c->vf[{rd}].vi;')
            elif op == 'qmtc2':
                if rd:
                    a(f'c->vf[{rd}].vi = c->r[{rt}].v;')
            elif op == 'cfc2':
                a(set32(rt, f'ee_cfc2(c, {rd})'))
            elif op == 'ctc2':
                a(f'ee_ctc2(c, {rd}, {U32(rt)});')
            else:
                a(f'#error unhandled cop2 {op}')
        elif i.kind == 'vu0':
            a(self.emit_vu0(i))
        elif i.kind == 'mmi':
            a(f'mmi_{op}(c, {rd}, {rs}, {rt}, {sa});')
        elif op == 'invalid':
            a(f'ee_invalid(c, {hx(i.pc)}, {hx(i.word)});')
        else:
            a(f'#error unhandled op {op} at {i.pc:08x}')
        return L

    def emit_fpu(self, i):
        op = i.op
        fs, ft, fd = i.fs, i.ft, i.fd
        F = lambda n: f'c->fpr[{n}].f'
        if op == 'mfc1':
            return set32(i.rt, f'c->fpr[{fs}].u')
        if op == 'mtc1':
            return f'c->fpr[{fs}].u = {U32(i.rt)};'
        if op == 'cfc1':
            return set32(i.rt, f'ee_cfc1(c, {fs})')
        if op == 'ctc1':
            return f'ee_ctc1(c, {fs}, {U32(i.rt)});'
        if op == 'add.s':
            return f'{F(fd)} = fpu_add({F(fs)}, {F(ft)});'
        if op == 'sub.s':
            return f'{F(fd)} = fpu_sub({F(fs)}, {F(ft)});'
        if op == 'mul.s':
            return f'{F(fd)} = fpu_mul({F(fs)}, {F(ft)});'
        if op == 'div.s':
            return f'{F(fd)} = fpu_div(c, {F(fs)}, {F(ft)});'
        if op == 'sqrt.s':
            return f'{F(fd)} = fpu_sqrt(c, {F(ft)});'
        if op == 'rsqrt.s':
            return f'{F(fd)} = fpu_rsqrt(c, {F(fs)}, {F(ft)});'
        if op == 'abs.s':
            return f'c->fpr[{fd}].u = c->fpr[{fs}].u & 0x7FFFFFFFu;'
        if op == 'neg.s':
            return f'c->fpr[{fd}].u = c->fpr[{fs}].u ^ 0x80000000u;'
        if op == 'mov.s':
            return f'c->fpr[{fd}].u = c->fpr[{fs}].u;'
        if op == 'adda.s':
            return f'c->facc = fpu_add({F(fs)}, {F(ft)});'
        if op == 'suba.s':
            return f'c->facc = fpu_sub({F(fs)}, {F(ft)});'
        if op == 'mula.s':
            return f'c->facc = fpu_mul({F(fs)}, {F(ft)});'
        if op == 'madd.s':
            return f'{F(fd)} = fpu_add(c->facc, fpu_mul({F(fs)}, {F(ft)}));'
        if op == 'msub.s':
            return f'{F(fd)} = fpu_sub(c->facc, fpu_mul({F(fs)}, {F(ft)}));'
        if op == 'madda.s':
            return f'c->facc = fpu_add(c->facc, fpu_mul({F(fs)}, {F(ft)}));'
        if op == 'msuba.s':
            return f'c->facc = fpu_sub(c->facc, fpu_mul({F(fs)}, {F(ft)}));'
        if op == 'cvt.w.s':
            return f'c->fpr[{fd}].s = fpu_cvtw({F(fs)});'
        if op == 'cvt.s.w':
            return f'{F(fd)} = (float)c->fpr[{fs}].s;'
        if op == 'max.s':
            return f'{F(fd)} = fpu_max({F(fs)}, {F(ft)});'
        if op == 'min.s':
            return f'{F(fd)} = fpu_min({F(fs)}, {F(ft)});'
        if op == 'c.f.s':
            return 'c->fcond = 0;'
        if op == 'c.eq.s':
            return f'c->fcond = ({F(fs)} == {F(ft)});'
        if op == 'c.lt.s':
            return f'c->fcond = ({F(fs)} < {F(ft)});'
        if op == 'c.le.s':
            return f'c->fcond = ({F(fs)} <= {F(ft)});'
        return f'#error unhandled fpu {op}'

    def emit_vu0(self, i):
        op = i.op
        w = i.word
        dest = (w >> 21) & 15
        ft = (w >> 16) & 31
        fs = (w >> 11) & 31
        fd = (w >> 6) & 31
        bc = w & 3
        if op in ('vcallms', 'vcallmsr'):
            imm15 = (w >> 6) & 0x7FFF
            if op == 'vcallms':
                return f'vu0_callms(c, {imm15 * 8});'
            return 'vu0_callmsr(c);'
        if op in ('viadd', 'visub', 'viand', 'vior'):
            return f'vu0_{op}(c, {fd & 15}, {fs & 15}, {ft & 15});'
        if op == 'viaddi':
            im5 = fd if fd < 16 else fd - 32
            return f'vu0_viaddi(c, {ft & 15}, {fs & 15}, {im5});'
        if op in ('vdiv', 'vsqrt', 'vrsqrt'):
            fsf = (w >> 21) & 3
            ftf = (w >> 23) & 3
            return f'vu0_{op}(c, {fs}, {fsf}, {ft}, {ftf});'
        if op in ('vmtir',):
            fsf = (w >> 21) & 3
            return f'vu0_vmtir(c, {ft & 15}, {fs}, {fsf});'
        if op in ('vmfir',):
            return f'vu0_vmfir(c, {dest}, {ft}, {fs & 15});'
        if op in ('vlqi', 'vsqi', 'vlqd', 'vsqd'):
            if op in ('vlqi', 'vlqd'):
                return f'vu0_{op}(c, {dest}, {ft}, {fs & 15});'
            return f'vu0_{op}(c, {dest}, {fs}, {ft & 15});'
        if op in ('vilwr', 'viswr'):
            return f'vu0_{op}(c, {dest}, {ft & 15}, {fs & 15});'
        if op in ('vrnext', 'vrget'):
            return f'vu0_{op}(c, {dest}, {ft});'
        if op in ('vrinit', 'vrxor'):
            fsf = (w >> 21) & 3
            return f'vu0_{op}(c, {fs}, {fsf});'
        if op in ('vwaitq', 'vnop'):
            return ''
        if op.endswith('bc'):
            return f'vu0_{op}(c, {dest}, {fd}, {fs}, {ft}, {bc});'
        # generic: (dest, fd, fs, ft)
        return f'vu0_{op}(c, {dest}, {fd}, {fs}, {ft});'

    # ------------------------------------------------------------------
    def branch_cond(self, i):
        op = i.op
        rs, rt = i.rs, i.rt
        if op in ('beq', 'beql'):
            if rs == rt:
                return '1'
            return f'{U64(rs)} == {U64(rt)}'
        if op in ('bne', 'bnel'):
            return f'{U64(rs)} != {U64(rt)}'
        if op in ('blez', 'blezl'):
            return f'{S64(rs)} <= 0'
        if op in ('bgtz', 'bgtzl'):
            return f'{S64(rs)} > 0'
        if op in ('bltz', 'bltzl', 'bltzal', 'bltzall'):
            return f'{S64(rs)} < 0'
        if op in ('bgez', 'bgezl', 'bgezal', 'bgezall'):
            return f'{S64(rs)} >= 0'
        if op in ('bc1f', 'bc1fl'):
            return '!c->fcond'
        if op in ('bc1t', 'bc1tl'):
            return 'c->fcond'
        if op in ('bc0f', 'bc0fl'):
            return '!ee_cpcond0(c)'
        if op in ('bc0t', 'bc0tl'):
            return 'ee_cpcond0(c)'
        if op in ('bc2f', 'bc2fl'):
            return '!vu0_cpcond(c)'
        if op in ('bc2t', 'bc2tl'):
            return 'vu0_cpcond(c)'
        raise ValueError(op)

    def goto_or_call(self, target, f, backward):
        """Code to transfer control to target (branch target)."""
        if f.start <= target < f.end:
            if backward:
                # interrupt check on a backward branch; a thread preempted here resumes at the target
                self._resume.add(target)
                if target in self.idle_loops:
                    return f'{{ ee_idle_loop(c, {hx(target)}); goto L_{target:08X}; }}'
                return f'{{ EE_LOOP_CHECK_AT(c, {hx(target)}); goto L_{target:08X}; }}'
            return f'goto L_{target:08X};'
        # tail branch to another function
        return f'{{ {self.call_code(target, f)} return; }}'

    def gen_function(self, f):
        mod = self.mod
        out = []
        o = out.append
        name = self.resolver.body_name(f.start, self.prefix)
        o(f'void {name}(EEContext* c) {{')
        o(f'    EE_FUNC_ENTRY(c, {hx(f.start)});')
        # extra labels needed for delay slots that are branch targets
        labels = set(f.labels)
        pc = f.start
        end = f.end
        skip = set()
        # resume points (savestates): a thread suspended in a callee, a syscall or an interrupt check
        # continues there after a restore (after calls and syscalls, targets of loop checks)
        self._resume = set()
        q = f.start
        while q < end:
            qi = mod.insn(q)
            if qi.is_branch:
                if qi.kind in ('jal', 'jalr'):
                    self._resume.add(q + 8)
                q += 8
            else:
                if qi.op == 'syscall':
                    self._resume.add(q + 4)
                q += 4
        labels |= self._resume
        while pc < end:
            i = mod.insn(pc)
            if pc in labels:
                o(f'L_{pc:08X}:;')
            if pc in skip:
                pc += 4
                continue
            cmt = f'    // {pc:08X}: {disasm(i)}'
            if i.is_branch:
                o(cmt)
                ds = mod.insn(pc + 4) if pc + 4 < mod.code_end else None
                ds_code = self.emit_insn(ds, f) if ds else []
                if ds and ds.is_branch:
                    self.warnings.append(f'branch in delay slot at {pc + 4:08x}')
                    ds_code = [f'/* branch in delay slot ignored: {disasm(ds)} */']
                elif ds and pc + 4 in self.patches:
                    # optional patch of a delay slot instruction
                    chain = ''
                    for idx, word in self.patches[pc + 4]:
                        alt = decode(word, pc + 4)
                        assert not alt.is_branch, f'patched branch at {pc + 4:08x}'
                        chain += f'if (g_ee_patch[{idx}]) {{ ' + ' '.join(self.emit_insn(alt, f)) + ' } else '
                    ds_code = [chain + '{ ' + ' '.join(ds_code) + ' }']
                dsc = ' '.join(ds_code)
                if ds:
                    o(f'    // {pc + 4:08X}: {disasm(ds)} (delay)')
                k = i.kind
                if k in ('branch', 'branchl'):
                    cond = self.branch_cond(i)
                    link = i.op in ('bltzal', 'bgezal', 'bltzall', 'bgezall')
                    back = i.target <= pc
                    tgt = self.goto_or_call(i.target, f, back)
                    if link:
                        o(f'    c->r[31].ud[0] = {hx(pc + 8)};')
                    if k == 'branch':
                        if cond == '1':
                            o(f'    {{ {dsc} {tgt} }}')
                        else:
                            o(f'    {{ int cond_ = ({cond}); {dsc} if (cond_) {tgt} }}')
                    else:
                        o(f'    if ({cond}) {{ {dsc} {tgt} }}')
                elif k == 'jump':
                    tgt = self.goto_or_call(i.target, f, i.target <= pc)
                    o(f'    {{ {dsc} {tgt} }}')
                elif k == 'jal':
                    o(f'    c->r[31].ud[0] = {hx(pc + 8)};')
                    o(f'    {{ {dsc} }}')
                    o(f'    {self.call_code(i.target, f)}')
                elif k == 'jr':
                    if i.rs == 31:
                        o(f'    {{ {dsc} return; }}')
                    elif pc in f.switches:
                        o(f'    {{ uint32_t jt_ = {U32(i.rs)}; {dsc}')
                        o('      switch (jt_) {')
                        for t in sorted(set(f.switches[pc])):
                            if f.start <= t < f.end:
                                o(f'        case {hx(t)}: goto L_{t:08X};')
                        o('        default: ee_call(c, jt_); return;')
                        o('      }')
                        o('    }')
                    else:
                        o(f'    {{ uint32_t jt_ = {U32(i.rs)}; {dsc} ee_call(c, jt_); return; }}')
                elif k == 'jalr':
                    rd = i.rd
                    o(f'    {{ uint32_t jt_ = {U32(i.rs)};')
                    if rd:
                        o(f'      c->r[{rd}].ud[0] = {hx(pc + 8)};')
                    o(f'      {dsc}')
                    o('      ee_call(c, jt_); }')
                # delay slot that is itself a label target
                if pc + 4 in labels:
                    o(f'    goto L_{pc + 8:08X};')
                    o(f'L_{pc + 4:08X}:;')
                    o('    ' + ' '.join(ds_code))
                    labels.add(pc + 8)
                pc += 8
                continue
            if pc in self.patches:
                # optional patch (config "patches"): all variants, chosen at run time
                code = self.emit_insn(i, f)
                o(cmt)
                chain = ''
                for idx, word in self.patches[pc]:
                    alt = decode(word, pc)
                    assert not alt.is_branch, f'patched branch at {pc:08x}'
                    o(f'    // patch {idx}: {disasm(alt)}')
                    chain += f'if (g_ee_patch[{idx}]) {{ ' + ' '.join(self.emit_insn(alt, f)) + ' } else '
                o('    ' + chain + '{ ' + ' '.join(code) + ' }')
                pc += 4
                continue
            code = self.emit_insn(i, f)
            if code:
                o(cmt)
                for line in code:
                    if line:
                        o('    ' + line)
            pc += 4
        # fallthrough past end of function: falls into next function
        last = mod.insn(end - 8) if end - 8 >= f.start else None
        if end in labels:
            o(f'L_{end:08X}:;')
        if end < mod.code_end:
            o(f'    {self.call_code(end, f)} return;')
        else:
            o('    return;')
        o('}')
        # resume dispatch at the entry (only taken right after a savestate restore)
        sites = sorted(t for t in self._resume if f.start <= t <= end)
        pro = ['    if (EE_UNLIKELY(g_ee_resume)) switch (ee_resume_take()) {']
        pro += [f'        case {hx(t)}: goto L_{t:08X};' for t in sites]
        pro.append(f'        default: ee_resume_fail(c, {hx(f.start)}); return;')
        pro.append('    }')
        out[2:2] = pro
        return '\n'.join(out)

    # ------------------------------------------------------------------
    def write(self, outdir, header_name='ee_recomp.h'):
        os.makedirs(outdir, exist_ok=True)
        names = []
        files = []
        chunk = []
        skip = set(self.config.get('skip', ()))
        for f in self.funcs:
            if f.start in self.resolver.hle or f.start in skip:
                continue
            chunk.append(f)
            if len(chunk) >= FUNCS_PER_FILE:
                files.append(chunk)
                chunk = []
        if chunk:
            files.append(chunk)
        # declarations
        decl = [f'#pragma once', f'#include "{header_name}"']
        for f in self.funcs:
            decl.append(f'void {self.resolver.body_name(f.start, self.prefix)}(EEContext* c);')
            if f.start in self.resolver.hooks:
                decl.append(f'void {self.resolver.hooks[f.start]}(EEContext* c);  /* hook */')
        with open(os.path.join(outdir, f'{self.prefix}_decls.h'), 'w') as fh:
            fh.write('\n'.join(decl) + '\n')
        written = []
        for n, ch in enumerate(files):
            fn = os.path.join(outdir, f'{self.prefix}_{n:03d}.c')
            body = [f'#include "{self.prefix}_decls.h"', '#include "all_decls.h"', '']
            for f in ch:
                body.append(self.gen_function(f))
                body.append('')
            data = '\n'.join(body)
            # only rewrite if changed (keeps ninja incremental)
            if not os.path.exists(fn) or open(fn).read() != data:
                with open(fn, 'w') as fh:
                    fh.write(data)
            written.append(fn)
        # function table
        tbl = [f'#include "{self.prefix}_decls.h"', '', f'const EEFuncEntry {self.prefix}_functable[] = {{']
        for f in self.funcs:
            nm = self.resolver.name_for(f.start) or f'{self.prefix}_{f.start:08X}'
            body = self.resolver.body_name(f.start, self.prefix)
            if f.start in self.resolver.hle or f.start in skip:
                body = '0'  # no generated body (no resume points)
            tbl.append(f'    {{ {hx(f.start)}, {hx(f.end)}, {nm}, {body} }},')
        tbl.append('    { 0, 0, 0, 0 }')
        tbl.append('};')
        tbl.append(f'const unsigned {self.prefix}_functable_count = {len(self.funcs)};')
        fn = os.path.join(outdir, f'{self.prefix}_table.c')
        with open(fn, 'w') as fh:
            fh.write('\n'.join(tbl) + '\n')
        written.append(fn)
        return written
