# Preset parameter × function matrix (x264 authoritative → oracle)

Source of truth: `build/x264-ref-src/common/base.c:384-602` (defaults + `x264_param_apply_preset`). Oracle side: `Vk264-rewrite/src/ref/nal.c:62-109` and the files listed below.

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
| i_subme (value stored) | `param.h:23, nal.c:70/82-98` | STORED but comment nal.c:59 says analysis knobs "not modelled" |
| i_me_method (DIA/HEX/UMH/TESA) | `lowres.c:dia_search (DIA only)` | PARTIAL — only DIA; HEX/UMH/TESA absent |
| i_me_range | `lowres.c search bounds` | PARTIAL — halfres range only |
| i_trellis | `none (grep: one comment in nal.c)` | MISSING — x264 slow+ uses trellis=2 |
| b_adapt FAST/TRELLIS | `slicetype.c b-adapt fast` | PARTIAL — fast only; TRELLIS (slower+) absent |
| b_mb_tree | `none` | MISSING — affects lowres edge-MB gating |
| i_lookahead | `slicetype.c single-pass` | MISSING — encoder-driven multi-pass |
| intra ABCD / inter partitions PSUB8x8 | `cabac_enc.c I4/I8; pframe.c` | PARTIAL — I4/I8 present; PSUB8x8 absent |
| b_fast_pskip | `pframe probe + cabac p_probe` | PARTIAL — probe path only, no ME early-termination |
| i_direct_mv_pred | `none obvious` | MISSING for B slices |
| weighted bipred | `lowres.c bipred_cost` | PARTIAL — equal/dsf weights |

## 3. Function inventory (Vk264-rewrite/src/ref)

`bitstream.c`, `bitstream.h`, `bslice.c`, `bslice.h`, `cabac.c`, `cabac.h`, `cabac_enc.c`, `cabac_enc.h`, `cabac_tables.h`, `cavlc.c`, `cavlc.h`, `cavlc_tables.h`, `encoder.c`, `encoder.h`, `hpel.c`, `hpel.h`, `i4x4.c`, `i4x4.h`, `i4x4enc.c`, `i4x4enc.h`, `i8x8.c`, `i8x8.h`, `i8x8decide.c`, `i8x8decide.h`, `i8x8enc.c`, `i8x8enc.h`, `intra16.c`, `intra16.h`, `lowres.c`, `lowres.h`, `lowres_path.c`, `mbtree.c`, `mbtree.h`, `motion.c`, `motion.h`, `nal.c`, `param.h`, `pframe.c`, `pframe.h`, `ratecontrol.c`, `ratecontrol.h`, `slice.c`, `slice.h`, `slicetype.c`, `slicetype.h`, `subpel.c`, `subpel.h`

## 4. Correlation with measured parity

The parameter table explains the verdict tiers exactly:
- ultrafast: subme=0, trellis=0, no I4 analysis, no B — **12/12 byte-exact** (the oracle implements precisely this tier).
- superfast/veryfast/faster: subme=1..4, trellis=0 — IDR matches to byte 16, P-slice diverges at bit 49.
- fast/medium/slow/slower: subme=6..9, trellis=1/2 — IDR diverges at byte 5 (RD I4 modes differ).
- veryslow/placebo: subme=10/11, ME range 24, TESA — IDR diverges at byte 6.

See `docs/PARITY_FINDINGS.md` for the full 120-case breakdown.
