# 04 Analysis / mode decision — status and next step

The dashboard rates 04 Analysis at 2/10 weighted — the most behind stage:

| Family | Status |
|---|---|
| rd_cost* / trellis | 26 MISSING |
| mb_analyse_inter_b* | 9 MISSING |
| mb_analyse_inter_p* | 4 PARTIAL |
| macroblock_analyse | 1 PARTIAL |
| mb_analyse_intra | 1 PARTIAL |
| analyse_weight_frame | 1 PARTIAL |
| mb_analyse_intra_chroma | EXACT |
| macroblock_probe_skip | EXACT |

## This turn: combined I8x8 path attempted

Ported x264's full I8x8 decision into `vk_i8x8_decide`: filtered `edge[36]`
(`predict_8x8_filter`), `intra_sad_x3_8x8` (V/H/DC SAD), the
`intra_analysis_shortcut` candidate tables, and exact 4/5/7-mode predictors
with F1/F2 filtering. Corrected two bugs found on the way:

1. `I8_SC[0][0][favor]` was indexing an `int8_t` as a pointer (crash);
   correct is `I8_SC[cand9][favor]`.
2. x264 only uses x3+shortcut for neighbour class 4 (>=6 candidates);
   edge blocks use the full candidate list. With this, MB3's I8 modes
   matched x264 exactly (9,9,2,1).
3. x264 returns DC (mode 2) as the predicted mode when either neighbour
   is unavailable (not the edge-DC variants).

## Result

MB3 I8 modes matched, but the overall CABAC stream regressed
(first divergence #343 -> #324) and byte-exact NAL2 prefix stayed at 49.
The changes were reverted; all gates remain green. MB5 modes still differ
(oracle 1,7,2,1 vs x264 2,5,2,4) — the remaining predictor detail needs
per-block cost dumps (`build/dump_x264_i8blocks.py`) for modes 5/VR and
4/DDR, which are still being mis-predicted.

## Recommendation

04 Analysis is correctly identified as the largest gap. The tractable
sub-goal is MB5's I8 modes: compare per-mode SADs for VR/DDR against the
`X8C` dumps and fix the edge-sample indexing before re-landing the
combined path. `rd_cost/trellis` (26 MISSING) only affects subme>=6
presets and will not move the superfast byte-exact prefix.
