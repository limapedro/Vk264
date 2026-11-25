# Shader backend design derived from the ported functions

Purpose: make the `SHADER: A/B/C` annotations in `docs/FUNCTION_PORTS.md`
actionable. This document is the design review artifact required by
`docs/PORTING_ROADMAP.md` WP9 ("a design review document for the shader backend
derived from it"). Evidence base: `docs/SHADER_MAPPING.md` for the three
classes and the `phx_mb_decision_t` boundary, `docs/DATA_STRUCTURES.md` for
state ownership, and the headers in `src/` for concrete array sizes. Nothing
here adds a function that is not in the inventory.

## 1. The class contract, restated as implementation rules

| Class | Rule derived from `SHADER_MAPPING.md` | Backend consequence |
|---|---|---|
| **A** (104 function definitions + 11 tables) | "one invocation per work item, no cross-item state … inputs passed explicitly, outputs written explicitly, no hidden state" (`:9–25`) | Pure compute shaders. All inputs are UBO/SSBO reads; all outputs are explicit SSBO writes. No atomics needed, no barriers between work items. |
| **B** (10 functions) | "ordered but local … need neighbour information within a frame/MB but not across frames … workgroup with shared memory or a two-pass reduce" (`:27–38`) | One workgroup per MB (or per MB row), populated from the per-frame MB-state SSBO. Wavefront ordering with a workgroup barrier between diagonals. State dies at frame end. |
| **C** (94 functions) | "cannot be naively parallelized because they mutate a single evolving state … dedicated serial stage" (`:40–56`) | One logical thread owns the coder cursor, CABAC `state[]`, lookahead `last_nonb` and the NAL byte stream. Dispatch is a single invocation per slice/frame, never per MB. |

Host lifecycle functions (allocation/free, preset/profile application,
SPS/PPS init) are C because they have no work item; their *data* is read by A
stages. This is a classification of the **function**, not of the buffer it
produces — e.g. `phx_frame_init_lowres` is A while `phx_frame_new` is C.

## 2. Pipeline

```
[C serial]  param_default / apply_preset / apply_profile / finalize   (4 × C)
[C serial]  sps_init / pps_init                                       (2 × C)
[A]         analyse_init_costs, init_costs, mb_analyse_init_qp,
            mb_analyse_load_costs, bs_size_*                          (6 × A)
[A]         frame_init_lowres: core filter + 4-plane border expansion (4 × A)
[A]         align_stride / align_plane_size (folded into host setup)   (2 × A)
[B]         predictor dispatch tables (5 × *_init, filled once)        (5 × B)
--- frame loop ---------------------------------------------------------
[C]         lookahead / slicetype (NOT YET PORTED — WP3)
[B]         cache_load_neighbours -> cache_load  (per-MB wavefront)
[A]         intra predictors (49), DCT/IDCT/Hadamard (21), quant (19)  (89 × A)
[B]         quant flat-CQM table init                                  (1 × B)
[A/B]       intra/inter analysis, transform RD         (NOT YET PORTED — WP4–6)
[B]         backup_intra -> cache_save           (per-MB wavefront)
[boundary]  phx_mb_decision_t[] written here; read only by the stage below
[C]         CABAC syntax (30), CAVLC tokens (15), slice header (2)      (47 × C)
[C]         RBSP/NAL assembly + headers                                 (C)
[C]         encoder envelope: open/headers/encode/close                 (10 × C)
------------------------------------------------------------------------
```

This mirrors `SHADER_MAPPING.md:58–65`, with one addition: the B stages and
the A mode-decision stages both sit between the lowres pass and the entropy
pass, and both use the same per-frame MB-state buffer. The predictor and DCT
stages were added by parallel workers while this design was written; the
encoder envelope is the serial host driver that wires UBO 0 and SSBO 0–2.

## 3. Buffers and SSBO layout

All sizes below come from the existing Phoenix headers; nothing is
speculative. `n = mb_width * mb_height` (`phx_mbcache.h:42`).

### 3.1 UBO 0 — parameters (`phx_param_t`, `phx_param.h:24–74`)
Read-only for the whole encode. Contains `i_width/i_height`, `i_frame_reference`,
`i_keyint_max/min`, `i_bframe`, `i_scenecut_threshold`, `i_bframe_adaptive`,
`b_cabac`, `b_transform_8x8`, `i_me_method`, `i_me_range`, `i_subpel_refine`,
`i_trellis`, `b_psy`, `f_psy_rd`, `i_chroma_qp_index_offset`,
`analyse_intra/inter`, `rc_method`, `i_lookahead`, `b_mb_tree`. Written only by
the four C `param_*` functions, before any dispatch.

### 3.2 UBO 1 — immutable tables (`src/phx_tables.h:18–28`)
`uint16 phx_lambda_tab[82]`, `int phx_lambda2_tab[82]`,
`int phx_trellis_lambda2_tab[2][82]`, `uint16 phx_chroma_lambda2_offset_tab[37]`,
`uint8 phx_ue_size_tab[256]`, `uint8 phx_chroma_qp_base[76]`,
`uint8 phx_cabac_range_lps[64][4]`, `uint8 phx_cabac_transition[128][2]`,
`uint8 phx_cabac_renorm_shift[64]`,
`int8 phx_cabac_context_init_I[1024][2]`,
`int8 phx_cabac_context_init_PB[3][1024][2]`, plus `phx_scan8[51]`
(`phx_mbcache.h:31`) and `phx_left_indices[4]` (`phx_mbcache.h:38`). All Class A
data; a single bind survives every frame and every seek.

### 3.3 SSBO 0 — cost tables (`phx_cost_tables_t`, `phx_cost.h:23–28`)
`cost_mv[70]`, `cost_mv_fpel[70][4]`, `cost_ref[70][3][33]`,
`cost_i4x4_mode[70][17]`. The `cost_mv*` fields are **pointers centred at
`±2*4*mv_range`** in the CPU port (`phx_cost.c:32`); for the shader they must be
flattened to fixed-size arrays with an explicit base offset, or the pointer
arithmetic becomes non-representable. The per-MB analysis struct
(`phx_mb_analysis_t`, `phx_cost.h:31–41`) carries the lambda values plus
`p_cost_mv`, `p_cost_ref[2]`; in the shader it is a few UBO scalars/indices, not
a pointer.

### 3.4 SSBO 1 — frame planes (`phx_frame_t`, `phx_frame.h:24–38`)
One luma plane `plane` with stride `i_stride`, and four lowres planes
`lowres[0..3]` (Orig/H/V/HV) each with stride `i_stride_lowres` and
`i_lines_lowres + 2*PHX_PADV` rows (`PHX_PADV = 32`). The lowres planes are the
read-only input to the lookahead A/B work (`DATA_STRUCTURES.md:46–59`).
Frame-local lookahead caches: `i_cost_est[18][18]`,
`i_row_satds[18][18]` (host pointers to `mb_rows` ints),
`lowres_mvs[2][17][2]` — all initialised by `phx_frame_init_lowres`
(`phx_frame.c:114–122`). Store these as flat arrays in the same SSBO; a shader
must index them, not dereference pointers.

### 3.5 SSBO 2 — per-frame MB state (`phx_mb_state_t`, `phx_mbcache.h:41–58`)
| field | shader layout | writer | reader |
|---|---|---|---|
| `type` | `uint8[n]` | `phx_macroblock_cache_save` | `cache_load_neighbours` |
| `intra4x4_pred_mode` | `int8[n][8]` | `cache_save` | `cache_load` |
| `non_zero_count` | `uint8[n][48]` | `cache_save` | `cache_load` |
| `cbp` | `int16[n]` | `cache_save` | `cache_load` |
| `mb_transform_size` | `uint8[n]` | `cache_save` | `cache_load` |
| `ref[2]` | `int8[2][n*4]` | `cache_save` | `cache_load` |
| `mv[2]` | `int16[2][n*16][2]` | `cache_save` | `cache_load` |
| `mvd[2]` | `int16[2][n][8][2]` | `cache_save` (CABAC only) | `cache_load` |
| `slice_table`, `field` | `uint8[n]` each | `cache_save` / MBAFF path | `cache_load_neighbours` |
| `intra_border_backup` | `uint8[3][3][16]` | `phx_macroblock_backup_intra` | next row's intra prediction |

This is the explicit, SSBO-able replacement for x264's `h->mb.*`
(`DATA_STRUCTURES.md:41–42`). It lives for exactly one frame.

### 3.6 Workgroup shared memory — per-MB cache (`phx_mb_cache_t`, `phx_mbcache.h:61–79`)
`intra4x4_pred_mode[40]`, `non_zero_count[120]`, `ref[2][40]`,
`mv[2][40][2]`, `mvd[2][40][2]`, plus the neighbour indices
(`top`, `left[2]`, `topleft`, `topright`, `i_neighbour`, `i_neighbour_intra`,
`i_neighbour_skip`, `i_cbp_top/left`, `i_neighbour_transform_size`). The 51-entry
`phx_scan8` map (`phx_mbcache.h:31`, from `common/base.h:180`) is the block→slot
addressing function; `phx_left_indices[3]` is the progressive neighbour-table
entry (`phx_mbcache.c:96`). Copy verbatim — `DATA_STRUCTURES.md:77–81` records
that the earlier off-by-one bugs E83–E95 came from this layout.

### 3.7 SSBO 3 — per-MB decisions: `phx_mb_decision_t[n]`
Mandated by `SHADER_MAPPING.md:67–70` and `PORTING_ROADMAP.md:98`. It does **not
exist in `src/` yet**; this is its required shape. The four fields named in
`SHADER_MAPPING.md:69` are the mandatory core; the others are the fields the
already-ported caches need in order to consume a decision, taken from
`phx_mb_cache_t` / `phx_mb_state_t`.

| field | type | source of the value | why C needs it |
|---|---|---|---|
| `i_type` | `uint8` | MB type constants `phx_mbcache.h:22–29` | CABAC/CAVLC `mb_type` syntax |
| `i_qp` | `uint8` | `phx_mb_analysis_t.i_qp` | `cabac_qp_delta` |
| `mv` | `int16[2][16][2]` | `phx_mb_cache_t.mv[2][40][2]` indexed by `phx_scan8[0..15]` | motion-vector-difference bins |
| `ref` | `int8[2][4]` | `phx_mb_cache_t.ref[2][40]` indexed by `phx_scan8[0..3]` | `cabac_ref_p/b` |
| `mvd` | `int16[2][8][2]` | `phx_mb_cache_t.mvd[2][40][2]` | CABAC MVD bins; also saved to `phx_mb_state_t.mvd` |
| `i_cbp_luma`, `i_cbp_chroma` | `uint8`, `uint8` | `phx_mb_cache_t.i_cbp_*` | `cabac_cbp_luma/chroma` (`encoder/cabac.c:118/129`) |
| `non_zero_count` | `uint8[48]` | `phx_mb_cache_t.non_zero_count[120]` via `nz_idx` table (`phx_mbcache.c:481–484`) | `cbf` context derivation, coefficient count |
| `intra4x4_pred_mode` | `int8[8]` | `phx_mb_cache_t.intra4x4_pred_mode[40]` | `cabac_intra4x4_pred_mode` (`encoder/cabac.c:83`) |
| `b_transform_8x8` | `uint8` | `phx_mb_cache_t.b_transform_8x8` | `cabac_transform_size` (`encoder/cabac.c:235`) |
| `coeff` | `int16[48]` | quantised coefficients (produced by WP6 quant/trellis) | residual bins / CAVLC tokens |

The array is written **once** by the B/A stages and is read-only afterwards;
the serial stage must never write back into it. That one-way edge is the whole
point of the boundary: it lets the entropy thread consume decisions in scan
order while the A/B stages have already produced them per MB.

### 3.8 SSBO 4 — serial entropy state (`phx_cabac_t`, `phx_cabac.h:20–26`)
`i_low`, `i_range`, `i_queue`, `i_bytes_outstanding`, `state[1024]` (460 active
contexts for 4:2:0, `PHX_CABAC_CTX_COUNT`), and `p_start/p/p_end` into the RBSP
byte buffer. Exactly one owner thread. The output RBSP and NAL descriptors
(`phx_nal_t`, `phx_bitstream.h:26–32`) live in a separate byte SSBO because
`phx_nal_encode` rewrites `p_payload`/`i_payload` in place
(`phx_bitstream.c:138–139`).

### 3.9 Working memory for the ported pixel stages
- Predictor dispatch tables from `src/phx_predict.h`: `phx_predict_t pf[7]`
  (16x16, 8x8c, 8x16c), `phx_predict_t pf[12]` (4x4), and
  `phx_predict8x8_t pf[12]` plus a filter-function pointer (8x8), filled once
  by the five `*_init` functions. In a shader these become enum→mode-index
  tables in UBO 1 rather than function pointers.
- Transform scratch: `phx_dctcoef` blocks of 16 (4x4 / DC), 64 (8x8),
  16×16 (16x16 4x4-DCT set), 4×64 (16x16 8x8-DCT set), and the 8-entry chroma
  DC array of `phx_dct2x4dc`. These are per-invocation registers/shared memory,
  never persistent SSBO state.
- `edge[36]` for `phx_predict_8x8_filter` and its consumers is invocation-local
  (`phx_predict.c:541`, `:536–540`), produced before the 8x8 prediction modes.

## 4. Class A — concrete stages

1. **Configuration stage** (host or one tiny compute dispatch):
   `phx_param_default`, `phx_param_apply_preset`, `phx_param_finalize`,
   `phx_param_apply_profile` → UBO 0. No parallelism, but no ordering bug is
   possible either.
2. **Constant/setup stage:** the 11 tables (UBO 1) plus
   `phx_analyse_init_costs`/`phx_init_costs` → SSBO 0, and
   `phx_mb_analyse_init_qp`/`phx_mb_analyse_load_costs` → per-MB UBO scalars.
   `phx_bs_size_ue`/`phx_bs_size_se`/`phx_bs_size_te` and `phx_clz` are pure
   in-stage helpers. `phx_analyse_free_costs` is the host teardown of SSBO 0.
3. **Lowres stage:** one invocation per 2×2 source block, exactly as
   `SHADER_MAPPING.md:19` prescribes for `common/mc.c:458`.
   `phx_frame_init_lowres_core` writes the four half planes; then
   `phx_plane_expand_border` / `phx_frame_expand_border_lowres` pad them.
   `phx_frame_init_lowres` additionally clears `i_cost_est`, `i_row_satds` and
   `lowres_mvs` — explicit writes, no hidden state. `phx_align_stride` /
   `phx_align_plane_size` determine the strides used here.
4. **Ported A pixel stages:** the 49 predictor functions and 21 transform
   functions (`src/phx_predict.c`, `src/phx_dct.c`) are already Class A.
   Predictors read neighbours through image loads into `dst` / `edge[36]`
   (`SHADER_MAPPING.md:16`); DCT/IDCT/Hadamard run a butterfly per block
   (`SHADER_MAPPING.md:14`). All 70 are seek-safe from their argument block.
   Note `phx_add8x8_idct8`/`phx_add16x16_idct8` mutate the coefficient array in
   place as x264 does — copy coefficients into registers before writing `dst`.
   The 5 `*_init` predictor functions fill the dispatch tables used by the B
   stages (they are B by their own annotations).
5. **Ported quant stage:** `src/phx_quant.c` supplies 19 A functions
   (deadzone quant, dequant, chroma-DC optimizers, `denoise_dct`) plus one B
   function (`phx_quant_tables_init`, flat-CQM table construction matching
   `common/set.c:73–205`). They are per-coefficient/per-block and seek-safe;
   `phx_shift`/`phx_div` are pure integer helpers (`common/set.c:28–29`).
6. **Future A work (WP3–WP6):** `slicetype_mb_cost`/`macroblock_tree`
   propagation and the still-missing per-MB analysis functions. Each is one
   invocation per block/MB over UBO 0/1 and SSBO 1, writing only its own slot
   in SSBO 3 or SSBO 2.

## 5. Class B — workgroup stages and neighbour state

1. Workgroup = one MB (or one MB row sharing a tile). Populate the shared
   `phx_mb_cache_t` from SSBO 2 using `phx_mb_cache_load_neighbours` followed
   by `phx_macroblock_cache_load`, both from `common/macroblock.c:673/855`.
2. Ordering is the doc's "left/top MBs must be complete"
   (`SHADER_MAPPING.md:34`). Use raster-order wavefronts: MB `(x,y)` may enter
   B only after `(x-1,y)` and `(x,y-1)` have completed `cache_save`. Insert a
   `barrier()` between diagonals; no global sync across the frame.
3. After mode decisions, `phx_macroblock_backup_intra` copies the reconstructed
   bottom rows into `intra_border_backup` (consumed by the next row), then
   `phx_macroblock_cache_save` publishes `type`, nnz, cbp, ref/mv/mvd and the
   8x8 transform flag back into SSBO 2. The caller must have set
   `s->type[xy]` before save (`phx_mbcache.c:452–454`); the shader equivalent is
   an explicit write to SSBO 2 from the decision stage.
4. MBAFF paths (`b_mbaff`, field pairs, MAP_MVS, `b_allow_skip`,
   `i_neighbour_skip`) are already in the CPU port (`phx_mbcache.c:105–129,
   179–214, 360–408`) and must be preserved — they are neighbour state, not
   something to approximate with a simpler shader rule.

## 6. Class C — strictly serial stages

Two independent serial owners, never merged (the entropy owner now has three
ported modules: `cabac`, `cabac_syntax`, `cavlc`, plus `slice` for the slice
header):

- **Lookahead / slice-type (frame module, WP3, not yet ported).**
  `x264_slicetype_analyse` (`slicetype.c:1473`) walks `lookahead->last_nonb`
  in order; `scenecut` (`:1430`) accumulates per-frame flags;
  `x264_slicetype_decide` (`:1745`) assigns types and frame_num/POC;
  `x264_lookahead_put_frame`/`_get_frames` (`lookahead.c:192/223`) queue and
  release frames. `SHADER_MAPPING.md:49–50` is explicit that these are serial.
  Their state is the lookahead window only — no CABAC state.
- **Entropy / NAL (bitstream, cabac, cabac_syntax, cavlc, slice, headers
  modules).** `phx_cabac_context_init`
  installs `state[1024]`; `phx_cabac_encode_init_core`/`phx_cabac_encode_init`
  reset the coder; `phx_cabac_encode_decision`, `_bypass`, `_ue_bypass`,
  `_terminal`, `_flush` (with static `phx_cabac_putbyte` / `_encode_renorm` and
  pure `phx_clz`) consume SSBO 3 in scan order. `phx_bs_*` writers append RBSP
  bytes; `phx_nal_escape` (2-byte lookbehind) and `phx_nal_encode` emit NALs;
  `phx_sps_write`/`phx_pps_write`/`phx_encoder_headers` emit parameter-set NALs.
  The 30 `phx_cabac_mb_*` / `phx_cabac_block_residual*` syntax writers and
  the 15 `phx_cavlc_*` token writers go through the same coder/cursor — no
  second stream is opened. `phx_slice_header_write` emits slice syntax on the
  same RBSP. `SHADER_MAPPING.md:47,52` place exactly these in C.
  `phx_cabac_encode_flush`
  uses `frame & 31` (`phx_cabac.c:146`), so the frame number must be supplied to
  the serial stage — a shader cannot infer it from the decision array.

## 7. Seek-safety rules

| Stage | Safe entry point | Required reset/recompute |
|---|---|---|
| Params, tables, cost tables, quant flat-CQM tables | any time | re-run the C config functions; the table builders are deterministic and table data is immutable |
| Frame / lowres | any frame boundary | `phx_frame_new` + `phx_frame_init_lowres` clears all three lookahead caches (`phx_frame.c:114–122`) |
| B neighbour cache | frame or slice start | fresh zeroed `phx_mb_state_t` (`phx_mb_state_new`); `threadslice_start`/`first_mb` bound the wavefront |
| Lookahead (C) | lookahead-window boundary | re-prime with `i_lookahead` frames (default 40, 0 for ultrafast — `phx_param.c:43/75`); restore `last_nonb`/`i_last_keyframe` |
| CABAC (C) | slice start | `context_init(slice_type, qp, model)` + `encode_init_core` + `encode_init`; supply `frame` for flush |
| CABAC syntax / CAVLC / slice header (C) | MB in scan order from slice start | consume `phx_mb_decision_t[]` in scan order; CAVLC token tables (`common/vlc.c:33/35/74`) built once; never resume mid-MB |
| RBSP / NAL (C) | NAL start | `phx_bs_init` zeros the buffer; `nal_escape` needs the two preceding bytes, so never begin mid-RBSP |
| `phx_mb_decision_t[]` | same frame as the entropy stage | fully regenerated by A/B before the serial stage reads it; never cached across frames |

The rule behind the table: **A and B stages are seek-safe because their inputs
are explicit and frame-local; C stages are seek-safe only at their documented
reset boundary.** That is precisely why `DATA_STRUCTURES.md:4–6` forbids the
old `vk_cur_*` globals — a global hidden state would make the reset boundary
unrepresentable.

## 8. Function → stage routing (all 208 ported function definitions — working-tree snapshot)

| Function (module) | Class | Stage / buffer |
|---|---|---|
| `phx_param_default` (param) | C | config → UBO 0 |
| `phx_param_apply_preset` (param) | C | config → UBO 0 |
| `phx_param_finalize` (param) | C | config → UBO 0 |
| `phx_param_apply_profile` (param) | C | config → UBO 0 |
| `phx_lambda_tab` + 10 tables (tables) | A | UBO 1, read-only |
| `phx_bs_size_te` (cost) | A | setup, pure query |
| `phx_init_costs` (cost) | A | setup → SSBO 0 |
| `phx_analyse_init_costs` (cost) | A | setup → SSBO 0 |
| `phx_analyse_free_costs` (cost) | C | host teardown of SSBO 0 |
| `phx_mb_analyse_init_qp` (cost) | A | per-MB setup, pure |
| `phx_mb_analyse_load_costs` (cost) | A | per-MB index binding |
| `phx_align_stride` (frame) | A | host/UBO stride setup |
| `phx_align_plane_size` (frame) | A | host/UBO stride setup |
| `phx_plane_expand_border` (frame) | A | lowres border pass → SSBO 1 |
| `phx_frame_expand_border_lowres` (frame) | A | lowres border pass → SSBO 1 |
| `phx_frame_init_lowres_core` (frame) | A | lowres 2×2 pass → SSBO 1 |
| `phx_frame_init_lowres` (frame) | A | lowres orchestration + cache clears |
| `phx_frame_new` (frame) | C | host allocation of SSBO 1 |
| `phx_frame_delete` (frame) | C | host teardown |
| `phx_mb_state_new` (mbcache) | C | host allocation of SSBO 2 |
| `phx_mb_state_delete` (mbcache) | C | host teardown |
| `phx_macroblock_cache_load_neighbours` (mbcache) | B | workgroup gather, shared cache |
| `phx_macroblock_cache_load` (mbcache) | B | workgroup gather, shared cache |
| `phx_macroblock_backup_intra` (mbcache) | B | workgroup write-back, next row |
| `phx_macroblock_cache_save` (mbcache) | B | workgroup write-back → SSBO 2 |
| `phx_bs_init` (bitstream) | C | serial RBSP reset |
| `phx_bs_pos` (bitstream) | C | serial cursor query |
| `phx_bs_write` (bitstream) | C | serial RBSP write |
| `phx_bs_write1` (bitstream) | C | serial RBSP write |
| `phx_bs_align_0` (bitstream) | C | serial alignment |
| `phx_bs_align_10` (bitstream) | C | serial alignment |
| `phx_bs_size_ue` (bitstream) | A | pure size query |
| `phx_bs_size_se` (bitstream) | A | pure size query |
| `phx_bs_write_ue_big` (bitstream) | C | serial RBSP write |
| `phx_bs_write_ue` (bitstream) | C | serial RBSP write |
| `phx_bs_write_se` (bitstream) | C | serial RBSP write |
| `phx_bs_write_te` (bitstream) | C | serial RBSP write |
| `phx_bs_rbsp_trailing` (bitstream) | C | serial RBSP termination |
| `phx_nal_escape` (bitstream) | C | serial NAL emulation prevention |
| `phx_nal_encode` (bitstream) | C | serial NAL assembly |
| `phx_cabac_context_init` (cabac) | C | entropy stage → `state[]` |
| `phx_cabac_encode_init_core` (cabac) | C | entropy stage reset |
| `phx_cabac_encode_init` (cabac) | C | entropy stage reset + buffer bind |
| `phx_cabac_putbyte` (cabac) | C | serial byte flush |
| `phx_cabac_encode_renorm` (cabac) | C | serial renormalisation |
| `phx_cabac_encode_decision` (cabac) | C | serial context bin |
| `phx_cabac_encode_bypass` (cabac) | C | serial bypass bin |
| `phx_clz` (cabac) | A | pure bit helper (inlined by C stage) |
| `phx_cabac_encode_ue_bypass` (cabac) | C | serial ue(v) bypass bins |
| `phx_cabac_encode_terminal` (cabac) | C | serial terminal bin |
| `phx_cabac_encode_flush` (cabac) | C | serial slice flush (frame-dependent) |
| `phx_sps_init` (headers) | C | host header prep |
| `phx_pps_init` (headers) | C | host header prep |
| `phx_sps_write` (headers) | C | serial header RBSP |
| `phx_pps_write` (headers) | C | serial header RBSP |
| `phx_encoder_headers` (headers) | C | serial SPS+PPS NAL assembly |
| `phx_clip_pixel` (dct) | A | pure clamp helper, pixel stage |
| `phx_pixel_sub_wxh` (dct) | A | residual source for any DCT block |
| `phx_dct4x4dc`, `phx_idct4x4dc`, `phx_dct2x4dc` (dct) | A | DC Hadamard stages, 16-coeff / 8-coeff blocks |
| `phx_sub4x4_dct`, `phx_sub8x8_dct`, `phx_sub16x16_dct` (dct) | A | residual DCT, per 4x4/8x8/16x16 block |
| `phx_sub4x4_dct_dc`, `phx_sub8x8_dct_dc`, `phx_sub8x16_dct_dc` (dct) | A | DC-only residual DCT stages |
| `phx_add4x4_idct`, `phx_add8x8_idct`, `phx_add16x16_idct` (dct) | A | inverse 4x4-DCT reconstruction |
| `phx_sub8x8_dct8`, `phx_sub16x16_dct8` (dct) | A | 8x8 DCT butterfly, per block |
| `phx_add8x8_idct8`, `phx_add16x16_idct8` (dct) | A | 8x8 IDCT reconstruction (in-place coeff mutation) |
| `phx_add4x4_idct_dc`, `phx_add8x8_idct_dc`, `phx_add16x16_idct_dc` (dct) | A | DC-only reconstruction |
| `phx_clip_pixel`, `phx_store4` (predict) | A | pure per-pixel/per-4-byte helpers |
| `phx_predict_16x16_dc`, `_dc_left`, `_dc_top`, `_dc_128`, `_h`, `_v`, `_p` (predict) | A | 16x16 intra predictors, one invocation per block |
| `phx_predict_8x8c_dc`, `_dc_left`, `_dc_top`, `_dc_128`, `_h`, `_v`, `_p` (predict) | A | 8x8 chroma intra predictors |
| `phx_predict_8x16c_dc`, `_dc_left`, `_dc_top`, `_dc_128`, `_h`, `_v`, `_p` (predict) | A | 8x16 chroma intra predictors |
| `phx_predict_4x4_dc`, `_dc_left`, `_dc_top`, `_dc_128`, `_h`, `_v`, `_ddl`, `_ddr`, `_vr`, `_hd`, `_vl`, `_hu` (predict) | A | 4x4 intra predictors |
| `phx_predict_8x8_filter`, `phx_predict_8x8_fill` (predict) | A | 8x8 edge filtering + DC fill helper |
| `phx_predict_8x8_dc`, `_dc_left`, `_dc_top`, `_dc_128`, `_h`, `_v`, `_ddl`, `_ddr`, `_vr`, `_hd`, `_vl`, `_hu` (predict) | A | 8x8 intra predictors from `edge[36]` |
| `phx_predict_16x16_init`, `phx_predict_8x8c_init`, `phx_predict_8x16c_init`, `phx_predict_4x4_init`, `phx_predict_8x8_init` (predict) | B | one-time dispatch tables consumed by B stages |
| `phx_encoder_validate`, `phx_encoder_release`, `phx_encoder_open`, `phx_open` (encoder) | C | serial host open/validate/teardown helpers |
| `phx_headers`, `phx_encoder_header_bytes` (encoder) | C | serial header emission (SPS+PPS, no SEI) |
| `phx_encode`, `phx_encoder_encode`, `phx_encoder_close`, `phx_close` (encoder) | C | serial encode envelope and aliases |
| `phx_coeff_last4/8/15/16/64` (cabac_syntax) | C | pure last-nonzero scans feeding residual bins |
| `phx_cabac_mb_init`, `phx_predict_intra4x4_mode`, `phx_predict_mv`, `phx_cabac_mvd_sum`, `phx_cache_mvd`, `phx_cabac_cbf_ctxidxinc` (cabac_syntax) | C | serial state setup / prediction helpers |
| `phx_cabac_mb_type_intra`, `phx_cabac_intra4x4_pred_mode`, `phx_cabac_intra_chroma_pred_mode`, `phx_cabac_subpartition_p/b`, `phx_cabac_transform_size`, `phx_cabac_cbp_luma/chroma`, `phx_cabac_qp_delta`, `phx_cabac_mb_skip`, `phx_cabac_mvd_cpn`, `phx_cabac_mvd`, `phx_cabac_8x8_mvd`, `phx_cabac_ref`, `phx_cabac_mb_header_i/p/b`, `phx_cabac_block_residual`/`_cbf` (cabac_syntax) | C | scan-order MB syntax bins |
| `phx_clz`, `phx_cavlc_coeff_last`, `phx_cavlc_coeff_level_run` (cavlc) | C | integer/coefficient helpers in serial writer |
| `phx_cavlc_init`, `phx_cavlc_init_level_token`, `phx_cavlc_init_run_before` (cavlc) | C | one-time VLC table generation |
| `phx_cavlc_write_vlc`, `phx_cavlc_write_vlc_large`, `phx_cavlc_block_residual_escape`, `phx_cavlc_block_residual_internal`, `phx_cavlc_block_residual` (cavlc) | C | serial CAVLC token emission |
| `phx_cavlc_coeff0`, `phx_cavlc_coeff_token_at`, `phx_cavlc_total_zeros_at`, `phx_cavlc_total_zeros_dc_at` (cavlc) | C | VLC table lookups for parity/writer |
| `phx_shift`, `phx_div` (quant) | A | pure integer helpers |
| `phx_quant_tables_init` (quant) | B | one-time flat-CQM table construction |
| `phx_quant_8x8`, `phx_quant_4x4`, `phx_quant_4x4x4`, `phx_quant_4x4_dc`, `phx_quant_2x2_dc` (quant) | A | deadzone quant, per block/coefficient |
| `phx_dequant_4x4`, `phx_dequant_8x8`, `phx_dequant_4x4_dc` (quant) | A | dequant for reconstruction |
| `phx_idct_dequant_2x4_dc`, `phx_idct_dequant_2x4_dconly`, `phx_optimize_chroma_idct_dequant_2x4`, `phx_optimize_chroma_idct_dequant_2x2`, `phx_optimize_chroma_round`, `phx_optimize_chroma_dc_internal`, `phx_optimize_chroma_2x2_dc`, `phx_optimize_chroma_2x4_dc`, `phx_denoise_dct` (quant) | A | chroma-DC optimizers and DCT denoise |
| `phx_slice_header_init`, `phx_slice_header_write` (slice) | C | serial slice-header setup and RBSP emission |
| `phx_slice_group_change_bits` (slice) | A | pure SPS-derived bit count |

### 8.1 Full function-name index by class (name coverage check)

Every function definition in `src/*.c` appears exactly once below; compound
rows in §8 are expanded here so generated indices can verify coverage.

- **A (104 definitions):** `phx_bs_size_ue`, `phx_bs_size_se` (bitstream);
  `phx_bs_size_te`, `phx_init_costs`, `phx_analyse_init_costs`,
  `phx_mb_analyse_init_qp`, `phx_mb_analyse_load_costs` (cost);
  `phx_align_stride`, `phx_align_plane_size`, `phx_plane_expand_border`,
  `phx_frame_expand_border_lowres`, `phx_frame_init_lowres_core`,
  `phx_frame_init_lowres` (frame); `phx_clz` (cabac);
  `phx_clip_pixel` (dct), `phx_pixel_sub_wxh`, `phx_dct4x4dc`,
  `phx_idct4x4dc`, `phx_dct2x4dc`, `phx_sub4x4_dct`, `phx_sub8x8_dct`,
  `phx_sub16x16_dct`, `phx_sub4x4_dct_dc`, `phx_sub8x8_dct_dc`,
  `phx_sub8x16_dct_dc`, `phx_add4x4_idct`, `phx_add8x8_idct`,
  `phx_add16x16_idct`, `phx_sub8x8_dct8`, `phx_sub16x16_dct8`,
  `phx_add8x8_idct8`, `phx_add16x16_idct8`, `phx_add4x4_idct_dc`,
  `phx_add8x8_idct_dc`, `phx_add16x16_idct_dc` (dct);
  `phx_clip_pixel` (predict), `phx_store4`, `phx_predict_16x16_dc`,
  `phx_predict_16x16_dc_left`, `phx_predict_16x16_dc_top`,
  `phx_predict_16x16_dc_128`, `phx_predict_16x16_h`, `phx_predict_16x16_v`,
  `phx_predict_16x16_p`, `phx_predict_8x8c_dc_128`,
  `phx_predict_8x8c_dc_left`, `phx_predict_8x8c_dc_top`,
  `phx_predict_8x8c_dc`, `phx_predict_8x8c_h`, `phx_predict_8x8c_v`,
  `phx_predict_8x8c_p`, `phx_predict_8x16c_dc_128`,
  `phx_predict_8x16c_dc_left`, `phx_predict_8x16c_dc_top`,
  `phx_predict_8x16c_dc`, `phx_predict_8x16c_h`, `phx_predict_8x16c_v`,
  `phx_predict_8x16c_p`, `phx_predict_4x4_dc_128`,
  `phx_predict_4x4_dc_left`, `phx_predict_4x4_dc_top`, `phx_predict_4x4_dc`,
  `phx_predict_4x4_h`, `phx_predict_4x4_v`, `phx_predict_4x4_ddl`,
  `phx_predict_4x4_ddr`, `phx_predict_4x4_vr`, `phx_predict_4x4_hd`,
  `phx_predict_4x4_vl`, `phx_predict_4x4_hu`, `phx_predict_8x8_filter`,
  `phx_predict_8x8_fill`, `phx_predict_8x8_dc_128`,
  `phx_predict_8x8_dc_left`, `phx_predict_8x8_dc_top`, `phx_predict_8x8_dc`,
  `phx_predict_8x8_h`, `phx_predict_8x8_v`, `phx_predict_8x8_ddl`,
  `phx_predict_8x8_ddr`, `phx_predict_8x8_vr`, `phx_predict_8x8_hd`,
  `phx_predict_8x8_vl`, `phx_predict_8x8_hu` (predict);
  `phx_shift`, `phx_div`, `phx_quant_8x8`, `phx_quant_4x4`,
  `phx_quant_4x4x4`, `phx_quant_4x4_dc`, `phx_quant_2x2_dc`,
  `phx_dequant_4x4`, `phx_dequant_8x8`, `phx_dequant_4x4_dc`,
  `phx_idct_dequant_2x4_dc`, `phx_idct_dequant_2x4_dconly`,
  `phx_optimize_chroma_idct_dequant_2x4`,
  `phx_optimize_chroma_idct_dequant_2x2`, `phx_optimize_chroma_round`,
  `phx_optimize_chroma_dc_internal`, `phx_optimize_chroma_2x2_dc`,
  `phx_optimize_chroma_2x4_dc`, `phx_denoise_dct` (quant);
  `phx_slice_group_change_bits` (slice).
- **B (10 definitions):** `phx_macroblock_cache_load_neighbours`,
  `phx_macroblock_cache_load`, `phx_macroblock_backup_intra`,
  `phx_macroblock_cache_save` (mbcache); `phx_predict_16x16_init`,
  `phx_predict_8x8c_init`, `phx_predict_8x16c_init`, `phx_predict_4x4_init`,
  `phx_predict_8x8_init` (predict); `phx_quant_tables_init` (quant).
- **C (94 definitions):** `phx_param_default`, `phx_param_apply_preset`,
  `phx_param_finalize`, `phx_param_apply_profile` (param);
  `phx_analyse_free_costs` (cost); `phx_frame_new`, `phx_frame_delete` (frame);
  `phx_mb_state_new`, `phx_mb_state_delete` (mbcache);
  `phx_bs_init`, `phx_bs_pos`, `phx_bs_write`, `phx_bs_write1`,
  `phx_bs_align_0`, `phx_bs_align_10`, `phx_bs_write_ue_big`,
  `phx_bs_write_ue`, `phx_bs_write_se`, `phx_bs_write_te`,
  `phx_bs_rbsp_trailing`, `phx_nal_escape`, `phx_nal_encode` (bitstream);
  `phx_cabac_context_init`, `phx_cabac_encode_init_core`,
  `phx_cabac_encode_init`, `phx_cabac_putbyte`, `phx_cabac_encode_renorm`,
  `phx_cabac_encode_decision`, `phx_cabac_encode_bypass`,
  `phx_cabac_encode_ue_bypass`, `phx_cabac_encode_terminal`,
  `phx_cabac_encode_flush` (cabac);
  `phx_sps_init`, `phx_pps_init`, `phx_sps_write`, `phx_pps_write`,
  `phx_encoder_headers` (headers);
  `phx_encoder_validate`, `phx_encoder_release`, `phx_encoder_open`,
  `phx_open`, `phx_headers`, `phx_encoder_header_bytes`, `phx_encode`,
  `phx_encoder_encode`, `phx_encoder_close`, `phx_close` (encoder);
  `phx_coeff_last4`, `phx_coeff_last8`, `phx_coeff_last15`,
  `phx_coeff_last16`, `phx_coeff_last64`, `phx_cabac_mb_init`,
  `phx_predict_intra4x4_mode`, `phx_cabac_mb_type_intra`,
  `phx_cabac_intra4x4_pred_mode`, `phx_cabac_intra_chroma_pred_mode`,
  `phx_cabac_subpartition_p`, `phx_cabac_subpartition_b`,
  `phx_cabac_transform_size`, `phx_cabac_cbp_luma`, `phx_cabac_cbp_chroma`,
  `phx_cabac_qp_delta`, `phx_cabac_mb_skip`, `phx_predict_mv`,
  `phx_cabac_mvd_sum`, `phx_cache_mvd`, `phx_cabac_mvd_cpn`, `phx_cabac_mvd`,
  `phx_cabac_8x8_mvd`, `phx_cabac_ref`, `phx_cabac_mb_header_i`,
  `phx_cabac_mb_header_p`, `phx_cabac_mb_header_b`,
  `phx_cabac_cbf_ctxidxinc`, `phx_cabac_block_residual`,
  `phx_cabac_block_residual_cbf` (cabac_syntax);
  `phx_clz`, `phx_cavlc_coeff_last`, `phx_cavlc_coeff_level_run`,
  `phx_cavlc_init_level_token`, `phx_cavlc_init_run_before`,
  `phx_cavlc_init`, `phx_cavlc_write_vlc`, `phx_cavlc_write_vlc_large`,
  `phx_cavlc_block_residual_escape`, `phx_cavlc_block_residual_internal`,
  `phx_cavlc_block_residual`, `phx_cavlc_coeff0`,
  `phx_cavlc_coeff_token_at`, `phx_cavlc_total_zeros_at`,
  `phx_cavlc_total_zeros_dc_at` (cavlc);
  `phx_slice_header_init`, `phx_slice_header_write` (slice).

## 9. Consequences for the remaining work

- `docs/FUNCTION_PORTS.md` "NOT YET PORTED" is the order in which the above
  buffers get their writers. WP3 gives SSBO 1's lookahead caches a consumer;
  WP4–WP6 give SSBO 3 its producers; WP7 gives the serial stage its syntax
  writers; WP8 supplies the per-frame/per-MB QP. Parallel workers have already
  supplied the predictor/DCT/quant and CABAC/CAVLC/slice syntax writers, so
  this index is a working-tree snapshot — re-scan `src/*.c` before gating.
- `phx_mb_decision_t` must be introduced as a real type before the first
  WP4–WP6 decision function is written, so those functions write exactly one
  named boundary rather than growing new globals.
- The centred `cost_mv` pointers and the `i_row_satds` pointer arrays are the
  two CPU representations that need flattening before an SSBO can hold them;
  do this in the owning module, not in the shader.
- Keep the `SHADER: A|B|C` line on every new function, as required by
  `docs/SHADER_MAPPING.md:72–80` and `README.md` rule 2; regenerate
  `docs/FUNCTION_INVENTORY.md` after each port so this index stays complete.
