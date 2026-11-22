"""Decode the first macroblock of a CAVLC I-slice using x264's VLC tables."""
import re, sys
from pathlib import Path

hdr = Path(sys.argv[2] if len(sys.argv)>2 else 'Vk264-rewrite/src/cavlc_tables.h').read_text()

def table(name):
    m = re.search(r'static const vk_vlc_t '+name+r'(?:\[[^\]]*\])+\s*=\s*\{(.*?)\n\};', hdr, re.S)
    if not m: raise SystemExit('missing '+name)
    vals = re.findall(r'\{(\d+), (\d+)\}', m.group(1))
    return [(int(b),int(s)) for b,s in vals]

def unflatten(flat, dims):
    total = 1
    for d in dims: total *= d
    flat = list(flat) + [(0,0)] * (total - len(flat))
    it = iter(flat)
    def build(ds):
        if not ds: return next(it)
        return [build(ds[1:]) for _ in range(ds[0])]
    return build(dims)

coeff0 = table('vk_coeff0_token')
coeff_token = unflatten(table('vk_coeff_token'), [6,16,4])
total_zeros = unflatten(table('vk_total_zeros'), [15,16])
tz2x2 = unflatten(table('vk_total_zeros_2x2_dc'), [3,4])
# run_before_init table is not needed for nnz<=2 here

class BR:
    def __init__(self,b,bit=0): self.b=b; self.p=bit
    def u(self,n):
        v=0
        for _ in range(n):
            v=(v<<1)|((self.b[self.p>>3]>>(7-(self.p&7)))&1); self.p+=1
        return v
    def ue(self):
        z=0
        while self.u(1)==0: z+=1
        return (1<<z)-1+(self.u(z) if z else 0)
    def se(self):
        k=self.ue(); return (k+1)//2 if k&1 else -(k//2)
    def vlc(self,t):
        b,s=t; return self.u(s)==b

def unescape(b):
    out=bytearray(); i=0
    while i<len(b):
        if i+2<len(b) and b[i]==0 and b[i+1]==0 and b[i+2]==3:
            out+=b[i:i+2]; i+=3
        else: out.append(b[i]); i+=1
    return bytes(out)

data=Path(sys.argv[1]).read_bytes()
for m in re.finditer(b'\x00\x00\x01(.)',data):
    nal=data[m.start()+3:]
    if (nal[0]&0x1f)==5:
        rbsp=unescape(nal[1:]); r=BR(rbsp,24)
        print('mb_type',r.ue(),'chroma_pred',r.ue(),'qp_delta',r.se(),'bit',r.p)
        # luma DC: try coeff0 token
        print('luma DC next 8 bits', ''.join(str(r.u(1)) for _ in range(8)))
        break
