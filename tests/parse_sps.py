#!/usr/bin/env python3
"""Debug helper: decode SPS fields from Phoenix dump and x264 reference."""
import pathlib, subprocess, sys

class BR:
    def __init__(self, bits): self.b, self.p = bits, 0
    def u(self, n):
        v = int(self.b[self.p:self.p+n], 2) if n else 0; self.p += n; return v
    def ue(self):
        z = 0
        while self.b[self.p] == '0': z += 1; self.p += 1
        self.p += 1
        v = (1<<z)-1 + (int(self.b[self.p:self.p+z],2) if z else 0); self.p += z
        return v

def unesc(b):
    o = bytearray(); i = 0
    while i < len(b):
        if i+2 < len(b) and b[i] == 0 and b[i+1] == 0 and b[i+2] == 3:
            o += b[i:i+2]; i += 3
        else: o.append(b[i]); i += 1
    return bytes(o)

def parse(nal):
    r = BR(''.join(f'{x:08b}' for x in unesc(nal)[1:]))
    d = {'profile':r.u(8),'flags':r.u(8),'level':r.u(8),'sps_id':r.ue()}
    if d['profile'] >= 100:
        d['chroma']=r.ue(); d['bd_l']=r.ue(); d['bd_c']=r.ue()
        d['qpprime']=r.u(1); d['scaling']=r.u(1)
    d['log2fn']=r.ue(); d['poc_type']=r.ue()
    if d['poc_type']==0: d['log2poc']=r.ue()
    d['num_refs']=r.ue(); d['gaps']=r.u(1); d['mbw']=r.ue(); d['mbh']=r.ue()
    d['frame_only']=r.u(1)
    if not d['frame_only']: d['mbaff']=r.u(1)
    d['direct']=r.u(1); d['crop']=r.u(1)
    if d['crop']: d['cropvals']=[r.ue() for _ in range(4)]
    d['vui']=r.u(1)
    if d['vui']:
        d['aspect_present']=r.u(1)
        if d['aspect_present']:
            d['aspect_idc']=r.u(8)
        d['overscan']=r.u(1)
        d['signal']=r.u(1); d['chroma_loc']=r.u(1); d['timing']=r.u(1)
        if d['timing']:
            d['units']=r.u(32); d['scale']=r.u(32); d['fixed']=r.u(1)
        d['nal_hrd']=r.u(1); d['vcl_hrd']=r.u(1)
        d['pic_struct']=r.u(1); d['restr']=r.u(1)
        if d['restr']:
            d['mvbound']=r.u(1); d['maxbytes']=r.ue(); d['maxbits']=r.ue()
            d['mv_h']=r.ue(); d['mv_v']=r.ue(); d['reorder']=r.ue(); d['maxdec']=r.ue()
    d['endpos']=r.p
    return d

def nals(data):
    out, i = [], 0
    while True:
        j = data.find(b'\x00\x00\x01', i)
        if j < 0: break
        s = j+3; k = data.find(b'\x00\x00\x01', s)
        if k < 0: k = len(data)
        out.append(data[s:k]); i = k
    return out

if __name__ == '__main__':
    R=pathlib.Path(__file__).resolve().parents[1]
    for preset in (sys.argv[1:] or ['ultrafast','medium']):
        h=subprocess.run([str(R/'build/tests/Release/phx_header_dump.exe'),preset,'28','cabac'],
                         capture_output=True,text=True).stdout.strip()
        got=bytes.fromhex(h); gs=nals(got)[0]
        rs=nals((R/f'build/parity/synth-{preset}-cabac.ref.h264').read_bytes())[0]
        a,b=parse(gs),parse(rs)
        print('===',preset)
        for k in sorted(set(a)|set(b)):
            if a.get(k)!=b.get(k): print(f'  {k:12s} ours={a.get(k)} ref={b.get(k)}')
