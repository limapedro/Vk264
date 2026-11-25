# Preset parameter × function matrix (x264 authoritative → oracle)

Source of truth: `build/x264-ref-src/common/base.c:384-602` (defaults + `x264_param_apply_preset`). Oracle side: `Vk264-rewrite/src/ref/nal.c:65-122` and the files listed below. Verified against `main @ c7f5ea2` (2026-11-24); the table in §1 is x264's values, not necessarily the oracle's (see §2/§4).

## 1. Effective parameter values per preset

| parameter | ultrafast | superfast | veryfast | faster | fast | medium | slow | slower | veryslow | placebo |
|---|---|---|---|---|---|---|---|---|---|---|
| refs | 1 | 1 | 1 | 2 | 2 | 3 | 5 | 8 | 16 | 16 |
| bframes | 0 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 8 | 16 |
| scenecut | 0 | 40 | 40 | 40 | 40 | 40 | 40 | 40 | 40 | 40 |
| badapt | NONE | FAST | FAST | FAST | FAST | FAST | FAST | TRELLIS | TRELLIS | TRELLIS |
| deblock | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| cabac | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| intra | 0 | I4x4|I8x8 | I4x4|I8x8 | I4x4|I8x8 | I4x4|I8x8 | I4x4|I8x8 | I4x4|I8x8 | I4x4|I8x8 | I4x4|I8x8 | I4x4|I8x8 |
| inter | 0 | I8x8\|I4x4 | I4x4\|I8x8\|PSUB16x16\|BSUB16x16 | I4x4\|I8x8\|PSUB16x16\|BSUB16x16 | I4x4\|I8x8\|PSUB16x16\|BSUB16x16 | I4x4\|I8x8\|PSUB16x16\|BSUB16x16 | I4x4\|I8x8\|PSUB16x16\|BSUB16x16 | I4x4\|I8x8\|PSUB16x16\|BSUB16x16\|PSUB8x8 | I4x4\|I8x8\|PSUB16x16\|BSUB16x16\|PSUB8x8 | I4x4\|I8x8\|PSUB16x16\|BSUB16x16\|PSUB8x8 |
| transform8x8 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| me | DIA | DIA | HEX | HEX | HEX | HEX | HEX | UMH | UMH | TESA |
| subme | 0 | 1 | 2 | 4 | 6 | 7 | 8 | 9 | 10 | 11 |
| merange | 16 | 16 | 16 | 16 | 16 | 16 | 16 | 16 | 24 | 24 |
| mixedref | 0 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 |
| fastpskip | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 |
| bipred | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| wpred | NONE | SIMPLE | SIMPLE | SIMPLE | SIMPLE | SMART | SMART | SMART | SMART | SMART |
| trellis | 0 | 0 | 0 | 1 | 1 | 1 | 2 | 2 | 2 | 2 |
| mbtree | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| lookahead | 0 | 0 | 10 | 20 | 30 | 40 | 50 | 60 | 60 | 60 |
| direct | SPATIAL | SPATIAL | SPATIAL | SPATIAL | SPATIAL | SPATIAL | AUTO | AUTO | AUTO | AUTO |

`i_chroma_qp_offset`: oracle sets -2 for subme>=6 (`nal.c:105`), matching x264 `encoder.c` psy-RD behaviour.

## 2. Oracle implementation status by parameter

| parameter group | oracle location | status |
|---|---|---|
| refs / bframes / cabac / deblock / transform8x8 / wpred | `nal.c:62-109 + nal.c:108 (`vk_i8_analysis_enabled`)` | IMPLEMENTED — SPS/PPS byte-identical in all 120 cases |
| i_subme (value stored + gating) | `param.h:23, nal.c:73/78-112` | USED — gates I4 analysis, chroma QP offset, RD paths; comment at nal.c:62 is stale |
| i_me_method (DIA/HEX/UMH/TESA) | `lowres.c:dia_search (DIA only)` | PARTIAL — only DIA; HEX/UMH/TESA absent |
| i_me_range | `lowres.c search bounds` | PARTIAL — halfres range only |
| i_trellis | `trellis.c/h`, `nal.c:113-121`, calls in `cabac_enc.c:273/670` | **FOUNDATION, disabled by default** — x264 uses 1 (faster..medium) / 2 (slow+); E18b proved analysis-time wiring regresses (faster 17→11), so `vk_cur_trellis=0` unless `VK_TRELLIS` env set; DC/chroma helpers are stubs |
| b_adapt FAST/TRELLIS | `slicetype.c b-adapt fast` | PARTIAL — fast only; TRELLIS (slower+) absent |
| b_mb_tree | `none` | MISSING — affects lowres edge-MB gating |
| i_lookahead | `slicetype.c single-pass` | MISSING — encoder-driven multi-pass |
| intra ABCD / inter partitions PSUB8x8 | `cabac_enc.c I4/I8; pframe.c` | PARTIAL — I4/I8 present; PSUB8x8 absent |
| b_fast_pskip | `pframe probe + cabac p_probe` | PARTIAL — probe path only, no ME early-termination |
| i_direct_mv_pred | `none obvious` | MISSING for B slices |
| weighted bipred | `lowres.c bipred_cost` | PARTIAL — equal/dsf weights |

## 2b. Parameter alignment audit (oracle value vs x264 value)

100% byte-exact parity requires the oracle to use **the same parameter values
as x264** for every preset. Current alignment (verified at `main @ 8f9112e`):

| x264 parameter | x264 preset values | oracle value | status |
|---|---|---|---|
| `i_frame_reference` | 1/1/1/2/2/3/5/8/16/16 | same (`nal.c:69-101`) | **ALIGNED** (SPS/PPS exact) |
| `i_bframe` | 0/3/3/3/3/3/3/3/8/16 | same (`nal.c`) | **ALIGNED** in headers; emission partial |
| `b_cabac` | 0/1/.../1 | same | **ALIGNED** |
| `b_deblocking_filter` | 0 ultrafast, 1 else | same | **ALIGNED** |
| `b_transform_8x8` | 0 ultrafast, 1 else | same (`vk_i8_analysis_enabled`) | **ALIGNED** |
| `i_subpel_refine` | 0/1/2/4/6/7/8/9/10/11 | same (`vk_cur_subme`) | **ALIGNED** |
| `i_chroma_qp_offset` | 0 ultra..faster, -2 fast+ | same | **ALIGNED** |
| `analyse.i_trellis` | 0/0/0/1/1/1/2/2/2/2 | now same (`vk_cur_trellis`) | **ALIGNED** since E45; primitive still partial |
| `analyse.i_me_method` | DIA/DIA/HEX/HEX/HEX/HEX/HEX/UMH/UMH/TESA | DIA only | **MISSING** — plan item 12 |
| `analyse.i_me_range` | 16..16/24/24 | 16 | **PARTIAL** — plan item 12 |
| `analyse.b_fast_pskip` | 1..1/0 placebo | same; subme>=3 probes unconditionally (E47) | **ALIGNED** (ME path still partial) |
| `i_bframe_adaptive` | NONE/FAST/FAST/.../TRELLIS | FAST only | **PARTIAL** — plan item 11 |
| `rc.b_mb_tree` | 0/0/1... | not wired into lookahead | **PARTIAL** — plan item 14 |
| `rc.i_lookahead` | 0/0/10/20/30/40/50/60/60/60 | single-pass | **MISSING** — plan item 14 |
| `analyse.i_weighted_pred` | NONE/SIMPLE.../SMART | partial | **PARTIAL** — plan item 13 |
| `analyse.i_psy_rd` | 1.0 fast+ | not implemented | **MISSING** — plan item 7 |

Rule: an ALIGNED parameter must not be disabled just because its current
implementation is incomplete — the value must match x264, and the
implementation is what gets fixed next.

## 3. Function inventory (Vk264-rewrite/src/ref)

`bitstream.c`, `bitstream.h`, `bslice.c`, `bslice.h`, `cabac.c`, `cabac.h`, `cabac_enc.c`, `cabac_enc.h`, `cabac_tables.h`, `cavlc.c`, `cavlc.h`, `cavlc_tables.h`, `encoder.c`, `encoder.h`, `hpel.c`, `hpel.h`, `i4x4.c`, `i4x4.h`, `i4x4enc.c`, `i4x4enc.h`, `i8x8.c`, `i8x8.h`, `i8x8decide.c`, `i8x8decide.h`, `i8x8enc.c`, `i8x8enc.h`, `intra16.c`, `intra16.h`, `lowres.c`, `lowres.h`, `lowres_path.c`, `mbtree.c`, `mbtree.h`, `motion.c`, `motion.h`, `nal.c`, `param.h`, `pframe.c`, `pframe.h`, `ratecontrol.c`, `ratecontrol.h`, `slice.c`, `slice.h`, `slicetype.c`, `slicetype.h`, `subpel.c`, `subpel.h`

## 4. Correlation with measured parity

Current verdict tiers (fresh 120-case audit at `main @ c7f5ea2`):
- ultrafast: subme=0, trellis=0, no I4 analysis, no B — **12/12 byte-exact** (the oracle implements precisely this tier).
- superfast: subme=1 — IDR NAL2 byte4 (CAVLC), P-slice NAL3 byte7 (CABAC).
- veryfast: subme=2, trellis=0 — CABAC NAL2 **byte79** after PR #288 SATD + E37 chroma SATD; MB1 I4 modes match x264 exactly; next gap is a later MB's I8-vs-I4 transform-size flag (bin541 ctx400).
- faster: subme=4, x264 trellis=1 — CABAC NAL2 **byte17**; E18/E18b: trellis is the cause; oracle trellis foundation exists but is disabled until write-time integration lands.
- fast/medium/slow/slower: subme=6..9, x264 trellis=1/2 — CABAC NAL2 **byte15** (MB-level RD/pred-mode tier); oracle trellis disabled.
- veryslow/placebo: subme=10/11, ME range 24, TESA — CABAC NAL2 **byte16**.
- 30-frame clips: 24 NAL-count mismatches from missing B-slice emission (plan item 11).

See `docs/PARITY_FINDINGS.md` for the full 120-case breakdown.
