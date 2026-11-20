"""Decode CAVLC MB syntax of an H.264 IDR slice into coefficient arrays.
Usage: decode_mb.py <file.h264> [tables.h]
"""
import re, sys
from pathlib import Path

HDR = Path(sys.argv[2] if len(sys.argv) > 2 else
           Path(__file__).resolve().parent.parent/'src'/'ref'/'cavlc_tables.h').read_text()

def table(name):
    m = re.search(r'static const vk_vlc_t '+name+r'(?:\[[^\]]*\])+\s*=\s*\{(.*?)\n\};', HDR, re.S)
    flat = [(int(a),int(b)) for a,b in re.findall(r'\{(\d+), (\d+)\}', m.group(1))]
    return flat

def unflat(flat, dims):
    total=1
    for d in dims: total*=d
    flat = flat+[(0,0)]*(total-len(flat))
    it=iter(flat)
    def b(ds):
        if not ds: return next(it)
        return [b(ds[1:]) for _ in range(ds[0])]
    return b(dims)

C0=table('vk_coeff0_token'); CT=unflat(table('vk_coeff_token'),[6,16,4])
TZ=unflat(table('vk_total_zeros'),[15,16]); TZ2=unflat(table('vk_total_zeros_2x2_dc'),[3,4])
RBI=unflat(table('vk_run_before_init'),[7,16])

def gen_level():
    T={}
    for s in range(7):
        a={}
        for lv in range(-64,64):
            mask=-1 if lv<0 else 0; ab=(lv^mask)-mask; c=ab*2-mask-2 if ab else 0; nx=s
            if (c>>s)<14: sz=(c>>s)+1+s; bits=(1<<s)+(c&((1<<s)-1))
            elif s==0 and c<30: sz=19; bits=16+(c-14)
            elif s>0 and (c>>s)==14: sz=15+s; bits=(1<<s)+(c&((1<<s)-1))
            else:
                c-=15<<s
                if s==0: c-=15
                sz=28; bits=4096+c
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
        b,s=t
        if s==0: raise ValueError('empty VLC')
        got=self.u(s)
        if got!=b: raise ValueError(f'VLC mismatch at {self.p-s}: got {got:0{s}b} exp {b:0{s}b}')
    def find_vlc(self,cands):
        st=self.p
        for t in cands:
            if t[1]==0: continue
            if self.u(t[1])==t[0]: return t
            self.p=st
        raise ValueError('no VLC match')

def clz(x):
    if not x: return 32
    n=0
    for sh in (16,8,4,2,1):
        if not (x & (0xffffffff << (32-sh))): n+=sh; x<<=sh
    return n

RUN_BEFORE={}
for i in range(1<<16):
    # replicate x264_cavlc_init: use a 16-bit mask with bits at set positions
    mask=i
    # compute total/last from set bit positions
    pos=[j for j in range(16) if mask&(1<<j)]
    if not pos: RUN_BEFORE[i]=(0,0); continue
    last=pos[-1]; total=len(pos); zeros=last+1-total
    m=((mask << (clz(mask)+1)) & 0xffffffff) if mask else 0
    size=0; bits=0
    idx=min(zeros,7)-1
    for _ in range(total-1):
        if zeros<=0: break
        idx=min(zeros,7)-1
        run=clz(m) if m else 0
        t=RBI[idx][run] if run<16 else (0,0)
        size+=t[1]; bits=(bits<<t[1])|t[0]
        zeros-=run
        m=((m << (run+1)) & 0xffffffff) if run<32 else 0
    RUN_BEFORE[i]=(bits,size)

def decode_block(r, cat, nC, count, chroma=False):
    # caller passes nnz flag; if zero, coeff0 token
    start=r.p
    for t in C0 if False else []: pass
    # determine total/trailing by searching coeff_token (nnz>0)
    found=None
    for t in range(1,count+1):
        for tr in range(4):
            if CT[nC][t-1][tr][1] and True:
                pass
    st=r.p
    match=None
    for t in range(1,count+1):
        for tr in range(4):
            tok=CT[nC][t-1][tr]
            if tok[1]==0: continue
            if r.u(tok[1])==tok[0]:
                match=(t,tr); break
            r.p=st
        if match: break
    if not match: raise ValueError('no coeff token')
    total,trail=match
    signs=[-1 if r.u(1) else 1 for _ in range(trail)]
    levels=[]
    suffix=1 if (total>10 and trail<3) else 0
    for i in range(trail,total):
        st=r.p; lv=None
        val0=i-trail
        for cand in range(-512,513):
            val=cand+64-(1 if i==trail and trail<3 else 0)+64
            if not (0<=val<128): continue
            b,s,nx=LT[suffix][val]
            if r.u(s)==b: lv=cand; suffix=nx; break
            r.p=st
        if lv is None: raise ValueError('no level token')
        levels.append(lv)
    coeff=signs+levels
    # total_zeros
    if cat==3:
        if total < 4:
            b,s=TZ2[total-1][0]; r.vlc((b,s))
            tz=0  # approximate
        else: tz=0
    elif total < 16:
        # find total_zero by searching values 0..count-total
        st=r.p; tz=None
        for z in range(0,count-total+1):
            b,s=TZ[total-1][z]
            if s==0: continue
            if r.u(s)==b: tz=z; break
            r.p=st
        if tz is None: tz=0
    else: tz=0
    # run_before: decode (total-1) codes
    runs=[]
    for _ in range(total-1):
        st=r.p; rb=None
        for cand,(b,s) in RUN_BEFORE.items():
            if s==0 or s>25: continue
            if r.u(s)==b: rb=cand; break
            r.p=st
        if rb is None: rb=0
        runs.append(rb>>5)
    # reconstruct 16 coefficients in scan order
    out=[0]*count
    pos=tz
    for i,c in enumerate(coeff):
        pos+=runs[i] if i<len(runs) else 0
        if pos>=count: break
        out[pos]=c; pos+=1
    return out

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
    if (nal[0]&0x1f)!=5: continue
    rb=unescape(nal[1:]); r=BR(rb,24)
    mt=r.ue(); cp=r.ue(); qd=r.se()
    cbp_l=15 if mt>=19 else 0
    cbp_c=2 if mt>=31 else 1 if mt>=23 else 0
    print('mb_type',mt,'cbp_luma',cbp_l,'cbp_chroma',cbp_c,'bit',r.p)
    blocks={}
    # luma DC: nnz from mt>=? use coeff0 if impossible; just try full path
    try:
        blocks['dc']=decode_block(r,0,0,16)
        print('dc',blocks['dc'])
    except Exception as e:
        print('dc decode failed',e); break
    for b in range(16):
        if not (cbp_l & (1<<(b//4))): continue
        try: blocks[f'ac{b}']=decode_block(r,1,0,16)
        except Exception as e: print(f'ac{b} failed',e); break
    for p in range(2):
        try: blocks[f'cdc{p}']=decode_block(r,3,4,4)
        except Exception as e: print(f'cdc{p} failed',e); break
    if cbp_c==2:
        for p in range(2):
            for b in range(4):
                try: blocks[f'cac{p}_{b}']=decode_block(r,4,0,16)
                except Exception as e: print(f'cac failed',e); break
    print('decoded blocks:',len(blocks),'bit',r.p)
    break
