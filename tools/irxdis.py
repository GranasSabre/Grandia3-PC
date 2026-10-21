import sys, struct
from iop_recomp import IRX
from r5900 import decode, disasm
irx=IRX(sys.argv[1], int(sys.argv[2],16))
imps=irx.imports()
a0=int(sys.argv[3],16); a1=int(sys.argv[4],16)
for a in range(a0,a1,4):
    w=struct.unpack_from('<I',irx.img,a-irx.base)[0]
    i=decode(w,a)
    s=disasm(i)
    if i.kind=='jal' and i.target in imps: s+=f'   <{imps[i.target][0]}:{imps[i.target][1]}>'
    print(f"{a:08x}: {w:08x}  {s}")
