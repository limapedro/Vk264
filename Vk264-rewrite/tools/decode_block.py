"""Decode CAVLC blocks from an x264/oracle slice using correctly parsed tables."""
import re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from c_tables import CT, C0, TZ, TZ2, RBI

# Generated level tokens (x264 common/vlc.c:33)
LT = {}
for s in range(7):
    arr = {}
    for lv in range(-512, 513):
        mask = -1 if lv < 0 else 0
        ab = (lv ^ mask) - mask
        c = ab*2 - mask - 2 if ab else 0
        nx = s
        if (c >> s) < 14:
            sz = (c >> s) + 1 + s; bits = (1 << s) + (c & ((1 << s)-1))
        elif s == 0 and c < 30:
            sz = 19; bits = 16 + (c - 14)
        elif s > 0 and (c >> s) == 14:
            sz = 15 + s; bits = (1 << s) + (c & ((1 << s)-1))
        else:
            c -= 15 << s
            if s == 0: c -= 15
            sz = 28; bits = 4096 + c
        if nx == 0: nx += 1
        if ab > (3 << (nx-1)) and nx < 6: nx += 1
        arr[lv+512] = (bits, sz, nx, lv)
    LT[s] = arr

class BR:
    def __init__(self, b, bit=0): self.b=b; self.p=bit
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
    def match(self,t):
        bits,size=t
        if size==0: raise ValueError('empty VLC')
        st=self.p
        got=self.u(size)
        if got==bits: return True
        self.p=st
        return False

def decode(r, cat, nC, count):
    st=r.p
    # coeff0 path
    if r.match(C0[nC]):
        return [0]*count
    total=trail=None
    for t in range(1, count+1):
        if t-1 >= len(CT[nC]): break
        for tr_i,tok in enumerate(CT[nC][t-1]):
            if r.match(tok): total,trail=t,tr_i; break
        if total: break
    if total is None: raise ValueError(f'no coeff token at bit {st}')
    signs=[-1 if r.u(1) else 1 for _ in range(trail)]
    levels=[]
    suffix=1 if (total>10 and trail<3) else 0
    for i in range(trail,total):
        st=r.p; found=None
        for tok in LT[suffix].values():
            bits,sz,nx,lv=tok
            if r.match((bits,sz)): found=(lv,nx); break
        if found is None: raise ValueError(f'no level at bit {st}')
        levels.append(found[0]); suffix=found[1]
    coeffs=signs+levels
    # total_zeros
    tz=0
    if cat==3:
        if total < 4 and total>=1:
            for z,tok in enumerate(TZ2[total-1]):
                if r.match(tok): tz=z; break
    elif total < count and total>=1:
        for z,tok in enumerate(TZ[total-1]):
            if r.match(tok): tz=z; break
    # run_before: emulate x264_run_before table search
    runs=[]
    if total>1:
        st=r.p
        # brute force: find a code whose bit pattern matches then use its run info
        # rebuild RUN_BEFORE mapping compactly
        for mask in range(1, 1<<count if count<=16 else 1<<16):
            pass
        # simpler: read until we have total-1 runs using RBI directly
        # x264 writes x264_run_before[mask] = (bits<<5)|size
        found=None
        for mask in range(1, 1<<16):
            # compute bits/size for this mask using RBI (port of vlc.c)
            pos=[j for j in range(16) if mask&(1<<j)]
            if not pos: continue
            last=pos[-1]; nz=len(pos); zeros=last+1-nz
            m=(mask << (32-(mask.bit_length()))) & 0xffffffff if mask else 0
            size=0; bits=0
            for _ in range(nz-1):
                if zeros<=0: break
                idx=min(zeros,7)-1
                run=32-(m.bit_length()) if m else 0
                if run>=16: break
                tok=RBI[idx][run]
                size+=tok[1]; bits=(bits<<tok[1])|tok[0]
                zeros-=run
                m=(m << (run+1)) & 0xffffffff
            code=(bits<<5)|size
            b,s=code&0x1f,code>>5
            if s and r.match((b,s)): found=(mask,nz); break
            r.p=st
        if found: runs=[1]*0  # placeholder
    # reconstruct: use tz + runs approximated
    out=[0]*count
    for i,c in enumerate(coeffs):
        p=tz+i
        if p<count: out[p]=c
    return out

def unescape(b):
    out=bytearray(); i=0
    while i<len(b):
        if i+2<len(b) and b[i]==0 and b[i+1]==0 and b[i+2]==3:
            out+=b[i:i+2]; i+=3
        else: out.append(b[i]); i+=1
    return bytes(out)

for f in sys.argv[1:]:
    data=Path(f).read_bytes()
    print('===',f)
    for m in re.finditer(b'\x00\x00\x01(.)',data):
        nal=data[m.start()+3:]
        if (nal[0]&0x1f)!=5: continue
        r=BR(unescape(nal[1:]),24)
        mt=r.ue(); cp=r.ue(); qd=r.se()
        print('mb_type',mt,'bit',r.p)
        try:
            dc=decode(r,0,0,16)
            print('luma DC',dc,'bit',r.p)
        except Exception as e:
            print('luma DC failed',e); continue
        break
