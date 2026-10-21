import sys, collections
from codeinfo import *
from r5900 import disasm
mod=load_main_elf('../../extracted/disc1/SLUS_213.34', 0x1e6000)
funcs, switches, ptrs, jtt = discover_functions(mod)
print("functions", len(funcs), "switches", len(switches), "ptr candidates", len(ptrs))
starts={f.start for f in funcs}
# check cross-boundary branches
bad=collections.Counter()
ex=[]
for f in funcs:
    for a in range(f.start, f.end, 4):
        i=mod.insn(a)
        if i.kind in ('branch','branchl') and not (f.start<=i.target<f.end):
            bad['branch_out']+=1; ex.append((f.start,i.pc,i.target,'b'))
        if i.kind=='jump' and not (f.start<=i.target<f.end):
            if i.target in starts: bad['tail_j']+=1
            else: bad['j_mid']+=1; ex.append((f.start,i.pc,i.target,'j'))
        if i.kind=='jr' and i.rs!=31 and i.pc not in switches: bad['jr_unknown']+=1; ex.append((f.start,i.pc,None,'jr'))
print(bad)
for e in ex[:40]: print([hex(x) if isinstance(x,int) else x for x in e])
# ptr candidates that are not function starts
nf=[p for p in ptrs if p not in starts]
print("ptr not start", len(nf), [hex(p) for p in sorted(nf)[:30]])
# unrecognized jump tables
