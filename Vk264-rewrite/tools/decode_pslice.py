"""Decode a small P-slice header + skip runs (debug aid)."""
import pathlib, re, sys

class BR:
    def __init__(self, b): self.b=b; self.p=0
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

b = pathlib.Path(sys.argv[1]).read_bytes()
mbits = 4 if len(sys.argv)<3 else int(sys.argv[2])
for m in re.finditer(b'\x00\x00\x01(.)', b):
    nal = b[m.start()+3:]
    if (nal[0]&0x1f) != 1: continue
    r = BR(nal[1:])
    print('nal_ref_idc', nal[0]>>5)
    print('first_mb', r.ue())
    print('slice_type', r.ue())
    print('pps_id', r.ue())
    print('frame_num', r.u(mbits))
    print('num_ref_override', r.u(1))
    print('ref_list_mod', r.u(1))
    print('adaptive_ref_marking', r.u(1))
    print('slice_qp_delta', r.se())
    print('deblock_disable', r.ue())
    print('skip_run', r.ue(), 'bit', r.p)
    break
