# x264 → Vk264 function parity matrix

Generated 2026-11-20 from `references/x264-master` (x264 `c24e06c2`).
669 functions across 35 `.c` files. This document tracks which x264 functions
have a Vk264 counterpart, whether that counterpart is byte-exact against the
verbatim x264 C oracle, and the evidence. Status values:

- **EXACT** — a byte-exact fixture exists and passes (CTest named below).
- **PARTIAL** — a GPU implementation exists but is only exact for restricted inputs.
- **C++** — faithfully ported in C++, no GPU compute needed (control/tables/state).
- **MISSING** — no counterpart yet; candidate for a new PR.
- **N/A** — platform/CLI/threading/infrastructure, out of scope for encoder parity.

Verification uses real footage (see `docs/real-content-verification.md`) plus
the synthetic byte-exact gates. The encoding pipeline order is the x264 call
order in `encoder/encoder.c::x264_encoder_encode`.

## Pipeline stage 1 — parameters, SPS/PPS, headers (`common/base.c`, `common/set.c`, `encoder/set.c`)

| x264 function(s) | Vk264 counterpart | Status | Evidence |
|---|---|---|---|
| `x264_param_default`, `param_apply_preset`, `x264_param_apply_profile` | `src/x264_presets.hpp` | PARTIAL | preset knobs only, CLI defaults differ |
| `x264_cqm_init`, `x264_cqm_parse_file` | — | MISSING | CQM not supported |
| `x264_sps_init`, `x264_sps_write` | `parameters.comp`, `verifyParameters` | EXACT | `x264_parameters_parity` |
| `x264_pps_init`, `x264_pps_write` | `parameters.comp`, `verifyParameters` | EXACT | `x264_parameters_parity` |
| `x264_slice_header_init/write` | `slice_header.comp`, `verifySliceHeader` | EXACT | `x264_slice_header_parity` |
| `x264_sei_*` (10 functions) | `nal.comp`, `verifyNal` | PARTIAL | `x264_nal_parity`; encoder-identity SEI excluded |
| `x264_validate_levels` | `src/x264_levels.hpp` | C++ | port present, CLI never calls it |

## Pipeline stage 2 — frame management (`common/frame.c`)

All 29 functions (`x264_frame_new/copy_picture/expand_border*`, sync-frame lists,
weight scale) are **C++** infrastructure; border extension is implicitly handled
by the shader padding. No byte-exact GPU fixture yet → candidate for MISSING audit.

## Pipeline stage 3 — lookahead & slice type (`encoder/lookahead.c`, `encoder/slicetype.c`, `slicetype-cl.c`)

All 43 functions (`x264_lookahead_*`, `slicetype_*`, `macroblock_tree*`, `scenecut*`,
`x264_weights_analyse`) are **MISSING** — Vk264 has no lookahead/B-frame decision
architecture yet. This blocks `medium+` presets (issue #4).

## Pipeline stage 4 — analysis / mode decision (`encoder/analyse.c`)

| x264 function | Vk264 counterpart | Status | Evidence |
|---|---|---|---|
| `x264_macroblock_analyse` | `inter_frame.comp`, `intra16.comp` | PARTIAL | real footage diverges at MB(0,0) for `fast` |
| `mb_analyse_intra` | `inter_frame.comp` | PARTIAL | I16x16 exact; I4x4 emission missing (#2) |
| `mb_analyse_intra_chroma` | `intra_chroma.comp`, `verifyIntraChroma` | EXACT | `x264_intra_chroma_parity` |
| `mb_analyse_inter_p16x16/8x8/16x8/8x16` | `inter_frame.comp` | PARTIAL | `x264_inter8x8_parity`, `x264_inter4x4_parity` |
| `mb_analyse_inter_b*` (9 functions) | — | MISSING | B-frames (#4) |
| `x264_macroblock_probe_skip` | `motion_elimination.comp` | EXACT | `x264_motion_elimination_parity` |
| `mb_analyse_transform*`, `x264_rd_cost*` (`rdo.c`) | — | MISSING | RD/trellis not ported |
| `x264_analyse_weight_frame` | `rate_control.comp` | PARTIAL | `x264_aq_parity` |

## Pipeline stage 5 — intra prediction (`common/predict.c`, 51 functions)

| x264 family | Vk264 shader | Status | Evidence |
|---|---|---|---|
| `x264_predict_4x4_*` (9 modes) | `predict_directional.comp`, `verifyPredictDirectional` | EXACT | `x264_predict_directional_parity` |
| `x264_predict_8x8*, 8x8c*, 8x16c*` | `predict_intra.comp`, `verifyPredictIntra` | EXACT | `x264_predict_intra_parity` |
| `x264_predict_16x16_*` | `intra16.comp`, `verifyLuma4x4` | EXACT | `x264_intra16_parity` |
| `x264_predict_lossless_*` (`encoder/macroblock.c`) | `intra16.comp` | PARTIAL | I16x16 fixture only |

## Pipeline stage 6 — transform, quant, zigzag (`common/dct.c`, `common/quant.c`)

| x264 family | Vk264 shader | Status | Evidence |
|---|---|---|---|
| `sub4x4_dct`, `add4x4_idct`, `sub8x8_dct*`, `add8x8_idct*`, `sub16x16_dct*` | `dct4x4.comp`, `dct8x8.comp` | EXACT | `x264_dct4x4_group{4,8,16}_parity`, `x264_dct8x8_group{8,16}_parity` |
| `dct4x4dc`, `idct4x4dc`, `sub4x4_dct_dc` | `dc.comp`, `reconstruct_dc.comp` | EXACT | `x264_dc_parity`, `x264_reconstruct_dc_parity` |
| `quant_4x4`, `quant_8x8`, `dequant_*` | `quant.comp`, `quant_group.comp` | EXACT | `x264_quant{4,8}x{4,8}_parity` |
| `zigzag_scan_*`, `zigzag_sub_*` | `zigzag.comp`, `verifyZigzag` | EXACT | `x264_zigzag_parity` |
| `optimize_chroma_*` (6 functions) | `chroma_optimize.comp` | EXACT | `x264_chroma_optimize_parity` |
| `decimate_score*`, `denoise_dct` | `coeff_analysis.comp` | EXACT | `x264_coeff_analysis_parity` |

## Pipeline stage 7 — motion (`common/mc.c`, `common/me.c`, `common/mvpred.c`, `common/pixel.c`)

| x264 family | Vk264 shader | Status | Evidence |
|---|---|---|---|
| `x264_pixel_satd_*`, `sa8d_*`, `ads*`, `ssd*` (pixel.c) | `motion_metrics.comp` | EXACT | `x264_motion_metrics_parity` |
| `mc_luma`, `mc_chroma`, `pixel_avg*`, `hpel_filter` | `motion_compensation.comp`, `reference_frame.comp` | EXACT | `x264_motion_compensation_parity`, `x264_reference_frame_parity` |
| `x264_frame_init_lowres`, `frame_init_lowres_core` | `motion_lowres.comp` | EXACT | `x264_motion_lowres_parity` |
| `integral_init*` | `motion_integral.comp` | EXACT | `x264_motion_integral_parity` |
| `x264_me_search_ref`, `x264_me_refine_*` (me.c) | `motion_search.comp`, `motion_bidir.comp` | EXACT | `x264_motion_search_parity`, `x264_motion_exhaustive_parity`, `x264_motion_bidir_parity` |
| `x264_mb_predict_mv*` (mvpred.c) | `mv_predict.comp` | EXACT | `x264_mv_predict_parity` |
| `mbtree_*`, `x264_frame_filter` | — | MISSING | not ported |

## Pipeline stage 8 — macroblock encode / reconstruct (`encoder/macroblock.c`, 25 functions)

| x264 function | Vk264 counterpart | Status | Evidence |
|---|---|---|---|
| `mb_encode_i16x16`, `macroblock_encode_internal` | `intra16.comp`, `verifyLuma4x4` | EXACT | `x264_intra16_parity` |
| `x264_mb_encode_chroma`, `mb_encode_chroma_internal` | `intra_chroma.comp` | EXACT | `x264_intra_chroma_parity` |
| I4x4 encode path | `encodeI4x4Frame()` host model + `encodeIntra4Macroblock` GPU encoder | PARTIAL | 256/256 reconstruction exact; GPU CABAC emission byte-exact (PR #45) but host mode decision omits x264's DDL mode 3 and the x9 bit-cost/shortcut behaviour (#2) |
| `macroblock_encode_skip`, `x264_macroblock_probe_skip` | `motion_elimination.comp` | EXACT | `x264_motion_elimination_parity` |
| `x264_macroblock_deblock` | `deblock_frame.comp` | EXACT | `x264_deblock_frame_parity` |

## Pipeline stage 9 — deblocking (`common/deblock.c`, 30 functions)

| x264 family | Vk264 shader | Status | Evidence |
|---|---|---|---|
| `deblock_*_c`, `deblock_edge*`, `x264_frame_deblock_row` | `deblock_filter.comp`, `deblock_frame.comp`, `deblock_strength.comp` | EXACT | `x264_deblock_{filter,frame,strength}_parity`, tiled/subgroup variants |

## Pipeline stage 10 — rate control (`encoder/ratecontrol.c`, 51 functions)

| x264 function | Vk264 counterpart | Status | Evidence |
|---|---|---|---|
| `x264_adaptive_quant_frame`, `ac_energy_*` | `aq.comp`, `verifyAqVariance` | EXACT | `x264_aq_parity` |
| `x264_ratecontrol_*` (45 functions) | `rate_control.comp` | PARTIAL | fixed-QP path only; no VBV/CRF/mbtree |

## Pipeline stage 11 — entropy coding (`common/cabac.c`, `encoder/cabac.c`, `encoder/cavlc.c`, `common/vlc.c`, `common/bitstream.c`)

| x264 family | Vk264 shader | Status | Evidence |
|---|---|---|---|
| `x264_cabac_encode_*`, `x264_cabac_init*` | `cabac.comp` | EXACT | `x264_cabac_parity` (9946 sequences) |
| `cabac_ref_p/b`, `cabac_mvd*`, `cabac_block_residual*` | `cabac.comp`, `cabac_syntax.glsl` | EXACT | `x264_cabac_refidx_parity` (1536 sequences) |
| `cabac_mb_header_i` I4x4 path + residual/chroma DC | `encodeIntra4Macroblock` in `cabac_syntax.glsl` | EXACT (encoder) | `x264_i4x4_frame_parity`: GPU emits x264's exact 20-byte superfast flat slice (PR #45). Mode decision still missing (#2). |
| B mb_type header | `cabac.comp` | EXACT | 90 sequences in `x264_cabac_parity` |
| `cavlc_block_residual*`, `cavlc_qp_delta`, `cavlc_mvd` | `cavlc_frame.comp`, `cavlc_residual_test.comp` | EXACT | `x264_cavlc_residual_parity` (2327 sequences) |
| `x264_nal_encode` | `nal.comp` | EXACT | `x264_nal_parity` |
| `x264_bitstream_init` | `bitstream.comp` | EXACT | `x264_bitstream_parity` |

## Pipeline stage 12 — encoder orchestration (`encoder/encoder.c`, `encoder/api.c`)

| x264 function | Vk264 counterpart | Status | Evidence |
|---|---|---|---|
| `x264_encoder_encode` | `x264_vulkan::encode`, `encodeCabacClip` | PARTIAL | real footage diverges (#2/#3/#8) |
| `x264_encoder_headers` | `parameters.comp`, `slice_header.comp` | EXACT | header fixtures |
| `slice_write`, `nal_end`, `encoder_frame_end` | `packAnnexB` | EXACT | bitstream/NAL fixtures |


## Verification additions this session (Subaru, 2026-11-20)

| New byte-exact fixture | CTest | Cases |
|---|---|---|
| I16x16 mode decision (`encoder/analyse.cpp`) | `x264_intra16_mode_parity` | 16640 (5 neighbour configs x QP 0..51 x 64) |
| CAVLC neighbour context (`encoder/cavlc.cpp`) | `x264_cavlc_context_parity` | 400 |
| Production `intra16_frame.spv` | `x264_intra16_frame_parity` | full intra16 suite |
| Production `inter8x8_frame.spv` | `x264_inter8x8_frame_parity` | full inter8x8 suite (frame subset) |
| Production `intra_chroma_frame.spv` | `x264_intra_chroma_frame_parity` | full chroma suite (frame subset) |
| Real-gradient single-MB reproducer | `x264_intra16_parity` | +1 case |

Full suite is now **57/57**. The added fixtures close gaps where the algorithm
was correct but unverified; none of them changed production compute logic.

## Open integration mismatch (not function-level)

Real-footage tracking (`docs/real-content-verification.md`) still shows a
single-MB divergence for a narrow gradient crop at (1792,896): x264 (verified
with a locally built, patched CLI) encodes it at I-frame QP 25, which vk264
also uses (`min_qp:25`), and the intra16 compute shader matches x264 on the
exact source at QP 25. Therefore the divergence is in production staging or a
side channel, not in the DCT/quant/predict functions. Tracked in issue #25.



## Reclassification: platform/accelerator-only functions are N/A

The x264 inventory includes functions that have no meaningful counterpart in a
Vulkan encoder. They are **not** parity gaps and are now classified N/A:

| x264 file | functions | reason |
|---|---|---|
| `common/opencl.c` | 11 | OpenCL lookahead acceleration, replaced by Vulkan compute |
| `encoder/slicetype-cl.c` | 8 | OpenCL slicetype acceleration, replaced by Vulkan compute |
| `common/threadpool.c`, `common/win32thread.c`, `common/osdep.c` | 23 | host threading / OS abstraction, not part of encoded output |

Genuinely missing for full preset parity (still open): `encoder/lookahead.c`
(9), `encoder/slicetype.c` (26), `encoder/rdo.c` trellis/RD (26), mbtree
(`common/mc.c` + `ratecontrol.c`), B-frame inter analysis (`encoder/analyse.c`,
`encoder/cabac.c`, `encoder/cavlc.c`), CQM scaling lists. These are tracked as
issues #4 (B-frames/reordering), #2 (I4x4 emission), and the broader preset
roadmap. Multi-MB staging is issue #30.

## Round 2 verification (Subaru, 2026-11-20)

- **Issue #25 FIXED** (PR #28): vk264 was applying DCT decimation to intra MBs,
  while x264's `mb_encode_i16x16` starts `decimate_score` at 9 for I-slices
  (decimation is P-frame only). The (1792,896) real 16x16 gradient crop is now
  BYTE-EXACT, and 10/10 sampled real single-MB crops pass.
- New byte-exact fixture: chroma mode selection (`x264_chroma_mode_parity`,
  16640/16640 cases). Together with `x264_intra16_mode_parity`, both mode
  decision algorithms are verified against x264's oracle. Suite is 58/58.
- **Multi-MB divergence localised (refined after #25)**: on a 32x32 tiled-real
  crop, after the #25 fix the reconstructed **Y plane is byte-exact** (0 differing
  bytes). Only chroma reconstruction differs (U 64 bytes, V 48 bytes), confined
  to MB(1,1), caused by chroma prediction mode (x264=H, vk264=DC). x264's dumped
  per-mode costs for that MB are V=330, H=102, DC=258, P=336 (lambda 4), so H is
  the correct choice. vk264's staged chroma neighbours were dumped from the
  shader and match x264's (mixed 129/135 left column), and both mode-selection
  algorithms are byte-exact in isolation, so the remaining fault is inside the
  GPU chroma-mode cost path. Forcing H unconditionally breaks other patterns and
  is not a valid fix. Tracked in issue #30.

  Further evidence (this turn): the production flag `frameInput[13]` was dumped
  from the host as `flag13=3` (intra bit + ultrafast mode-decision bit) with
  `preset=ultrafast`, so `selectChromaMode()` is definitely selected — the fault
  is inside its per-mode cost computation, not in flag plumbing. Attempts to
  read the per-mode costs back from the shader via spare fixture fields were
  inconclusive (the chroma pass overwrites several fields); a dedicated debug
  field was added but the readback still returned stale values, so this needs a
  purpose-built debug buffer rather than field reuse.

  Issue #2 note: the harness "flat" pattern is Y=0 (confirmed by inspecting
  `build/sf-now/source.yuv`), which is why x264 selects I4x4 for superfast
  (`mb I I16..4: 0.0% 0.0% 100.0%`). A Y=128 flat frame selects I16x16 instead,
  so I4x4 ground truth must be captured with the exact Y=0 input.
- **Untested production shader variants**: `motion_frame{, _parallel, _batched,
  _tiled}.spv` are compiled from `motion_search.comp` with FRAME_MOTION defines
  and have a different fixture struct layout (extra source/reference stride
  fields). They cannot be tested with the existing `motion_search` fixture; a
  dedicated frame-motion fixture is needed.
## Summary

| Status | Count (approx, function families) |
|---|---|
| EXACT | 60+ families covering DCT, quant, predict, deblock, motion, entropy, headers |
| PARTIAL | preset knobs, analysis/orchestration, rate control, I4x4, lossless |
| MISSING | lookahead/slicetype (~43), RD/trellis (~26), mbtree, CQM, B-frames |
| C++ / N/A | threading, OS, CLI, frame management (~100) |

## Open PRs for mismatches

This matrix is updated as mismatches are found and fixed. See the PRs referenced
in `docs/PROGRESS.dm` and issue #18. Each row that is not EXACT is a candidate.


## Issue #30 CLOSED - full real-footage parity for ultrafast (PR #37)

Root cause #4 (final): x264 `encoder.c::mbcmp_init` selects the intra mode
metric with `satd = !b_lossless && i_subpel_refine > 1`. Ultrafast (subme=0)
and superfast (subme=1) therefore score I16x16 / chroma modes with **SAD**,
not SATD. vk264 always used SATD. An extracted-oracle fixture calling x264's
own `x264_pixel_satd_16x16` on the exact MB(3,1) proved the transform was not
the cause. Fix: carry an x264-faithful bit2 flag in `frameInput[13]` and use
`sad16()`/`sad8()` when subpel_refine <= 1.

Verified with `parity_check.py` (BYTE-EXACT, ignoring encoder-identity SEI):
- real 16x16, 32x32, 64x64 crops from the forza clip
- full 1920x1080 frames, 1 and 3 frames
- 10/10 real 16x16 crops sampled across the frame
- two other 1080p clips (forza_F5eKZa, warzone Verdansk)

58/58 CTests and 4/4 byte-exact parity gates. Issue #30 is closed.
Remaining: #2 (I4x4 emission for superfast) and the MISSING families below.
