# I4x4 CABAC bit-level differential methodology

This document records the reproducible differential method used to compare the
oracle's I4x4 CABAC macroblock stream against x264 decision-by-decision, and the
exact state of the comparison as of this writing.

## Tooling

| File | Purpose |
|---|---|
| `build/dump_x264_trace.py` | Patches x264 `common/cabac.c` C encoder functions to log `(ctx, bin)` for every decision, rebuilds via `build_ref_x264_scratch.ps1`, encodes the superfast 64x64 clip, restores source. Output: `build/x264_trace.txt`. |
| `build/dump_x264_modes.py` | Patches `encoder/analyse.c` to dump raw `i_predict4x4[16]`, MB type and cbp for MB0. |
| `build/dump_x264_dct.py` | Patches `encoder/macroblock.h` `x264_mb_encode_i4x4()` to dump pre/post-quant DCT and prediction samples for MB0. |
| `build/cabac_trace.py` | Standalone Python CABAC decoder (used to validate interpretation; x264 decision logging is the gold standard). |

Note: x264's MSVC scratch build uses the **C** CABAC functions (`HAVE_MMX 0`),
so the `x264_cabac_encode_*_c` functions can be instrumented directly. The
instrumented build produces a byte-identical stream to the shipped `x264.exe`
(verified: both produce NAL2 `65888400ffec837a293fc16b3b8...`).

## Clip / invocation

- Source: `build/preset_audit_work/c004.yuv`, 64x64, 2 frames, `--qp 28`
- x264: `--preset superfast --cabac --keyint 30 --frames 2 --threads 1`
- Oracle CLI: `vk264_oracle_video_cli c004.yuv 64 64 25 28 1 30 2 out.h264 superfast`
  (the CLI uses `argv[4]`/`25` as the I-frame QP, matching x264's IP-ratio QP 25.)

## Decision-stream comparison (after the fixes in commit `edf4727`)

First 25 decisions agree **exactly**:

```
idx  x264        oracle      meaning
 0   d 3 0       d 3 0       I4 mb_type
 1   d 399 0     d 399 0     4x4 transform size
 2..17 d 68 1    d 68 1      16 intra4x4 pred modes == predictor
 18  d 64 0      d 64 0      chroma pred mode DC
 19  d 73 1      d 73 1      cbp_luma bit0 (blocks 0-3)
 20  d 73 0      d 73 1      cbp_luma bit1  <-- FIRST DIVERGENCE
 21  d 73 0      d 73 0      cbp_luma bit2
 22  d 76 0      d 76 0      cbp_luma bit3
 23  d 77 0      d 77 0      cbp_chroma
 24  d 60 0      d 60 0      qp_delta
```

Bugs fixed to get here:
1. Raw intra4x4 modes 9/10/11 were signalled directly; x264 applies
   `x264_mb_pred_mode4x4_fix()` → 2 first. (`cabac_enc.c`)
2. Unavailable CBP neighbours used `0`; x264 uses `-1` (all bits set).
   (`cabac_enc.c`)

## Remaining divergence

x264's `i_predict4x4[16]` for MB0 (`11,9,10,2,9,9,2,2,10,2,10,2,2,2,2,2`)
matches the oracle's `vk_i4x4_decide` output exactly, so the mode decision is
correct. The cbp mismatch comes from the **residual prediction** of blocks 2/4:
x264's FDEC reconstruction makes those predictions ≈16, the oracle's local
`rmb[]` makes them ≈128. Porting x264's FDEC padding/reconstruction state is
the next concrete step.

Also ruled out:
- QP mismatch (verified qp=25 both).
- Quant table scale (x264 flat CQM scales dequant by 16; oracle matches).
- SATD vs SAD in mode decision (both produce identical modes on this clip).

## Update (this turn)

The decision stream now agrees through MB0 completely and into MB1
(first divergence at #53). See FINDINGS.md for the four CABAC fixes
(block order, DCT_LUMA_4x4 category, CBF bin, scan8 nnz cache) and the
mode-decision candidate-class fix. The next remaining issue is MB1's
intra mode selection on textured content.

## Diagnostic update (this turn): x3 input discrepancy

Direct instrumentation of x264's `mb_analyse_intra` for MB1 idx2 produced
conflicting fenc values across differently-configured scratch runs
(`16,15,25,55,...` with `--frames 2` vs `16,16,16,15,...` with
`--frames 1`), showing x264 invokes intra analysis more than once per MB
(lookahead plus encode) and the two passes see different buffers. The
gold decision trace was captured with `--frames 2`; matching the oracle's
analysis input therefore requires pinning the exact pass, not just the MB.

Measured x3 results with `--frames 2`: x264 V/H/DC = 992/548/892,
oracle = 290/82/146. Given that a single-frame run shows flat fenc data,
the remaining mode mismatch is attributed to the wrong analysis pass
being compared rather than a predictor bug. Resolving it requires tagging
the x264 dump with the pass identifier; this is the concrete next step.

No code changes landed this turn (all gates remain green at 42e2caf).
