#!/usr/bin/env python3
"""Locate the FIRST Phoenix/x264 divergence inside one Annex-B stream.

The probe is a diagnostic for the parity iteration loop, not a replacement for
the byte-exact harness.  It answers three questions for one case:

  1. Which NALs are present in each stream, and which are byte-exact?
  2. Where is the first differing byte (with a hexdump window)?
  3. For NAL type 1/5, do the slice-header field values match?  If they do,
     the divergence is in entropy/macroblock data; if they do not, it is in a
     header writer (SPS/PPS/slice header).

Slice-header fields are decoded and printed in the H.264 7.3.3 syntax order
used by src/phx_slice.c::phx_slice_header_write and
reference/x264/encoder/encoder.c:213-364 (slice_header_write).  Parameter-set
fields follow src/phx_headers.c (set.c:305-570).  Python standard library only.

Stream names (relative to --phx-dir / --refs-dir):
    synth-<preset>-<entropy>.phx.h264
    synth-<preset>-<entropy>.ref.h264

Usage:
    python tools/first_frame_probe.py --preset medium --entropy cabac
    python tools/first_frame_probe.py --preset all --entropy cavlc
    python tools/first_frame_probe.py --preset fast --entropy cabac \
        --refs-dir build/parity --phx-dir build/parity --all-slices

Exit codes:
    0  every compared NAL is byte-exact, NAL counts match, and both streams
       contain a slice NAL (the case is byte-exact by this probe's measure)
    1  a divergence was found (DIFF, missing/extra NAL, header field mismatch,
       or no slice NAL in the Phoenix stream)
    2  usage / missing file / unparsable stream (probe could not decide)
"""
from __future__ import annotations

import argparse
import difflib
import pathlib
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_DIR = ROOT / 'build' / 'parity'

PRESETS = ('ultrafast', 'superfast', 'veryfast', 'faster', 'fast',
           'medium', 'slow', 'slower', 'veryslow', 'placebo')
ENTROPIES = ('cabac', 'cavlc')

NAL_NAMES = {
    1: 'non-IDR slice', 5: 'IDR slice', 6: 'SEI', 7: 'SPS', 8: 'PPS',
    9: 'AUD', 10: 'end of sequence', 11: 'end of stream', 12: 'filler',
    14: 'prefix', 20: 'slice extension',
}

HIGH_PROFILES = (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135)


class ParseError(Exception):
    """A NAL's RBSP ended before its syntax could be decoded."""


def nal_name(nal_type: int) -> str:
    return NAL_NAMES.get(nal_type, 'reserved/other')


# ---------------------------------------------------------------------------
# Annex-B NAL parsing (157 B of stream, no emulation-prevention confusion)
# ---------------------------------------------------------------------------

@dataclass
class Nal:
    index: int                  # 0-based index among start codes in the stream
    byte: int                   # NAL header byte
    payload: bytes              # NAL unit bytes, trailing_zero_8bits removed
    start_code: bytes           # 00 00 01 or 00 00 00 01
    stream_offset: int          # offset of payload[0] in the whole stream

    @property
    def type(self) -> int:
        return self.byte & 0x1f

    @property
    def nal_ref_idc(self) -> int:
        return (self.byte >> 5) & 0x03

    @property
    def label(self) -> str:
        return f'{self.type} ({nal_name(self.type)})'

    @property
    def hex_prefix(self) -> str:
        return self.payload[:8].hex(' ')


def annexb_nals(data: bytes) -> List[Nal]:
    """Split an Annex-B byte stream into NAL units.

    Handles both 3- and 4-byte start codes and strips trailing_zero_8bits from
    each NAL (the zero bytes between NALs and before EOF).  Emulation
    prevention bytes are kept here; they are removed only for RBSP decoding.
    """
    starts: List[Tuple[int, int]] = []  # (start_code_pos, payload_pos)
    i, n = 0, len(data)
    while i + 2 < n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            sc_pos = i - 1 if i > 0 and data[i - 1] == 0 else i
            starts.append((sc_pos, i + 3))
            i += 3
        else:
            i += 1

    out: List[Nal] = []
    for k, (sc_pos, payload_pos) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else n
        payload = data[payload_pos:end].rstrip(b'\x00')
        if not payload:
            continue
        out.append(Nal(index=k, byte=payload[0], payload=payload,
                       start_code=data[sc_pos:payload_pos],
                       stream_offset=payload_pos))
    return out


def rbsp_unescape(nal_payload_after_header: bytes) -> bytes:
    """Remove H.264 emulation prevention bytes (00 00 03 -> 00 00)."""
    out = bytearray()
    zeros = 0
    for b in nal_payload_after_header:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def first_difference(a: bytes, b: bytes) -> int:
    """Offset of the first differing byte; min(len) for a pure length diff."""
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    if len(a) != len(b):
        return min(len(a), len(b))
    return -1


def hexdump_window(payload: bytes, diff: int, radius: int = 16) -> str:
    """8-byte-per-line hexdump around diff, with offsets relative to the NAL."""
    lo = max(0, diff - radius)
    hi = min(len(payload), diff + radius + 1)
    lo = (lo // 8) * 8
    hi = min(len(payload), ((hi // 8) + 1) * 8)
    lines = []
    for off in range(lo, hi, 8):
        chunk = payload[off:off + 8]
        hexs = ' '.join(f'{b:02x}' for b in chunk)
        asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        mark = '  <== first diff' if off <= diff < off + len(chunk) else ''
        lines.append(f'    {off:04x}: {hexs:<23}  |{asc}|{mark}')
    return '\n'.join(lines)


# ---------------------------------------------------------------------------
# Bit reader and Exp-Golomb (mirrors phx_bitstream.c / common/bitstream.h)
# ---------------------------------------------------------------------------

class BitReader:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0                       # bit position from RBSP start

    @property
    def nbits(self) -> int:
        return len(self.data) * 8

    def u(self, n: int) -> int:
        if n < 0 or self.pos + n > self.nbits:
            raise ParseError(f'bit underflow at bit {self.pos} (need {n})')
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.data[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def flag(self) -> int:
        return self.u(1)

    def ue(self) -> int:
        zeros = 0
        while True:
            if self.pos >= self.nbits:
                raise ParseError(f'ue() ran past RBSP end at bit {self.pos}')
            if self.u(1):
                break
            zeros += 1
            if zeros > 31:
                raise ParseError(f'ue() leading-zero overflow at bit {self.pos}')
        return (1 << zeros) - 1 + (self.u(zeros) if zeros else 0)

    def se(self) -> int:
        """common/bitstream.h:246-261 / phx_bs_write_se: k -> (k+1)/2 or -k/2."""
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)

    def more_rbsp_data(self) -> bool:
        """True when bits remain beyond the rbsp_stop_one_bit and alignment."""
        i = self.pos
        while i < self.nbits and not ((self.data[i >> 3] >> (7 - (i & 7))) & 1):
            i += 1
        if i >= self.nbits:
            return False
        return any((self.data[j >> 3] >> (7 - (j & 7))) & 1
                   for j in range(i + 1, self.nbits))


@dataclass
class Syntax:
    """Ordered syntax elements: (name, value, bit position after the read)."""
    title: str
    fields: List[Tuple[str, object, int]] = field(default_factory=list)
    end_bit_override: Optional[int] = None

    def add(self, name: str, value: object, bitpos: int):
        self.fields.append((name, value, bitpos))
        return value

    @property
    def values(self) -> Dict[str, object]:
        return {name: value for name, value, _ in self.fields}

    @property
    def end_bit(self) -> int:
        """Bit position just after the last decoded syntax element."""
        return (self.end_bit_override if self.end_bit_override is not None
                else (self.fields[-1][2] if self.fields else 0))

    def render(self) -> str:
        lines = [f'  {self.title}: {len(self.fields)} fields, '
                 f'last field ends at bit {self.end_bit}']
        for name, value, bitpos in self.fields:
            shown = ' '.join(str(v) for v in value) if isinstance(value, list) else str(value)
            lines.append(f'    {name:<46} = {shown:<12} @bit {bitpos}')
        return '\n'.join(lines)


def diff_syntax(a: Syntax, b: Syntax) -> List[Tuple[str, object, object]]:
    """Field-name/value differences, ignoring bit offsets."""
    diffs = []
    for i in range(max(len(a.fields), len(b.fields))):
        af = a.fields[i] if i < len(a.fields) else None
        bf = b.fields[i] if i < len(b.fields) else None
        av = (af[0], af[1]) if af else ('<none>', '<none>')
        bv = (bf[0], bf[1]) if bf else ('<none>', '<none>')
        if av != bv:
            diffs.append((av[0], av[1], bv[1]))
    return diffs


# ---------------------------------------------------------------------------
# Parameter sets (phx_headers.c: set.c:305-570)
# ---------------------------------------------------------------------------

def _skip_hrd(r: BitReader, s: Syntax, prefix: str):
    cnt = r.ue() + 1
    s.add(f'{prefix}_cpb_cnt_minus1', cnt - 1, r.pos)
    s.add(f'{prefix}_bit_rate_scale', r.u(4), r.pos)
    s.add(f'{prefix}_cpb_size_scale', r.u(4), r.pos)
    for i in range(min(cnt, 32)):
        s.add(f'{prefix}_bit_rate_value_minus1[{i}]', r.ue(), r.pos)
        s.add(f'{prefix}_cpb_size_value_minus1[{i}]', r.ue(), r.pos)
        s.add(f'{prefix}_cbr_flag[{i}]', r.flag(), r.pos)
    s.add(f'{prefix}_initial_cpb_removal_delay_length_minus1', r.u(5), r.pos)
    s.add(f'{prefix}_cpb_removal_delay_length_minus1', r.u(5), r.pos)
    s.add(f'{prefix}_dpb_output_delay_length_minus1', r.u(5), r.pos)
    s.add(f'{prefix}_time_offset_length', r.u(5), r.pos)


def _parse_vui(r: BitReader, s: Syntax):
    def rec(name, value):
        return s.add(name, value, r.pos)

    if rec('aspect_ratio_info_present_flag', r.flag()):
        idc = rec('aspect_ratio_idc', r.u(8))
        if idc == 255:
            rec('sar_width', r.u(16))
            rec('sar_height', r.u(16))
    if rec('overscan_info_present_flag', r.flag()):
        rec('overscan_appropriate_flag', r.flag())
    if rec('video_signal_type_present_flag', r.flag()):
        rec('video_format', r.u(3))
        rec('video_full_range_flag', r.flag())
        if rec('colour_description_present_flag', r.flag()):
            rec('colour_primaries', r.u(8))
            rec('transfer_characteristics', r.u(8))
            rec('matrix_coefficients', r.u(8))
    if rec('chroma_loc_info_present_flag', r.flag()):
        rec('chroma_sample_loc_type_top_field', r.ue())
        rec('chroma_sample_loc_type_bottom_field', r.ue())
    if rec('timing_info_present_flag', r.flag()):
        rec('num_units_in_tick', r.u(32))
        rec('time_scale', r.u(32))
        rec('fixed_frame_rate_flag', r.flag())
    nal_hrd = rec('nal_hrd_parameters_present_flag', r.flag())
    if nal_hrd:
        _skip_hrd(r, s, 'nal_hrd')
    vcl_hrd = rec('vcl_hrd_parameters_present_flag', r.flag())
    if vcl_hrd:
        _skip_hrd(r, s, 'vcl_hrd')
    if nal_hrd or vcl_hrd:
        rec('low_delay_hrd_flag', r.flag())
    rec('pic_struct_present_flag', r.flag())
    if rec('bitstream_restriction_flag', r.flag()):
        rec('motion_vectors_over_pic_boundaries_flag', r.flag())
        rec('max_bytes_per_pic_denom', r.ue())
        rec('max_bits_per_mb_denom', r.ue())
        rec('log2_max_mv_length_horizontal', r.ue())
        rec('log2_max_mv_length_vertical', r.ue())
        rec('max_num_reorder_frames', r.ue())
        rec('max_dec_frame_buffering', r.ue())


def parse_sps(nal: Nal) -> Syntax:
    """seq_parameter_set_data(), phx_headers.c:98-162 / set.c:305-475."""
    r = BitReader(rbsp_unescape(nal.payload[1:]))
    s = Syntax(f'SPS (NAL {nal.index}, type 7)')

    def rec(name, value):
        return s.add(name, value, r.pos)

    profile = rec('profile_idc', r.u(8))
    rec('constraint_set0_flag', r.flag())
    rec('constraint_set1_flag', r.flag())
    rec('constraint_set2_flag', r.flag())
    rec('constraint_set3_flag', r.flag())
    rec('constraint_set4_flag', r.flag())
    rec('constraint_set5_flag', r.flag())
    rec('reserved_zero_2bits', r.u(2))
    rec('level_idc', r.u(8))
    rec('seq_parameter_set_id', r.ue())

    chroma = 1                              # implied for baseline/main
    if profile in HIGH_PROFILES:
        chroma = rec('chroma_format_idc', r.ue())
        if chroma == 3:
            rec('separate_colour_plane_flag', r.flag())
        rec('bit_depth_luma_minus8', r.ue())
        rec('bit_depth_chroma_minus8', r.ue())
        rec('qpprime_y_zero_transform_bypass_flag', r.flag())
        if rec('seq_scaling_matrix_present_flag', r.flag()):
            nlists = 12 if chroma != 3 else 8
            for i in range(nlists):
                if rec(f'seq_scaling_list_present_flag[{i}]', r.flag()):
                    size = 16 if i < 6 else 64
                    last = nxt = 8
                    for _ in range(size):
                        if nxt != 0:
                            nxt = (last + r.se() + 256) % 256
                        last = nxt if nxt != 0 else last

    log2_fn = rec('log2_max_frame_num_minus4', r.ue()) + 4
    s.add('log2_max_frame_num', log2_fn, r.pos)
    poc_type = rec('pic_order_cnt_type', r.ue())
    if poc_type == 0:
        lsb = rec('log2_max_pic_order_cnt_lsb_minus4', r.ue()) + 4
        s.add('log2_max_pic_order_cnt_lsb', lsb, r.pos)
    elif poc_type == 1:
        rec('delta_pic_order_always_zero_flag', r.flag())
        rec('offset_for_non_ref_pic', r.se())
        rec('offset_for_top_to_bottom_field', r.se())
        cnt = rec('num_ref_frames_in_pic_order_cnt_cycle', r.ue())
        for i in range(min(cnt, 256)):
            rec(f'offset_for_ref_frame[{i}]', r.se())

    rec('max_num_ref_frames', r.ue())
    rec('gaps_in_frame_num_value_allowed_flag', r.flag())
    rec('pic_width_in_mbs_minus1', r.ue())
    rec('pic_height_in_map_units_minus1', r.ue())
    frame_only = rec('frame_mbs_only_flag', r.flag())
    if not frame_only:
        rec('mb_adaptive_frame_field_flag', r.flag())
    rec('direct_8x8_inference_flag', r.flag())
    if rec('frame_cropping_flag', r.flag()):
        rec('frame_crop_left_offset', r.ue())
        rec('frame_crop_right_offset', r.ue())
        rec('frame_crop_top_offset', r.ue())
        rec('frame_crop_bottom_offset', r.ue())
    if rec('vui_parameters_present_flag', r.flag()):
        _parse_vui(r, s)

    v = s.values
    v.setdefault('chroma_format_idc', chroma)
    v.setdefault('log2_max_pic_order_cnt_lsb', 0)
    v.setdefault('mb_adaptive_frame_field_flag', 0)
    return s


def parse_pps(nal: Nal) -> Syntax:
    """pic_parameter_set_rbsp(), phx_headers.c:165-194 / set.c:505-570."""
    r = BitReader(rbsp_unescape(nal.payload[1:]))
    s = Syntax(f'PPS (NAL {nal.index}, type 8)')

    def rec(name, value):
        return s.add(name, value, r.pos)

    rec('pic_parameter_set_id', r.ue())
    rec('seq_parameter_set_id', r.ue())
    rec('entropy_coding_mode_flag', r.flag())
    rec('bottom_field_pic_order_in_frame_present_flag', r.flag())
    groups = rec('num_slice_groups_minus1', r.ue())
    if groups > 0:
        # x264 pins num_slice_groups_minus1 = 0 (set.c:486); keep the probe
        # usable on other streams by recording the map type without decoding
        # the full map.  slice_group_change_cycle is handled in the header.
        map_type = rec('slice_group_map_type', r.ue())
        if map_type in (2, 3, 4, 5):
            for i in range(groups + 1):
                rec(f'slice_group_change_rate_minus1[{i}]', r.ue())
        elif map_type == 6:
            rec('pic_size_in_map_units_minus1', r.ue())
    l0 = rec('num_ref_idx_l0_default_active_minus1', r.ue())
    l1 = rec('num_ref_idx_l1_default_active_minus1', r.ue())
    rec('weighted_pred_flag', r.flag())
    rec('weighted_bipred_idc', r.u(2))
    rec('pic_init_qp_minus26', r.se())
    rec('pic_init_qs_minus26', r.se())
    rec('chroma_qp_index_offset', r.se())
    rec('deblocking_filter_control_present_flag', r.flag())
    rec('constrained_intra_pred_flag', r.flag())
    rec('redundant_pic_cnt_present_flag', r.flag())

    if r.more_rbsp_data():
        rec('transform_8x8_mode_flag', r.flag())
        if rec('pic_scaling_matrix_present_flag', r.flag()):
            for i in range(8):
                if rec(f'pic_scaling_list_present_flag[{i}]', r.flag()):
                    size = 16 if i < 4 else 64
                    last = nxt = 8
                    for _ in range(size):
                        if nxt != 0:
                            nxt = (last + r.se() + 256) % 256
                        last = nxt if nxt != 0 else last
        # set.c:567 — second chroma_qp_index_offset write for 8x8dct
        rec('second_chroma_qp_index_offset', r.se())

    v = s.values
    v.setdefault('num_ref_idx_l0_active', l0 + 1)
    v.setdefault('num_ref_idx_l1_active', l1 + 1)
    v.setdefault('num_slice_groups_minus1', 0)
    return s


# ---------------------------------------------------------------------------
# Slice header (phx_slice.c:90-243 / encoder.c:213-364, H.264 7.3.3)
# ---------------------------------------------------------------------------

def _ref_list_modification(r: BitReader, s: Syntax, rec, list_id: int):
    def nm(name):
        return f'{name}_l{list_id}'

    if not rec(nm('ref_pic_list_modification_flag'), r.flag()):
        return
    for i in range(32):                     # bounded; idc == 3 ends the list
        idc = rec(nm(f'modification_of_pic_nums_idc[{i}]'), r.ue())
        if idc in (0, 1):
            rec(nm(f'abs_diff_pic_num_minus1[{i}]'), r.ue())
        elif idc == 2:
            rec(nm(f'long_term_pic_num[{i}]'), r.ue())
        else:                               # 3 == end of list
            break


def _dec_ref_pic_marking(r: BitReader, s: Syntax, rec, is_idr: bool):
    if is_idr:
        rec('no_output_of_prior_pics_flag', r.flag())
        rec('long_term_reference_flag', r.flag())
        return
    if not rec('adaptive_ref_pic_marking_mode_flag', r.flag()):
        return
    for i in range(32):
        op = rec(f'memory_management_control_operation[{i}]', r.ue())
        if op == 0:
            break
        if op in (1, 3):
            rec(f'difference_of_pic_nums_minus1[{i}]', r.ue())
        if op == 2:
            rec(f'long_term_pic_num[{i}]', r.ue())
        if op in (3, 6):
            rec(f'long_term_frame_idx[{i}]', r.ue())
        if op == 4:
            rec(f'max_long_term_frame_idx_plus1[{i}]', r.ue())


def parse_slice_header(nal: Nal, sps: Dict[str, object],
                       pps: Dict[str, object]) -> Syntax:
    """Decode slice_header() in the exact order written by encoder.c:213-364."""
    r = BitReader(rbsp_unescape(nal.payload[1:]))
    s = Syntax(f'slice header (NAL {nal.index}, type {nal.type}, '
               f'nal_ref_idc {nal.nal_ref_idc})')

    def rec(name, value):
        return s.add(name, value, r.pos)

    mb_width = int(sps.get('pic_width_in_mbs_minus1', 0)) + 1
    mb_height_map = int(sps.get('pic_height_in_map_units_minus1', 0)) + 1
    frame_only = int(sps.get('frame_mbs_only_flag', 1))
    mbaff = 0 if frame_only else int(sps.get('mb_adaptive_frame_field_flag', 0))
    log2_fn = int(sps.get('log2_max_frame_num', 4))
    log2_poc = int(sps.get('log2_max_pic_order_cnt_lsb', 4))
    poc_type = int(sps.get('pic_order_cnt_type', 0))
    chroma = int(sps.get('chroma_format_idc', 1))
    bottom_present = int(pps.get('bottom_field_pic_order_in_frame_present_flag', 0))
    redundant = int(pps.get('redundant_pic_cnt_present_flag', 0))
    weighted_pred = int(pps.get('weighted_pred_flag', 0))
    weighted_bipred = int(pps.get('weighted_bipred_idc', 0))
    deblock_ctrl = int(pps.get('deblocking_filter_control_present_flag', 0))
    cabac = int(pps.get('entropy_coding_mode_flag', 0))
    is_idr = nal.type == 5
    nal_ref_idc = nal.nal_ref_idc

    # phx_slice.c:99-106 / encoder.c:215-223 — first_mb_in_slice
    if mbaff:
        first_mb = rec('first_mb_in_slice', r.ue())
        first_x = first_mb % mb_width
        first_y = first_mb // mb_width
        s.add('  first_mb (MBAFF decoded)',
              (2 * first_x + mb_width * (first_y & ~1) + (first_y & 1)) >> 1,
              r.pos)
    else:
        rec('first_mb_in_slice', r.ue())

    slice_code = rec('slice_type', r.ue())
    st = {0: 'P', 1: 'B', 2: 'I', 3: 'SP', 4: 'SI',
          5: 'P', 6: 'B', 7: 'I', 8: 'SP', 9: 'SI'}.get(slice_code, '?')
    s.add('  slice_type (decoded)', st, r.pos)
    rec('pic_parameter_set_id', r.ue())
    rec('frame_num', r.u(log2_fn))

    field_pic = 0
    if not frame_only:                       # :229-234
        field_pic = rec('field_pic_flag', r.flag())
        if field_pic:
            rec('bottom_field_flag', r.flag())

    if is_idr:                               # :236-237
        rec('idr_pic_id', r.ue())

    if poc_type == 0:                        # :239-244
        rec('pic_order_cnt_lsb', r.u(log2_poc))
        if bottom_present and not field_pic:
            rec('delta_pic_order_cnt_bottom', r.se())
    elif poc_type == 1:
        always_zero = int(sps.get('delta_pic_order_always_zero_flag', 1))
        if not always_zero:
            rec('delta_pic_order_cnt[0]', r.se())
            if bottom_present and not field_pic:
                rec('delta_pic_order_cnt[1]', r.se())

    if redundant:                            # :246-247
        rec('redundant_pic_cnt', r.ue())

    if st == 'B':                            # :249-250
        rec('direct_spatial_mv_pred_flag', r.flag())

    l0_active = int(pps.get('num_ref_idx_l0_active', 1))
    l1_active = int(pps.get('num_ref_idx_l1_active', 1))
    if st in ('P', 'B', 'SP'):               # :252-259
        if rec('num_ref_idx_active_override_flag', r.flag()):
            l0_active = rec('num_ref_idx_l0_active_minus1', r.ue()) + 1
            if st == 'B':
                l1_active = rec('num_ref_idx_l1_active_minus1', r.ue()) + 1
        s.add('  num_ref_idx_l0_active (effective)', l0_active, r.pos)
        s.add('  num_ref_idx_l1_active (effective)', l1_active, r.pos)

    if st not in ('I', 'SI'):                # :263-276 (list 0)
        _ref_list_modification(r, s, rec, 0)
    if st == 'B':                            # :277-289 (list 1)
        _ref_list_modification(r, s, rec, 1)

    # pred_weight_table(): :291-322
    if weighted_pred and st in ('P', 'SP'):
        luma_denom = rec('luma_log2_weight_denom', r.ue())
        chroma_denom = None
        if chroma:
            chroma_denom = rec('chroma_log2_weight_denom', r.ue())
        for i in range(min(l0_active, 32)):
            if rec(f'luma_weight_l0_flag[{i}]', r.flag()):
                rec(f'luma_weight_l0[{i}]', r.se())
                rec(f'luma_offset_l0[{i}]', r.se())
            if chroma and rec(f'chroma_weight_l0_flag[{i}]', r.flag()):
                for j in (1, 2):
                    rec(f'chroma_weight_l0[{i}][{j}]', r.se())
                    rec(f'chroma_offset_l0[{i}][{j}]', r.se())
        s.add('  (luma/chroma log2_weight_denom)',
              [luma_denom, chroma_denom], r.pos)
    elif weighted_bipred == 1 and st == 'B':
        # encoder.c:323-326 is a TODO in x264 and emits nothing.
        s.add('  weighted_bipred implicit (x264 TODO, no bits)', 1, r.pos)

    if nal_ref_idc != 0:                     # :328-348
        _dec_ref_pic_marking(r, s, rec, is_idr)

    if cabac and st not in ('I', 'SI'):      # :350-351
        rec('cabac_init_idc', r.ue())

    qp_delta = rec('slice_qp_delta', r.se())  # :353
    pic_init_qp = int(pps.get('pic_init_qp_minus26', 0)) + 26
    s.add('  slice_qp (derived)', qp_delta + pic_init_qp, r.pos)

    if deblock_ctrl:                         # :355-361
        disable = rec('disable_deblocking_filter_idc', r.ue())
        if disable != 1:
            rec('slice_alpha_c0_offset_div2', r.se())
            rec('slice_beta_offset_div2', r.se())

    if int(pps.get('num_slice_groups_minus1', 0)) > 0:
        # H.264 7.3.3, only for slice_group_map_type == 6 (x264 never sets it).
        pic_size = mb_width * mb_height_map * (2 if not frame_only else 1)
        bits = max(1, (pic_size - 1).bit_length())
        rec('slice_group_change_cycle', r.u(bits))

    s.end_bit_override = r.pos               # first bit after slice_header()
    return s


# ---------------------------------------------------------------------------
# Case reporting
# ---------------------------------------------------------------------------

@dataclass
class Stream:
    path: pathlib.Path
    data: bytes
    nals: List[Nal]
    sps: Optional[Syntax] = None
    pps: Optional[Syntax] = None
    problems: List[str] = field(default_factory=list)


def load_stream(path: pathlib.Path) -> Stream:
    if not path.is_file():
        raise FileNotFoundError(str(path))
    data = path.read_bytes()
    nals = annexb_nals(data)
    st = Stream(path=path, data=data, nals=nals)
    for nal in nals:
        try:
            if nal.type == 7 and st.sps is None:
                st.sps = parse_sps(nal)
            elif nal.type == 8 and st.pps is None:
                st.pps = parse_pps(nal)
        except ParseError as exc:
            st.problems.append(f'SPS/PPS parse failed on NAL {nal.index}: {exc}')
    if st.sps is None:
        st.problems.append('no SPS NAL found')
    if st.pps is None:
        st.problems.append('no PPS NAL found')
    return st


def align_nals(ref_nals: Sequence[Nal], phx_nals: Sequence[Nal]
               ) -> List[Tuple[Optional[int], Optional[int]]]:
    """Pair NALs by their type sequence, not by raw index.

    A missing SEI/AUD at the front must not make every following slice appear
    to be a type-mismatched NAL.  difflib keeps repeated slice NALs paired in
    order while exposing genuine insert/delete/replace events.
    """
    matcher = difflib.SequenceMatcher(
        None, [n.type for n in ref_nals], [n.type for n in phx_nals],
        autojunk=False)
    pairs: List[Tuple[Optional[int], Optional[int]]] = []
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == 'equal':
            pairs.extend((i1 + k, j1 + k) for k in range(i2 - i1))
        elif tag == 'replace':
            for k in range(max(i2 - i1, j2 - j1)):
                pairs.append((i1 + k if i1 + k < i2 else None,
                              j1 + k if j1 + k < j2 else None))
        elif tag == 'delete':
            pairs.extend((i, None) for i in range(i1, i2))
        else:  # insert
            pairs.extend((None, j) for j in range(j1, j2))
    return pairs


def report_nals(ref: Stream, phx: Stream) -> bool:
    """Print one line per aligned NAL; return True only if the NAL sequences
    match and every payload is byte-exact."""
    print('NAL comparison (payload bytes, start codes and trailing zeros '
          'excluded; NALs paired by type sequence):')
    print(f'  {"ref#":>4} {"phx#":>4}  {"ref NAL":<22} {"phx NAL":<22} '
          f'{"len ref":>7} {"len phx":>7} {"status":<18} first diff')
    clean = True
    for ri, pi in align_nals(ref.nals, phx.nals):
        rn = ref.nals[ri] if ri is not None else None
        pn = phx.nals[pi] if pi is not None else None
        if rn is None or pn is None:
            clean = False
            side = 'MISSING in Phoenix' if rn else 'EXTRA in Phoenix'
            only = rn if rn else pn
            print(f'  {ri if ri is not None else "-":>4} '
                  f'{pi if pi is not None else "-":>4}  '
                  f'{rn.label if rn else "-":<22} '
                  f'{pn.label if pn else "-":<22} '
                  f'{len(rn.payload) if rn else 0:>7} '
                  f'{len(pn.payload) if pn else 0:>7}  {side:<18} '
                  f'({only.label}, {len(only.payload)} bytes, '
                  f'head {only.hex_prefix})')
            continue
        d = first_difference(pn.payload, rn.payload)
        status = 'EXACT' if d < 0 else f'DIFF@{d}'
        if d >= 0 or rn.type != pn.type:
            clean = False
        print(f'  {ri:>4} {pi:>4}  {rn.label:<22} {pn.label:<22} '
              f'{len(rn.payload):>7} {len(pn.payload):>7}  {status:<18} '
              f'{d if d >= 0 else "-"}')
        if d >= 0:
            print('    ref:')
            print(hexdump_window(rn.payload, d))
            print('    phx:')
            print(hexdump_window(pn.payload, d))
    return clean and len(ref.nals) == len(phx.nals)


def _slice_nals(st: Stream) -> List[Nal]:
    return [n for n in st.nals if n.type in (1, 5)]


def _header_for(st: Stream, nal: Nal) -> Optional[Syntax]:
    if st.sps is None or st.pps is None:
        print(f'    cannot decode slice header: {"; ".join(st.problems)}')
        return None
    try:
        return parse_slice_header(nal, st.sps.values, st.pps.values)
    except ParseError as exc:
        print(f'    slice header decode failed on NAL {nal.index}: {exc}')
        return None


def report_slices(ref: Stream, phx: Stream, show_all: bool) -> bool:
    """Print slice headers; return True only if headers match AND the bytes
    after each matched header are byte-exact."""
    ref_slices, phx_slices = _slice_nals(ref), _slice_nals(phx)
    print()
    print(f'Slice NALs: ref={len(ref_slices)}, phx={len(phx_slices)}')
    if not ref_slices:
        print('  reference stream has no slice NAL -- check the reference file '
              'and x264 flags.')
        return False
    if not phx_slices:
        print('  NO SLICE NAL in Phoenix stream: phx_cli currently emits '
              'SPS+PPS only (full-frame encoding is TODO).')
        print('  Expected first slice header from the reference:')
        first = _header_for(ref, ref_slices[0])
        if first:
            print(first.render())
        return False

    ok = True
    for k in range(max(len(ref_slices), len(phx_slices))):
        rn = ref_slices[k] if k < len(ref_slices) else None
        pn = phx_slices[k] if k < len(phx_slices) else None
        tag = f'slice #{k + 1}'
        if rn is None or pn is None:
            print(f'  {tag}: {"missing in Phoenix" if rn else "extra in Phoenix"}')
            ok = False
            continue
        rs, ps = _header_for(ref, rn), _header_for(phx, pn)
        if rs is None or ps is None:
            ok = False
            continue
        diffs = diff_syntax(rs, ps)
        if show_all or k == 0:
            print(f'  {tag} — reference NAL {rn.index} / Phoenix NAL {pn.index}')
            print(rs.render().replace('  slice header', '  ref  slice header', 1))
            print(ps.render().replace('  slice header', '  phx  slice header', 1))
        if diffs:
            ok = False
            print(f'  {tag}: HEADER MISMATCH ({len(diffs)} differing field(s)):')
            for name, rv, pv in diffs:
                print(f'    {name:<46} ref={rv!s:<10} phx={pv!s}')
            print('    => fix the parameter-set / slice-header writer, not '
                  'the entropy stage')
            continue

        hb = (rs.end_bit + 7) // 8               # first byte after header
        tail_r = rbsp_unescape(rn.payload[1:])[hb:]
        tail_p = rbsp_unescape(pn.payload[1:])[hb:]
        d = first_difference(tail_p, tail_r)
        if d >= 0:
            ok = False
            print(f'  {tag}: HEADER MATCH ({rs.end_bit} bits), '
                  f'ENTROPY/MB MISMATCH at post-header byte {d} '
                  f'(RBSP byte {hb + d}, NAL payload byte {hb + d + 1})')
            print('    ref:')
            print(hexdump_window(rbsp_unescape(rn.payload[1:]), hb + d))
            print('    phx:')
            print(hexdump_window(rbsp_unescape(pn.payload[1:]), hb + d))
        else:
            print(f'  {tag}: header exact ({rs.end_bit} bits), '
                  f'post-header data exact ({len(tail_r)} bytes)')
    return ok


def probe_case(preset: str, entropy: str, refs_dir: pathlib.Path,
               phx_dir: pathlib.Path, show_all: bool) -> bool:
    ref_path = refs_dir / f'synth-{preset}-{entropy}.ref.h264'
    phx_path = phx_dir / f'synth-{preset}-{entropy}.phx.h264'
    print('=' * 78)
    print(f'CASE synth-{preset}-{entropy}')
    print(f'  ref: {ref_path}')
    print(f'  phx: {phx_path}')
    try:
        ref, phx = load_stream(ref_path), load_stream(phx_path)
    except FileNotFoundError as exc:
        print(f'  ERROR: missing stream: {exc}')
        return False
    print(f'  stream bytes: ref={len(ref.data)}, phx={len(phx.data)}; '
          f'NALs: ref={len(ref.nals)}, phx={len(phx.nals)}')
    for st, label in ((ref, 'ref'), (phx, 'phx')):
        for problem in st.problems:
            print(f'  WARNING [{label}]: {problem}')

    nal_clean = report_nals(ref, phx)
    ps_pairs = []
    for ri, pi in align_nals(ref.nals, phx.nals):
        rn = ref.nals[ri] if ri is not None else None
        pn = phx.nals[pi] if pi is not None else None
        if rn and pn and rn.type in (7, 8) and rn.type == pn.type:
            ps_pairs.append((nal_name(rn.type),
                             first_difference(pn.payload, rn.payload) < 0))
    exact_ps = sum(1 for _, ok in ps_pairs if ok)
    print(f'  parameter sets: {exact_ps}/{len(ps_pairs)} byte-exact '
          f'({", ".join(n for n, _ in ps_pairs) or "none"})')
    slice_clean = report_slices(ref, phx, show_all)

    overall = first_difference(phx.data, ref.data)
    print()
    if nal_clean and slice_clean:
        print('VERDICT: BYTE-EXACT (all NALs and slice headers match)')
    else:
        print(f'VERDICT: DIVERGENCE (first stream byte diff '
              f'{overall if overall >= 0 else "at end of stream"})')
        if not _slice_nals(phx):
            print('         no slice NAL in Phoenix stream -- the entropy/MB '
                  'stage has not been reached')
    return nal_clean and slice_clean


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description='Probe the first Phoenix/x264 stream divergence at '
                    'NAL and slice-header granularity.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Examples:\n'
               '  python tools/first_frame_probe.py --preset medium '
               '--entropy cabac\n'
               '  python tools/first_frame_probe.py --preset all '
               '--entropy cavlc\n'
               '  python tools/first_frame_probe.py --preset fast '
               '--entropy cabac --all-slices\n')
    ap.add_argument('--preset', default='medium', metavar='NAME|all',
                    help=f'x264 preset name ({", ".join(PRESETS)}) or "all"; '
                         'default: medium')
    ap.add_argument('--entropy', default='cabac', choices=ENTROPIES,
                    help='entropy mode; default: cabac')
    ap.add_argument('--refs-dir', type=pathlib.Path, default=DEFAULT_DIR,
                    help='directory holding synth-*.ref.h264 '
                         '(default: build/parity)')
    ap.add_argument('--phx-dir', type=pathlib.Path, default=DEFAULT_DIR,
                    help='directory holding synth-*.phx.h264 '
                         '(default: build/parity)')
    ap.add_argument('--all-slices', action='store_true',
                    help='print full header fields for every slice NAL, not '
                         'only the first')
    args = ap.parse_args(argv)

    presets = list(PRESETS) if args.preset == 'all' else [args.preset]
    bad = [p for p in presets if p not in PRESETS]
    if bad:
        ap.error(f'unknown preset(s): {", ".join(bad)}')
    refs_dir = args.refs_dir.resolve()
    phx_dir = args.phx_dir.resolve()
    if not refs_dir.is_dir():
        print(f'ERROR: reference directory not found: {refs_dir}', file=sys.stderr)
        return 2
    if not phx_dir.is_dir():
        print(f'ERROR: Phoenix directory not found: {phx_dir}', file=sys.stderr)
        return 2

    all_exact = True
    decided = False
    for preset in presets:
        exact = probe_case(preset, args.entropy, refs_dir, phx_dir,
                           args.all_slices)
        decided = True
        all_exact &= exact
    if not decided:
        return 2
    print()
    print(f'SUMMARY: {len(presets)} case(s), '
          f'{"all byte-exact" if all_exact else "at least one divergence"}')
    return 0 if all_exact else 1


if __name__ == '__main__':
    sys.exit(main())
