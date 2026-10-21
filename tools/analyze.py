import struct, sys, collections
from r5900 import decode, disasm
d=open(sys.argv[1],'rb').read()
base=0x100000; seg=d[0x100:0x100+0xf7180]
n=len(seg)//4
words=struct.unpack('<%dI'%n, seg)
ins=[decode(w, base+4*k) for k,w in enumerate(words)]
# find invalid density windows
win=256
for k in range(0,n,win*4):
    inv=sum(1 for x in ins[k:k+win*4] if x.kind=='invalid')
    print(f"{base+4*k:08x} invalid {inv}/{min(win*4,n-k)}")
