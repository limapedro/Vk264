"""Full CAVLC MB decoder for the reference I16 slice (debug aid)."""
import re
from pathlib import Path

H = Path(__file__).resolve().parent.parent / 'src' / 'cavlc_tables.h'
hdr = H.read_text()

def table(name):
    m = re.search(r'static const vk_vlc_t '+name+r'(?:\[[^\]]*\])+\s*=\s*\{(.*?)\n\};', hdr, re.S)
    flat = [(int(a),int(b)) for a,b in re.findall(r'\{(\d+), (\d+)\}', m.group(1))]
    return flat

def unflatten(flat, dims):
    total=1
    for d in dims: total*=d
    flat = flat+[(0,0)]*(total-len(flat))
    it=iter(flat)
    def b(ds):
        if not ds: return next(it)
        return [b(ds[1:]) for _ in range(ds[0])]
    return b(dims)

C0=table('vk_coeff0_token')
CT=unflatten(table('vk_coeff_token'),[6,16,4])
TZ=unflatten(table('vk_total_zeros'),[15,16])
TZ2=unflatten(table('vk_total_zeros_2x2_dc'),[3,4])

def gen_level():
    T={}
    for s in range(7):
        a={}
        for lv in range(-64,64):
            mask=-1 if lv<0 else 0; ab=(lv^mask)-mask
            c=ab*2-mask-2 if ab else 0; nx=s
            if (c>>s)<14: sz=(c>>s)+1+s; bits=(1<<s)+(c&((1<<s)-1))
            elif s==0 and c<30: sz=19; bits=16+(c-14)
            elif s>0 and (c>>s)==14: sz=15+s; bits=(1<<s)+(c&((1<<s)-1))
            else:
                c-=15<<s
                if s==0: c-=15
                sz=28; bits=(1<<12)+c
            if nx==0: nx+=1
            if ab>(3<<(nx-1)) and nx<6: nx+=1
            a[lv+64]=(bits,sz,nx)
        T[s]=a
    return T
LT=gen_level()

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
        b,s=t; got=self.u(s)
        if got!=b: raise ValueError(f'vlc mismatch at bit {self.p-s}: got {got:0{s}b} exp {b:0{s}b}')
    def bits(self,n): return self.u(n)

def decode_block(r, cat, nC, count, chroma=False):
    # coeff0 path handled by caller via nnz flag; here assume nnz known
    total,trail = None,None
    for t in range(4):
        for tt in range(4):
            if CT[nC][t-1 if t else 0][tt] and True:
                pass
    # find matching coeff_token by trying patterns
    start=r.p
    found=False
    for t in range(1,17):
        for tr in range(4):
            bits,size=CT[nC][t-1][tr]
            if size==0: continue
            r.p=start
            if r.u(size)==bits:
                total,trail=t,tr; r.p=start+size; found=True; break
        if found: break
    if not found: raise ValueError('no coeff token')
    print(f'  coeff_token total={total} trailing={trail} (nC={nC})')
    # signs
    signs=[]
    for i in range(trail):
        signs.append(-1 if r.bits(1) else 1)
    levels=[]
    suffix = 1 if (total>10 and trail<3) else 0
    for i in range(trail,total):
        lv = None
        v0 = i-trail
        # try level tokens for this suffix
        st=r.p
        for l in range(-64,65):
            val=l+64-(1 if i==trail and trail<3 else 0)+64
            if not (0<=val<128): continue
            b,s,nx=LT[suffix][val]
            r.p=st
            if r.u(s)==b:
                lv=l; suffix=nx; r.p=st+s; break
        if lv is None: raise ValueError('no level token')
        levels.append(lv)
    print('  levels',signs,levels)
    # total zeros
    if cat==3:
        if total < 4:
            b,s=TZ2[total-1][0]
            pass
    # skip total zeros/run before for brevity
    return total

def unescape(b):
    out=bytearray(); i=0
    while i<len(b):
        if i+2<len(b) and b[i]==0 and b[i+1]==0 and b[i+2]==3:
            out+=b[i:i+2]; i+=3
        else: out.append(b[i]); i+=1
    return bytes(out)

data=(Path(__file__).resolve().parent.parent/'build'/'ref16.h264').read_bytes()
for m in re.finditer(b'\x00\x00\x01(.)',data):
    nal=data[m.start()+3:]
    if (nal[0]&0x1f)==5:
        r=BR(unescape(nal[1:]),24)
        print('mb_type',r.ue(),'chroma_pred',r.ue(),'qp_delta',r.se(),'bit',r.p)
        print('luma DC block:')
        decode_block(r,0,0,16)
        print('bit after luma DC',r.p)
        break
