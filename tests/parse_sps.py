#!/usr/bin/env python3
"""Debug helper: decode SPS/PPS fields from Phoenix dump and x264 reference.

Source: reference/x264/encoder/set.c:305-475 (SPS write),
        reference/x264/encoder/set.c:505-570 (PPS write, including the
        duplicated chroma_qp_index_offset at :522 and :567),
        common/bitstream.h:241-272 (ue/se/rbsp_trailing).
"""
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
    def se(self):
        """bitstream.h:246-261 — map signed Exp-Golomb codeNum to value.

        SHADER: C — diagnostic mirror of phx_bs_write_se.
        """
        k = self.ue()
        return (k+1)//2 if k & 1 else -(k//2)

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

def ue(v):
    """Exp-Golomb codeNum for v (common/bitstream.h:221-245).

    SHADER: C — serial header stage; mirrors phx_bs_write_ue.
    """
    n = v + 1
    k = n.bit_length() - 1
    return '0' * k + format(n, '0' + str(k + 1) + 'b')


def se(v):
    """Signed Exp-Golomb for v (common/bitstream.h:246-261).

    SHADER: C — serial header stage; mirrors phx_bs_write_se.
    """
    return ue(2*v - 1 if v > 0 else -2*v)


def pps_tail_bits(cqo, transform, tail_pos=0):
    """Encode set.c:529-570's PPS tail; flat CQM => no scaling lists.

    Source: reference/x264/encoder/set.c:551-567 (transform_8x8 block and the
    second chroma_qp_index_offset write), :570 and common/bitstream.h:272
    (rbsp stop bit / byte alignment).  tail_pos is the absolute bit position
    at which the tail starts, so alignment is relative to the RBSP start.

    SHADER: C — serial header stage; used as the focused tail oracle.
    """
    bits = ''
    if transform:
        bits += '10'          # transform_8x8_mode_flag=1, scaling_matrix_present=0
        bits += se(cqo)       # set.c:567 — duplicate of the :522 field
    bits += '1'               # rbsp_stop_one_bit (bitstream.h:272)
    bits += '0' * ((-(tail_pos + len(bits))) % 8)
    return bits


def parse_pps(nal):
    """Decode a PPS NAL, exposing both chroma_qp_index_offset writes.

    Source: reference/x264/encoder/set.c:505-570 and common/bitstream.h:
    241-272.  Returns the bit position where the tail begins, the raw tail,
    the decoded tail fields, and the value of the second se() when present.

    SHADER: C — serial header stage; diagnostic mirror of phx_pps_write.
    """
    bits = ''.join(f'{x:08b}' for x in unesc(nal)[1:])
    r = BR(bits)
    d = {'pps_id': r.ue(), 'sps_id': r.ue(), 'cabac': r.u(1),
         'pic_order': r.u(1), 'groups': r.ue(), 'l0': r.ue(), 'l1': r.ue(),
         'wp': r.u(1), 'wbp': r.u(2), 'qp': r.se(), 'qs': r.se(),
         'cqo': r.se(), 'deblock': r.u(1), 'constrained': r.u(1),
         'redundant': r.u(1)}
    d['tail_pos'] = r.p
    d['tail'] = bits[r.p:]
    tb = BR(d['tail'])
    d['transform'] = 0
    d['cqo2'] = None
    if len(d['tail']) >= 4 and tb.u(1) == 1 and tb.u(1) == 0:
        try:
            d['transform'] = 1
            d['cqo2'] = tb.se()
        except IndexError:      # the no-transform tail is just 1 + zeros
            d['transform'] = 0
            d['cqo2'] = None
    d['rbsp_tail'] = d['tail'][tb.p:] if d['transform'] else d['tail']
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
