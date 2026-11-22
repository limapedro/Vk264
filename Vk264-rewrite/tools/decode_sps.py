"""Decode an x264 SPS RBSP into named fields (debug aid for the C port)."""
import sys, pathlib, re

class BR:
    def __init__(self, data):
        self.d = data; self.p = 0; self.b = 0
    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.d[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v
    def ue(self):
        z = 0
        while self.u(1) == 0: z += 1
        return (1 << z) - 1 + (self.u(z) if z else 0)
    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)

def unescape(b):
    out = bytearray(); i = 0
    while i < len(b):
        if i + 2 < len(b) and b[i] == 0 and b[i+1] == 0 and b[i+2] == 3:
            out += b[i:i+2]; i += 3
        else:
            out.append(b[i]); i += 1
    return bytes(out)

data = pathlib.Path(sys.argv[1]).read_bytes()
for m in re.finditer(b'\x00\x00\x01(.)', data):
    nal = data[m.start()+3:]
    if nal[0] & 0x1f == 7:
        r = BR(unescape(nal[1:]))
        f = {}
        f['profile_idc'] = r.u(8); f['constraint0'] = r.u(1); f['constraint1'] = r.u(1)
        f['constraint2'] = r.u(1); f['constraint3'] = r.u(1); f['reserved'] = r.u(4)
        f['level_idc'] = r.u(8); f['sps_id'] = r.ue()
        if f['profile_idc'] >= 100:
            f['chroma_format_idc'] = r.ue()
            if f['chroma_format_idc'] == 3: f['separate_colour_plane'] = r.u(1)
            f['bit_depth_luma_minus8'] = r.ue(); f['bit_depth_chroma_minus8'] = r.ue()
            f['qpprime_bypass'] = r.u(1); f['scaling_matrix_present'] = r.u(1)
        f['log2_max_frame_num_minus4'] = r.ue()
        f['poc_type'] = r.ue()
        if f['poc_type'] == 0: f['log2_max_poc_lsb_minus4'] = r.ue()
        f['num_ref_frames'] = r.ue(); f['gaps'] = r.u(1)
        f['mb_width_minus1'] = r.ue(); f['mb_height_minus1'] = r.ue()
        f['frame_mbs_only'] = r.u(1)
        if not f['frame_mbs_only']: f['mbaff'] = r.u(1)
        f['direct8x8'] = r.u(1); f['crop'] = r.u(1)
        if f['crop']:
            f['crop_l'] = r.ue(); f['crop_r'] = r.ue(); f['crop_t'] = r.ue(); f['crop_b'] = r.ue()
        f['vui'] = r.u(1)
        if f['vui']:
            f['aspect_info'] = r.u(1)
            if f['aspect_info']:
                f['aspect_idc'] = r.u(8)
                if f['aspect_idc'] == 255: f['sar_w'] = r.u(16); f['sar_h'] = r.u(16)
            f['overscan_info'] = r.u(1)
            if f['overscan_info']: f['overscan'] = r.u(1)
            f['signal_type'] = r.u(1)
            if f['signal_type']:
                f['vidformat'] = r.u(3); f['fullrange'] = r.u(1); f['color_desc'] = r.u(1)
                if f['color_desc']:
                    f['colorprim'] = r.u(8); f['transfer'] = r.u(8); f['colmatrix'] = r.u(8)
            f['chroma_loc_info'] = r.u(1)
            if f['chroma_loc_info']: f['chroma_loc_top'] = r.ue(); f['chroma_loc_bottom'] = r.ue()
            f['timing_info'] = r.u(1)
            if f['timing_info']:
                f['units_in_tick'] = r.u(32); f['time_scale'] = r.u(32); f['fixed_fps'] = r.u(1)
            f['nal_hrd'] = r.u(1)
            if f['nal_hrd']:
                f['cpb_cnt_minus1'] = r.ue(); f['bit_rate_scale'] = r.u(4); f['cpb_size_scale'] = r.u(4)
                f['bit_rate_value_minus1'] = r.ue(); f['cpb_size_value_minus1'] = r.ue(); f['cbr'] = r.u(1)
                f['init_cpb_removal_length_minus1'] = r.u(5); f['cpb_removal_length_minus1'] = r.u(5)
                f['dpb_output_length_minus1'] = r.u(5); f['time_offset_length'] = r.u(5)
            f['vcl_hrd'] = r.u(1)
            if f['nal_hrd'] or f['vcl_hrd']: f['low_delay_hrd'] = r.u(1)
            f['pic_struct_present'] = r.u(1); f['bitstream_restriction'] = r.u(1)
            if f['bitstream_restriction']:
                f['mv_over_boundaries'] = r.u(1); f['max_bytes_per_pic_denom'] = r.ue()
                f['max_bits_per_mb_denom'] = r.ue(); f['log2_max_mv_h'] = r.ue(); f['log2_max_mv_v'] = r.ue()
                f['num_reorder_frames'] = r.ue(); f['max_dec_frame_buffering'] = r.ue()
        for k, v in f.items(): print(f'{k:32s} {v}')
        print('bits consumed', r.p)
        break
