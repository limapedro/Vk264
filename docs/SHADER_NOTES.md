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
| **A** (185 function definitions + 11 tables) | "one invocation per work item, no cross-item state … inputs passed explicitly, outputs written explicitly, no hidden state" (`:9–25`) | Pure compute shaders. All inputs are UBO/SSBO reads; all outputs are explicit SSBO writes. No atomics needed, no barriers between work items. Now includes the full-pel ME candidate kernels (`phx_me.c`) and the lowres SAD/SATD/hpel kernels (`phx_lowres.c`). |
| **B** (109 functions) | "ordered but local … need neighbour information within a frame/MB but not across frames … workgroup with shared memory or a two-pass reduce" (`:27–38`) | One workgroup per MB (or per MB row), populated from the per-frame MB-state SSBO. Wavefront ordering with a workgroup barrier between diagonals. State dies at frame end. Now includes analysis, MB reconstruction, chroma, deblock, MC and the size-only RDO accumulator. |
| **C** (211 functions) | "cannot be naively parallelized because they mutate a single evolving state … dedicated serial stage" (`:40–56`) | One logical thread owns the coder cursor, CABAC `state[]`, lookahead `last_nonb`, rate-control state and the NAL byte stream. Dispatch is a single invocation per slice/frame, never per MB. |

Counts are the full working-tree scan recorded in `docs/FUNCTION_PORTS.md` (505 definitions across all 27 `src/*.c` files at the time of that scan; re-scan before using them as a gate).

Host lifecycle functions (allocation/free, preset/profile application,
SPS/PPS init) are C because they have no work item; their *data* is read by A
stages. This is a classification of the **function**, not of the buffer it
produces — e.g. `phx_frame_init_lowres` is A while `phx_frame_new` is C.

## 2. Pipeline

```
[C serial]  param_default / apply_preset / apply_profile / finalize   (4 × C)
[C serial]  sps_init / pps_init / sei_version_write                   (3 × C)
[C serial]  ratecontrol configure/start/end (WP8, ported)              (C)
[A]         analyse_init_costs, init_costs, mb_analyse_init_qp,
            mb_analyse_load_costs, bs_size_*                          (6 × A)
[A]         frame_init_lowres: core filter + 4-plane border expansion (5 × A)
[A]         align_stride / align_plane_size (folded into host setup)   (2 × A)
[B]         predictor dispatch tables (5 × *_init, filled once)        (5 × B)
--- frame loop ---------------------------------------------------------
[C]         lookahead queue: put_frame -> get_frames -> B/BREF decide   (13 × C)
[A]         lowres SATD/SAD kernels: lr_sad8, lr_satd_8x4/8,
            lr_get_ref, lr_bidir_avg, lr_copy_fenc, lr_eval_*          (~15 × A)
[A]         full-pel ME: phx_me16 + COST_MV[_X3/_X4_DIR], BITS_MVD      (9 × A)
[B]         frame/path cost accumulation (lr_mb_cost, lowres_frame_cost,
            lowres_path_cost)                                          (3 × C/B)
[B]         cache_load_neighbours -> cache_load  (per-MB wavefront)
[A]         intra predictors (49), DCT/IDCT/Hadamard (21), quant (19)  (89 × A)
[A]         MC sample kernels (phx_mc_pixel_avg/copy/luma/chroma)      (A)
[B]         quant flat-CQM table init, RDO size-only accumulator       (B)
[A/B/C]     analysis -> mbencode -> chroma decision producers          (B, A helper)
[B]         deblock strength + edge filters (8 edges / MB)             (B)
[B]         backup_intra -> cache_save           (per-MB wavefront)
[boundary]  phx_mb_decision_t[] written here; read only by the stage below
[C]         trellis QNS: phx_rdo_*_trellis + size-only bit-cost API     (C, B)
[C]         CABAC syntax (30), CAVLC tokens (15), CAVLC MB syntax (12),
            slice header (2)                                           (59 × C)
[C]         RBSP/NAL assembly + headers + version SEI                  (C)
[C]         encoder envelope: open/headers/encode/close + frame loop   (32 × C)
------------------------------------------------------------------------
```

This mirrors `SHADER_MAPPING.md:58–65`, with three additions since the first
draft: (1) the lookahead/slicetype owner is now ported and emits B/BREF, so it
is a real C stage rather than a placeholder; (2) the lowres SATD/hpel and
full-pel ME kernels are Class A producers feeding a Class C cost accumulator;
(3) the trellis QNS module exposes a **size-only** RDO API whose accumulator is
Class B (per-MB local `state[460]` + `f8_bits_encoded`) while the coefficient
state machine stays Class C. The encoder envelope is the serial host driver
that wires UBO 0–2 and SSBO 0–7.

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

### 3.10 SSBO 5 — lowres per-MB cost/MV mirrors (`phx_lowres.h:23–28`)
x264 keeps one MV and cost per 8x8 block (`reference/x264/common/frame.h:103–127`);
`phx_frame_t` exposes only `lowres_mvs[2][bframes+1][2]`. The port therefore owns
a per-frame mirror in `struct phx_lowres` (`phx_lowres.c:53–72`): `mvs[2][ndist]`,
`mv_costs[2][ndist]`, `lowres_costs[(bframes+2)^2]` and the aliased
`intra_cost[]`, plus `b_intra_calculated` (`frame.h:126`). These are linear
`mb_count`-indexed arrays allocated by `lr_get_frame()` (`common/frame.c:211–220,
285–288`) and cleared by `lr_frame_clear()`. In the shader they are a per-frame
SSBO: A-stage lowres kernels write one block's slot, the C-stage frame/path
cost scan reads them in frame order. `phx_lowres_frame_reset()` is the explicit
invalidation entry point (`mc.c:473–481`).

### 3.11 SSBO 6 — ME / lowres search context (`phx_me.h`, `phx_lowres.h`)
`phx_me16()` reads two luma planes plus `p_cost_mv` from SSBO 0 and writes one
`x264_me_t`-shaped result (`mv`, `i_sad`, `i_cost`, `i_ref`) into a per-MB slot.
`phx_lr_me_t` (`phx_lowres.c:43–62`) is the lowres analogue: hpel reference
planes, `bmx/bmy`, `cost`, `bits` and the `mvp[2]` predictor list. In a shader
both are invocation-local scratch for the A-stage search; only the winning
MV/cost is published to SSBO 5. `phx_mc_hpel_cache`
(`phx_mc.c:193–195`, invalidated by `phx_mc_hpel_invalidate()`) is host-owned
scratch, not shader state — hpel planes must be uploaded as SSBO 1 views.

### 3.12 SSBO 7 / UBO 2 — RDO size-only API and trellis state
`phx_cabac_size_t` (`phx_rdo.h:153–156`) is exactly the two fields x264's
`COPY_CABAC` copies (`encoder/rdo.c:71–73`): `state[0..459]` and
`f8_bits_encoded`. It is **not** a second arithmetic coder — no `i_low`,
`i_range`, queue or output bytes are touched. `phx_rdo_size_init` seeds it from
the live coder or rebuilds contexts; `phx_rdo_size_decision[_noup]`,
`_terminal`, `_bypass`, `_ue_bypass`, `_intra4x4_pred_mode` and
`_block_residual_cbf` advance it with the RDO_SKIP_BS==1 bit costs
(`common/cabac.h:99–124`, `encoder/rdo.c:31–69,315–347`). Allocate one
`phx_cabac_size_t` per MB workgroup (Class B), never one per bin. The trellis
node arrays (`phx_trellis_node_t`, `phx_rdo.c:586–834`) stay invocation-local
for the Class C `phx_rdo_quant_trellis_cabac/cavlc` state machine.
UBO 2 is `phx_rdo_config_t` (`phx_rdo.h:80–87`): chroma QP offset, psy-trellis
(FIX8), interlaced flag, block index and the two `fenc_dct` pointers; it is
written once by the host via `phx_rdo_set_config()` and is immutable during a
dispatch.

### 3.13 SSBO 8 — deblock strength and rate-control scalars
`phx_macroblock_deblock_strength()` (`common/deblock.c:277–304`) fills one
`bs[4][4]` edge array per 4x4 edge; the eight edges of an MB are independent
(Class B within one workgroup). `phx_ratecontrol_*` keeps only scalar per-frame
QP / QP-offset state (`phx_ratecontrol.h:22`), uploaded as UBO scalars; the AQ
pass writes one independent offset per MB. Neither introduces a cross-frame
hidden global.

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
6. **Full-pel ME stage (WP5, ported):** `phx_me16` and its helpers
   (`phx_pixel_sad_16x16`, `phx_bits_mvd`, `phx_cost_mv`,
   `phx_cost_mv_x4_dir`/`_x3_dir`, `phx_check_mv_range`,
   `phx_mv_cost_formula`, `phx_resolve_cost_mv`) implement x264
   `encoder/me.c:60–107,182–419`. One invocation per MB: read fenc/fref from
   SSBO 1 and `p_cost_mv` from SSBO 0, run DIA/HEX over full-pel candidates,
   write one MV/cost result to SSBO 6. No sub-pel, no global search state, no
   frame history — `SHADER: A` (`phx_me.h:33`).
7. **Lowres SATD/hpel stage (WP3, ported):** `lr_sad8`, `lr_satd_8x4`,
   `lr_satd8`, `lr_get_ref`, `lr_bidir_avg`, `lr_copy_fenc`, `lr_eval_full`,
   `lr_eval_spel_sad`/`lr_eval_spel_satd`, `lr_check_predictors`,
   `lr_fullpel_search`, `lr_refine_subpel`, `lr_motion_search`, `lr_try_bidir`
   (`common/pixel.c:49–71,265–332`, `common/mc.c:63–99,221–249`,
   `encoder/me.c:63–107,218–275,320–419,826–963`,
   `encoder/slicetype.c:579–709`). One work item per lowres 8x8 block:
   pure SAD/SATD reductions, hpel plane selection and MV evaluation; results
   go to SSBO 5/6. Cost accumulation (`lr_mb_cost`,
   `phx_lowres_frame_cost`, `phx_lowres_path_cost`) remains the serial Class C
   consumer (`phx_lowres.h:30–33`).
8. **Future A work:** `slicetype_mb_cost`/`macroblock_tree` propagation and the
   still-missing per-MB analysis functions. Each is one invocation per
   block/MB over UBO 0/1 and SSBO 1, writing only its own slot in SSBO 3 or
   SSBO 2.

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

- **Lookahead / slice-type (WP3, now ported).**
  `phx_lh_frame_cost`/`phx_lh_path_cost` (`slicetype.c:836–995/1288–1327`),
  `phx_lh_scenecut_internal`/`phx_lh_scenecut` (`:1384–1471`),
  `phx_lh_slicetype_analyse` (`:1473–1742`) and
  `phx_lh_slicetype_decide` (`:1745–1985`) walk `last_nonb`, accumulate
  scenecut flags and assign types plus frame_num/POC in order.
  `phx_lookahead_put_frame`/`phx_lookahead_get_frames_full`
  (`lookahead.c:192/223–254`) queue and release frames; the A-stage lowres
  kernels are called underneath but the queue and decision are serial.
  `phx_lookahead_get_frames_ex` preserves the BREF tag on the reference B
  frame. `SHADER_MAPPING.md:49–50` is explicit that these are serial.
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
- **CAVLC MB syntax (cavlc_syntax module, WP7, ported).**
  `phx_cavlc_mb_init`, `phx_cavlc_predict_mv`, `phx_cavlc_mvd`,
  `phx_cavlc_8x8_mvd`, `phx_cavlc_mb_header_i/p_l0/p_8x8/b_8x8/b_inter` and
  `phx_cavlc_mb_header` (`encoder/cavlc.c:241–477,487–595`) write MB headers
  and MVDs into the same ordered RBSP. `SHADER: C — all writers are strictly
  serial; no parallel stage may reorder the bit stream` (`phx_cavlc_syntax.c:4`).
  The borrowed `phx_mb_cache_t` is the neighbour boundary, not a second stream.
- **Trellis QNS and rate control (rdo / ratecontrol modules).**
  `phx_rdo_quant_trellis_cabac/cavlc` and the four `phx_quant_*_trellis`
  wrappers (`encoder/rdo.c:641–1184`) run the coefficient state machine in scan
  order; the RDO_SKIP_BS bit counters come from the Class **B** size-only
  accumulator in §3.12, never from a copied live coder. `phx_rdo_init` builds
  the flat-CQM tables once (B); the table accessors
  (`phx_rdo_quant4/8_mf`, `*_bias0`, `*_unquant*`) are immutable A lookups.
  `phx_ratecontrol_configure/start/end/mb_qp` (`encoder/ratecontrol.c`) are
  serial per-frame scalar passes; their outputs are UBO scalars consumed by
  the MB stages.

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
| ME / lowres kernels (A) | any frame with valid fenc/fref | pure per-block search; re-run `lr_frame_clear` / `phx_lowres_frame_reset` on the SSBO 5 mirrors before reuse |
| Lowres frame/path cost (C) | lookahead-window boundary | re-prime the lowres cost/MV mirrors; the B-discount and path prefix sum start from frame 0 of the window |
| RDO size-only accumulator (B) | one MB workgroup | `phx_rdo_size_init` reseeds `state[460]` + `f8_bits_encoded`; never carry it across MBs |
| Trellis QNS / rate control (C) | MB in scan order / frame boundary | set UBO 2 config before dispatch; `phx_ratecontrol_*` is invoked once per decided/finished frame |

The rule behind the table: **A and B stages are seek-safe because their inputs
are explicit and frame-local; C stages are seek-safe only at their documented
reset boundary.** That is precisely why `DATA_STRUCTURES.md:4–6` forbids the
old `vk_cur_*` globals — a global hidden state would make the reset boundary
unrepresentable.

## 8. Function → stage routing (full working-tree snapshot)

The authoritative per-function index, with exact x264 `file:line`, inputs,
outputs, mutated state, seek-safety and class for **every** definition in
`src/*.c`, is `docs/FUNCTION_PORTS.md` (505 definitions at this scan). The
table below is the compact stage map; new modules added since the first draft
are listed first so the A-stage ME/lowres kernels and the B/C RDO API are
explicit.

| Function (module) | Class | Stage / buffer |
|---|---|---|
| `phx_me16`, `phx_cost_mv`, `phx_cost_mv_x3_dir`/`_x4_dir`, `phx_bits_mvd`, `phx_check_mv_range`, `phx_pixel_sad_16x16` (me) | A | full-pel DIA/HEX search → SSBO 6 result slot |
| `lr_sad8`, `lr_satd_8x4`, `lr_satd8`, `lr_get_ref`, `lr_bidir_avg`, `lr_copy_fenc`, `lr_eval_full`, `lr_eval_spel_sad`/`_satd`, `lr_check_predictors`, `lr_fullpel_search`, `lr_refine_subpel`, `lr_motion_search`, `lr_try_bidir` (lowres) | A | lowres SAD/SATD + hpel/qpel kernels → SSBO 5/6 |
| `lr_mb_cost`, `phx_lowres_frame_cost`, `phx_lowres_path_cost`, `phx_slicetype_frame_cost`, `phx_lowres_new`/`_delete`/`_frame_reset`/`_set_mbtree` (lowres) | C | frame/path cost accumulation, B discount, prefix sum |
| `phx_lh_frame_cost`, `phx_lh_path_cost`, `phx_lh_scenecut*`, `phx_lh_slicetype_analyse`/`_decide`, `phx_lookahead_*` (lookahead) | C | serial lookahead queue, scenecut, B/BREF decision |
| `phx_macroblock_analyse`, `phx_macroblock_analyse_slice`, `phx_search_i16/i8/i4`, `phx_analysis_*` (analysis) | B/A | per-MB decision producers → SSBO 3 |
| `phx_macroblock_encode_ex`, `phx_macroblock_encode`, `phx_mb_encode_*`, `phx_mb_store_block` (mbencode) | B | per-MB reconstruction → SSBO 3 |
| `phx_macroblock_encode_chroma`, `phx_chroma_*` (chroma) | B | 4:2:0 chroma DC/AC → SSBO 3 chroma fields |
| `phx_db_*`, `phx_deblock_*`, `phx_frame_deblock*` (deblock) | B/A | deblock strength + eight edge filters per MB |
| `phx_mc_*` (mc) | B/A | hpel filter + MC sample kernels over SSBO 1 |
| `phx_cavlc_mb_init`, `phx_cavlc_predict_mv`, `phx_cavlc_mvd`, `phx_cavlc_8x8_mvd`, `phx_cavlc_mb_header*` (cavlc_syntax) | C | serial CAVLC MB-header/MVD bins |
| `phx_rdo_size_init`, `phx_rdo_size_decision[_noup]`, `phx_rdo_size_terminal`/`_bypass`/`_ue_bypass`, `phx_rdo_size_intra4x4_pred_mode`, `phx_rdo_size_block_residual_cbf`, `phx_rdo_size_partition_i4x4` (rdo) | B | size-only RDO accumulator → SSBO 7 (per-MB workgroup) |
| `phx_rdo_quant4/8_mf`, `phx_rdo_unquant4/8_mf`, `phx_rdo_size_unary`, `phx_rdo_transition_unary`, `phx_rdo_size_5ones`, `phx_rdo_transition_5ones` (rdo) | A | immutable flat-CQM / cost tables → UBO 1 |
| `phx_rdo_init`, `phx_rdo_set_config`, `phx_rdo_get_config`, `phx_rdo_quant_trellis_cabac/cavlc`, `phx_quant_*_trellis` (rdo) | C/B | trellis QNS state machine + config → UBO 2 |
| `phx_ratecontrol_*`, `rc_*` (ratecontrol) | C | serial per-frame QP/CRF scalars → UBO scalars |
| `phx_sei_*` (sei) | C | serial header stage, version SEI |
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

Every function definition currently in `src/*.c` appears exactly once below.
This index is copied from `docs/FUNCTION_PORTS.md`; duplicate static names are
listed once per defining file.

#### Class A — 185 definitions

`phx_clip3` (`analysis`), `phx_zigzag_scan4` (`analysis`), `phx_abs2` (`analysis`), `phx_satd4` (`analysis`),
`phx_satd8x4` (`analysis`), `phx_satd_4x4` (`analysis`), `phx_sa8d_8x8` (`analysis`),
`phx_pixel_sad_16x16` (`analysis`), `phx_inter_sad` (`analysis`), `phx_i16_modes` (`analysis`),
`phx_chroma_modes` (`analysis`), `phx_i8_modes` (`analysis`), `phx_i4_modes` (`analysis`),
`phx_predict16` (`analysis`), `phx_predict4_v_fixed` (`analysis`), `phx_predict4` (`analysis`),
`phx_predict8` (`analysis`), `phx_encode_i4` (`analysis`), `phx_encode_i8` (`analysis`),
`phx_encode_i16` (`analysis`), `phx_pixel_sad_4x4` (`analysis`), `phx_rdo_satd_ac_4x4` (`analysis`),
`phx_rdo_ssd4` (`analysis`), `phx_analysis_chroma_load` (`analysis`), `phx_bs_size_ue` (`bitstream`),
`phx_bs_size_se` (`bitstream`), `phx_clz` (`cabac`), `phx_bs_size_te` (`cost`), `phx_init_costs` (`cost`),
`phx_analyse_init_costs` (`cost`), `phx_mb_analyse_init_qp` (`cost`), `phx_mb_analyse_load_costs` (`cost`),
`phx_clip_pixel` (`dct`), `phx_pixel_sub_wxh` (`dct`), `phx_dct4x4dc` (`dct`), `phx_idct4x4dc` (`dct`),
`phx_dct2x4dc` (`dct`), `phx_sub4x4_dct` (`dct`), `phx_sub8x8_dct` (`dct`), `phx_sub16x16_dct` (`dct`),
`phx_sub4x4_dct_dc` (`dct`), `phx_sub8x8_dct_dc` (`dct`), `phx_sub8x16_dct_dc` (`dct`),
`phx_add4x4_idct` (`dct`), `phx_add8x8_idct` (`dct`), `phx_add16x16_idct` (`dct`), `phx_sub8x8_dct8` (`dct`),
`phx_sub16x16_dct8` (`dct`), `phx_add8x8_idct8` (`dct`), `phx_add16x16_idct8` (`dct`),
`phx_add4x4_idct_dc` (`dct`), `phx_add8x8_idct_dc` (`dct`), `phx_add16x16_idct_dc` (`dct`),
`phx_db_clip3` (`deblock`), `phx_db_clip_pixel` (`deblock`), `phx_db_table_index` (`deblock`),
`phx_deblock_alpha` (`deblock`), `phx_deblock_beta` (`deblock`), `phx_deblock_tc0` (`deblock`),
`phx_db_is_intra` (`deblock`), `phx_db_is_16x16` (`deblock`), `phx_db_chroma_qp` (`deblock`),
`phx_align_stride` (`frame`), `phx_align_plane_size` (`frame`), `phx_plane_expand_border` (`frame`),
`phx_frame_expand_border_lowres` (`frame`), `phx_frame_expand_border_chroma` (`frame`),
`phx_frame_init_lowres_core` (`frame`), `phx_frame_init_lowres` (`frame`), `lr_sad8` (`lowres`),
`lr_satd_8x4` (`lowres`), `lr_satd8` (`lowres`), `lr_get_ref` (`lowres`), `lr_bidir_avg` (`lowres`),
`lr_copy_fenc` (`lowres`), `lr_bits_full` (`lowres`), `lr_bits_spel` (`lowres`), `lr_eval_full` (`lowres`),
`lr_eval_spel_sad` (`lowres`), `lr_eval_spel_satd` (`lowres`), `lr_in_fpel_range` (`lowres`),
`lr_init_me` (`lowres`), `lr_check_predictors` (`lowres`), `lr_fullpel_search` (`lowres`),
`lr_refine_subpel` (`lowres`), `lr_motion_search` (`lowres`), `lr_try_bidir` (`lowres`),
`phx_predict4_v_fixed` (`mbencode`), `phx_mc_clip_pixel` (`mc`), `phx_mc_pixel_avg` (`mc`),
`phx_mc_copy` (`mc`), `phx_pixel_sad_16x16` (`me`), `phx_mv_cost_formula` (`me`),
`phx_resolve_cost_mv` (`me`), `phx_check_mv_range` (`me`), `phx_bits_mvd` (`me`), `phx_cost_mv` (`me`),
`phx_cost_mv_x4_dir` (`me`), `phx_cost_mv_x3_dir` (`me`), `phx_me16` (`me`), `phx_clip_pixel` (`predict`),
`phx_store4` (`predict`), `phx_predict_16x16_dc` (`predict`), `phx_predict_16x16_dc_left` (`predict`),
`phx_predict_16x16_dc_top` (`predict`), `phx_predict_16x16_dc_128` (`predict`),
`phx_predict_16x16_h` (`predict`), `phx_predict_16x16_v` (`predict`), `phx_predict_16x16_p` (`predict`),
`phx_predict_8x8c_dc_128` (`predict`), `phx_predict_8x8c_dc_left` (`predict`),
`phx_predict_8x8c_dc_top` (`predict`), `phx_predict_8x8c_dc` (`predict`), `phx_predict_8x8c_h` (`predict`),
`phx_predict_8x8c_v` (`predict`), `phx_predict_8x8c_p` (`predict`), `phx_predict_8x16c_dc_128` (`predict`),
`phx_predict_8x16c_dc_left` (`predict`), `phx_predict_8x16c_dc_top` (`predict`),
`phx_predict_8x16c_dc` (`predict`), `phx_predict_8x16c_h` (`predict`), `phx_predict_8x16c_v` (`predict`),
`phx_predict_8x16c_p` (`predict`), `phx_predict_4x4_dc_128` (`predict`),
`phx_predict_4x4_dc_left` (`predict`), `phx_predict_4x4_dc_top` (`predict`),
`phx_predict_4x4_dc` (`predict`), `phx_predict_4x4_h` (`predict`), `phx_predict_4x4_v` (`predict`),
`phx_predict_4x4_ddl` (`predict`), `phx_predict_4x4_ddr` (`predict`), `phx_predict_4x4_vr` (`predict`),
`phx_predict_4x4_hd` (`predict`), `phx_predict_4x4_vl` (`predict`), `phx_predict_4x4_hu` (`predict`),
`phx_predict_8x8_filter` (`predict`), `phx_predict_8x8_fill` (`predict`),
`phx_predict_8x8_dc_128` (`predict`), `phx_predict_8x8_dc_left` (`predict`),
`phx_predict_8x8_dc_top` (`predict`), `phx_predict_8x8_dc` (`predict`), `phx_predict_8x8_h` (`predict`),
`phx_predict_8x8_v` (`predict`), `phx_predict_8x8_ddl` (`predict`), `phx_predict_8x8_ddr` (`predict`),
`phx_predict_8x8_vr` (`predict`), `phx_predict_8x8_hd` (`predict`), `phx_predict_8x8_vl` (`predict`),
`phx_predict_8x8_hu` (`predict`), `phx_shift` (`quant`), `phx_div` (`quant`), `phx_quant_8x8` (`quant`),
`phx_quant_4x4` (`quant`), `phx_quant_4x4x4` (`quant`), `phx_quant_4x4_dc` (`quant`),
`phx_quant_2x2_dc` (`quant`), `phx_dequant_4x4` (`quant`), `phx_dequant_8x8` (`quant`),
`phx_dequant_4x4_dc` (`quant`), `phx_idct_dequant_2x4_dc` (`quant`), `phx_idct_dequant_2x4_dconly` (`quant`),
`phx_optimize_chroma_idct_dequant_2x4` (`quant`), `phx_optimize_chroma_idct_dequant_2x2` (`quant`),
`phx_optimize_chroma_round` (`quant`), `phx_optimize_chroma_dc_internal` (`quant`),
`phx_optimize_chroma_2x2_dc` (`quant`), `phx_optimize_chroma_2x4_dc` (`quant`), `phx_denoise_dct` (`quant`),
`phx_rdo_quant4_mf` (`rdo`), `phx_rdo_quant4_bias0` (`rdo`), `phx_rdo_unquant4_mf` (`rdo`),
`phx_rdo_quant8_mf` (`rdo`), `phx_rdo_quant8_bias0` (`rdo`), `phx_rdo_unquant8_mf` (`rdo`),
`phx_rdo_bs_size_ue_big` (`rdo`), `phx_rdo_size_unary` (`rdo`), `phx_rdo_transition_unary` (`rdo`),
`phx_rdo_size_5ones` (`rdo`), `phx_rdo_transition_5ones` (`rdo`), `phx_rdo_cavlc_predict_nc` (`rdo`),
`phx_coeff_last` (`rdo`), `phx_rdo_chroma_qp` (`rdo`), `phx_rdo_lambda2` (`rdo`),
`phx_cavlc_count_bits` (`rdo`), `phx_slice_group_change_bits` (`slice`).

#### Class B — 109 definitions

`phx_zigzag_scan8` (`analysis`), `phx_satd_8x8` (`analysis`), `phx_satd_16x16` (`analysis`),
`phx_median3` (`analysis`), `phx_predict_mv16` (`analysis`), `phx_predict_mv_pskip` (`analysis`),
`phx_predict4_mode` (`analysis`), `phx_init_neighbours` (`analysis`), `phx_setup_fdec` (`analysis`),
`phx_analysis_quant4` (`analysis`), `phx_search_i16` (`analysis`), `phx_search_i8` (`analysis`),
`phx_search_i4` (`analysis`), `phx_pixel_ssd_4x4` (`analysis`), `phx_analysis_rd_i4` (`analysis`),
`phx_analysis_intra_rd_refine_i4` (`analysis`), `phx_analysis_intra_chroma` (`analysis`),
`phx_analysis_inter_p16x16` (`analysis`), `phx_analysis_init` (`analysis`),
`phx_analysis_finalise` (`analysis`), `phx_macroblock_analyse_slice` (`analysis`),
`phx_macroblock_analyse` (`analysis`), `phx_zigzag_scan_2x2_dc` (`chroma`),
`phx_idct_dequant_2x2_dc` (`chroma`), `phx_idct_dequant_2x2_dconly` (`chroma`), `phx_dct2x2dc` (`chroma`),
`phx_chroma_mode_available` (`chroma`), `phx_clip3` (`chroma`), `phx_chroma_load_windows` (`chroma`),
`phx_chroma_store` (`chroma`), `phx_macroblock_encode_chroma` (`chroma`), `phx_db_edge_luma_c` (`deblock`),
`phx_db_luma_c` (`deblock`), `phx_deblock_h_luma_mbaff_c` (`deblock`), `phx_deblock_v_luma_c` (`deblock`),
`phx_deblock_h_luma_c` (`deblock`), `phx_db_edge_chroma_c` (`deblock`), `phx_db_chroma_c` (`deblock`),
`phx_deblock_h_chroma_mbaff_c` (`deblock`), `phx_deblock_v_chroma_c` (`deblock`),
`phx_deblock_h_chroma_c` (`deblock`), `phx_db_edge_luma_intra_c` (`deblock`),
`phx_db_luma_intra_c` (`deblock`), `phx_deblock_h_luma_intra_mbaff_c` (`deblock`),
`phx_deblock_v_luma_intra_c` (`deblock`), `phx_deblock_h_luma_intra_c` (`deblock`),
`phx_db_edge_chroma_intra_c` (`deblock`), `phx_db_chroma_intra_c` (`deblock`),
`phx_deblock_h_chroma_intra_mbaff_c` (`deblock`), `phx_deblock_v_chroma_intra_c` (`deblock`),
`phx_deblock_h_chroma_intra_c` (`deblock`), `phx_db_strength` (`deblock`),
`phx_deblock_strength_c` (`deblock`), `phx_macroblock_deblock_strength` (`deblock`),
`phx_deblock_edge` (`deblock`), `phx_db_neighbours` (`deblock`), `phx_db_qp_at` (`deblock`),
`phx_db_filter_edge` (`deblock`), `phx_frame_deblock_ex` (`deblock`), `phx_frame_deblock` (`deblock`),
`phx_macroblock_cache_load_neighbours` (`mbcache`), `phx_macroblock_cache_load` (`mbcache`),
`phx_macroblock_backup_intra` (`mbcache`), `phx_macroblock_cache_save` (`mbcache`),
`phx_mb_store_block` (`mbencode`), `phx_mb_load_fdec_neighbours` (`mbencode`),
`phx_mb_neighbour4` (`mbencode`), `phx_mb_encode_i16x16` (`mbencode`), `phx_mb_b_trellis` (`mbencode`),
`phx_mb_encode_i4x4` (`mbencode`), `phx_mb_encode_pcm` (`mbencode`),
`phx_mb_clear_inter_residual` (`mbencode`), `phx_mb_mc_l0_16x16` (`mbencode`),
`phx_mb_mc_chroma_l0` (`mbencode`), `phx_macroblock_encode_ex` (`mbencode`),
`phx_macroblock_encode` (`mbencode`), `phx_mb_clip3i` (`mbencode`), `phx_mb_chroma_qp` (`mbencode`),
`phx_mb_chroma_mode_ok` (`mbencode`), `phx_mb_zigzag2_dc` (`mbencode`), `phx_mb_dct2x2dc` (`mbencode`),
`phx_mb_chroma_raw_dc` (`mbencode`), `phx_mb_encode_chroma_trellis` (`mbencode`),
`phx_mc_hpel_filter` (`mc`), `phx_mc_hpel_filter_frame` (`mc`), `phx_mc_luma` (`mc`), `phx_mc_chroma` (`mc`),
`phx_mc_deinterleave_chroma` (`mc`), `phx_mc_hpel_invalidate` (`mc`), `phx_mc_luma_mb` (`mc`),
`phx_predict_16x16_init` (`predict`), `phx_predict_8x8c_init` (`predict`),
`phx_predict_8x16c_init` (`predict`), `phx_predict_4x4_init` (`predict`), `phx_predict_8x8_init` (`predict`),
`phx_quant_tables_init` (`quant`), `phx_cabac_size_decision2` (`rdo`),
`phx_cabac_size_decision_noup2` (`rdo`), `phx_rdo_init` (`rdo`), `phx_rdo_size_init` (`rdo`),
`phx_rdo_size_decision` (`rdo`), `phx_rdo_size_decision_noup` (`rdo`), `phx_rdo_size_terminal` (`rdo`),
`phx_rdo_size_bypass` (`rdo`), `phx_rdo_size_ue_bypass` (`rdo`), `phx_rdo_size_intra4x4_pred_mode` (`rdo`),
`phx_rdo_size_block_residual` (`rdo`), `phx_rdo_size_block_residual_cbf` (`rdo`),
`phx_rdo_size_partition_i4x4` (`rdo`).

#### Class C — 211 definitions

`phx_bs_init` (`bitstream`), `phx_bs_pos` (`bitstream`), `phx_bs_write` (`bitstream`),
`phx_bs_write1` (`bitstream`), `phx_bs_align_0` (`bitstream`), `phx_bs_align_10` (`bitstream`),
`phx_bs_write_ue_big` (`bitstream`), `phx_bs_write_ue` (`bitstream`), `phx_bs_write_se` (`bitstream`),
`phx_bs_write_te` (`bitstream`), `phx_bs_rbsp_trailing` (`bitstream`), `phx_nal_escape` (`bitstream`),
`phx_nal_encode` (`bitstream`), `phx_trace_open` (`cabac`), `phx_trace_decision` (`cabac`),
`phx_trace_bypass` (`cabac`), `phx_trace_ue` (`cabac`), `phx_trace_terminal` (`cabac`),
`phx_cabac_context_init` (`cabac`), `phx_cabac_encode_init_core` (`cabac`),
`phx_cabac_encode_init` (`cabac`), `phx_cabac_putbyte` (`cabac`), `phx_cabac_encode_renorm` (`cabac`),
`phx_cabac_encode_decision` (`cabac`), `phx_cabac_encode_bypass` (`cabac`),
`phx_cabac_encode_ue_bypass` (`cabac`), `phx_cabac_encode_terminal` (`cabac`),
`phx_cabac_encode_flush` (`cabac`), `phx_coeff_last4` (`cabac_syntax`), `phx_coeff_last8` (`cabac_syntax`),
`phx_coeff_last15` (`cabac_syntax`), `phx_coeff_last16` (`cabac_syntax`),
`phx_coeff_last64` (`cabac_syntax`), `phx_cabac_mb_init` (`cabac_syntax`),
`phx_predict_intra4x4_mode` (`cabac_syntax`), `phx_cabac_mb_type_intra` (`cabac_syntax`),
`phx_cabac_intra4x4_pred_mode` (`cabac_syntax`), `phx_cabac_intra_chroma_pred_mode` (`cabac_syntax`),
`phx_cabac_subpartition_p` (`cabac_syntax`), `phx_cabac_subpartition_b` (`cabac_syntax`),
`phx_cabac_transform_size` (`cabac_syntax`), `phx_cabac_cbp_luma` (`cabac_syntax`),
`phx_cabac_cbp_chroma` (`cabac_syntax`), `phx_cabac_qp_delta` (`cabac_syntax`),
`phx_cabac_mb_skip` (`cabac_syntax`), `phx_predict_mv` (`cabac_syntax`),
`phx_cabac_mvd_sum` (`cabac_syntax`), `phx_cache_mvd` (`cabac_syntax`), `phx_cabac_mvd_cpn` (`cabac_syntax`),
`phx_cabac_mvd` (`cabac_syntax`), `phx_cabac_8x8_mvd` (`cabac_syntax`), `phx_cabac_ref` (`cabac_syntax`),
`phx_cabac_mb_header_i` (`cabac_syntax`), `phx_cabac_mb_header_p` (`cabac_syntax`),
`phx_cabac_mb_header_b` (`cabac_syntax`), `phx_cabac_cbf_ctxidxinc` (`cabac_syntax`),
`phx_cabac_block_residual` (`cabac_syntax`), `phx_cabac_block_residual_cbf` (`cabac_syntax`),
`phx_clz` (`cavlc`), `phx_cavlc_coeff_last` (`cavlc`), `phx_cavlc_coeff_level_run` (`cavlc`),
`phx_cavlc_init_level_token` (`cavlc`), `phx_cavlc_init_run_before` (`cavlc`), `phx_cavlc_init` (`cavlc`),
`phx_cavlc_write_vlc` (`cavlc`), `phx_cavlc_write_vlc_large` (`cavlc`),
`phx_cavlc_block_residual_escape` (`cavlc`), `phx_cavlc_block_residual_internal` (`cavlc`),
`phx_cavlc_block_residual` (`cavlc`), `phx_cavlc_coeff0` (`cavlc`), `phx_cavlc_coeff_token_at` (`cavlc`),
`phx_cavlc_total_zeros_at` (`cavlc`), `phx_cavlc_total_zeros_dc_at` (`cavlc`),
`phx_cavlc_mb_init` (`cavlc_syntax`), `phx_cavlc_predict_mv` (`cavlc_syntax`),
`phx_cavlc_mvd` (`cavlc_syntax`), `phx_predict_intra4x4_mode` (`cavlc_syntax`),
`phx_cavlc_mb_header_i` (`cavlc_syntax`), `phx_cavlc_8x8_mvd` (`cavlc_syntax`),
`phx_cavlc_mb_header_p_l0` (`cavlc_syntax`), `phx_cavlc_mb_header_p_8x8` (`cavlc_syntax`),
`phx_cavlc_mb_header_b_8x8` (`cavlc_syntax`), `phx_cavlc_mb_header_b_inter` (`cavlc_syntax`),
`phx_cavlc_mb_header` (`cavlc_syntax`), `phx_cavlc_mb_skip` (`cavlc_syntax`),
`phx_analyse_free_costs` (`cost`), `phx_encoder_validate` (`encoder`), `phx_encoder_release` (`encoder`),
`phx_encoder_open` (`encoder`), `phx_open` (`encoder`), `phx_headers` (`encoder`),
`phx_encoder_header_bytes` (`encoder`), `phx_encoder_copy_picture` (`encoder`),
`phx_encoder_new_input_frame` (`encoder`), `phx_encoder_queue_coded` (`encoder`),
`phx_encoder_drain` (`encoder`), `phx_encoder_slice_type` (`encoder`), `phx_encoder_slice_qp` (`encoder`),
`phx_encoder_dpb_remove` (`encoder`), `phx_encoder_dpb_refresh` (`encoder`),
`phx_encoder_dpb_lookup` (`encoder`), `phx_encoder_build_ref_lists` (`encoder`),
`phx_encoder_frame_context` (`encoder`), `phx_encoder_bs_align_1` (`encoder`),
`phx_encoder_reset_mb_state` (`encoder`), `phx_encoder_apply_decision` (`encoder`),
`phx_encoder_block` (`encoder`), `phx_encoder_block_nz` (`encoder`),
`phx_mb_transform_8x8_allowed` (`encoder`), `phx_encoder_cabac_block` (`encoder`),
`phx_encoder_cavlc_nc` (`encoder`), `phx_encoder_cavlc_block` (`encoder`),
`phx_encoder_predict_i4` (`encoder`), `phx_encoder_predict_mv16` (`encoder`),
`phx_encoder_cavlc_header_i` (`encoder`), `phx_encoder_cavlc_qp_delta` (`encoder`),
`phx_encoder_write_cavlc_mb` (`encoder`), `phx_encoder_write_cabac_mb` (`encoder`), `phx_encode` (`encoder`),
`phx_encoder_encode` (`encoder`), `phx_encoder_close` (`encoder`), `phx_close` (`encoder`),
`phx_frame_new` (`frame`), `phx_frame_delete` (`frame`), `phx_sps_init` (`headers`),
`phx_pps_init` (`headers`), `phx_sps_write` (`headers`), `phx_pps_write` (`headers`),
`phx_encoder_headers` (`headers`), `phx_lh_frame_cost` (`lookahead`), `phx_lh_path_cost` (`lookahead`),
`phx_lh_scenecut_internal` (`lookahead`), `phx_lh_scenecut` (`lookahead`),
`phx_lh_slicetype_analyse` (`lookahead`), `phx_lh_slicetype_decide` (`lookahead`),
`phx_lookahead_new` (`lookahead`), `phx_lookahead_put_frame` (`lookahead`),
`phx_lh_shift_to_ofbuf` (`lookahead`), `phx_lookahead_get_frames_full` (`lookahead`),
`phx_lookahead_get_frames_ex` (`lookahead`), `phx_lookahead_get_frames` (`lookahead`),
`phx_lookahead_delete` (`lookahead`), `lr_frame_clear` (`lowres`), `lr_get_frame` (`lowres`),
`phx_lowres_set_mbtree` (`lowres`), `phx_lowres_frame_reset` (`lowres`), `phx_lowres_new` (`lowres`),
`phx_lowres_delete` (`lowres`), `lr_mb_cost` (`lowres`), `phx_lowres_frame_cost` (`lowres`),
`phx_lowres_path_cost` (`lowres`), `phx_slicetype_frame_cost` (`lowres`), `phx_mb_state_new` (`mbcache`),
`phx_mb_state_delete` (`mbcache`), `phx_param_default` (`param`), `phx_param_apply_preset` (`param`),
`phx_param_finalize` (`param`), `phx_param_apply_profile` (`param`), `rc_clip3i` (`ratecontrol`),
`rc_clip3f` (`ratecontrol`), `rc_max2` (`ratecontrol`), `rc_qp2qscale` (`ratecontrol`),
`rc_qscale2qp` (`ratecontrol`), `rc_exp2fix8` (`ratecontrol`), `rc_clz` (`ratecontrol`),
`rc_log2_u32` (`ratecontrol`), `rc_clip_duration` (`ratecontrol`), `phx_ratecontrol_init` (`ratecontrol`),
`phx_ratecontrol_delete` (`ratecontrol`), `phx_ratecontrol_configure` (`ratecontrol`),
`phx_ratecontrol_set_complexity` (`ratecontrol`), `phx_ratecontrol_set_quant_offsets` (`ratecontrol`),
`rc_accum_p_qp_update` (`ratecontrol`), `phx_ratecontrol_start` (`ratecontrol`),
`phx_ratecontrol_end` (`ratecontrol`), `rc_ac_energy_mb` (`ratecontrol`),
`phx_ratecontrol_aq_frame` (`ratecontrol`), `phx_ratecontrol_mb_qp` (`ratecontrol`),
`phx_ratecontrol_qp_offsets` (`ratecontrol`), `phx_ratecontrol_qp_offsets_aq` (`ratecontrol`),
`phx_ratecontrol_inv_qscale` (`ratecontrol`), `phx_ratecontrol_qp_constant` (`ratecontrol`),
`phx_ratecontrol_chroma_qp` (`ratecontrol`), `phx_rdo_set_config` (`rdo`), `phx_rdo_get_config` (`rdo`),
`phx_trellis_dc_shortcut` (`rdo`), `phx_trellis_coef` (`rdo`), `phx_trellis_coef0_0` (`rdo`),
`phx_trellis_coef0_1` (`rdo`), `phx_trellis_coef1_0` (`rdo`), `phx_trellis_coef1_1` (`rdo`),
`phx_trellis_coefn_0` (`rdo`), `phx_trellis_coefn_1` (`rdo`), `phx_rdo_quant_trellis_cabac` (`rdo`),
`phx_rdo_quant_trellis_cavlc` (`rdo`), `phx_quant_4x4_trellis` (`rdo`), `phx_quant_8x8_trellis` (`rdo`),
`phx_quant_luma_dc_trellis` (`rdo`), `phx_quant_chroma_dc_trellis` (`rdo`), `phx_sei_clip3` (`sei`),
`phx_sei_cpu_num_processors` (`sei`), `phx_sei_derive_runtime` (`sei`), `phx_sei_analyse_intra` (`sei`),
`phx_sei_analyse_inter` (`sei`), `phx_sei_me_name` (`sei`), `phx_sei_rc_name` (`sei`),
`phx_sei_param2string` (`sei`), `phx_sei_write` (`sei`), `phx_sei_version_write` (`sei`),
`phx_slice_header_init` (`slice`), `phx_slice_header_write` (`slice`).

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
