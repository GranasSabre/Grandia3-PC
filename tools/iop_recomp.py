"""IRX loader (with relocation) and R3000 (IOP) -> C recompiler.

Usage: python iop_recomp.py <irx> <base_hex> <outdir> <prefix>
"""
import os
import struct
import sys

from r5900 import decode, disasm
from codeinfo import Module, discover_functions

R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


class IRX:
    def __init__(self, path, base):
        d = open(path, 'rb').read()
        self.raw = d
        (e_type, e_machine, e_version, e_entry, e_phoff, e_shoff, e_flags, e_ehsize, e_phentsize, e_phnum,
         e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
        sh = [struct.unpack_from('<10I', d, e_shoff + i * 40) for i in range(e_shnum)]
        st = sh[e_shstrndx]

        def nm(o):
            s = d[st[4] + o:]
            return s[:s.index(b'\0')].decode()
        self.sections = {nm(s[0]): s for s in sh}
        self.sh = sh
        self.names = [nm(s[0]) for s in sh]
        im = self.sections['.iopmod']
        mid, start, gp, text, data, bss, ver = struct.unpack_from('<IIIIIIH', d, im[4])
        self.entry = start + base
        self.gp = gp + base
        self.text_size = text
        self.data_size = data
        self.bss_size = bss
        self.base = base
        # load program segment
        phoff = e_phoff
        ph = [struct.unpack_from('<8I', d, phoff + i * 32) for i in range(e_phnum)]
        seg = [p for p in ph if p[0] == 1][0]
        self.memsz = seg[5]
        img = bytearray(seg[5])
        img[:seg[4]] = d[seg[1]:seg[1] + seg[4]]
        self.img = img
        self.relocate()

    def w(self, off):
        return struct.unpack_from('<I', self.img, off)[0]

    def setw(self, off, v):
        struct.pack_into('<I', self.img, off, v & 0xFFFFFFFF)

    def relocate(self):
        base = self.base
        self.reloc_targets = []  # (offset, kind) for R_MIPS_32 entries
        for i, s in enumerate(self.sh):
            if s[1] != 9:  # SHT_REL
                continue
            target_sec = self.sh[s[7]]
            n = s[5] // 8
            pending_hi = []
            for k in range(n):
                off, info = struct.unpack_from('<II', self.raw, s[4] + k * 8)
                typ = info & 0xFF
                addr = off  # section addresses are absolute within image for IRX (sh_addr based)
                addr = target_sec[3] + off if target_sec[3] and off < target_sec[5] else off
                if typ == R_MIPS_32:
                    self.setw(addr, self.w(addr) + base)
                    self.reloc_targets.append(addr)
                elif typ == R_MIPS_26:
                    v = self.w(addr)
                    t = ((v & 0x3FFFFFF) << 2) + base
                    self.setw(addr, (v & 0xFC000000) | ((t >> 2) & 0x3FFFFFF))
                elif typ == R_MIPS_HI16:
                    pending_hi.append(addr)
                elif typ == R_MIPS_LO16:
                    lo = self.w(addr)
                    lo16 = lo & 0xFFFF
                    if lo16 & 0x8000:
                        lo16 -= 0x10000
                    for h in pending_hi:
                        hv = self.w(h)
                        ahl = ((hv & 0xFFFF) << 16) + lo16
                        nv = ahl + base
                        self.setw(h, (hv & 0xFFFF0000) | (((nv + 0x8000) >> 16) & 0xFFFF))
                    pending_hi = []
                    self.setw(addr, (lo & 0xFFFF0000) | ((lo16 + base) & 0xFFFF))
                else:
                    raise ValueError(f'unsupported reloc type {typ}')

    def imports(self):
        """Find import stub tables in text: returns dict addr -> (libname, index)."""
        res = {}
        text = self.img[:self.text_size]
        pos = 0
        while True:
            pos = text.find(struct.pack('<I', 0x41E00000), pos)
            if pos < 0:
                break
            if pos % 4 == 0:
                name = bytes(text[pos + 12:pos + 20]).rstrip(b'\0').decode()
                q = pos + 20
                while q + 8 <= len(text):
                    a, b = struct.unpack_from('<II', text, q)
                    if a != 0x03E00008:
                        break
                    res[self.base + q] = (name, b & 0xFFFF)
                    q += 8
            pos += 4
        return res


def U(n):
    return '0u' if n == 0 else f'c->r[{n}]'


def S(n):
    return '0' if n == 0 else f'(int32_t)c->r[{n}]'


def setr(n, e):
    if n == 0:
        return f'(void)({e});'
    return f'c->r[{n}] = (uint32_t)({e});'


def hx(v):
    return f'0x{v & 0xFFFFFFFF:08X}u'


class IOPRecompiler:
    def __init__(self, mod, funcs, imports, prefix):
        self.mod = mod
        self.funcs = funcs
        self.imports = imports
        self.prefix = prefix
        self.names = {f.start: f'{prefix}_{f.start:08X}' for f in funcs}

    def call_code(self, t):
        if t in self.imports:
            lib, idx = self.imports[t]
            return f'iop_imp_{lib}_{idx}(c);'
        if t in self.names:
            return f'{self.names[t]}(c);'
        return f'iop_call(c, {hx(t)});'

    def addr(self, i):
        if i.rs == 0:
            return hx(i.imm)
        return f'({U(i.rs)} + {hx(i.imm)})'

    def insn(self, i):
        op = i.op
        rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
        if op in ('nop', 'sync', 'cache', 'pref'):
            return ''
        if op in ('addi', 'addiu'):
            return setr(rt, f'{U(rs)} + {hx(i.imm)}')
        if op == 'slti':
            return setr(rt, f'{S(rs)} < {i.imm}')
        if op == 'sltiu':
            return setr(rt, f'{U(rs)} < {hx(i.imm)}')
        if op == 'andi':
            return setr(rt, f'{U(rs)} & {i.uimm}u')
        if op == 'ori':
            return setr(rt, f'{U(rs)} | {i.uimm}u')
        if op == 'xori':
            return setr(rt, f'{U(rs)} ^ {i.uimm}u')
        if op == 'lui':
            return setr(rt, hx(i.uimm << 16))
        if op in ('add', 'addu'):
            return setr(rd, f'{U(rs)} + {U(rt)}')
        if op in ('sub', 'subu'):
            return setr(rd, f'{U(rs)} - {U(rt)}')
        if op == 'and':
            return setr(rd, f'{U(rs)} & {U(rt)}')
        if op == 'or':
            return setr(rd, f'{U(rs)} | {U(rt)}')
        if op == 'xor':
            return setr(rd, f'{U(rs)} ^ {U(rt)}')
        if op == 'nor':
            return setr(rd, f'~({U(rs)} | {U(rt)})')
        if op == 'slt':
            return setr(rd, f'{S(rs)} < {S(rt)}')
        if op == 'sltu':
            return setr(rd, f'{U(rs)} < {U(rt)}')
        if op == 'sll':
            return setr(rd, f'{U(rt)} << {sa}')
        if op == 'srl':
            return setr(rd, f'{U(rt)} >> {sa}')
        if op == 'sra':
            return setr(rd, f'{S(rt)} >> {sa}')
        if op == 'sllv':
            return setr(rd, f'{U(rt)} << ({U(rs)} & 31)')
        if op == 'srlv':
            return setr(rd, f'{U(rt)} >> ({U(rs)} & 31)')
        if op == 'srav':
            return setr(rd, f'{S(rt)} >> ({U(rs)} & 31)')
        if op == 'mult':
            return f'{{ int64_t p_ = (int64_t){S(rs)} * {S(rt)}; c->lo = (uint32_t)p_; c->hi = (uint32_t)((uint64_t)p_ >> 32); }}'
        if op == 'multu':
            return f'{{ uint64_t p_ = (uint64_t){U(rs)} * {U(rt)}; c->lo = (uint32_t)p_; c->hi = (uint32_t)(p_ >> 32); }}'
        if op == 'div':
            return f'iop_div(c, {S(rs)}, {S(rt)});'
        if op == 'divu':
            return f'iop_divu(c, {U(rs)}, {U(rt)});'
        if op == 'mfhi':
            return setr(rd, 'c->hi')
        if op == 'mflo':
            return setr(rd, 'c->lo')
        if op == 'mthi':
            return f'c->hi = {U(rs)};'
        if op == 'mtlo':
            return f'c->lo = {U(rs)};'
        if op == 'lb':
            return setr(rt, f'(int32_t)(int8_t)iop_rd8({self.addr(i)})')
        if op == 'lbu':
            return setr(rt, f'iop_rd8({self.addr(i)})')
        if op == 'lh':
            return setr(rt, f'(int32_t)(int16_t)iop_rd16({self.addr(i)})')
        if op == 'lhu':
            return setr(rt, f'iop_rd16({self.addr(i)})')
        if op == 'lw':
            return setr(rt, f'iop_rd32({self.addr(i)})')
        if op in ('lwl', 'lwr', 'swl', 'swr'):
            return f'iop_{op}(c, {rt}, {self.addr(i)});'
        if op == 'sb':
            return f'iop_wr8({self.addr(i)}, (uint8_t){U(rt)});'
        if op == 'sh':
            return f'iop_wr16({self.addr(i)}, (uint16_t){U(rt)});'
        if op == 'sw':
            return f'iop_wr32({self.addr(i)}, {U(rt)});'
        if op == 'syscall':
            return f'iop_syscall(c, {hx(i.pc)});'
        if op == 'break':
            return f'iop_break(c, {hx(i.pc)});'
        if op == 'mfc0':
            return setr(rt, f'iop_mfc0(c, {rd})')
        if op == 'mtc0':
            return f'iop_mtc0(c, {rd}, {U(rt)});'
        if op == 'eret' or op == 'invalid':
            return f'iop_invalid(c, {hx(i.pc)}, {hx(i.word)});'
        return f'iop_invalid(c, {hx(i.pc)}, {hx(i.word)}); /* {op} */'

    def cond(self, i):
        op = i.op
        if op in ('beq', 'beql'):
            return '1' if i.rs == i.rt else f'{U(i.rs)} == {U(i.rt)}'
        if op in ('bne', 'bnel'):
            return f'{U(i.rs)} != {U(i.rt)}'
        if op in ('blez', 'blezl'):
            return f'{S(i.rs)} <= 0'
        if op in ('bgtz', 'bgtzl'):
            return f'{S(i.rs)} > 0'
        if op in ('bltz', 'bltzl', 'bltzal'):
            return f'{S(i.rs)} < 0'
        if op in ('bgez', 'bgezl', 'bgezal'):
            return f'{S(i.rs)} >= 0'
        raise ValueError(op)

    def tgt(self, t, f, back):
        if f.start <= t < f.end:
            if back:
                self._resume.add(t)  # a thread yielding here continues at the target after a restore
                return f'{{ IOP_LOOP_CHECK_AT(c, {hx(t)}); goto L_{t:08X}; }}'
            return f'goto L_{t:08X};'
        return f'{{ {self.call_code(t)} return; }}'

    def gen(self, f):
        mod = self.mod
        o = [f'void {self.names[f.start]}(IOPContext* c) {{', f'    IOP_FUNC_ENTRY(c, {hx(f.start)});']
        labels = set(f.labels)
        # resume points (savestates): after calls (a thread blocked in a callee or in an import such
        # as DelayThread continues at its return address) and targets of loop checks
        self._resume = set()
        q = f.start
        while q < f.end:
            qi = mod.insn(q)
            if qi.is_branch:
                if qi.kind in ('jal', 'jalr'):
                    self._resume.add(q + 8)
                q += 8
            else:
                q += 4
        labels |= self._resume
        pc = f.start
        while pc < f.end:
            i = mod.insn(pc)
            if pc in labels:
                o.append(f'L_{pc:08X}:;')
            if i.is_branch:
                ds = mod.insn(pc + 4)
                dsc = self.insn(ds) if not ds.is_branch else ''
                o.append(f'    // {pc:08X}: {disasm(i)} ; {disasm(ds)}')
                k = i.kind
                if k in ('branch', 'branchl'):
                    c = self.cond(i)
                    t = self.tgt(i.target, f, i.target <= pc)
                    if i.op in ('bltzal', 'bgezal'):
                        o.append(f'    c->r[31] = {hx(pc + 8)};')
                    if k == 'branch':
                        o.append(f'    {{ int cond_ = ({c}); {dsc} if (cond_) {t} }}')
                    else:
                        o.append(f'    if ({c}) {{ {dsc} {t} }}')
                elif k == 'jump':
                    o.append(f'    {{ {dsc} {self.tgt(i.target, f, i.target <= pc)} }}')
                elif k == 'jal':
                    o.append(f'    c->r[31] = {hx(pc + 8)}; {dsc}')
                    o.append(f'    {self.call_code(i.target)}')
                elif k == 'jr':
                    if i.rs == 31:
                        o.append(f'    {{ {dsc} return; }}')
                    elif pc in f.switches:
                        o.append(f'    {{ uint32_t jt_ = {U(i.rs)}; {dsc} switch (jt_) {{')
                        for t in sorted(set(f.switches[pc])):
                            if f.start <= t < f.end:
                                o.append(f'      case {hx(t)}: goto L_{t:08X};')
                        o.append('      default: iop_call(c, jt_); return; } }')
                    else:
                        o.append(f'    {{ uint32_t jt_ = {U(i.rs)}; {dsc} iop_call(c, jt_); return; }}')
                elif k == 'jalr':
                    o.append(f'    {{ uint32_t jt_ = {U(i.rs)}; {"c->r[%d] = %s;" % (i.rd, hx(pc + 8)) if i.rd else ""} {dsc} iop_call(c, jt_); }}')
                if pc + 4 in labels:
                    o.append(f'    goto L_{pc + 8:08X};')
                    o.append(f'L_{pc + 4:08X}:;')
                    o.append('    ' + dsc)
                    labels.add(pc + 8)
                pc += 8
                continue
            code = self.insn(i)
            if code:
                o.append(f'    // {pc:08X}: {disasm(i)}')
                o.append('    ' + code)
            pc += 4
        if f.end in labels:
            o.append(f'L_{f.end:08X}:;')
        if f.end < mod.code_end:
            o.append(f'    {self.call_code(f.end)} return;')
        o.append('}')
        sites = sorted(t for t in self._resume if f.start <= t <= f.end)
        pro = ['    if (__builtin_expect(g_iop_resume, 0)) switch (iop_resume_take()) {']
        pro += [f'        case {hx(t)}: goto L_{t:08X};' for t in sites]
        pro.append(f'        default: iop_resume_fail(c, {hx(f.start)}); return;')
        pro.append('    }')
        o[2:2] = pro
        return '\n'.join(o)


def main():
    path, base, outdir, prefix = sys.argv[1], int(sys.argv[2], 16), sys.argv[3], sys.argv[4]
    irx = IRX(path, base)
    imports = irx.imports()
    # code ends where import/export tables start? keep whole text; stubs are tiny functions
    mod = Module(prefix, base, bytes(irx.img), base + irx.text_size, base + irx.text_size + irx.data_size,
                 base + irx.memsz)
    extra = [irx.entry] + list(imports.keys())
    # function pointers from relocated words in rodata/data
    for off in irx.reloc_targets:
        v = struct.unpack_from('<I', irx.img, off)[0]
        if mod.in_code(v) and off >= irx.text_size:
            pass  # handled by discover (data pointer scan)
    funcs, switches, ptrs, jtt = discover_functions(mod, extra_starts=extra)
    # remove import stub functions (handled natively) but keep them for address resolution
    os.makedirs(outdir, exist_ok=True)
    rc = IOPRecompiler(mod, funcs, imports, prefix)
    body = ['#include "iop_recomp.h"', '']
    for (lib, idx) in sorted(set(imports.values())):
        body.append(f'void iop_imp_{lib}_{idx}(IOPContext* c);')
    for f in funcs:
        body.append(f'void {rc.names[f.start]}(IOPContext* c);')
    body.append('')
    for f in funcs:
        if f.start in imports:
            lib, idx = imports[f.start]
            body.append(f'void {rc.names[f.start]}(IOPContext* c) {{ iop_imp_{lib}_{idx}(c); }}')
            continue
        body.append(rc.gen(f))
        body.append('')
    body.append(f'const IOPFuncEntry {prefix}_functable[] = {{')
    for f in funcs:
        body.append(f'    {{ {hx(f.start)}, {hx(f.end)}, {rc.names[f.start]} }},')
    body.append('    { 0, 0, 0 }')
    body.append('};')
    body.append(f'const unsigned {prefix}_functable_count = {len(funcs)};')
    with open(os.path.join(outdir, f'{prefix}.c'), 'w') as fh:
        fh.write('\n'.join(body) + '\n')
    # module image (relocated) for loading at runtime
    with open(os.path.join(outdir, f'{prefix}_image.c'), 'w') as fh:
        fh.write('#include <stdint.h>\n')
        fh.write(f'const uint32_t {prefix}_base = {hx(base)};\n')
        fh.write(f'const uint32_t {prefix}_entry = {hx(irx.entry)};\n')
        fh.write(f'const uint32_t {prefix}_gp = {hx(irx.gp)};\n')
        fh.write(f'const uint32_t {prefix}_memsz = {hx(irx.memsz)};\n')
        fh.write(f'const uint32_t {prefix}_filesz = {hx(irx.text_size + irx.data_size)};\n')
        fh.write(f'const uint8_t {prefix}_image[] = {{\n')
        data = bytes(irx.img[:irx.text_size + irx.data_size])
        for k in range(0, len(data), 32):
            fh.write('  ' + ','.join(str(b) for b in data[k:k + 32]) + ',\n')
        fh.write('};\n')
    print(f'{prefix}: {len(funcs)} functions, {len(imports)} imports, entry {irx.entry:08x}, gp {irx.gp:08x}, '
          f'memsz {irx.memsz:x}')
    libs = {}
    for a, (l, i) in imports.items():
        libs.setdefault(l, []).append(i)
    for l, v in libs.items():
        print('  import', l, sorted(v))


if __name__ == '__main__':
    main()
