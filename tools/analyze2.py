import struct, sys, collections
from r5900 import decode, disasm
d=open(sys.argv[1],'rb').read()
base=0x100000; seg=d[0x100:0x100+0xf7180]
n=len(seg)//4
words=struct.unpack('<%dI'%n, seg)
ins=[decode(w, base+4*k) for k,w in enumerate(words)]
def show(a0,a1):
    for k in range((a0-base)//4,(a1-base)//4):
        x=ins[k]; print(f"{x.pc:08x}: {x.word:08x}  {disasm(x)}")
for k,x in enumerate(ins[:(0x1e6000-base)//4]):
    if x.kind=='invalid': print("INV", hex(x.pc), hex(x.word))
show(int(sys.argv[2],16), int(sys.argv[3],16))
