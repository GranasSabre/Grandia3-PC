"""MIPS R5900 (PS2 Emotion Engine) instruction decoder.

decode(word, pc) -> Insn with fields:
  op      mnemonic (lowercase, e.g. 'addiu', 'vmaddbc')
  rs rt rd sa   raw register fields
  imm     sign-extended 16-bit immediate
  uimm    zero-extended immediate
  target  absolute branch/jump target (or None)
  kind    classification string, one of:
          'alu','load','store','branch','branchl','jump','jal','jr','jalr',
          'syscall','break','nop','cop0','fpu','cop2','vu0','mmi','sync','cache','pref','other','invalid'
"""

GPR = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3',
       't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
       't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


class Insn:
    __slots__ = ('word', 'pc', 'op', 'rs', 'rt', 'rd', 'sa', 'funct', 'imm', 'uimm',
                 'target', 'kind', 'fmt', 'ft', 'fs', 'fd', 'dest', 'bc', 'fsf', 'ftf')

    def __init__(self, word, pc):
        self.word = word
        self.pc = pc
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.funct = word & 63
        u = word & 0xFFFF
        self.uimm = u
        self.imm = u - 0x10000 if u & 0x8000 else u
        self.target = None
        self.kind = 'alu'
        self.op = '?'
        # FPU / VU fields
        self.fmt = self.rs
        self.ft = self.rt
        self.fs = self.rd
        self.fd = self.sa
        self.dest = (word >> 21) & 15
        self.bc = word & 3
        self.fsf = (word >> 21) & 3
        self.ftf = (word >> 23) & 3

    def __repr__(self):
        return f"<{self.pc:08x} {self.op}>"

    # ---- convenience ----
    @property
    def is_branch(self):
        return self.kind in ('branch', 'branchl', 'jump', 'jal', 'jr', 'jalr')

    @property
    def has_delay_slot(self):
        return self.is_branch


SPECIAL = {
    0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav',
    8: 'jr', 9: 'jalr', 10: 'movz', 11: 'movn', 12: 'syscall', 13: 'break', 15: 'sync',
    16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo', 20: 'dsllv', 22: 'dsrlv', 23: 'dsrav',
    24: 'mult', 25: 'multu', 26: 'div', 27: 'divu',
    32: 'add', 33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor',
    40: 'mfsa', 41: 'mtsa', 42: 'slt', 43: 'sltu', 44: 'dadd', 45: 'daddu', 46: 'dsub', 47: 'dsubu',
    48: 'tge', 49: 'tgeu', 50: 'tlt', 51: 'tltu', 52: 'teq', 54: 'tne',
    56: 'dsll', 58: 'dsrl', 59: 'dsra', 60: 'dsll32', 62: 'dsrl32', 63: 'dsra32',
}

REGIMM = {
    0: 'bltz', 1: 'bgez', 2: 'bltzl', 3: 'bgezl',
    8: 'tgei', 9: 'tgeiu', 10: 'tlti', 11: 'tltiu', 12: 'teqi', 14: 'tnei',
    16: 'bltzal', 17: 'bgezal', 18: 'bltzall', 19: 'bgezall',
    24: 'mtsab', 25: 'mtsah',
}

OPCODES = {
    2: 'j', 3: 'jal', 4: 'beq', 5: 'bne', 6: 'blez', 7: 'bgtz',
    8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori', 15: 'lui',
    20: 'beql', 21: 'bnel', 22: 'blezl', 23: 'bgtzl', 24: 'daddi', 25: 'daddiu',
    26: 'ldl', 27: 'ldr', 30: 'lq', 31: 'sq',
    32: 'lb', 33: 'lh', 34: 'lwl', 35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr', 39: 'lwu',
    40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw', 44: 'sdl', 45: 'sdr', 46: 'swr', 47: 'cache',
    49: 'lwc1', 51: 'pref', 54: 'lqc2', 55: 'ld', 57: 'swc1', 62: 'sqc2', 63: 'sd',
}

LOADS = {'ldl', 'ldr', 'lq', 'lb', 'lh', 'lwl', 'lw', 'lbu', 'lhu', 'lwr', 'lwu', 'lwc1', 'lqc2', 'ld'}
STORES = {'sq', 'sb', 'sh', 'swl', 'swr', 'sw', 'sdl', 'sdr', 'swc1', 'sqc2', 'sd'}

MMI = {
    0: 'madd', 1: 'maddu', 4: 'plzcw', 16: 'mfhi1', 17: 'mthi1', 18: 'mflo1', 19: 'mtlo1',
    24: 'mult1', 25: 'multu1', 26: 'div1', 27: 'divu1', 32: 'madd1', 33: 'maddu1',
    48: 'pmfhl', 49: 'pmthl', 52: 'psllh', 54: 'psrlh', 55: 'psrah', 60: 'psllw', 62: 'psrlw', 63: 'psraw',
}
MMI0 = {0: 'paddw', 1: 'psubw', 2: 'pcgtw', 3: 'pmaxw', 4: 'paddh', 5: 'psubh', 6: 'pcgth', 7: 'pmaxh',
        8: 'paddb', 9: 'psubb', 10: 'pcgtb', 16: 'paddsw', 17: 'psubsw', 18: 'pextlw', 19: 'ppacw',
        20: 'paddsh', 21: 'psubsh', 22: 'pextlh', 23: 'ppach', 24: 'paddsb', 25: 'psubsb', 26: 'pextlb',
        27: 'ppacb', 30: 'pext5', 31: 'ppac5'}
MMI1 = {1: 'pabsw', 2: 'pceqw', 3: 'pminw', 4: 'padsbh', 5: 'pabsh', 6: 'pceqh', 7: 'pminh', 10: 'pceqb',
        16: 'padduw', 17: 'psubuw', 18: 'pextuw', 20: 'padduh', 21: 'psubuh', 22: 'pextuh',
        24: 'paddub', 25: 'psubub', 26: 'pextub', 27: 'qfsrv'}
MMI2 = {0: 'pmaddw', 2: 'psllvw', 3: 'psrlvw', 4: 'pmsubw', 8: 'pmfhi', 9: 'pmflo', 10: 'pinth',
        12: 'pmultw', 13: 'pdivw', 14: 'pcpyld', 16: 'pmaddh', 17: 'phmadh', 18: 'pand', 19: 'pxor',
        20: 'pmsubh', 21: 'phmsbh', 26: 'pexeh', 27: 'prevh', 28: 'pmulth', 29: 'pdivbw', 30: 'pexew',
        31: 'prot3w'}
MMI3 = {0: 'pmadduw', 3: 'psravw', 8: 'pmthi', 9: 'pmtlo', 10: 'pinteh', 12: 'pmultuw', 13: 'pdivuw',
        14: 'pcpyud', 18: 'por', 19: 'pnor', 26: 'pexch', 27: 'pcpyh', 30: 'pexcw'}

FPU_S = {0: 'add.s', 1: 'sub.s', 2: 'mul.s', 3: 'div.s', 4: 'sqrt.s', 5: 'abs.s', 6: 'mov.s', 7: 'neg.s',
         22: 'rsqrt.s', 24: 'adda.s', 25: 'suba.s', 26: 'mula.s', 28: 'madd.s', 29: 'msub.s',
         30: 'madda.s', 31: 'msuba.s', 36: 'cvt.w.s', 40: 'max.s', 41: 'min.s',
         48: 'c.f.s', 50: 'c.eq.s', 52: 'c.lt.s', 54: 'c.le.s'}

# VU0 macro mode (COP2 with CO bit)
VU_SPECIAL1 = {}
for i, n in enumerate(['vaddbc', 'vsubbc', 'vmaddbc', 'vmsubbc', 'vmaxbc', 'vminibc', 'vmulbc']):
    for b in range(4):
        VU_SPECIAL1[i * 4 + b] = n
VU_SPECIAL1.update({
    0x1C: 'vmulq', 0x1D: 'vmaxi', 0x1E: 'vmuli', 0x1F: 'vminii', 0x20: 'vaddq', 0x21: 'vmaddq',
    0x22: 'vaddi', 0x23: 'vmaddi', 0x24: 'vsubq', 0x25: 'vmsubq', 0x26: 'vsubi', 0x27: 'vmsubi',
    0x28: 'vadd', 0x29: 'vmadd', 0x2A: 'vmul', 0x2B: 'vmax', 0x2C: 'vsub', 0x2D: 'vmsub',
    0x2E: 'vopmsub', 0x2F: 'vmini', 0x30: 'viadd', 0x31: 'visub', 0x32: 'viaddi', 0x34: 'viand',
    0x35: 'vior', 0x38: 'vcallms', 0x39: 'vcallmsr'})
VU_SPECIAL2 = {}
for i, n in enumerate(['vaddabc', 'vsubabc', 'vmaddabc', 'vmsubabc']):
    for b in range(4):
        VU_SPECIAL2[i * 4 + b] = n
for b in range(4):
    VU_SPECIAL2[0x18 + b] = 'vmulabc'
VU_SPECIAL2.update({
    0x10: 'vitof0', 0x11: 'vitof4', 0x12: 'vitof12', 0x13: 'vitof15',
    0x14: 'vftoi0', 0x15: 'vftoi4', 0x16: 'vftoi12', 0x17: 'vftoi15',
    0x1C: 'vmulaq', 0x1D: 'vabs', 0x1E: 'vmulai', 0x1F: 'vclipw',
    0x20: 'vaddaq', 0x21: 'vmaddaq', 0x22: 'vaddai', 0x23: 'vmaddai',
    0x24: 'vsubaq', 0x25: 'vmsubaq', 0x26: 'vsubai', 0x27: 'vmsubai',
    0x28: 'vadda', 0x29: 'vmadda', 0x2A: 'vmula', 0x2C: 'vsuba', 0x2D: 'vmsuba',
    0x2E: 'vopmula', 0x2F: 'vnop', 0x30: 'vmove', 0x31: 'vmr32',
    0x34: 'vlqi', 0x35: 'vsqi', 0x36: 'vlqd', 0x37: 'vsqd',
    0x38: 'vdiv', 0x39: 'vsqrt', 0x3A: 'vrsqrt', 0x3B: 'vwaitq',
    0x3C: 'vmtir', 0x3D: 'vmfir', 0x3E: 'vilwr', 0x3F: 'viswr',
    0x40: 'vrnext', 0x41: 'vrget', 0x42: 'vrinit', 0x43: 'vrxor'})


def decode(word, pc=0):
    i = Insn(word, pc)
    opc = word >> 26
    if word == 0:
        i.op = 'nop'
        i.kind = 'nop'
        return i
    if opc == 0:
        f = i.funct
        name = SPECIAL.get(f)
        if name is None:
            i.op = 'invalid'
            i.kind = 'invalid'
            return i
        i.op = name
        if name == 'jr':
            i.kind = 'jr'
        elif name == 'jalr':
            i.kind = 'jalr'
        elif name == 'syscall':
            i.kind = 'syscall'
        elif name == 'break':
            i.kind = 'break'
        elif name == 'sync':
            i.kind = 'sync'
        elif name.startswith('t') and name in ('tge', 'tgeu', 'tlt', 'tltu', 'teq', 'tne'):
            i.kind = 'other'
        return i
    if opc == 1:
        name = REGIMM.get(i.rt)
        if name is None:
            i.op = 'invalid'
            i.kind = 'invalid'
            return i
        i.op = name
        if name.startswith('b'):
            i.target = (pc + 4 + (i.imm << 2)) & 0xFFFFFFFF
            i.kind = 'branchl' if name.endswith('l') else 'branch'
        elif name.startswith('t'):
            i.kind = 'other'
        return i
    if opc in (2, 3):
        i.op = 'j' if opc == 2 else 'jal'
        i.target = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        i.kind = 'jump' if opc == 2 else 'jal'
        return i
    if opc == 16:
        return decode_cop0(i)
    if opc == 17:
        return decode_cop1(i)
    if opc == 18:
        return decode_cop2(i)
    if opc == 28:
        return decode_mmi(i)
    name = OPCODES.get(opc)
    if name is None:
        i.op = 'invalid'
        i.kind = 'invalid'
        return i
    i.op = name
    if name in ('beq', 'bne', 'blez', 'bgtz', 'beql', 'bnel', 'blezl', 'bgtzl'):
        i.target = (pc + 4 + (i.imm << 2)) & 0xFFFFFFFF
        i.kind = 'branchl' if name.endswith('l') else 'branch'
    elif name in LOADS:
        i.kind = 'load'
    elif name in STORES:
        i.kind = 'store'
    elif name == 'cache':
        i.kind = 'cache'
    elif name == 'pref':
        i.kind = 'pref'
    return i


def decode_cop0(i):
    rs = i.rs
    i.kind = 'cop0'
    if rs == 0:
        i.op = 'mfc0'
    elif rs == 4:
        i.op = 'mtc0'
    elif rs == 8:
        i.op = {0: 'bc0f', 1: 'bc0t', 2: 'bc0fl', 3: 'bc0tl'}.get(i.rt, 'invalid')
        i.target = (i.pc + 4 + (i.imm << 2)) & 0xFFFFFFFF
        i.kind = 'branchl' if i.op.endswith('l') else 'branch'
    elif rs == 16:
        f = i.funct
        i.op = {1: 'tlbr', 2: 'tlbwi', 6: 'tlbwr', 8: 'tlbp', 24: 'eret', 56: 'ei', 57: 'di'}.get(f, 'invalid')
    else:
        i.op = 'invalid'
    if i.op == 'invalid':
        i.kind = 'invalid'
    return i


def decode_cop1(i):
    rs = i.rs
    i.kind = 'fpu'
    if rs == 0:
        i.op = 'mfc1'
    elif rs == 2:
        i.op = 'cfc1'
    elif rs == 4:
        i.op = 'mtc1'
    elif rs == 6:
        i.op = 'ctc1'
    elif rs == 8:
        i.op = {0: 'bc1f', 1: 'bc1t', 2: 'bc1fl', 3: 'bc1tl'}.get(i.rt, 'invalid')
        i.target = (i.pc + 4 + (i.imm << 2)) & 0xFFFFFFFF
        i.kind = 'branchl' if i.op.endswith('l') else 'branch'
    elif rs == 16:
        i.op = FPU_S.get(i.funct, 'invalid')
    elif rs == 20:
        i.op = 'cvt.s.w' if i.funct == 32 else 'invalid'
    else:
        i.op = 'invalid'
    if i.op == 'invalid':
        i.kind = 'invalid'
    return i


def decode_cop2(i):
    w = i.word
    i.kind = 'cop2'
    if w & (1 << 25):
        i.kind = 'vu0'
        f = w & 63
        if f >= 0x3C:
            idx = (w & 3) | ((w >> 4) & 0x7C)
            i.op = VU_SPECIAL2.get(idx, 'invalid')
        else:
            i.op = VU_SPECIAL1.get(f, 'invalid')
        if i.op == 'invalid':
            i.kind = 'invalid'
        return i
    rs = i.rs
    if rs == 1:
        i.op = 'qmfc2'
    elif rs == 2:
        i.op = 'cfc2'
    elif rs == 5:
        i.op = 'qmtc2'
    elif rs == 6:
        i.op = 'ctc2'
    elif rs == 8:
        i.op = {0: 'bc2f', 1: 'bc2t', 2: 'bc2fl', 3: 'bc2tl'}.get(i.rt, 'invalid')
        i.target = (i.pc + 4 + (i.imm << 2)) & 0xFFFFFFFF
        i.kind = 'branchl' if i.op.endswith('l') else 'branch'
    else:
        i.op = 'invalid'
    if i.op == 'invalid':
        i.kind = 'invalid'
    return i


def decode_mmi(i):
    f = i.funct
    i.kind = 'mmi'
    if f == 8:
        i.op = MMI0.get(i.sa, 'invalid')
    elif f == 40:
        i.op = MMI1.get(i.sa, 'invalid')
    elif f == 9:
        i.op = MMI2.get(i.sa, 'invalid')
    elif f == 41:
        i.op = MMI3.get(i.sa, 'invalid')
    else:
        i.op = MMI.get(f, 'invalid')
    if i.op == 'invalid':
        i.kind = 'invalid'
    return i


def fmt_dest(d):
    return ''.join(c for c, b in zip('xyzw', (8, 4, 2, 1)) if d & b)


def disasm(i):
    """Return a human-readable disassembly string."""
    op = i.op
    g = GPR
    k = i.kind
    if op == 'nop':
        return 'nop'
    if op == 'invalid':
        return f'.word 0x{i.word:08x}'
    if k == 'load' or k == 'store':
        if op in ('lwc1', 'swc1'):
            return f'{op} $f{i.rt}, {i.imm}({g[i.rs]})'
        if op in ('lqc2', 'sqc2'):
            return f'{op} vf{i.rt}, {i.imm}({g[i.rs]})'
        return f'{op} {g[i.rt]}, {i.imm}({g[i.rs]})'
    if op in ('j', 'jal'):
        return f'{op} 0x{i.target:08x}'
    if op == 'jr':
        return f'jr {g[i.rs]}'
    if op == 'jalr':
        return f'jalr {g[i.rd]}, {g[i.rs]}'
    if k in ('branch', 'branchl'):
        if op in ('beq', 'bne', 'beql', 'bnel'):
            return f'{op} {g[i.rs]}, {g[i.rt]}, 0x{i.target:08x}'
        if op.startswith('bc'):
            return f'{op} 0x{i.target:08x}'
        return f'{op} {g[i.rs]}, 0x{i.target:08x}'
    if op in ('addi', 'addiu', 'slti', 'sltiu', 'daddi', 'daddiu'):
        return f'{op} {g[i.rt]}, {g[i.rs]}, {i.imm}'
    if op in ('andi', 'ori', 'xori'):
        return f'{op} {g[i.rt]}, {g[i.rs]}, 0x{i.uimm:x}'
    if op == 'lui':
        return f'lui {g[i.rt]}, 0x{i.uimm:x}'
    if op in ('sll', 'srl', 'sra', 'dsll', 'dsrl', 'dsra', 'dsll32', 'dsrl32', 'dsra32'):
        return f'{op} {g[i.rd]}, {g[i.rt]}, {i.sa}'
    if op in ('sllv', 'srlv', 'srav', 'dsllv', 'dsrlv', 'dsrav'):
        return f'{op} {g[i.rd]}, {g[i.rt]}, {g[i.rs]}'
    if op in ('mfhi', 'mflo', 'mfhi1', 'mflo1', 'mfsa'):
        return f'{op} {g[i.rd]}'
    if op in ('mthi', 'mtlo', 'mthi1', 'mtlo1', 'mtsa'):
        return f'{op} {g[i.rs]}'
    if op in ('mult', 'multu', 'mult1', 'multu1', 'madd', 'maddu', 'madd1', 'maddu1'):
        return f'{op} {g[i.rd]}, {g[i.rs]}, {g[i.rt]}'
    if op in ('div', 'divu', 'div1', 'divu1'):
        return f'{op} {g[i.rs]}, {g[i.rt]}'
    if op in ('syscall', 'break', 'sync'):
        return op
    if op in ('mtsab', 'mtsah'):
        return f'{op} {g[i.rs]}, {i.imm}'
    if k == 'cop0':
        if op in ('mfc0', 'mtc0'):
            return f'{op} {g[i.rt]}, cop0r{i.rd}'
        return op
    if k == 'fpu':
        if op in ('mfc1', 'mtc1'):
            return f'{op} {g[i.rt]}, $f{i.fs}'
        if op in ('cfc1', 'ctc1'):
            return f'{op} {g[i.rt]}, fcr{i.fs}'
        if op.startswith('c.'):
            return f'{op} $f{i.fs}, $f{i.ft}'
        if op in ('adda.s', 'suba.s', 'mula.s', 'madda.s', 'msuba.s'):
            return f'{op} $f{i.fs}, $f{i.ft}'
        if op in ('sqrt.s',):
            return f'{op} $f{i.fd}, $f{i.ft}'
        if op in ('abs.s', 'mov.s', 'neg.s', 'cvt.w.s', 'cvt.s.w'):
            return f'{op} $f{i.fd}, $f{i.fs}'
        return f'{op} $f{i.fd}, $f{i.fs}, $f{i.ft}'
    if k == 'cop2':
        if op in ('qmfc2', 'qmtc2'):
            return f'{op}{".i" if i.word & 1 else ""} {g[i.rt]}, vf{i.rd}'
        if op in ('cfc2', 'ctc2'):
            return f'{op}{".i" if i.word & 1 else ""} {g[i.rt]}, vi{i.rd}'
    if k == 'vu0':
        d = fmt_dest(i.dest)
        return f'{op}.{d} fd{i.fd} fs{i.fs} ft{i.ft} bc{i.bc}'
    if k == 'mmi':
        return f'{op} {g[i.rd]}, {g[i.rs]}, {g[i.rt]}'
    if op in ('cache', 'pref'):
        return f'{op} 0x{i.rt:x}, {i.imm}({g[i.rs]})'
    return f'{op} {g[i.rd]}, {g[i.rs]}, {g[i.rt]}'
