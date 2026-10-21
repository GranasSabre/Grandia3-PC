import sys
from codeinfo import *
from r5900 import disasm
mod=load_main_elf('../../extracted/disc1/SLUS_213.34', 0x1e6000)
funcs,_,_,_=discover_functions(mod, extra_starts=[0x100008])
fstart={}
for f in funcs:
    for a in range(f.start,f.end,4): fstart[a]=f.start
# syscall stubs: addiu v1, zero, N ; syscall
res={}
for k,i in enumerate(mod.insns):
    if i.kind=='syscall':
        # find preceding addiu v1
        for j in range(k-1,max(0,k-6),-1):
            p=mod.insns[j]
            if p.op=='addiu' and p.rt==3 and p.rs==0:
                res.setdefault(p.imm,[]).append((fstart.get(i.pc,0),i.pc)); break
for n in sorted(res): print(n, hex(n&0xffffffff), [(hex(a),hex(b)) for a,b in res[n]])
