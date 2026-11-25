#!/usr/bin/env python3
"""Ordered-CABAC-bin golden harness and first-divergence report (worker L1).

Question answered
-----------------
tools/first_frame_probe.py reports SPS/PPS/SEI byte-exact and the medium/cabac
IDR slice DIFF@11 (first 11 NAL payload bytes identical).  That leaves MB0's
CABAC bins as the first possible divergence.  This tool answers *which bin*.

Method (no x264 bytes are copied into Phoenix; the two streams are generated)
-----------------------------------------------------------------------------
1. Build a temporary instrumented x264 ("golden harness") from
   reference/x264/common/cabac.c and reference/x264/encoder/cabac.c.  The
   encoder's real macroblock_write_cabac_internal() drives the real context
   state for the case (qp=25 I slice, I_4x4 MB0 with the x264-chosen modes,
   chroma, CBP and coefficients from the reference fixture).  Temporary hooks
   in the coder dump every ordered bin as (ctx, b, state, low/range before and
   after) to L1_BINS_X264.txt.  All other x264 objects are the C-only reference
   objects built by tools/build_reference_x264.py.
2. Build a temporary Phoenix debug CLI with -DPHX_CABAC_BIN_TRACE.  The
   compile-time-only hooks in src/phx_cabac.c emit the identical text format
   to phx_bins.txt while the real encoder writes the same fixture.
3. Compare bin-by-bin (ue_bypass groups are expanded to their individual
   bypass bins).  Report the first differing bin index, its syntax element,
   both ctx/value pairs and the source function responsible.

Usage
-----
    python tools/cabac_bin_diff.py --all               # build + run + compare
    python tools/cabac_bin_diff.py --compare-only      # reuse existing logs
    python tools/cabac_bin_diff.py --all --fixture build/parity/fixture.yuv

All generated sources, objects and logs live under a temp directory (default
C:\\Users\\multi\\AppData\\Local\\Temp\\opencode\\l1_cabac_bin_diff); nothing is
written inside the repository.  SHADER: C -- the instrumented code only
observes the strictly serial coder state.
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional, Sequence, Tuple

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / 'reference' / 'x264'
REF_OBJ = ROOT / 'build' / 'x264_ref' / 'obj'
REF_RSP = ROOT / 'build' / 'x264_ref' / 'objs.rsp'
VC = pathlib.Path(r'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat')
DEFAULT_TMP = pathlib.Path(r'C:\Users\multi\AppData\Local\Temp\opencode\l1_cabac_bin_diff')
DEFAULT_FIXTURE = ROOT / 'build' / 'parity' / 'fixture.yuv'

DEFS = ('/DBIT_DEPTH=8 "/Dx264_template(w)=x264_glue3(x264,8,w)" '
        '/D__STDC_CONSTANT_MACROS /DHAVE_CONFIG_H')

# ---- x264 context-index -> syntax-element name (encoder/cabac.c line refs) --
CTX_LABELS: List[Tuple[int, int, str]] = [
    (0, 9, 'mb_type (I slice, cabac_mb_header_i, :381-392)'),
    (10, 13, 'mb_skip_flag (P/B, :189-195)'),
    (14, 16, 'mb_type prefix/suffix (P slice, :435-450)'),
    (17, 26, 'mb_type intra suffix (P slice, :435-497)'),
    (27, 41, 'mb_type (B slice, :499-610)'),
    (60, 63, 'mb_qp_delta (cabac_qp_delta, :150-186)'),
    (64, 67, 'intra_chroma_pred_mode (:98-116)'),
    (68, 69, 'intra4x4_pred_mode (:83-96)'),
    (70, 72, 'field_decoding_flag (:63-72)'),
    (73, 76, 'coded_block_pattern luma (cabac_cbp_luma, :118-127)'),
    (77, 80, 'coded_block_pattern chroma (cabac_cbp_chroma, :129-148)'),
    (81, 83, 'coded_block_pattern chroma suffix (cabac_cbp_chroma, :129-148)'),
    (84, 95, 'mb_qp_delta / cbp neighbour contexts'),
    (96, 165, 'significant_coeff_flag 4x4 luma (cabac_block_residual, :664-748)'),
    (166, 199, 'significant_coeff_flag chroma/8x8 (cabac_block_residual, :664-748)'),
    (200, 223, 'last_significant_coeff_flag (cabac_block_residual, :664-748)'),
    (224, 239, 'coeff_abs_level_minus1 (cabac_block_residual, :664-748)'),
    (240, 275, 'coeff_abs_level_minus1 gt1 / chroma DC'),
    (276, 398, 'coeff_abs_level / 8x8 residual contexts'),
    (399, 401, 'transform_size_8x8_flag (cabac_transform_size, :233-237)'),
]


def label_ctx(ctx: int) -> str:
    if ctx < 0:
        return {'-1': 'bypass bin', '-2': 'ue_bypass group',
                '-3': 'terminating bin'}.get(str(ctx), 'bypass bin')
    for lo, hi, name in CTX_LABELS:
        if lo <= ctx <= hi:
            return name
    return 'residual/other CABAC context'


# ---------------------------------------------------------------------------
# Trace record model
# ---------------------------------------------------------------------------

@dataclass
class Bin:
    kind: str                 # 'dec', 'byp', 'ue', 'term'
    ctx: int                  # -1 bypass, -2 ue group, -3 terminal
    b: int                    # encoded bin value (0/1); ue/term: -1
    state: int                # context state before (dec only), else 0
    low: int                  # i_low before
    rng: int                  # i_range before
    low2: int                 # i_low after
    rng2: int                 # i_range after
    extra: Tuple[int, ...] = ()  # ue: (exp_bits, val, nbins)

    def identity(self) -> Tuple[str, int, int]:
        return (self.kind, self.ctx, self.b)

    def render(self) -> str:
        head = f'{self.kind:<3} ctx={self.ctx:<4} b={self.b:<2}'
        if self.kind == 'dec':
            head += f' state={self.state:<4}'
        if self.kind == 'ue':
            head += f' exp={self.extra[0]} val={self.extra[1]} n={self.extra[2]}'
        return (head + f' low/range {self.low}/{self.rng} -> '
                f'{self.low2}/{self.rng2}   {label_ctx(self.ctx)}')


def parse_trace(path: pathlib.Path) -> List[Bin]:
    """Parse the shared textual trace format into ordered records."""
    out: List[Bin] = []
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        f = line.split()
        if not f:
            continue
        if f[0] == 'dec' and len(f) == 8:
            out.append(Bin('dec', int(f[1]), int(f[2]), int(f[3]),
                           int(f[4]), int(f[5]), int(f[6]), int(f[7])))
        elif f[0] == 'byp' and len(f) == 7:
            out.append(Bin('byp', -1, int(f[1]), int(f[2]),
                           int(f[3]), int(f[4]), int(f[5]), int(f[6])))
        elif f[0] == 'ue' and len(f) == 9:
            out.append(Bin('ue', -2, -1, int(f[4]),
                           int(f[5]), int(f[6]), int(f[7]), int(f[8]),
                           (int(f[1]), int(f[2]), int(f[3]))))
        elif f[0] == 'term' and len(f) == 6:
            out.append(Bin('term', -3, -1, int(f[1]),
                           int(f[2]), int(f[3]), int(f[4]), int(f[5])))
    return out


# ---------------------------------------------------------------------------
# Temporary instrumented builds
# ---------------------------------------------------------------------------

def cl_env() -> str:
    return f'"{VC}" && ' if VC.exists() else ''


def compile_one(src: pathlib.Path, obj: pathlib.Path, defs: str,
                inc: Sequence[pathlib.Path], cwd: pathlib.Path,
                extra: str = '') -> None:
    incs = ' '.join('/I"' + str(p) + '"' for p in inc)
    cmd = f'{cl_env()}cl -nologo -O2 -c {defs} {extra} {incs} "{src}" /Fo"{obj}"'
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                       cwd=str(cwd), timeout=240)
    if r.returncode:
        raise RuntimeError(f'compile failed: {src}\n{(r.stdout + r.stderr)[-2000:]}')


def link(exe: pathlib.Path, objs: Sequence[pathlib.Path],
         rsp_extra: Sequence[str] = ()) -> None:
    entries = [f'"{o}"' for o in objs] + list(rsp_extra)
    rsp = exe.with_suffix('.rsp')
    rsp.write_text('\n'.join(entries) + '\nshell32.lib\n', encoding='utf-8')
    cmd = f'{cl_env()}link -nologo /OUT:"{exe}" @"{rsp}"'
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                       timeout=300)
    if r.returncode:
        raise RuntimeError(f'link failed: {exe}\n{(r.stdout + r.stderr)[-2000:]}')


CABAC_TRACE_HDR = r'''
/* Temporary L1 golden-harness instrumentation; lives only in the temp dir. */
#include <stdio.h>
static FILE *l1_fp = NULL;
static int l1_n = 0;
static void l1_open(void)
{
    if (!l1_fp) l1_fp = fopen("L1_BINS_X264.txt", "w");
}
static void l1_dec(int ctx, int b, int st, int lo, int rg, int lo2, int rg2)
{
    l1_open(); if (!l1_fp || l1_n++ > 40000) return;
    fprintf(l1_fp, "dec %d %d %d %d %d %d %d\n", ctx, b, st, lo, rg, lo2, rg2);
}
static void l1_byp(int b, int lo, int rg, int lo2, int rg2)
{
    l1_open(); if (!l1_fp || l1_n++ > 40000) return;
    fprintf(l1_fp, "byp %d 0 %d %d %d %d\n", b, lo, rg, lo2, rg2);
}
static void l1_ue(int eb, int val, int nbins, int lo, int rg, int lo2, int rg2)
{
    l1_open(); if (!l1_fp || l1_n++ > 40000) return;
    fprintf(l1_fp, "ue %d %d %d 0 %d %d %d %d\n", eb, val, nbins, lo, rg, lo2, rg2);
}
static void l1_term(int lo, int rg, int lo2, int rg2)
{
    l1_open(); if (!l1_fp || l1_n++ > 40000) return;
    fprintf(l1_fp, "term 0 %d %d %d %d\n", lo, rg, lo2, rg2);
}
'''


def patch_common_cabac(tmp: pathlib.Path) -> pathlib.Path:
    """Patch a copy of reference/x264/common/cabac.c with bin hooks."""
    s = (SRC / 'common' / 'cabac.c').read_text(encoding='utf-8')
    if '#include "common.h"' not in s:
        raise RuntimeError('unexpected common/cabac.c layout')
    s = s.replace('#include "common.h"', '#include "common.h"\n' + CABAC_TRACE_HDR, 1)

    pairs = [
        ("""void x264_cabac_encode_decision_c( x264_cabac_t *cb, int i_ctx, int b )\n{\n    int i_state = cb->state[i_ctx];""",
         """void x264_cabac_encode_decision_c( x264_cabac_t *cb, int i_ctx, int b )\n{\n    int i_state = cb->state[i_ctx];\n    int l1_lo = cb->i_low, l1_rg = cb->i_range;"""),
        ("""    cb->state[i_ctx] = x264_cabac_transition[i_state][b];\n    cabac_encode_renorm( cb );\n}""",
         """    cb->state[i_ctx] = x264_cabac_transition[i_state][b];\n    cabac_encode_renorm( cb );\n    l1_dec(i_ctx, b, i_state, l1_lo, l1_rg, cb->i_low, cb->i_range);\n}"""),
        ("""void x264_cabac_encode_bypass_c( x264_cabac_t *cb, int b )\n{\n    cb->i_low <<= 1;""",
         """void x264_cabac_encode_bypass_c( x264_cabac_t *cb, int b )\n{\n    int l1_lo = cb->i_low, l1_rg = cb->i_range;\n    cb->i_low <<= 1;"""),
        ("""    cb->i_queue += 1;\n    cabac_putbyte( cb );\n}""",
         """    cb->i_queue += 1;\n    cabac_putbyte( cb );\n    l1_byp(!b, l1_lo, l1_rg, cb->i_low, cb->i_range);\n}"""),
        ("""void x264_cabac_encode_ue_bypass( x264_cabac_t *cb, int exp_bits, int val )\n{\n    uint32_t v = val + (1<<exp_bits);""",
         """void x264_cabac_encode_ue_bypass( x264_cabac_t *cb, int exp_bits, int val )\n{\n    int l1_lo = cb->i_low, l1_rg = cb->i_range;\n    uint32_t v = val + (1<<exp_bits);"""),
        ("""    } while( k > 0 );\n}\n\nvoid x264_cabac_encode_terminal_c""",
         """    } while( k > 0 );\n    l1_ue(exp_bits, val, 2*(31 - x264_clz(v))+1-exp_bits,\n          l1_lo, l1_rg, cb->i_low, cb->i_range);\n}\n\nvoid x264_cabac_encode_terminal_c"""),
        ("""void x264_cabac_encode_terminal_c( x264_cabac_t *cb )\n{\n    cb->i_range -= 2;\n    cabac_encode_renorm( cb );\n}""",
         """void x264_cabac_encode_terminal_c( x264_cabac_t *cb )\n{\n    int l1_lo = cb->i_low, l1_rg = cb->i_range;\n    cb->i_range -= 2;\n    cabac_encode_renorm( cb );\n    l1_term(l1_lo, l1_rg, cb->i_low, cb->i_range);\n}"""),
    ]
    for old, new in pairs:
        if old not in s:
            raise RuntimeError('unexpected common/cabac.c function body: ' + old[:60])
        s = s.replace(old, new, 1)
    out = tmp / 'common' / 'cabac.c'
    out.write_text(s, encoding='utf-8')
    return out


MB0_DUMP = r'''
    if( h->mb.i_mb_xy == 0 )
    {
        fprintf(stderr, "L1MB0 type=%d part=%d pred16=%d chroma=%d cbp_l=%d cbp_c=%d neigh=%d b8=%d\\n",
                h->mb.i_type, h->mb.i_partition, h->mb.i_intra16x16_pred_mode,
                h->mb.i_chroma_pred_mode, h->mb.i_cbp_luma, h->mb.i_cbp_chroma,
                h->mb.i_neighbour, h->mb.b_transform_8x8);
        for( int i = 0; i < 48; i++ )
        {
            fprintf(stderr, "L1BLK %d nnz=%d mode=%d :", i,
                    h->mb.cache.non_zero_count[x264_scan8[i]],
                    h->mb.cache.intra4x4_pred_mode[x264_scan8[i]]);
            for( int j = 0; j < 16; j++ )
                fprintf(stderr, " %d", h->dct.luma4x4[i][j]);
            fprintf(stderr, "\\n");
        }
        for( int ch = 0; ch < 2; ch++ )
            fprintf(stderr, "L1DC %d: %d %d %d %d\\n", ch,
                    h->dct.chroma_dc[ch][0], h->dct.chroma_dc[ch][1],
                    h->dct.chroma_dc[ch][2], h->dct.chroma_dc[ch][3]);
        fprintf(stderr, "L1QP %d %d %d\\n", h->mb.i_qp, h->mb.i_last_qp, h->mb.i_last_dqp);
    }
'''


def patch_encoder_cabac(tmp: pathlib.Path) -> pathlib.Path:
    """Patch a copy of reference/x264/encoder/cabac.c to log MB0 decisions."""
    s = (SRC / 'encoder' / 'cabac.c').read_text(encoding='utf-8')
    old = 'void x264_macroblock_write_cabac( x264_t *h, x264_cabac_t *cb )\n{\n'
    if old not in s:
        raise RuntimeError('unexpected encoder/cabac.c writer layout')
    s = s.replace(old, old + MB0_DUMP, 1)
    s = s.replace('#include "macroblock.h"',
                  '#include "macroblock.h"\n#include <stdio.h>', 1)
    blk_hook = r'''
static int l1_blk_n = 0;
static void l1_blk( int cat, int idx, int ctxinc, dctcoef *l )
{
    if( l1_blk_n++ >= 26 ) return;  /* MB0: 16 luma + 2 chroma DC + 8 AC */
    fprintf( stderr, "L1BLK %d %d %d:", cat, idx, ctxinc );
    for( int i = 0; i < 16; i++ ) fprintf( stderr, " %d", l[i] );
    fprintf( stderr, "\n" );
}
'''
    marker = '#define cabac_block_residual_dc_cbf( h, cb, ctx_block_cat, i_idx, l, b_intra )'
    if marker not in s:
        raise RuntimeError('unexpected cabac_block_residual_cbf_internal macro')
    s = s.replace(marker, blk_hook + '\n' + marker, 1)
    call_site = '    int ctxidxinc = cabac_cbf_ctxidxinc( h, ctx_block_cat, i_idx, b_intra, b_dc );\\\n'
    if call_site not in s:
        raise RuntimeError('unexpected cbf macro call site')
    s = s.replace(call_site,
                  call_site + '    l1_blk( ctx_block_cat, i_idx, ctxidxinc, l );\\\n', 1)
    out = tmp / 'encoder' / 'cabac.c'
    out.write_text(s, encoding='utf-8')
    return out


def build_x264_golden(tmp: pathlib.Path) -> pathlib.Path:
    """Build instrumented x264; encoder/cabac.c is compiled from the source."""
    (tmp / 'common').mkdir(parents=True, exist_ok=True)
    (tmp / 'encoder').mkdir(exist_ok=True)
    c_common = patch_common_cabac(tmp)
    c_encoder = patch_encoder_cabac(tmp)
    # Put the patched encoder first so #include "macroblock.h" resolves to
    # encoder/macroblock.h, not common/macroblock.h.
    inc = [tmp / 'encoder', tmp / 'common'] + [
        SRC / d for d in ['encoder', 'common', '.', 'input', 'output',
                          'filters', 'filters/video', 'extras']]
    o_common = tmp / 'trace_common_cabac.obj'
    o_encoder = tmp / 'trace_encoder_cabac.obj'
    compile_one(c_common, o_common, DEFS, inc, SRC)
    compile_one(c_encoder, o_encoder, DEFS, inc, SRC)
    rest: List[pathlib.Path] = []
    if REF_RSP.exists():
        for line in REF_RSP.read_text(encoding='utf-8').splitlines():
            line = line.strip().strip('"')
            if line and 'shell32' not in line and 'cabac.obj' not in line:
                rest.append(pathlib.Path(line))
    else:
        rest = [p for p in REF_OBJ.glob('*.obj') if 'cabac' not in p.name]
    exe = tmp / 'x264_trace.exe'
    link(exe, rest + [o_common, o_encoder])
    return exe


def build_phx_trace(tmp: pathlib.Path) -> pathlib.Path:
    """Build phx_cli with the compile-time -DPHX_CABAC_BIN_TRACE hook."""
    objs: List[pathlib.Path] = []
    sources = sorted((ROOT / 'src').glob('*.c')) + [ROOT / 'tools' / 'phx_cli.c']
    for s in sources:
        o = tmp / ('dbg_' + s.stem + '.obj')
        src = s
        if s.name == 'phx_mbencode.c':
            text = s.read_text(encoding='utf-8', errors='replace')
            # Temporary workaround for a concurrent worker's WIP diagnostic
            # that references out-of-scope mb_x/mb_y.  Only the two debug
            # lines are removed; no encoder behaviour is changed.
            lines = [ln for ln in text.splitlines(keepends=True)
                     if 'if (mb_x == 0 && mb_y == 0 && idx < 2)' not in ln
                     and 'fprintf(stderr, "TRELLIS idx=' not in ln]
            src = tmp / 'mbencode_sanitized.c'
            src.write_text(''.join(lines), encoding='utf-8')
        compile_one(src, o, '/std:c11 /DPHX_CABAC_BIN_TRACE',
                    [ROOT / 'src'], ROOT)
        objs.append(o)
    exe = tmp / 'phx_cli_trace.exe'
    link(exe, objs)
    return exe


# ---------------------------------------------------------------------------
# Run + compare
# ---------------------------------------------------------------------------

def fixture_frames(fixture: pathlib.Path) -> int:
    # 64x64 I420 is the synth fixture used by first_frame_probe.py.
    return max(1, fixture.stat().st_size // (64 * 64 * 3 // 2))


def run_traces(tmp: pathlib.Path, fixture: pathlib.Path,
               preset: str = 'medium') -> Tuple[pathlib.Path, pathlib.Path]:
    x264_log, phx_log = tmp / 'L1_BINS_X264.txt', tmp / 'phx_bins.txt'
    for p in (x264_log, phx_log):
        if p.exists():
            p.unlink()
    xexe, pexe = tmp / 'x264_trace.exe', tmp / 'phx_cli_trace.exe'
    frames = fixture_frames(fixture)
    xout = tmp / 'golden_trace.h264'
    cmd = [str(xexe), '--input-res', '64x64', '--input-csp', 'i420',
           '--fps', '25', '--frames', str(frames), '--qp', '28',
           '--keyint', '30', '--min-keyint', '30', '--scenecut', '0',
           '--preset', preset, '--cabac', str(fixture), '-o', str(xout)]
    r = subprocess.run(cmd, cwd=str(tmp), capture_output=True, text=True,
                       timeout=600)
    if r.returncode:
        raise RuntimeError('x264 trace run failed: ' + (r.stdout + r.stderr)[-800:])
    (tmp / 'golden_stderr.txt').write_text(r.stderr, encoding='utf-8',
                                           errors='replace')
    pout = tmp / 'phx_trace.h264'
    cmd = [str(pexe), str(fixture), '64', '64', '28', preset, 'cabac', str(pout)]
    r = subprocess.run(cmd, cwd=str(tmp), capture_output=True, text=True,
                       timeout=600)
    if r.returncode:
        raise RuntimeError('phx trace run failed: ' + (r.stdout + r.stderr)[-800:])
    if not x264_log.exists() or not phx_log.exists():
        raise RuntimeError('trace logs were not created')
    return x264_log, phx_log


def expand_ue_bits(exp_bits: int, val: int, nbins: int) -> List[int]:
    v = val + (1 << exp_bits)
    k = v.bit_length() - 1
    lut = [-1, 0x2, 0x14, 0x68, 0x1d0, 0x7a0, 0x1f40, 0x7e80,
           0x1fd00, 0x7fa00, 0x1ff400, 0x7fe800, 0x1ffd000, 0x7ffa000,
           0x1fff4000, 0x7ffe8000]
    x = (lut[k - exp_bits] << exp_bits) + v
    return [(x >> (nbins - 1 - j)) & 1 for j in range(nbins)]


def expand_bins(bins: Sequence[Bin]) -> List[Tuple[str, int, int, str]]:
    """Flatten trace records to (kind, ctx, bin value, provenance)."""
    out: List[Tuple[str, int, int, str]] = []
    for i, bn in enumerate(bins):
        if bn.kind == 'ue':
            eb, val, nb = bn.extra
            for bit in expand_ue_bits(eb, val, nb):
                out.append(('byp', -2, bit, f'record {i}: ue({eb},{val})'))
        else:
            out.append((bn.kind, bn.ctx, bn.b, f'record {i}'))
    return out


def compare(xbins: Sequence[Bin], pbins: Sequence[Bin],
            max_report: int = 12) -> int:
    """Print first divergence.  Return 0 if identical, 1 otherwise."""
    xf = expand_bins(xbins)
    pf = expand_bins(pbins)
    n = min(len(xf), len(pf))
    first = -1
    for i in range(n):
        if xf[i][:3] != pf[i][:3]:
            first = i
            break
    if first < 0 and len(xf) == len(pf):
        print('VERDICT: first MB0 bin streams are IDENTICAL '
              f'({len(xf)} ordered bins).')
        return 0

    print(f'x264 golden bins: {len(xf)}; Phoenix bins: {len(pf)}')
    if first < 0:
        print(f'VERDICT: first divergence is a LENGTH difference at bin {n}.')
        first = n
    print(f'FIRST DIVERGENT BIN: index {first} (0-based, bin #{first + 1})')
    lo = max(0, first - 4)
    hi = min(max(len(xf), len(pf)), first + 5)
    for i in range(lo, hi):
        x = xf[i] if i < len(xf) else None
        p = pf[i] if i < len(pf) else None
        mark = '  <== FIRST DIVERGENCE' if i == first else ''
        xs = (f'ctx={x[1]:<4} b={x[2]:<2} {x[0]:<4}' if x else '<none>')
        ps = (f'ctx={p[1]:<4} b={p[2]:<2} {p[0]:<4}' if p else '<none>')
        print(f'  bin {i:<5} x264: {xs:<22} phx: {ps:<22}{mark}')
    xi = xbins[min(first, len(xbins) - 1)]
    pi = pbins[min(first, len(pbins) - 1)]
    print(f'  x264 element: {label_ctx(xf[first][1] if first < len(xf) else -9)}')
    print(f'  phx  element: {label_ctx(pf[first][1] if first < len(pf) else -9)}')
    print(f'  x264 record: {xi.render()}')
    print(f'  phx  record: {pi.render()}')
    if first < len(xf) and first < len(pf):
        same_low = (xf[first] == pf[first] or
                    (xbins[first].low, xbins[first].rng) ==
                    (pbins[first].low, pbins[first].rng))
        print(f'  coder state before bin: '
              f'{"MATCH" if same_low else "DIVERGED"} '
              f'(x264 low/range {xbins[first].low}/{xbins[first].rng}, '
              f'phx {pbins[first].low}/{pbins[first].rng})')

    # Remaining divergence: align by dropping the extra Phoenix bin(s) and
    # report how much of the tail still agrees.
    print('\nREMAINING DIVERGENCE (after removing inserted Phoenix bin(s)):')
    px = [v[:3] for v in xf]
    pp = [v[:3] for v in pf]
    shifted = 0
    j = 0
    while j < min(len(px), len(pp)):
        if pp[j] == px[j]:
            j += 1
            continue
        if (len(pp) > len(px) and j + 1 < len(pp) and pp[j + 1] == px[j]):
            print(f'  extra Phoenix bin at index {j} '
                  f'(ctx={pp[j][1]}, b={pp[j][2]})')
            pp.pop(j)
            shifted += 1
            continue
        if (len(px) > len(pp) and j + 1 < len(px) and px[j + 1] == pp[j]):
            print(f'  missing Phoenix bin at index {j} '
                  f'(x264 ctx={px[j][1]}, b={px[j][2]})')
            px.pop(j)
            shifted += 1
            continue
        break
    k = 0
    while k < min(len(px), len(pp)) and px[k] == pp[k]:
        k += 1
    print(f'  {shifted} inserted/deleted bin(s) removed; '
          f'tail agrees for {k} bins; '
          f'next mismatch at aligned index {k}: '
          f'x264={px[k] if k < len(px) else None}, '
          f'phx={pp[k] if k < len(pp) else None}')
    if k < min(len(px), len(pp)) and k < max_report:
        print(f'  x264 element: {label_ctx(px[k][1])}')
        print(f'  phx  element: {label_ctx(pp[k][1])}')
    return 1


ZIGZAG4 = [0, 4, 1, 2, 5, 8, 12, 9, 6, 3, 7, 10, 13, 14, 11, 15]


def _parse_block_lines(lines: Sequence[str], prefix: str,
                       cat: int) -> Dict[int, Tuple[int, List[int]]]:
    out: Dict[int, Tuple[int, List[int]]] = {}
    for line in lines:
        f = line.split()
        if len(f) < 5 or f[0] != prefix or int(f[1]) != cat:
            continue
        try:
            idx, ctx = int(f[2]), int(f[3].rstrip(':'))
            vals = [int(v) for v in f[4:20]]
        except ValueError:
            continue
        if len(vals) == 16:
            out.setdefault(idx, (ctx, vals))
    return out


def compare_coefficients(tmp: pathlib.Path) -> int:
    """Check whether a zigzag rescan of Phoenix's arrays matches x264's.

    x264 leaves quant_4x4_trellis() output in natural order (rdo.c:890
    dct[zigzag[i]]) and applies zigzagf.scan_4x4() before CABAC
    (encoder/macroblock.c:391/410).  The Phoenix trellis handoff does not.
    """
    g = tmp / 'golden_stderr.txt'
    p = tmp / 'phx_blocks.txt'
    if not g.exists() or not p.exists():
        return 0
    # The MB0 decision dump uses a literal \\n in its format strings; split on
    # both real newlines and that literal so every record is visible.
    glines = []
    for line in g.read_text(encoding='utf-8', errors='replace').splitlines():
        glines.extend(line.split('\\n'))
    xb = _parse_block_lines(glines, 'L1BLK', 2)
    # The MB0 decision dump also carries every 4x4 array; fill any index the
    # residual hook missed (its first call is consumed by an earlier MB).
    for line in glines:
        f = line.split()
        if len(f) < 6 or f[0] != 'L1BLK' or not f[2].startswith('nnz='):
            continue
        try:
            idx = int(f[1])
            vals = [int(v.rstrip(':')) for v in f[5:21]]
        except ValueError:
            continue
        if len(vals) == 16:
            xb.setdefault(idx, (96, vals))
    pb = _parse_block_lines(p.read_text(encoding='utf-8', errors='replace')
                             .splitlines(), 'blk', 2)
    common = sorted(set(xb) & set(pb))
    if not common:
        return 0
    raw_ok = sum(1 for i in common if xb[i][1] == pb[i][1])
    rescan_ok = sum(1 for i in common
                    if xb[i][1] == [pb[i][1][k] for k in ZIGZAG4])
    print('\nCOEFFICIENT HANDOFF (MB0 luma 4x4 arrays):')
    print(f'  blocks compared: {len(common)}; '
          f'raw arrays equal: {raw_ok}/{len(common)}; '
          f'equal after zigzag rescan: {rescan_ok}/{len(common)}')
    for i in common:
        if xb[i][1] != pb[i][1]:
            print(f'  block {i}: x264 {xb[i][1][:6]} ... '
                  f'phx {pb[i][1][:6]} ... '
                  f'phx-zigzag {[pb[i][1][k] for k in ZIGZAG4][:6]} ...')
            break
    return 0 if rescan_ok == len(common) else 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--all', action='store_true',
                    help='build both harnesses, run them and compare (default)')
    ap.add_argument('--compare-only', action='store_true',
                    help='compare existing trace logs without rebuilding')
    ap.add_argument('--tmp', type=pathlib.Path, default=DEFAULT_TMP,
                    help='temporary build/log directory')
    ap.add_argument('--fixture', type=pathlib.Path, default=DEFAULT_FIXTURE,
                    help='raw I420 fixture (64x64)')
    ap.add_argument('--preset', default='medium', help='x264 preset')
    args = ap.parse_args(argv)
    tmp = args.tmp.resolve()
    tmp.mkdir(parents=True, exist_ok=True)
    try:
        if not args.compare_only:
            print('building instrumented x264 golden harness '
                  f'({SRC/"encoder/cabac.c"}, {SRC/"common/cabac.c"}) ...')
            build_x264_golden(tmp)
            print('building Phoenix -DPHX_CABAC_BIN_TRACE debug CLI ...')
            build_phx_trace(tmp)
            print('running both encoders on', args.fixture)
        xl, pl = run_traces(tmp, args.fixture.resolve(), args.preset)
        print(f'golden log: {xl}\nphoenix log: {pl}')
        rc = compare(parse_trace(xl), parse_trace(pl))
        compare_coefficients(tmp)
        return rc
    except (RuntimeError, OSError) as exc:
        print('ERROR:', exc, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
