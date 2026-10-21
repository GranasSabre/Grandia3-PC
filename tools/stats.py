import struct, sys, collections
from r5900 import decode, disasm
def load(fn):
    d=open(fn,'rb').read()
    if d[:4]==b'\x7fELF':
        return 0x100000, d[0x100:0x100+0xe6000]
    # overlay header
    load_addr=struct.unpack_from('<I',d,8)[0]
    return load_addr, d
for fn in sys.argv[1:]:
    base,seg=load(fn)
    n=len(seg)//4
    words=struct.unpack('<%dI'%n, seg[:n*4])
    ins=[decode(w, base+4*k) for k,w in enumerate(words)]
    c=collections.Counter(x.op for x in ins)
    k=collections.Counter(x.kind for x in ins)
    print("==",fn,hex(base),n)
    print(k.most_common())
    for kind in ('vu0','cop2','mmi','fpu','cop0','invalid','other','syscall','break'):
        print(kind, sorted(((op,cnt) for op,cnt in c.items() if any(x.op==op and x.kind==kind for x in ins[:0]) or True) , key=lambda t:-t[1])[:0])
    bykind=collections.defaultdict(collections.Counter)
    for x in ins: bykind[x.kind][x.op]+=1
    for kind in ('vu0','cop2','mmi','fpu','cop0','other','syscall','break','sync','cache'):
        print(" ",kind, dict(bykind[kind]))
