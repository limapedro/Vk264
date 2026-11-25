# Phoenix data structures — what must be ported and why

All references are to `reference/x264/`. A shader port is only
possible if every piece of state is explicit and owned by a named structure.
The current oracle uses globals (`vk_cur_subme`, `vk_cur_trellis`,
`vk_i4_analysis_enabled`, …) — those must disappear in Phoenix.

## 1. `x264_param_t` — `x264.h:310`

The complete parameter set. Groups:

| group | representative fields | why it matters for parity |
|---|---|---|
| video | `i_csp`, `i_width/height`, `vui.*` | SPS/PPS bytes |
| GOP | `i_keyint_max`, `i_keyint_min`, `i_bframe`, `b_open_gop`, `i_scenecut_threshold`, `i_bframe_adaptive` | IDR placement (E96–E101) |
| coding | `b_cabac`, `b_deblocking_filter`, `analyse.b_transform_8x8`, `analyse.i_me_method`, `i_me_range`, `i_subpel_refine`, `analyse.i_trellis` | preset identity |
| RD/psy | `analyse.b_psy`, `f_psy_rd`, `f_psy_trellis`, `analyse.i_chroma_qp_offset` | mode costs |
| RC | `rc.i_rc_method`, `f_rf_constant`, `rc.i_lookahead`, `rc.b_mb_tree`, `rc.i_aq_mode` | QP per MB/frame |

**Rule:** Phoenix copies x264's fields verbatim (names and types), even when a
field is unused by the current gate. Preset deltas from `common/base.c:489`
must set exactly the same fields.

## 2. `x264_t` — `common/common.h:270`

The encoder context. Sub-structures to port, in dependency order:

1. `param` — a full `x264_param_t` copy (the encoder must not read caller memory).
2. `mb` — all per-MB caches: `intra4x4_pred_mode`, `non_zero_count`, `i_cbp_*`,
   `b_transform_8x8`, neighbour flags, `i_neighbour`, `i_neighbour4`,
   `i_neighbour8`, `i_mb_xy`, `i_type`. `x264_macroblock_cache_load`
   (macroblock.c:855) and `_save` (common/macroblock.c:1680) own these.
3. `cabac` — `x264_cabac_t` (see §4) and the transition tables.
4. `dct` — `luma4x4[16]`, `luma8x8[4]`, `luma16x16_dc`, `chroma_dc[2]`.
5. `mb_analysis` / cost tables — `i_satd_i16x16`, `i_satd_i8x8`,
   `i_satd_i4x4`, `i_predict4x4[16]`, `i_predict8x8[4]`, per-mode costs.
6. `lookahead` — frame lists, `last_nonb`, `i_last_keyframe`, slicetype state.
7. `rc` — rate control state (QP accumulators, AQ, MB-tree).
8. `pixf` — function-pointer table for pixel ops (DCT/SAD/SATD/predict).

**Shader note:** `mb.*`, `dct.*`, `cabac.*` are per-frame/per-MB working state.
Keep them in a single struct that can be laid out in an SSBO.

## 3. `x264_frame_t` — `common/frame.h:37`

Key fields:

- `lowres[4]` — four half-res planes (orig/H/V/HV) used by all lookahead costs.
- `i_stride_lowres`, `i_width_lowres`, `i_lines_lowres` (frame.h:77).
- `lowres_mvs[2][X264_BFRAME_MAX+1]`, `lowres_costs[...]`,
  `lowres_mv_costs[2][...]` (103–114) — cached per-frame ME state.
- `i_cost_est[][]` / `i_cost_est_aq[][]` (123–124) — slicetype frame costs.
- `i_intra_cost[]` (135) — MB-tree input.
- `i_type`, `i_qp`, `b_scenecut`, `i_frame`, `i_frame_num`, `i_poc`.

The current oracle computes lowres costs on demand and throws them away. x264
**caches** them on the frame object across slicetype passes. That is the root
of the E98/E99 candidate-set mismatch: a global per-frame rule cannot know
which frames are `frames[1]` after `last_nonb`.

## 4. `x264_cabac_t` — `common/cabac.h:33`

`i_low`, `i_range`, `i_queue`, `state[1024]`, `f8_bits_encoded`. The state
array is serial and must be threaded through every bin write. In the oracle,
CABAC state is already live (`cb->state`) — keep that design, but make the
context offsets come from x264's tables (`x264_coeff_abs_level_m1_offset`,
`x264_significant_coeff_flag_offset`, `x264_last_coeff_flag_offset`) rather
than hand-maintained constants.

## 5. Cost tables — `encoder/analyse.c:143–260`

`init_costs` (143), `x264_analyse_init_costs` (179), `mb_analyse_load_costs`
(248), `mb_analyse_init_qp` (255). These produce `cost_i4x4_mode[]` and the
lambda/mode-bit tables used by **every** mode decision. Missing in the oracle
(E88/E89). Port these before touching any cost formula.

## 6. Neighbour caches — `common/macroblock.h`

`x264_scan8[51]` (macroblock.h:180) maps a 4x4 block to its cache slot;
`STORE_8x8_NNZ` (112) writes the four sub-block nnz flags. E83–E95 showed the
oracle had multiple off-by-one errors here. Port the cache layout verbatim.

## 7. What Phoenix must NOT do

- No global preset knobs (`vk_cur_*`). Put them in `phx_t`/`x264_t`.
- No approximated tables where x264 has a real table (Hadamard weights,
  chroma QP offsets, coefficient token VLCs).
- No "fix" that changes a parameter value to make a metric move.
- No function that only covers one preset; every function must be written for
  all presets from the start, gated by the ported parameter.
