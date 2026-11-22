"""Decode the slice header of an x264 Annex-B stream (debug aid)."""
import sys, pathlib, re

class BR:
    def __init__(self, d, bit=0): self.d=d; self.p=bit
    def u(self,n):
        v=0
        for _ in range(n):
            v=(v<<1)|((self.d[self.p>>3]>>(7-(self.p&7)))&1); self.p+=1
        return v
    def ue(self):
        z=0
        while self.u(1)==0: z+=1
        return (1<<z)-1+(self.u(z) if z else 0)
    def se(self):
        k=self.ue(); return (k+1)//2 if k&1 else -(k//2)

def unescape(b):
    out=bytearray(); i=0
    while i<len(b):
        if i+2<len(b) and b[i]==0 and b[i+1]==0 and b[i+2]==3:
            out+=b[i:i+2]; i+=3
        else: out.append(b[i]); i+=1
    return bytes(out)

data=pathlib.Path(sys.argv[1]).read_bytes()
starts=[m.start() for m in re.finditer(b'\x00\x00\x01',data)]
for s in starts:
    nal=data[s+3:]
    if (nal[0]&0x1f) in (1,5):
        r=BR(unescape(nal[1:]))
        f={}
        f['first_mb_in_slice']=r.ue()
        st=r.ue(); f['slice_type']=st-5
        f['pps_id']=r.ue()
        f['frame_num']=r.u(4)  # log2_max_frame_num=4 for this stream
        if st-5==2 and False: pass
        # IDR: idr_pic_id
        f['idr_pic_id']=r.ue()
        # poc_type=2: no poc fields
        # no redundant, I slice: no ref list, no weights
        # nal_ref_idc != 0 and IDR:
        f['no_output_of_prior_pics']=r.u(1)
        f['long_term_ref_flag']=r.u(1)
        # no cabac for I
        f['slice_qp_delta']=r.se()
        f['disable_deblocking_filter_idc']=r.ue()
        if f['disable_deblocking_filter_idc']!=1:
            f['alpha_c0_offset_div2']=r.se()
            f['beta_offset_div2']=r.se()
        for k,v in f.items(): print(f'{k:34s} {v}')
        print('slice header bits consumed',r.p)
        rbsp=unescape(nal[1:]); print('slice NAL bytes',len(nal),'RBSP',rbsp.hex(' '))
        break
