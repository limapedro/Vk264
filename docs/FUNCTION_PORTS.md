# Phoenix function port index — CPU oracle → shader backend

Scope: **every function defined in `src/*.c` on `vk264-phoenix`**. This is a
**snapshot of the working tree** while parallel workers are still adding
modules; re-run the scan before using it as a gate. At snapshot time there are
**208 function definitions** (206 unique names — `phx_clip_pixel` exists in
both `phx_dct.c` and `phx_predict.c`, and `phx_clz` in both `phx_cabac.c` and
`phx_cavlc.c`) across 14 modules; `src/phx_tables.c` defines data only (11
immutable arrays). Modules added by parallel workers during writing (`dct`,
`predict`, `encoder`, `cabac_syntax`, `cavlc`, `quant`, `slice`) are all
included below. All x264 references are relative to `reference/x264/`; the
`file:line` given is the exact x264 definition the Phoenix function follows,
as recorded in the port's own source comments. Classification uses the
rationale in `docs/SHADER_MAPPING.md` (A = data-parallel, B = workgroup with
neighbour state, C = strictly serial).

Counts: **104 × A, 10 × B, 94 × C** (host lifecycle and one ordered output
stream both fall in C, since neither has a per-work-item mapping). The five
predictor `*init` functions and `phx_quant_tables_init` are B because their
own comments mark them as dispatch/table construction for the ordered stages
(their entries are A).

SHADER_NOTES.md turns this index into the backend design;
FUNCTION_INVENTORY.md is the mechanical master list (1386 x264 functions).

---

## 1. Module `param` — `src/phx_param.c`

One-time host configuration. None of these has a per-pixel/per-block work
item, so all are **C** by elimination from A and B; each is a pure function of
constant data or a string argument and is trivially seek-safe.

### `phx_param_default` — `common/base.c:344` (`x264_param_default`)
- **Inputs:** `phx_param_t *p` (caller-owned output struct).
- **Outputs:** `p` zeroed, then filled with the medium defaults (every assignment carries its own source line, `phx_param.c:28–60`).
- **Mutates:** `p` only; no globals or hidden state.
- **Determinism / seek-safety:** constant result, independent of any frame; may be called at any time.
- **SHADER: C** — host-only one-time setup; no work-item analogue. Its result is the parameter buffer every shader stage reads.

### `phx_param_apply_preset` — `common/base.c:489` (`param_apply_preset`)
- **Inputs:** `phx_param_t *p`, `const char *preset` (10 names, case-insensitive).
- **Outputs:** `p` with the preset deltas applied; returns `0` on match, `-1` otherwise.
- **Mutates:** `p` only.
- **Determinism / seek-safety:** lookup table keyed by the preset string; deterministic and position-independent.
- **SHADER: C** — host-only configuration; the A/B stages consume the finished parameter block, never the string.

### `phx_param_finalize` — `encoder/encoder.c:1218–1231` (psy-RD chroma-QP compensation); keyint-min auto follows `encoder/encoder.c:1109–1111`
- **Inputs:** `phx_param_t *p` (already defaulted/preset/applied); reads `b_psy`, `i_subpel_refine`, `i_keyint_max`, `i_keyint_min`, `i_bframe`.
- **Outputs:** `p->i_chroma_qp_index_offset` and `p->i_keyint_min` resolved (`phx_param.c:126–133`). The keyint-min bound uses `i_bframe + 1` for the fps term (`phx_param.c:130`).
- **Mutates:** `p` only.
- **Determinism / seek-safety:** pure function of `p`; can be rerun before any encode window.
- **SHADER: C** — host-only derivation; produces values used by SPS/PPS init and QP setup, not a shader stage itself.

### `phx_param_apply_profile` — `common/base.c:749` (`x264_param_apply_profile`)
- **Inputs:** `phx_param_t *p`, `const char *profile` (`"baseline"` / `"main"`).
- **Outputs:** restricted `p`; returns `0` / `-1`.
- **Mutates:** `p` only (`b_transform_8x8`, `b_cabac`, `i_bframe`, `i_weighted_pred`).
- **Determinism / seek-safety:** constant restrictions; deterministic and position-independent.
- **SHADER: C** — host-only profile gate, applied before any stage reads parameters.

---

## 2. Module `tables` — `src/phx_tables.c` / `src/phx_tables.h`

No functions: this module is **generated data** (`tools/gen_tables.py`), so the
"function port" section is the table inventory. `phx_tables.h:5` already
records `SHADER: A — immutable lookup tables, can live in constant memory`.

| Phoenix array | x264 source | Shape | Consumers |
|---|---|---|---|
| `phx_lambda_tab` | `common/tables.c:97` | 82 × uint16 | `phx_init_costs`, `phx_mb_analyse_init_qp` |
| `phx_lambda2_tab` | `common/tables.c:114` | 82 × int | `phx_mb_analyse_init_qp` |
| `phx_trellis_lambda2_tab` | `common/tables.c:131` | 2 × 82 × int | `phx_mb_analyse_init_qp` |
| `phx_chroma_lambda2_offset_tab` | `common/tables.c:168` | 37 × uint16 | `phx_mb_analyse_init_qp` |
| `phx_ue_size_tab` | `common/bitstream.h:201` | 256 × uint8 | `phx_bs_*` size functions |
| `phx_chroma_qp_base` | `common/macroblock.h:249` (8-bit entries) | 76 × uint8 | `phx_mb_analyse_init_qp` |
| `phx_cabac_range_lps` | `common/tables.c:1668` | 64 × 4 × uint8 | `phx_cabac_encode_decision` |
| `phx_cabac_transition` | `common/tables.c:1688` | 128 × 2 × uint8 | `phx_cabac_encode_decision` |
| `phx_cabac_renorm_shift` | `common/tables.c:1708` | 64 × uint8 | `phx_cabac_encode_renorm` |
| `phx_cabac_context_init_I` | `common/tables.c:456` | 1024 × 2 × int8 | `phx_cabac_context_init` |
| `phx_cabac_context_init_PB` | `common/tables.c:768` | 3 × 1024 × 2 × int8 | `phx_cabac_context_init` |

- **Inputs / outputs:** none at runtime; read-only indexed lookups.
- **Mutates:** nothing.
- **Determinism / seek-safety:** absolute; identical for every frame, preset and seek point.
- **SHADER: A** — immutable constant/UBO data, no ordering and no cross-item state. (CABAC tables are read *by* a C stage; that does not change their own class.)

---

## 3. Module `cost` — `src/phx_cost.c`

Table construction and per-MB analysis setup from `encoder/analyse.c`. The
arrays produced here are read by every mode decision; no hidden state remains
in the ported functions.

### `phx_bs_size_te` — `common/bitstream.h:301` (`bs_size_te`)
- **Inputs:** `int x`, `int val`.
- **Outputs:** bit cost (`1` when `x == 1`, else `phx_ue_size_tab[val+1]`).
- **Mutates:** nothing.
- **Determinism / seek-safety:** pure lookup; fully seek-safe.
- **SHADER: A** — per-value pure query, one invocation per candidate; mirrors Class A "vectorizable / no cross-item state".

### `phx_init_costs` (static) — `encoder/analyse.c:143` (`init_costs`)
- **Inputs:** `phx_cost_tables_t *t`, `const float *logs` (built by `phx_analyse_init_costs`, `analyse.c:186–188`), `int qp`, `int mv_range`, `int me_method`.
- **Outputs:** `t->cost_mv[qp]` (centred at `±2*4*mv_range`), `t->cost_mv_fpel[qp][0..3]` (ESA/UMH/TESA only), `t->cost_ref[qp][3][33]`, `t->cost_i4x4_mode[qp][17]`.
- **Mutates:** `t` and the centred `malloc` allocations; no globals.
- **Determinism / seek-safety:** pure function of `(logs, qp, mv_range, me_method)`, guarded by `if (t->cost_mv[qp]) return 0`; rebuildable for any QP without frame history.
- **SHADER: A** — host-side table fill (per-QP/per-entry independent), matching the Class A requirement that table construction stay pure.

### `phx_analyse_init_costs` — `encoder/analyse.c:179` (`x264_analyse_init_costs`)
- **Inputs:** `phx_cost_tables_t *t`, `int mv_range`, `int me_method`, `int qp_min`, `int qp_max`.
- **Outputs:** zero-initialised `t` with all QPs in `[min(qp_min, 51), qp_max]` plus `PHX_LOOKAHEAD_QP` populated; returns `0` / `-1` (OOM).
- **Mutates:** `t`, the temporary `logs` buffer (freed at `analyse.c:197`).
- **Determinism / seek-safety:** pure function of its five arguments; rerun at any seek point.
- **SHADER: A** — pure table construction; the resulting arrays are the read-only Class A input to mode decisions.

### `phx_analyse_free_costs` — `encoder/analyse.c:204` (`x264_analyse_free_costs`)
- **Inputs:** `phx_cost_tables_t *t`, `int mv_range` (needed to recover the centred allocation base).
- **Outputs:** `t` zeroed; no return value.
- **Mutates:** `t` and its heap allocations.
- **Determinism / seek-safety:** pure teardown; position-independent.
- **SHADER: C** — host-only lifecycle (no work items); it releases the buffers that the A stages read.

### `phx_mb_analyse_init_qp` — `encoder/analyse.c:255` (`mb_analyse_init_qp`)
- **Inputs:** `const phx_param_t *p`, `int qp`, `int chroma_qp_index_offset`, `int mbrd`, `phx_mb_analysis_t *a`.
- **Outputs:** `a->i_qp`, `i_lambda`, `i_lambda2`, `b_trellis`, `i_trellis_lambda2[2][2]`, `i_psy_rd_lambda`, `i_chroma_lambda2_offset` (`analyse.c:257–280`).
- **Mutates:** `a` only; `p` is read-only.
- **Determinism / seek-safety:** pure function of QP/param/mbrd; recomputed per MB, no neighbour or frame history.
- **SHADER: A** — per-MB pure derivation, one invocation per work item; explicit inputs/outputs as Class A requires.

### `phx_mb_analyse_load_costs` — `encoder/analyse.c:248` (`mb_analyse_load_costs`)
- **Inputs:** `phx_cost_tables_t *t`, `phx_mb_analysis_t *a`, `int num_ref_l0`, `int num_ref_l1`.
- **Outputs:** `a->p_cost_mv`, `a->p_cost_ref[0..1]` selected by clamped reference counts (`analyse.c:250–252`).
- **Mutates:** `a` only (`t` is read-only).
- **Determinism / seek-safety:** pure wiring; no evolving state.
- **SHADER: A** — pointer/index binding for the per-MB cost arrays; mechanical in a shader as an index calculation.

---

## 4. Module `frame` — `src/phx_frame.c`

Frame allocation plus the lowres plane pipeline. The allocation functions are
host lifecycle (C); the plane preparation functions are the canonical Class A
lowres work (`SHADER_MAPPING.md:19`). The header-level `SHADER: A`
(`phx_frame.h:8`) describes the *data* these functions produce, not the
allocators themselves.

### `phx_align_stride` — `common/frame.c:30` (`align_stride`)
- **Inputs:** `int x`, `int align`, `int disalign`.
- **Outputs:** aligned stride (`frame.c:32–34`).
- **Mutates:** nothing.
- **Determinism / seek-safety:** pure integer arithmetic; position-independent.
- **SHADER: A** — per-plane pure computation; may be folded into host setup or a shader constant.

### `phx_align_plane_size` — `common/frame.c:38` (`align_plane_size`)
- **Inputs:** `int x`, `int disalign`.
- **Outputs:** plane size, adding 1024 when disaligned (`frame.c:39–40`).
- **Mutates:** nothing.
- **Determinism / seek-safety:** pure arithmetic; position-independent.
- **SHADER: A** — same as `phx_align_stride`; no cross-item state.

### `phx_plane_expand_border` — `common/frame.c:535` (`plane_expand_border`)
- **Inputs:** `uint8_t *pix`, `stride`, `width`, `height`, `padh`, `padv`, `pad_top`, `pad_bottom`, `chroma` (always 0 for lowres luma).
- **Outputs:** replicated edge samples in the four padding bands (`frame.c:538–552`).
- **Mutates:** the caller's plane buffer only.
- **Determinism / seek-safety:** depends only on the current plane's edge pixels; no other frame needed.
- **SHADER: A** — independent per row/column of the border; exactly the "one invocation per work item" pattern.

### `phx_frame_expand_border_lowres` — `common/frame.c:627` (`x264_frame_expand_border_lowres`)
- **Inputs:** `phx_frame_t *f` (reads `lowres[]`, strides, dimensions).
- **Outputs:** all four lowres planes padded via `phx_plane_expand_border`.
- **Mutates:** `f->lowres[0..3]`.
- **Determinism / seek-safety:** current-frame only; recomputable at any seek point.
- **SHADER: A** — four independent plane passes; each plane is its own dispatch.

### `phx_frame_init_lowres_core` — `common/mc.c:484` (`frame_init_lowres_core`)
- **Inputs:** `src0`, `dst0/dsth/dstv/dstc`, `src_stride`, `dst_stride`, `width`, `height`.
- **Outputs:** four half-resolution planes; the `FILTER` macro is verbatim from `mc.c:494`.
- **Mutates:** the four destination planes only; `src0` read-only (caller has already duplicated the last row/column).
- **Determinism / seek-safety:** pure local 2×2 filtering over the current frame; fully seek-safe.
- **SHADER: A** — the doc's named example: "Lowres plane construction | `common/mc.c:458` | per 2x2 source block" (`SHADER_MAPPING.md:19`).

### `phx_frame_init_lowres` — `common/mc.c:458` (`x264_frame_init_lowres`)
- **Inputs:** `phx_frame_t *f` (uses `plane`, `i_stride`, `i_width/i_height`, `i_*_lowres`, `bframes`).
- **Outputs:** four padded lowres planes; `i_cost_est` reset to `-1`, `i_row_satds[][][0]` to `-1`, `lowres_mvs[][][0]` to `0x7FFF` (`mc.c:473–481`).
- **Mutates:** `f->plane` (one duplicated row/column), `f->lowres[]` and the three lookahead caches.
- **Determinism / seek-safety:** frame-local initialisation; no previous-frame state, so safe for any seek target.
- **SHADER: A** — orchestrates A-stage work; the cache resets are independent writes. (The caches themselves are consumed by Class C lookahead when ported.)

### `phx_frame_new` (reduced `frame_new`) — `common/frame.c:60` (`frame_new`)
- **Inputs:** `int width`, `int height`, `int bframes`; rejects non-16 multiples (`frame.c:85–86`).
- **Outputs:** allocated `phx_frame_t *` or `NULL`, with strides/sizes/offsets following `frame.c:81–281`.
- **Mutates:** newly allocated heap only; on partial failure calls `phx_frame_delete`.
- **Determinism / seek-safety:** no frame history; allocate at any seek point.
- **SHADER: C** — host-side memory lifecycle; creates the SSBO buffers that the A stages fill. No invocation per work item.

### `phx_frame_delete` — `common/frame.c:312` (`x264_frame_delete`)
- **Inputs:** `phx_frame_t *f` (NULL tolerated).
- **Outputs:** none; frees `i_row_satds`, `f->base`, `f`.
- **Mutates:** heap owned by `f`.
- **Determinism / seek-safety:** position-independent teardown.
- **SHADER: C** — host-side lifecycle, no shader stage.

---

## 5. Module `mbcache` — `src/phx_mbcache.c`

Per-MB neighbour state. The cache load/save functions are the doc's Class B
example (`SHADER_MAPPING.md:34`: left/top MBs must be complete), with the
additional ordering notes from `DATA_STRUCTURES.md:6` and `:77–81`.

### `phx_mb_state_new` — `common/frame.c:81` allocation pattern; layout from `common/common.h` / `common/macroblock.h`
- **Inputs:** `int mb_width`, `int mb_height`.
- **Outputs:** zeroed `phx_mb_state_t *` with `mb_stride`, `b8_stride`, `b4_stride`, `mb_count` and all per-MB arrays (`phx_mbcache.c:35–59`).
- **Mutates:** newly allocated heap; sets `slice_type = 1` (P), `b_cabac = 1`, `threadslice_start = 0` (single-thread port).
- **Determinism / seek-safety:** pure allocation; a fresh state is a valid seek entry point.
- **SHADER: C** — host allocation of the per-frame MB-state SSBO; no work-item mapping.

### `phx_mb_state_delete` — teardown counterpart of `phx_mb_state_new`
- **Inputs:** `phx_mb_state_t *s` (NULL tolerated).
- **Outputs:** none; frees all 12 owned arrays and `s`.
- **Mutates:** heap owned by `s`.
- **Determinism / seek-safety:** position-independent teardown.
- **SHADER: C** — host lifecycle.

### `phx_macroblock_cache_load_neighbours` — `common/macroblock.c:673` (`macroblock_cache_load_neighbours`)
- **Inputs:** `phx_mb_state_t *s` (read-only), `phx_mb_cache_t *c`, `mb_x`, `mb_y`, `b_mbaff`.
- **Outputs:** fills `c->top/left/topleft/topright`, `type_*`, `i_neighbour`, `i_neighbour_intra`, `i_neighbour_skip`, `field_decoding_flag`, `b_allow_skip`, `left_index` and the MBAFF field pair indices (`phx_mbcache.c:78–214`).
- **Mutates:** `c` only.
- **Determinism / seek-safety:** needs the left/top MBs already saved in `s`; deterministic once they are, and safe when entered at a frame/slice boundary.
- **SHADER: B** — workgroup-stage neighbour discovery: "left/top MBs must be complete" (`SHADER_MAPPING.md:34`); requires a wavefront barrier, not a freestanding pure shader.

### `phx_macroblock_cache_load` — `common/macroblock.c:855` (`macroblock_cache_load`)
- **Inputs:** `phx_mb_state_t *s` (read-only), `phx_mb_cache_t *c`, `mb_x`, `mb_y`, `b_mbaff`.
- **Outputs:** top/left/toplen/topright `cbp`, `intra4x4_pred_mode`, `non_zero_count`, `ref`/`mv`/`mvd` caches; `i_neighbour_transform_size`; MAP_MVS conversion for MBAFF (`phx_mbcache.c:221–408`).
- **Mutates:** `c` only; calls the B-stage `load_neighbours` first.
- **Determinism / seek-safety:** requires all neighbouring MBs in `s` to be complete; not mid-frame seek-safe, but deterministic from a frame start.
- **SHADER: B** — the doc's explicit "Neighbour cache load/save" row (`SHADER_MAPPING.md:34`); implement as shared-memory gather plus two-pass reduce over neighbours.

### `phx_macroblock_backup_intra` — `common/macroblock.c:1638` (`macroblock_backup_intra`)
- **Inputs:** `phx_mb_state_t *s`, `mb_x`, `mb_y`, `b_mbaff`, `const uint8_t *p_fdec`, `stride`.
- **Outputs:** bottom luma row (16 bytes) and two 8-byte chroma rows copied into `s->intra_border_backup[backup_dst]` (`phx_mbcache.c:415–431`).
- **Mutates:** `s->intra_border_backup` only; `p_fdec` read-only.
- **Determinism / seek-safety:** needs this MB's reconstructed pixels, so ordered within a frame; deterministic from frame start.
- **SHADER: B** — writes state consumed by the *next* row's intra prediction; neighbour-ordered workgroup stage, not independent per work item.

### `phx_macroblock_cache_save` — `common/macroblock.c:1680` (`x264_macroblock_cache_save`)
- **Inputs:** `phx_mb_state_t *s`, `phx_mb_cache_t *c`, `b_mbaff`; reads `c->p_fdec/fdec_stride` for the intra backup.
- **Outputs:** writes `type` (caller-supplied), `intra4x4_pred_mode`, `non_zero_count` (via `nz_idx`/`nz_dst` and `PHX_STORE_8x8_NNZ`), `cbp`, `mb_transform_size`, `slice_table`, `ref`/`mv`/`mvd` (`phx_mbcache.c:437–514`).
- **Mutates:** `s` (the shared per-frame arrays) and, indirectly, `c->b_transform_8x8` for PCM/zero-luma cases.
- **Determinism / seek-safety:** caller must place the MB type in `s->type[xy]` first; requires previous MBs complete. Deterministic from frame start, not mid-frame seek-safe.
- **SHADER: B** — "Neighbour cache load/save | `common/macroblock.c:855/1680` | left/top MBs must be complete" (`SHADER_MAPPING.md:34`). This is the producer of the Class B neighbour state.

---

## 6. Module `bitstream` — `src/phx_bitstream.c`

All functions operate on one evolving output cursor. `SHADER_MAPPING.md:52`
places "NAL assembly / emulation prevention | `common/bitstream.c`" in C;
only the two *size queries* are pure enough to be A.

### `phx_bs_init` — `common/bitstream.h:86` (`bs_init`)
- **Inputs:** `phx_bs_t *s`, `void *data`, `int size`.
- **Outputs:** zeroed buffer, `p_start = p = data`, `bit = 0`.
- **Mutates:** `s` and the caller's buffer.
- **Determinism / seek-safety:** deterministic reset of a serial stream; rerun at any NAL start.
- **SHADER: C** — initialises the single ordered stream owned by the entropy stage.

### `phx_bs_pos` — `common/bitstream.h:100` (`bs_pos`)
- **Inputs:** `const phx_bs_t *s`.
- **Outputs:** `8*(p - p_start) + bit`.
- **Mutates:** nothing.
- **Determinism / seek-safety:** pure read of the current cursor; position defined only relative to the current stream.
- **SHADER: C** — observes the serial cursor; used by header writers and bit accounting.

### `phx_bs_write` — `common/bitstream.h:125` (`bs_write`)
- **Inputs:** `phx_bs_t *s`, `count`, `uint32_t bits`.
- **Outputs:** MSB-first bits appended to the stream.
- **Mutates:** `s->p`, `s->bit` and bytes in the output buffer.
- **Determinism / seek-safety:** strictly ordered; reproducible only from a known stream start state.
- **SHADER: C** — mutates the single evolving stream; cannot be run per work item.

### `phx_bs_write1` — `common/bitstream.h:169` (`bs_write1`)
- **Inputs:** `phx_bs_t *s`, `uint32_t bit`.
- **Outputs:** one bit appended.
- **Mutates:** `s->p`, `s->bit`, one byte in the buffer.
- **Determinism / seek-safety:** ordered; needs current cursor.
- **SHADER: C** — same serial write as `phx_bs_write`.

### `phx_bs_align_0` — `common/bitstream.h:182` (`bs_align_0`)
- **Inputs:** `phx_bs_t *s`.
- **Outputs:** flushes a partial byte (`phx_bitstream.c:39–42`).
- **Mutates:** `s->p`, `s->bit`.
- **Determinism / seek-safety:** ordered; needs current cursor.
- **SHADER: C** — serial stream alignment.

### `phx_bs_align_10` — `common/bitstream.h:192` (`bs_align_10`)
- **Inputs:** `phx_bs_t *s`.
- **Outputs:** a `1` bit then zero padding to byte boundary.
- **Mutates:** `s->p`, `s->bit`, output bytes.
- **Determinism / seek-safety:** ordered; needs current cursor.
- **SHADER: C** — serial stream alignment.

### `phx_bs_size_ue` — `common/bitstream.h:278` (`bs_size_ue`)
- **Inputs:** `unsigned val`.
- **Outputs:** `phx_ue_size_tab[val+1]`.
- **Mutates:** nothing.
- **Determinism / seek-safety:** pure lookup; fully seek-safe.
- **SHADER: A** — per-value pure query; no stream state.

### `phx_bs_size_se` — `common/bitstream.h:291` (`bs_size_se`)
- **Inputs:** `int val`.
- **Outputs:** signed Exp-Golomb bit size, with the `>>8` branch for large magnitudes (`phx_bitstream.c:58–64`).
- **Mutates:** nothing.
- **Determinism / seek-safety:** pure arithmetic + lookup; fully seek-safe.
- **SHADER: A** — per-value pure query; no stream state.

### `phx_bs_write_ue_big` — `common/bitstream.h:221` (`bs_write_ue_big`)
- **Inputs:** `phx_bs_t *s`, `unsigned val`.
- **Outputs:** Exp-Golomb code for arbitrary values (two writes, `phx_bitstream.c:67–76`).
- **Mutates:** serial cursor and output bytes.
- **Determinism / seek-safety:** ordered; needs current cursor.
- **SHADER: C** — emits bits into the single stream.

### `phx_bs_write_ue` — `common/bitstream.h:241` (`bs_write_ue`)
- **Inputs:** `phx_bs_t *s`, `unsigned val` (`val < 255`).
- **Outputs:** one Exp-Golomb code.
- **Mutates:** serial cursor and output bytes.
- **Determinism / seek-safety:** ordered; needs current cursor.
- **SHADER: C** — emits bits into the single stream.

### `phx_bs_write_se` — `common/bitstream.h:246` (`bs_write_se`)
- **Inputs:** `phx_bs_t *s`, `int val`.
- **Outputs:** signed Exp-Golomb (`phx_bitstream.c:85–94`).
- **Mutates:** serial cursor and output bytes.
- **Determinism / seek-safety:** ordered; needs current cursor.
- **SHADER: C** — emits bits into the single stream.

### `phx_bs_write_te` — `common/bitstream.h:264` (`bs_write_te`)
- **Inputs:** `phx_bs_t *s`, `int x`, `int val`.
- **Outputs:** truncated bit (`x == 1`) or Exp-Golomb.
- **Mutates:** serial cursor and output bytes.
- **Determinism / seek-safety:** ordered; needs current cursor.
- **SHADER: C** — emits bits into the single stream.

### `phx_bs_rbsp_trailing` — `common/bitstream.h:272` (`bs_rbsp_trailing`)
- **Inputs:** `phx_bs_t *s`.
- **Outputs:** `1` bit plus zeros to byte boundary (`phx_bitstream.c:104–109`).
- **Mutates:** serial cursor and output bytes.
- **Determinism / seek-safety:** ordered; needs current cursor.
- **SHADER: C** — terminates an RBSP in the serial stage.

### `phx_nal_escape` — `common/bitstream.c:29` (`nal_escape_c`)
- **Inputs:** `uint8_t *dst`, `uint8_t *src`, `uint8_t *end`.
- **Outputs:** `dst` end pointer; inserts `0x03` before `00 00 {0,1,2,3}` using the two previous output bytes (`phx_bitstream.c:112–122`).
- **Mutates:** destination buffer; `src` read-only.
- **Determinism / seek-safety:** deterministic from the NAL payload start; the 2-byte lookbehind means it is **not** seek-safe at an arbitrary byte offset (recompute from NAL start).
- **SHADER: C** — explicitly listed as "NAL assembly / emulation prevention | `common/bitstream.c` | single output stream" (`SHADER_MAPPING.md:52`).

### `phx_nal_encode` — `common/bitstream.c:55` (`x264_nal_encode`)
- **Inputs:** `uint8_t *dst`, `phx_nal_t *nal` (payload, size, ref_idc, type, long-startcode flag), `int b_annexb`.
- **Outputs:** Annex-B start code + NAL header + escaped payload; updates `nal->i_payload` and `nal->p_payload` (`phx_bitstream.c:125–140`).
- **Mutates:** `nal`, destination buffer.
- **Determinism / seek-safety:** deterministic from the NAL payload; start at NAL boundary.
- **SHADER: C** — single ordered output stream; part of the serial NAL-assembly stage.

---

## 7. Module `cabac` — `src/phx_cabac.c`

The doc's Class C exemplar: "`x264_cabac_encode_decision*` … arithmetic coder
`i_low/i_range` + context state" (`SHADER_MAPPING.md:47`). One helper
(`phx_clz`) is a pure integer primitive and is A.

### `phx_cabac_context_init` — `common/cabac.c:32/48` (`x264_cabac_init` / `x264_cabac_context_init`, inlined)
- **Inputs:** `phx_cabac_t *cb`, `slice_type`, `qp`, `model`.
- **Outputs:** `cb->state[0..PHX_CABAC_CTX_COUNT-1]` (460 active contexts, array size 1024) from the I or PB init table (`phx_cabac.c:11–23`).
- **Mutates:** `cb->state` only.
- **Determinism / seek-safety:** pure function of `(slice_type, qp, model)`; rerun at every slice start, so fully seek-safe from a slice boundary.
- **SHADER: C** — per-context arithmetic is independent, but the array is the live context state of the serial coder and must be installed by the entropy stage before any bin is encoded.

### `phx_cabac_encode_init_core` — `common/cabac.c:53` (`x264_cabac_encode_init_core`)
- **Inputs:** `phx_cabac_t *cb`.
- **Outputs:** `i_low = 0`, `i_range = 0x01FE`, `i_queue = -9`, `i_bytes_outstanding = 0`.
- **Mutates:** `cb` arithmetic state.
- **Determinism / seek-safety:** deterministic reset; required before encoding a slice.
- **SHADER: C** — serial coder reset; no work-item mapping.

### `phx_cabac_encode_init` — `common/cabac.c:61` (`x264_cabac_encode_init`)
- **Inputs:** `phx_cabac_t *cb`, `uint8_t *data`, `uint8_t *end`.
- **Outputs:** core reset plus `p_start = p = data`, `p_end = end`.
- **Mutates:** `cb`.
- **Determinism / seek-safety:** deterministic reset at slice start.
- **SHADER: C** — binds the serial coder to its output buffer.

### `phx_cabac_putbyte` (static) — `common/cabac.c:69` (`cabac_putbyte`)
- **Inputs:** `phx_cabac_t *cb`.
- **Outputs:** flushes one byte when `i_queue >= 0`, handles `0xFF` carry propagation and `i_bytes_outstanding` (`phx_cabac.c:41–63`).
- **Mutates:** `cb->i_low`, `i_queue`, `p`, and the output bytes (including back-patching `p[-1]`).
- **Determinism / seek-safety:** strictly ordered; depends on the whole preceding bin stream.
- **SHADER: C** — the heart of the arithmetic coder's serial byte emission.

### `phx_cabac_encode_renorm` (static) — `common/cabac.c:101` (`cabac_encode_renorm`)
- **Inputs:** `phx_cabac_t *cb`.
- **Outputs:** renormalised `i_range`/`i_low` via `phx_cabac_renorm_shift`, then `putbyte`.
- **Mutates:** `cb` arithmetic state and output bytes.
- **Determinism / seek-safety:** ordered; depends on current coder state.
- **SHADER: C** — part of the serial arithmetic coder.

### `phx_cabac_encode_decision` — `common/cabac.c:113` (`x264_cabac_encode_decision_c`)
- **Inputs:** `phx_cabac_t *cb`, `int ctx`, `int b`.
- **Outputs:** one context-coded bin; updates `cb->state[ctx]` via `phx_cabac_transition` (`phx_cabac.c:75–85`).
- **Mutates:** `cb->i_range`, `i_low`, `state[ctx]`, plus queue/output through renorm.
- **Determinism / seek-safety:** strictly serial; only reproducible from slice start in scan order.
- **SHADER: C** — the doc's named example: "`x264_cabac_encode_decision*` … `i_low/i_range` + context state" (`SHADER_MAPPING.md:47`).

### `phx_cabac_encode_bypass` — `common/cabac.c:128` (`x264_cabac_encode_bypass_c`)
- **Inputs:** `phx_cabac_t *cb`, `int b` (negated by caller, per `phx_cabac.c:87`).
- **Outputs:** one bypass bin.
- **Mutates:** `i_low`, `i_queue`, output bytes.
- **Determinism / seek-safety:** strictly serial; depends on current coder state.
- **SHADER: C** — serial arithmetic coder operation.

### `phx_clz` (static) — `common/osdep.h:514` (`x264_clz`)
- **Inputs:** `uint32_t v` (nonzero).
- **Outputs:** count of leading zero bits.
- **Mutates:** nothing.
- **Determinism / seek-safety:** pure bit operation; fully seek-safe.
- **SHADER: A** — pure integer primitive; may be inlined into the C-stage `ue_bypass` call but has no state itself.

### `phx_cabac_encode_ue_bypass` — `common/cabac.c:142` (`x264_cabac_encode_ue_bypass`)
- **Inputs:** `phx_cabac_t *cb`, `int exp_bits`, `int val`.
- **Outputs:** Exp-Golomb bypass bins using `bypass_lut` (`cabac.c:136–140`) and a loop of up to 8-bit chunks (`phx_cabac.c:112–127`).
- **Mutates:** `i_low`, `i_queue`, output bytes.
- **Determinism / seek-safety:** strictly serial; depends on current coder state.
- **SHADER: C** — serial arithmetic coder operation.

### `phx_cabac_encode_terminal` — `common/cabac.c:159` (`x264_cabac_encode_terminal_c`)
- **Inputs:** `phx_cabac_t *cb`.
- **Outputs:** terminal bin (`i_range -= 2`) then renormalise.
- **Mutates:** `cb` arithmetic state and output bytes.
- **Determinism / seek-safety:** strictly serial; depends on current coder state.
- **SHADER: C** — serial arithmetic coder operation.

### `phx_cabac_encode_flush` — `common/cabac.c:165` (`x264_cabac_encode_flush`)
- **Inputs:** `phx_cabac_t *cb`, `int frame` (only `frame & 31` is used, for the termination bit at `cabac.c:174`).
- **Outputs:** final bytes of the slice, including outstanding `0xFF` bytes.
- **Mutates:** `i_low`, `i_queue`, `i_bytes_outstanding`, `p`, output bytes.
- **Determinism / seek-safety:** strictly serial and frame-number dependent; reproducible only from slice start with the correct frame number.
- **SHADER: C** — ends the serial CABAC stream; its frame-dependent bit is one of the exact-parity details a shader cannot guess.

---

## 8. Module `headers` — `src/phx_headers.c`

SPS/PPS init and write from `encoder/set.c`, plus NAL assembly mimicking
`x264_encoder_headers`. All emit or prepare one ordered stream, so all are C.

### `phx_sps_init` — `encoder/set.c:101` (`x264_sps_init`)
- **Inputs:** `phx_sps_t *s`, `const phx_param_t *p`, `int width`, `int height`.
- **Outputs:** SPS fields for 8-bit 4:2:0 progressive fixtures: mb dimensions, profile/level, refs/reorder, log2 frame-num/POC limits, VUI timing (`phx_headers.c:13–73`).
- **Mutates:** `s` only.
- **Determinism / seek-safety:** pure function of param + dimensions; recomputable at any time.
- **SHADER: C** — host-only header preparation; its result is written once by the serial stage.

### `phx_pps_init` — `encoder/set.c:479` (`x264_pps_init`)
- **Inputs:** `phx_pps_t *p`, `const phx_sps_t *s`, `const phx_param_t *param`.
- **Outputs:** PPS fields: CABAC flag, ref counts, weighted-pred flags, initial QP, chroma QP offset, transform-8x8 mode (`phx_headers.c:76–95`).
- **Mutates:** `p` only.
- **Determinism / seek-safety:** pure function of its inputs; recomputable at any time.
- **SHADER: C** — host-only header preparation.

### `phx_sps_write` — `encoder/set.c:305` (`x264_sps_write`)
- **Inputs:** `phx_bs_t *b`, `const phx_sps_t *s`.
- **Outputs:** SPS RBSP bit sequence (`set.c:308–475`).
- **Mutates:** `b` (cursor + output buffer).
- **Determinism / seek-safety:** ordered writes; start from a fresh `bs_init` (seek-safe at NAL boundary).
- **SHADER: C** — emits one bitstream in field order; no per-work-item mapping.

### `phx_pps_write` — `encoder/set.c:505` (`x264_pps_write`)
- **Inputs:** `phx_bs_t *b`, `const phx_sps_t *s` (unused today), `const phx_pps_t *p`.
- **Outputs:** PPS RBSP bit sequence (`set.c:508–570`; the scaling-list branch for 8x8dct was added in `phx_headers.c:183–192`).
- **Mutates:** `b` (cursor + output buffer).
- **Determinism / seek-safety:** ordered writes; start from a fresh `bs_init`.
- **SHADER: C** — emits one bitstream in field order.

### `phx_encoder_headers` — `encoder/api.c:159` / `encoder/encoder.c:2096` (`x264_encoder_headers`)
- **Inputs:** `uint8_t *dst`, `int capacity`, `const phx_param_t *p`, `int width`, `int height`.
- **Outputs:** SPS NAL followed by PPS NAL in `dst`; returns byte count or `-1` on overflow (`phx_headers.c:197–220`).
- **Mutates:** `dst`, local `sps`, `pps`, `rbsp`, `bs`, `nal`.
- **Determinism / seek-safety:** pure function of param + dimensions; a complete header bundle can be regenerated at any point in a stream.
- **SHADER: C** — "NAL assembly / emulation prevention … single output stream" (`SHADER_MAPPING.md:52`); belongs to the serial header/entropy stage.

---

## 9. Module `dct` — `src/phx_dct.c` (added during the parallel port)

Every transform in this file is Class A: the file header states
`SHADER: A for every transform: one invocation per block, no hidden state`,
matching `SHADER_MAPPING.md:14` ("DCT 4x4 / 8x8, Hadamard / IDCT — one
invocation per block"). `phx_dctcoef` is the coefficient type from
`phx_dct.h`. All rows: outputs are explicit, no globals are touched, and each
function is seek-safe because it depends only on its argument block/pixel
pairs.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_clip_pixel` (static) | `common/common.h:145–148` | `int x` | clamped `uint8_t` | nothing | pure clamp; any seek | **A** — per-pixel pure primitive (also defined in `phx_predict.c`) |
| `phx_pixel_sub_wxh` (static) | `common/dct.c:145–155` | `diff`, `size`, `pix1, i_pix1`, `pix2, i_pix2` | residual `diff[]` | `diff[]` only | pure 2-plane subtraction; any seek | **A** — independent per sample |
| `phx_dct4x4dc` | `common/dct.c:47–76` | `d[16]` | 4x4 Hadamard with `/2` rounding | `d[16]` (local `tmp`) | pure; any seek | **A** — one invocation per 16-coefficient DC block |
| `phx_idct4x4dc` | `common/dct.c:78–107` | `d[16]` | inverse Hadamard, no `/2` | `d[16]` | pure; any seek | **A** — one invocation per DC block |
| `phx_dct2x4dc` | `common/dct.c:109–143` | `dct[8]`, `dct4x4[8][16]` | chroma DC Hadamard into `dct[8]` | `dct[8]` and `dct4x4[i][0]` zeroed | pure; any seek | **A** — one invocation per chroma DC block |
| `phx_sub4x4_dct` | `common/dct.c:157–189` | `dct[16]`, `pix1, i_pix1`, `pix2, i_pix2` | 4x4 residual DCT | `dct[16]` | pure; any seek | **A** — one invocation per 4x4 block |
| `phx_sub8x8_dct` | `common/dct.c:191–197` | `dct[4][16]`, `pix1, i_pix1`, `pix2, i_pix2` | four 4x4 DCTs | `dct[4][16]` | pure; any seek | **A** — one invocation per 8x8 block |
| `phx_sub16x16_dct` | `common/dct.c:199–205` | `dct[16][16]`, `pix1, i_pix1`, `pix2, i_pix2` | sixteen 4x4 DCTs | `dct[16][16]` | pure; any seek | **A** — one invocation per 16x16 block |
| `phx_sub4x4_dct_dc` | `common/dct.c:207–214` | `pix1, i_pix1`, `pix2, i_pix2` | `int` DC sum | nothing | pure; any seek | **A** — one invocation per 4x4 DC block |
| `phx_sub8x8_dct_dc` | `common/dct.c:216–232` | `dct[4]`, `pix1, i_pix1`, `pix2, i_pix2` | 2x2 DC transform | `dct[4]` | pure; any seek | **A** — one invocation per 8x8 DC block |
| `phx_sub8x16_dct_dc` | `common/dct.c:234–270` | `dct[8]`, `pix1, i_pix1`, `pix2, i_pix2` | 2x4 DC transform | `dct[8]` | pure; any seek | **A** — one invocation per 8x16 DC block |
| `phx_add4x4_idct` | `common/dct.c:272–310` | `dst`, `stride`, `dct[16]` | reconstructed 4x4 added into `dst` | `dst` pixels | pure; any seek | **A** — one invocation per 4x4 block |
| `phx_add8x8_idct` | `common/dct.c:312–318` | `dst`, `stride`, `dct[4][16]` | four 4x4 IDCTs added | `dst` pixels | pure; any seek | **A** — one invocation per 8x8 block |
| `phx_add16x16_idct` | `common/dct.c:320–326` | `dst`, `stride`, `dct[16][16]` | sixteen 4x4 IDCTs added | `dst` pixels | pure; any seek | **A** — one invocation per 16x16 block |
| `phx_sub8x8_dct8` | `common/dct.c:359–378` | `dct[64]`, `pix1, i_pix1`, `pix2, i_pix2` | 8x8 DCT8 | `dct[64]` (local `tmp`) | pure; any seek | **A** — one invocation per 8x8 block, registers/shared for the butterfly |
| `phx_sub16x16_dct8` | `common/dct.c:380–386` | `dct[4][64]`, `pix1, i_pix1`, `pix2, i_pix2` | four 8x8 DCT8s | `dct[4][64]` | pure; any seek | **A** — one invocation per 16x16 block |
| `phx_add8x8_idct8` | `common/dct.c:415–432` | `dst`, `stride`, `dct[64]` | 8x8 IDCT8 added into `dst` | `dst` pixels **and `dct[64]` in place** (`dct[0] += 32`, butterflies) | pure; any seek | **A** — one invocation per block; shader must copy coefficients into registers first |
| `phx_add16x16_idct8` | `common/dct.c:434–440` | `dst`, `stride`, `dct[4][64]` | four 8x8 IDCT8s | `dst` and each `dct[i][]` in place | pure; any seek | **A** — one invocation per 16x16 block |
| `phx_add4x4_idct_dc` | `common/dct.c:442–452` | `dst`, `stride`, `dc` | DC-only 4x4 added | `dst` pixels | pure; any seek | **A** — one invocation per DC-only block |
| `phx_add8x8_idct_dc` | `common/dct.c:454–460` | `dst`, `stride`, `dct[4]` | DC-only 8x8 added | `dst` pixels | pure; any seek | **A** — one invocation per DC-only block |
| `phx_add16x16_idct_dc` | `common/dct.c:462–471` | `dst`, `stride`, `dct[16]` | DC-only 16x16 added | `dst` pixels | pure; any seek | **A** — one invocation per DC-only block |

## 10. Module `predict` — `src/phx_predict.c` (added during the parallel port)

File header: `SHADER: A for all predictors: one invocation per block, no hidden
state`, matching `SHADER_MAPPING.md:16` ("Predictors … neighbour samples via
image loads"). For every predictor row below, `dst` points at the top-left of
the block **inside a reconstructed plane that already carries the left/top
neighbours at negative offsets**; it both reads neighbours and writes the
block. They are pure given those samples, need no other frame, and are
seek-safe once the B-stage neighbour reconstruction has run. `edge[36]` is the
filtered 8x8 edge array produced by `phx_predict_8x8_filter`.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_clip_pixel` (static) | `common/common.h:145–148` | `int x` | clamped `uint8_t` | nothing | pure; any seek | **A** — per-pixel clamp helper |
| `phx_store4` (static) | `common/predict.c:531–532` helper | `dst`, `pa[2]`, `pb[2]` | four packed bytes | `dst[0..3]` | pure; any seek | **A** — per-4-pixel store |
| `phx_predict_16x16_dc` | `common/predict.c:67–79` | `dst, stride` | 16x16 block, average of 32 neighbours | `dst` block | pure from neighbours; frame-local | **A** — one invocation per 16x16 block |
| `phx_predict_16x16_dc_left` | `common/predict.c:80–89` | `dst, stride` | block from 16 left samples | `dst` block | pure from neighbours | **A** — per block |
| `phx_predict_16x16_dc_top` | `common/predict.c:90–99` | `dst, stride` | block from 16 top samples | `dst` block | pure from neighbours | **A** — per block |
| `phx_predict_16x16_dc_128` | `common/predict.c:100–103` | `dst, stride` | constant 128 fill | `dst` block | constant; any seek | **A** — per block |
| `phx_predict_16x16_h` | `common/predict.c:104–115` | `dst, stride` | left column broadcast | `dst` block | pure from neighbours | **A** — per block |
| `phx_predict_16x16_v` | `common/predict.c:116–131` | `dst, stride` | top row copied down | `dst` block | pure from neighbours | **A** — per block |
| `phx_predict_16x16_p` | `common/predict.c:132–160` | `dst, stride` | planar H/V interpolation + clip | `dst` block | pure from neighbours | **A** — per block |
| `phx_predict_8x8c_dc_128` | `common/predict.c:167–175` | `dst, stride` | 8x8 chroma constant fill | `dst` block | constant; any seek | **A** — per chroma block |
| `phx_predict_8x8c_dc_left` | `common/predict.c:176–201` | `dst, stride` | chroma DC from left | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x8c_dc_top` | `common/predict.c:202–220` | `dst, stride` | chroma DC from top | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x8c_dc` | `common/predict.c:221–259` | `dst, stride` | chroma DC from both | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x8c_h` | `common/predict.c:260–269` | `dst, stride` | chroma horizontal | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x8c_v` | `common/predict.c:270–281` | `dst, stride` | chroma vertical | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x8c_p` | `common/predict.c:282–308` | `dst, stride` | chroma planar | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x16c_dc_128` | `common/predict.c:314–322` | `dst, stride` | 8x16 chroma constant fill | `dst` block | constant; any seek | **A** — per chroma block |
| `phx_predict_8x16c_dc_left` | `common/predict.c:323–341` | `dst, stride` | 8x16 DC, four 4-row strips | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x16c_dc_top` | `common/predict.c:342–360` | `dst, stride` | 8x16 DC from top | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x16c_dc` | `common/predict.c:361–420` | `dst, stride` | 8x16 DC from both | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x16c_h` | `common/predict.c:421–430` | `dst, stride` | 8x16 horizontal | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x16c_v` | `common/predict.c:431–442` | `dst, stride` | 8x16 vertical | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_8x16c_p` | `common/predict.c:443–469` | `dst, stride` | 8x16 planar | `dst` block | pure from neighbours | **A** — per chroma block |
| `phx_predict_4x4_dc_128` | `common/predict.c:481–484` | `dst, stride` | constant 128 fill | `dst` block | constant; any seek | **A** — per 4x4 block |
| `phx_predict_4x4_dc_left` | `common/predict.c:485–489` | `dst, stride` | DC from left | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_4x4_dc_top` | `common/predict.c:490–494` | `dst, stride` | DC from top | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_4x4_dc` | `common/predict.c:495–500` | `dst, stride` | DC from both | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_4x4_h` | `common/predict.c:501–507` | `dst, stride` | horizontal | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_4x4_v` | `common/predict.c:508–511` | `dst, stride` | vertical | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_4x4_ddl` | `common/predict.c:534–545` | `dst, stride` | down-left diagonal | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_4x4_ddr` | `common/predict.c:546–558` | `dst, stride` | down-right diagonal | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_4x4_vr` | `common/predict.c:560–575` | `dst, stride` | vertical-right | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_4x4_hd` | `common/predict.c:577–592` | `dst, stride` | horizontal-down | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_4x4_vl` | `common/predict.c:594–608` | `dst, stride` | vertical-left | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_4x4_hu` | `common/predict.c:610–621` | `dst, stride` | horizontal-up | `dst` block | pure from neighbours | **A** — per 4x4 block |
| `phx_predict_8x8_filter` | `common/predict.c:627–675` | `dst, edge[36], stride, i_neighbor, i_filters` | filtered left/top edges in `edge[]` | `edge[0..32]` only | pure from neighbours + availability flags | **A** — one invocation per 8x8 block; edge is invocation-local |
| `phx_predict_8x8_fill` (static) | `common/predict.c:693–698` | `dst, stride, v` | 8x8 constant fill | `dst` block | constant; any seek | **A** — helper for the 8x8 DC modes |
| `phx_predict_8x8_dc_128` | `common/predict.c:700–703` | `dst, edge[36], stride` | 8x8 constant 128 | `dst` block | constant; any seek | **A** — per 8x8 block |
| `phx_predict_8x8_dc_left` | `common/predict.c:704–709` | `dst, edge[36], stride` | DC from `edge[7..14]` | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_8x8_dc_top` | `common/predict.c:710–715` | `dst, edge[36], stride` | DC from `edge[16..23]` | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_8x8_dc` | `common/predict.c:716–722` | `dst, edge[36], stride` | DC from left+top | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_8x8_h` | `common/predict.c:723–730` | `dst, edge[36], stride` | horizontal from edge | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_8x8_v` | `common/predict.c:731–740` | `dst, edge[36], stride` | vertical from edge | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_8x8_ddl` | `common/predict.c:741–760` | `dst, edge[36], stride` | down-left diagonal, `k = x+y` | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_8x8_ddr` | `common/predict.c:761–782` | `dst, edge[36], stride` | down-right diagonal | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_8x8_vr` | `common/predict.c:783–810` | `dst, edge[36], stride` | vertical-right | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_8x8_hd` | `common/predict.c:811–837` | `dst, edge[36], stride` | horizontal-down | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_8x8_vl` | `common/predict.c:838–864` | `dst, edge[36], stride` | vertical-left, `k = x+(y>>1)` | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_8x8_hu` | `common/predict.c:865–884` | `dst, edge[36], stride` | horizontal-up | `dst` block | pure from edge | **A** — per 8x8 block |
| `phx_predict_16x16_init` | `common/predict.c:889–898` | `phx_predict_t pf[7]` | seven 16x16 predictor entries in `pf` | `pf[]` | constant table; any seek | **B** — file annotation: dispatch table consumed by ordered per-MB stages (entries are A) |
| `phx_predict_8x8c_init` | `common/predict.c:936–945` | `phx_predict_t pf[7]` | seven chroma predictor entries | `pf[]` | constant table; any seek | **B** — same dispatch-table rationale |
| `phx_predict_8x16c_init` | `common/predict.c:977–986` | `phx_predict_t pf[7]` | seven 8x16 predictor entries | `pf[]` | constant table; any seek | **B** — same dispatch-table rationale |
| `phx_predict_4x4_init` | `common/predict.c:1042–1055` | `phx_predict_t pf[12]` | twelve 4x4 predictor entries | `pf[]` | constant table; any seek | **B** — same dispatch-table rationale |
| `phx_predict_8x8_init` | `common/predict.c:1000–1013` | `phx_predict8x8_t pf[12]`, `void (**filter)(...)` | twelve 8x8 entries plus the filter pointer | `pf[]`, `*filter` | constant table; any seek | **B** — same dispatch-table rationale |

## 11. Module `encoder` — `src/phx_encoder.c` (added during the parallel port)

The public serial envelope, following `encoder/api.c:76/130/159/166` and
`encoder/encoder.c:1507/2096/3323/4196`. File header:
`SHADER: C — serial host pipeline; every CABAC write happens in one thread`.

### `phx_encoder_validate` (static) — `encoder/encoder.c:423` (from `:1552`)
- **Inputs:** `const phx_param_t *p`.
- **Outputs:** `0` for the supported I420 / 16-multiple / ≤4096 geometry, else `-1` (`phx_encoder.c:20–28`).
- **Mutates:** nothing.
- **Determinism / seek-safety:** pure predicate; any time.
- **SHADER: C** — one-time host validation; no work item.

### `phx_encoder_release` (static) — `encoder/encoder.c:4196`, `:4518`, `:4520`, `:4527–4530`; `api.c:135`
- **Inputs:** `phx_t *h` (NULL tolerated).
- **Outputs:** none; frees cost tables, MB state, frame, bitstream and handle.
- **Mutates:** heap owned by `h`.
- **Determinism / seek-safety:** position-independent teardown.
- **SHADER: C** — serial lifecycle, shared by close and open-failure paths.

### `phx_encoder_open` — `encoder/api.c:76` → `encoder/encoder.c:1507`
- **Inputs:** `const phx_param_t *p`.
- **Outputs:** `phx_t *h` or `NULL`: private param copy (`:1516`), finalize (`:1218–1231`), SPS/PPS init (`:1579/:1581`), frame + MB state + bitstream allocations, cost tables (`:1708`), CABAC context/core init (`:1668–1669`), frame counters (`:1563–1564/:1630`).
- **Mutates:** newly allocated `h` and its buffers.
- **Determinism / seek-safety:** deterministic from `p`; creates the fresh state a seek needs.
- **SHADER: C** — serial host initialisation of UBO 0 / SSBO 0–2 / entropy state.

### `phx_open` — `encoder/api.c:76` (`x264_encoder_open`)
- **Inputs:** `const phx_param_t *p`.
- **Outputs:** forwards `phx_encoder_open(p)` (`phx_encoder.c:100–103`; alias per `ORACLE_DESIGN.md:40`).
- **Mutates:** via the forwarded call only.
- **Determinism / seek-safety:** same as `phx_encoder_open`.
- **SHADER: C** — alias of the serial open path.

### `phx_headers` — `encoder/api.c:159` → `encoder/encoder.c:2096–2133`
- **Inputs:** `phx_t *h`, `phx_nal_t **out`, `int *nnal`.
- **Outputs:** two NAL descriptors (SPS type 7, PPS type 8, both `nal_ref_idc = 3`, long start code) split from `h->header_bytes`; returns byte count (`phx_encoder.c:113–150`). SEI (`encoder.c:2118–2122`) is deliberately absent.
- **Mutates:** `h->header_nal[]`, `*out`, `*nnal`.
- **Determinism / seek-safety:** regenerable from `h->param`; seek-safe at stream start.
- **SHADER: C** — serial header/NAL stage; single ordered output block.

### `phx_encoder_header_bytes` — `encoder/encoder.c:2096`; `phx_headers.h:53`
- **Inputs:** `phx_t *h`, `uint8_t *dst`, `int capacity`.
- **Outputs:** Annex-B SPS+PPS bytes; returns count or `-1`.
- **Mutates:** `dst`.
- **Determinism / seek-safety:** same as `phx_encoder_headers`.
- **SHADER: C** — serial byte-form header emission.

### `phx_encode` — `encoder/api.c:166` → `encoder/encoder.c:3323`
- **Inputs:** `phx_t *h`, `const phx_picture_t *pic`, `phx_nal_t **out`, `int *nnal`, `uint8_t **recon`.
- **Outputs:** currently only the "no data out" envelope (`encoder.c:3354–3356`): nulls `*out`, `*nnal`, `*recon` and returns `PHX_ERR_UNIMPLEMENTED`; the six stage calls are documented TODOs at `phx_encoder.c:168–189`.
- **Mutates:** the three output pointers only.
- **Determinism / seek-safety:** intentionally no hidden per-frame state yet; seek-safe by construction.
- **SHADER: C** — the completed pipeline must remain single-threaded/seek-safe (`phx_encoder.c:191`).

### `phx_encoder_encode` — `encoder/api.c:166` / `encoder/encoder.c:3323`
- **Inputs:** same as `phx_encode`.
- **Outputs:** forwards `phx_encode` (`phx_encoder.c:206–210`).
- **Mutates:** via the forwarded call only.
- **Determinism / seek-safety:** same as `phx_encode`.
- **SHADER: C** — mirror-name alias of the serial pipeline.

### `phx_encoder_close` — `encoder/api.c:130` → `encoder/encoder.c:4196`
- **Inputs:** `phx_t *h`.
- **Outputs:** none; calls `phx_encoder_release`.
- **Mutates:** heap owned by `h`.
- **Determinism / seek-safety:** position-independent teardown.
- **SHADER: C** — serial teardown.

### `phx_close` — `encoder/api.c:130` (`x264_encoder_close`)
- **Inputs:** `phx_t *h`.
- **Outputs:** forwards `phx_encoder_close` (`phx_encoder.c:222–225`; alias per `ORACLE_DESIGN.md:45`).
- **Mutates:** via the forwarded call only.
- **Determinism / seek-safety:** same as `phx_encoder_close`.
- **SHADER: C** — alias of the serial close path.

---

---

## 12. Module `cabac_syntax` — `src/phx_cabac_syntax.c` (added during the parallel port)

File header: `SHADER: C — strictly serial bin order, keep in the entropy
stage`. Every function emits bins through `phx_cabac_encode_decision` (the
`DEC`/`DEC_NOUP` macros, `cabac.h:92`) and uses literal x264 context indices;
there is no second arithmetic coder. Inputs below always include the live
`phx_cabac_t *cb`, so none is seek-safe mid-slice — each is reproducible only
from a slice start (see §7, `cabac`).

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_coeff_last4` (static) | `common/quant.c:366–373` | `l[4]` | index of last nonzero | nothing | pure scan; any seek | **C** — coefficient-last helper for the serial residual writer |
| `phx_coeff_last8` (static) | `common/quant.c:366–373` | `l[8]` | last nonzero index | nothing | pure scan | **C** — same helper family |
| `phx_coeff_last15` (static) | `common/quant.c:366–373` | `l[15]` | last nonzero index | nothing | pure scan | **C** — same helper family |
| `phx_coeff_last16` (static) | `common/quant.c:366–373` | `l[16]` | last nonzero index | nothing | pure scan | **C** — same helper family |
| `phx_coeff_last64` (static) | `common/quant.c:366–373` | `l[64]` | last nonzero index | nothing | pure scan | **C** — same helper family |
| `phx_cabac_mb_init` | `quant.c:437–441/878–882`, `encoder.c:1446` | `phx_cabac_mb_t *mb`, `phx_mb_cache_t *cache` | zeroed MB syntax state + `coeff_last[]` function table | `mb` only | deterministic per slice start | **C** — serial stage setup for the MB syntax writer |
| `phx_predict_intra4x4_mode` (static) | `common/macroblock.h:420–431` | `mb`, `idx` | predicted I4 mode from left/top | nothing | pure from neighbour cache | **C** — supplies the serial `intra4x4_pred_mode` bin |
| `phx_cabac_mb_type_intra` | `encoder/cabac.c:35–67` | `cb`, `mb`, `i_mb_type`, slice params | MB-type bins | `cb` state/cursor | ordered; slice start only | **C** — `cabac_mb_type_intra`, serial bins |
| `phx_cabac_intra4x4_pred_mode` | `encoder/cabac.c:83–96` | `cb`, `i_pred`, `i_mode` | prediction-mode bins | `cb` | ordered; slice start only | **C** — serial syntax |
| `phx_cabac_intra_chroma_pred_mode` | `encoder/cabac.c:98–116` | `cb`, `mb` | chroma prediction-mode bins | `cb` | ordered; slice start only | **C** — serial syntax |
| `phx_cabac_subpartition_p` | `encoder/cabac.c:198–213` | `cb`, `i_sub` | P subpartition bins | `cb` | ordered; slice start only | **C** — `cabac_subpartition_p` |
| `phx_cabac_subpartition_b` | `encoder/cabac.c:215–233` | `cb`, `i_sub` | B subpartition bins | `cb` | ordered; slice start only | **C** — `cabac_subpartition_b` |
| `phx_cabac_transform_size` | `encoder/cabac.c:235–239` | `cb`, `mb` | 8x8-transform bin | `cb` | ordered; slice start only | **C** — serial syntax |
| `phx_cabac_cbp_luma` | `encoder/cabac.c:118–127` | `cb`, `mb` | luma CBP bins | `cb` | ordered; slice start only | **C** — `cabac_cbp_luma` |
| `phx_cabac_cbp_chroma` | `encoder/cabac.c:129–148` | `cb`, `mb` | chroma CBP bins | `cb` | ordered; slice start only | **C** — `cabac_cbp_chroma` |
| `phx_cabac_qp_delta` | `encoder/cabac.c:150–186` | `cb`, `mb` | QP-delta bins | `cb`, `mb->i_qp` | ordered; slice start only | **C** — serial syntax |
| `phx_cabac_mb_skip` | `encoder/cabac.c:189–195` | `cb`, `mb`, `b_skip` | skip bin | `cb` | ordered; slice start only | **C** — `x264_cabac_mb_skip` |
| `phx_predict_mv` (static) | `common/mvpred.c:30–127` | `mb`, `i_list`, `idx`, `ref[]` | predicted MV | nothing | pure from neighbour MVs | **C** — feeds serial MVD bins |
| `phx_cabac_mvd_sum` (static) | `common/base.h:259–266` | `left[2]`, `top[2]` | packed MVD sum | nothing | pure integer helper | **C** — serial MVD context derivation |
| `phx_cache_mvd` (static) | `common/rectangle.h:137–144` | `mb`, `idx`, `width/height`, list, delta | updated MVD cache | `mb->cache->mvd` | ordered within MB | **C** — `x264_macroblock_cache_mvd` |
| `phx_cabac_mvd_cpn` | `encoder/cabac.c:270–327` | `cb`, `mb`, list, idx, delta | MVD component bins | `cb`, `mb` MVD cache | ordered; slice start only | **C** — serial MVD coding |
| `phx_cabac_mvd` | `encoder/cabac.c:329–346` | `cb`, `mb`, list, idx, `mvd[2]` | full MVD bins | `cb`, MVD cache | ordered; slice start only | **C** — serial MVD coding |
| `phx_cabac_8x8_mvd` | `encoder/cabac.c:355–379` | `cb`, `mb`, `i` | 8x8 MVD bins | `cb`, MVD cache | ordered; slice start only | **C** — serial MVD coding |
| `phx_cabac_ref` | `encoder/cabac.c:241–259` | `cb`, `mb`, list, idx | reference-index bins | `cb` | ordered; slice start only | **C** — `cabac_ref_internal` |
| `phx_cabac_mb_header_i` | `encoder/cabac.c:381–433` | `cb`, `mb`, type, slice, chroma | I-slice MB header bins | `cb` | ordered; slice start only | **C** — serial header syntax |
| `phx_cabac_mb_header_p` | `encoder/cabac.c:435–497` | `cb`, `mb`, type, chroma | P-slice MB header bins | `cb` | ordered; slice start only | **C** — serial header syntax |
| `phx_cabac_mb_header_b` | `encoder/cabac.c:499–610` | `cb`, `mb`, type, chroma | B-slice MB header bins | `cb` | ordered; slice start only | **C** — serial header syntax |
| `phx_cabac_cbf_ctxidxinc` | `encoder/cabac.c:612–645` | `mb`, cat, idx, list | CBF context index increment | nothing | pure from neighbour nnz | **C** — feeds serial residual bins |
| `phx_cabac_block_residual` | `encoder/cabac.c:664–748` | `cb`, `mb`, cat, coeffs | residual coefficient bins | `cb`, `mb` state | ordered; slice start only | **C** — `cabac_block_residual_internal` |
| `phx_cabac_block_residual_cbf` | `encoder/cabac.c:892–916` | `cb`, `mb`, cat, idx, coeffs | CBF + residual bins | `cb`, `mb` state | ordered; slice start only | **C** — `cabac_block_residual_cbf*` family |

## 13. Module `cavlc` — `src/phx_cavlc.c` (added during the parallel port)

Every function carries `SHADER: C — serial entropy writer`; all table lookups
come from `common/vlc.c` / `common/tables.c` and all writes go through
`phx_bs_write*` on the single bitstream cursor.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_clz` (static) | `common/osdep.h:514–524` | `uint32_t x` | leading-zero count | nothing | pure; any seek | **C** — integer helper inside the serial writer |
| `phx_cavlc_coeff_last` (static) | `common/quant.c:366–373` | `l[num]`, `num` | last nonzero index | nothing | pure scan | **C** — residual helper in serial stage |
| `phx_cavlc_coeff_level_run` | `common/quant.c:381–395` | `dct`, `num`, `level[], run[]` | level/run pairs | output arrays | pure; any seek | **C** — helper for serial coefficient tokens |
| `phx_cavlc_init_level_token` (static) | `common/vlc.c:35–72` | none | level-token table | static init tables | deterministic init | **C** — one-time generation inside entropy module |
| `phx_cavlc_init_run_before` (static) | `common/vlc.c:74–99` | none | run-before table | static init tables | deterministic init | **C** — one-time generation inside entropy module |
| `phx_cavlc_init` | `common/vlc.c:33` | none | initialised CAVLC tables | static init tables | deterministic init | **C** — serial entropy init |
| `phx_cavlc_write_vlc` | `encoder/cavlc.c:65` | `phx_bs_t *s`, `phx_vlc_t v` | VLC bits | `s` cursor + bytes | ordered; NAL start only | **C** — `bs_write_vlc` on one stream |
| `phx_cavlc_write_vlc_large` | `encoder/cavlc.c:65`, `common/bitstream.h:78` | `phx_bs_t *s`, `phx_vlc_large_t v` | VLC bits | `s` cursor + bytes | ordered; NAL start only | **C** — large-token variant |
| `phx_cavlc_block_residual_escape` | `encoder/cavlc.c:70–119` | `phx_cavlc_t *c`, `i_suffix_length`, `level` | escape-coded prefix/suffix | `c` bitstream | ordered; NAL start only | **C** — RDO_SKIP_BS=0 branch, sets `b_overflow` |
| `phx_cavlc_block_residual_internal` | `encoder/cavlc.c:121–196` | `c`, `ctx_block_cat`, `l[]`, `num_coeff` | CAVLC coefficient tokens + nnz | `c`, `*nnz` | ordered; NAL start only | **C** — core residual token writer |
| `phx_cavlc_block_residual` | `encoder/cavlc.c:200–209` | `c`, `ctx_block_cat`, `l[]`, `nnz` | one block's residual tokens | `c`, `nnz` | ordered; NAL start only | **C** — `x264_cavlc_block_residual` macro |
| `phx_cavlc_coeff0` | `encoder/cavlc.c:206`, `common/tables.c:1799` | `nC` | coefficient-token VLC | nothing | pure lookup | **C** — token table read by serial writer |
| `phx_cavlc_coeff_token_at` | `encoder/cavlc.c:148`, `common/tables.c:1810` | `nC`, `total_minus1`, `trailing` | coefficient-token VLC | nothing | pure lookup | **C** — table access for parity tests and writer |
| `phx_cavlc_total_zeros_at` | `encoder/cavlc.c:190`, `common/tables.c:2265` | `total_minus1`, `zeros` | total-zeros VLC | nothing | pure lookup | **C** — table access |
| `phx_cavlc_total_zeros_dc_at` | `encoder/cavlc.c:184–185`, `common/tables.c:2435/2455` | `total_minus1`, `zeros`, `chroma_2x4_dc` | total-zeros DC VLC | nothing | pure lookup | **C** — table access |

## 14. Module `quant` — `src/phx_quant.c` (added during the parallel port)

File header: `SHADER: A for all quantizers/dequantizers: per coefficient,
vectorizable`, matching `SHADER_MAPPING.md:17`. One exception:
`phx_quant_tables_init` is B per its own comment (one-time flat-CQM table
construction, not a per-block kernel).

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_shift` (static) | `common/set.c:28` | `x`, `s` | shifted int | nothing | pure; any seek | **A** — per-coefficient integer helper |
| `phx_div` (static) | `common/set.c:29` | `n`, `d` | divided int | nothing | pure; any seek | **A** — per-coefficient integer helper |
| `phx_quant_tables_init` | `common/set.c:73–205`, `base.c:456–457/462–467` | `phx_quant_tables_t *t` | flat-CQM quant/dequant tables | `t` | deterministic init; any time | **B** — one-time table construction for the ordered stages |
| `phx_quant_8x8` | `common/quant.c:59–65` | `dct[64]`, `mf[64]`, `bias[64]` | nnz count | `dct[64]` | pure per block; any seek | **A** — one invocation per 8x8 block |
| `phx_quant_4x4` | `common/quant.c:67–73` | `dct[16]`, `mf[16]`, `bias[16]` | nnz count | `dct[16]` | pure per block | **A** — one invocation per 4x4 block |
| `phx_quant_4x4x4` | `common/quant.c:75–86` | `dct[4][16]`, `mf[16]`, `bias[16]` | 4-bit nnz map | `dct[4][16]` | pure per group | **A** — one invocation per four 4x4 blocks |
| `phx_quant_4x4_dc` | `common/quant.c:88–94` | `dct[16]`, `mf`, `bias` | nnz count | `dct[16]` | pure per block | **A** — DC-only 4x4 |
| `phx_quant_2x2_dc` | `common/quant.c:96–104` | `dct[4]`, `mf`, `bias` | nnz count | `dct[4]` | pure per block | **A** — chroma DC 2x2 |
| `phx_dequant_4x4` | `common/quant.c:112–128` | `dct[16]`, `dequant_mf[6][16]`, `i_qp` | dequantised coefficients | `dct[16]` | pure per coefficient | **A** — vectorizable |
| `phx_dequant_8x8` | `common/quant.c:130–146` | `dct[64]`, `dequant_mf[6][64]`, `i_qp` | dequantised coefficients | `dct[64]` | pure per coefficient | **A** — vectorizable |
| `phx_dequant_4x4_dc` | `common/quant.c:148–165` | `dct[16]`, `dequant_mf[6][16]`, `i_qp` | DC dequantised | `dct[16]` | pure per coefficient | **A** — vectorizable |
| `phx_idct_dequant_2x4_dc` | `common/quant.c:185–197` | `dct[8]`, `dct4x4[8][16]`, dequant params | inverse chroma DC | `dct4x4[][0]` | pure per DC block | **A** — one invocation per chroma DC block |
| `phx_idct_dequant_2x4_dconly` | `common/quant.c:199–211` | `dct[8]`, dequant params | DC-only inverse | `dct[8]` | pure per DC block | **A** — one invocation per chroma DC block |
| `phx_optimize_chroma_idct_dequant_2x4` | `common/quant.c:213–224` | `out[8]`, coeffs, dequant params | optimised chroma DC | `out[8]` | pure per DC block | **A** — one invocation per chroma DC block |
| `phx_optimize_chroma_idct_dequant_2x2` | `common/quant.c:227–237` | `out[4]`, coeffs, dequant params | optimised chroma DC | `out[4]` | pure per DC block | **A** — one invocation per chroma DC block |
| `phx_optimize_chroma_round` | `common/quant.c:239–252` | `ref`, `dct`, `dequant_mf`, `q`, `chroma422` | rounding decision | nothing | pure; any seek | **A** — per DC block |
| `phx_optimize_chroma_dc_internal` | `common/quant.c:254–292` | `dct`, `dequant_mf`, `chroma422` | nnz / optimised DC | `dct` | pure per DC block | **A** — one invocation per chroma DC block |
| `phx_optimize_chroma_2x2_dc` | `common/quant.c:294–297` | `dct[4]`, `dequant_mf` | nnz | `dct[4]` | pure per DC block | **A** — one invocation per 2x2 chroma DC |
| `phx_optimize_chroma_2x4_dc` | `common/quant.c:299–302` | `dct[8]`, `dequant_mf` | nnz | `dct[8]` | pure per DC block | **A** — one invocation per 2x4 chroma DC |
| `phx_denoise_dct` | `common/quant.c:304–315` | `dct`, `sum`, `i_qp` | denoised coefficients + energy | `dct`, `sum` | pure per coefficient | **A** — per coefficient |

## 15. Module `slice` — `src/phx_slice.c` (added during the parallel port)

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_slice_header_init` | `encoder/encoder.c:122–328` | `sh`, `param`, `sps`, `pps`, `frame` | explicit slice-header struct (`sh`) | `sh` only — no hidden `h->` reads | pure function of inputs; any seek | **C** — host setup for the serial slice writer |
| `phx_slice_header_write` | `encoder/encoder.c:213` | `bs`, `param`, `sps`, `pps`, `sh`, `frame` | slice-header RBSP syntax in order | `bs` cursor + bytes; `sh->i_nal_ref_idc` | ordered; NAL start only | **C** — single ordered bitstream, `SHADER_MAPPING.md:52` |
| `phx_slice_group_change_bits` | H.264 7.4.3 (frame-only; `x264` has no field pair) | `sps` | `Ceil(Log2(PicSizeInMapUnits))` | nothing | pure function of SPS geometry; any seek | **A** — per-header pure computation |

---

## NOT YET PORTED — highest-priority MISSING functions

Source: `docs/FUNCTION_INVENTORY.md` **MISSING** rows (the inventory is a
mechanical snapshot and has not been regenerated since the parallel workers
added code); priorities follow `docs/PORTING_ROADMAP.md` WP2→WP8 and the
porting order in `docs/X264_PIPELINE.md:136–143`. Roles are one-line summaries
taken from those documents and the inventory notes. **Caveat:** several
helper-level entries still marked MISSING in that inventory now exist in
`src/` under the corresponding `phx_*` names (`phx_quant_*`, `phx_predict_*`,
`phx_cabac_mb_*`, `phx_cavlc_*`, `phx_slice_header_*`); the top-level drivers
(`x264_macroblock_write_cabac`, `x264_macroblock_analyse`,
`x264_slicetype_analyse`, …) remain unported. Module names are the eight modules
above; where x264 has no dedicated Phoenix module yet, the function is
assigned to the module that owns the corresponding state/buffers (noted in
parentheses). No function here exists in `src/` today.

### `mbcache` — WP2 remainder
| x264 function | source:line | one-line role |
|---|---|---|
| `macroblock_store_pic` | `common/macroblock.c:1624` | reconstructs and writes the current MB's pixels back into the frame (pending neighbour-cache companion) |

### `frame` — WP3 lookahead / slice-type (Class C state machine; consumes Class A lowres planes)
| x264 function | source:line | one-line role |
|---|---|---|
| `x264_lookahead_put_frame` | `encoder/lookahead.c:192` | queues an input frame into the lookahead |
| `x264_lookahead_get_frames` | `encoder/lookahead.c:223` | runs slicetype analysis and returns frames in coding order |
| `slicetype_mb_cost` | `encoder/slicetype.c:514` | per-MB lowres I/P/B cost estimation |
| `slicetype_slice_cost` | `encoder/slicetype.c:814` | accumulates row/MB costs into a frame cost |
| `slicetype_frame_cost` | `encoder/slicetype.c:836` | computes cached lowres intra/P/B cost arrays for a frame |
| `macroblock_tree` | `encoder/slicetype.c:1091` | propagates lookahead costs into MB-tree QP offsets (Class A propagation) |
| `macroblock_tree_propagate` | `encoder/slicetype.c:1051` | per-frame propagation step feeding MB-tree |
| `macroblock_tree_finish` | `encoder/slicetype.c:1029` | finalises accumulated MB-tree costs |
| `scenecut_internal` | `encoder/slicetype.c:1384` | `pcost >= (1-bias)*icost` test with GOP-dependent bias |
| `scenecut` | `encoder/slicetype.c:1430` | flash detection / `real_scenecut` pruning |
| `x264_slicetype_analyse` | `encoder/slicetype.c:1473` | sequential walk over `lookahead->last_nonb`, checks `frames[1]` |
| `x264_slicetype_decide` | `encoder/slicetype.c:1745` | final I/P/B assignment, reordering, frame_num/POC |
| `x264_rc_analyse_slice` | `encoder/slicetype.c:1976` | CRF cost estimate for VBV/lookahead |

### `cost` — WP4/WP5 analysis (consumes the ported cost tables), WP8 rate control
| x264 function | source:line | one-line role |
|---|---|---|
| `mb_analyse_init` | `encoder/analyse.c:294` | initialises per-MB analysis state before mode decisions |
| `predict_16x16_mode_available` / `predict_8x8_mode_available` / `predict_4x4_mode_available` / `predict_chroma_mode_available` | `encoder/analyse.c:530/544/552/537` | neighbour-gated intra mode availability tables |
| `mb_analyse_intra_chroma` | `encoder/analyse.c:586` | chroma DC/H/V/DDL mode cost decisions |
| `mb_analyse_intra` | `encoder/analyse.c:668` | I16/I8/I4 intra mode and cost decision driver |
| `intra_rd` / `intra_rd_refine` | `encoder/analyse.c:982/1016` | full RD re-evaluation of intra candidates at high subme |
| `mb_analyse_inter_p16x16` | `encoder/analyse.c:1255` | P16 motion estimation (DIA/HEX/UMH/TESA) |
| `mb_analyse_inter_p8x8` | `encoder/analyse.c:1425` | P8x8 sub-partition analysis |
| `mb_analyse_inter_b16x16` / `mb_analyse_inter_b8x8` | `encoder/analyse.c:1890/2283` | B-slice motion analysis |
| `mb_analyse_transform` / `mb_analyse_transform_rd` | `encoder/analyse.c:2735/2773` | 8x8-vs-4x4 transform RD at subme≥6 |
| `x264_macroblock_analyse` | `encoder/analyse.c:2918` | top-level per-MB analysis dispatch |
| `analyse_update_cache` | `encoder/analyse.c:3734` | writes analysis results back into the neighbour caches |
| `x264_me_search_ref` | `encoder/me.c:182` | core motion search over the reference frame |
| `x264_me_refine_qpel` | `encoder/me.c:801` | subpel quarter-pixel refinement |
| `x264_ratecontrol_start` | `encoder/ratecontrol.c:1432` | resolves QP for the next frame (stateful across frames) |
| `x264_adaptive_quant_frame` | `encoder/ratecontrol.c:304` | AQ QP offsets for the frame |
| `x264_ratecontrol_end` | `encoder/ratecontrol.c:1829` | accumulates rate-control state after encoding |

### `frame` — WP6 predictors / DCT / quant (Class A when ported)
| x264 function | source:line | one-line role |
|---|---|---|
| `x264_predict_16x16_dc_c` / `_h_c` / `_v_c` / `_p_c` | `common/predict.c:67/104/116/132` | 16x16 intra predictors from neighbouring samples |
| `x264_predict_8x8c_*_c`, `x264_predict_4x4_*_c`, `x264_predict_8x8_*_c` | `common/predict.c:167–865` | chroma, 4x4 and 8x8 intra predictors incl. filtered edges |
| `dct4x4dc` / `idct4x4dc` | `common/dct.c:47/78` | DC 4x4 Hadamard transform and inverse |
| `quant_4x4` / `quant_8x8` / `quant_4x4x4` / `quant_2x2_dc` | `common/quant.c:67/59/75/96` | plain deadzone quantization |
| `dequant_4x4` / `dequant_8x8` / `dequant_4x4_dc` | `common/quant.c:112/130/148` | inverse quantization for reconstruction |
| `x264_quant_4x4_trellis` / `x264_quant_8x8_trellis` | `encoder/rdo.c:1142/1159` | CABAC trellis QNS for 4x4 / 8x8 (Class B state machine) |
| `quant_trellis_cavlc` | `encoder/rdo.c:921` | CAVLC bit-count-driven QNS |

### `mbcache` — WP5/WP6 encode path
| x264 function | source:line | one-line role |
|---|---|---|
| `x264_macroblock_encode` | `encoder/macroblock.c:974` | dispatches reconstruction by MB type |
| `idct_dequant_2x2_dc` | `encoder/macroblock.c:63` | chroma DC inverse transform/dequant |

### `cabac` — WP7 entropy syntax (all Class C)
| x264 function | source:line | one-line role |
|---|---|---|
| `x264_macroblock_write_cabac` | `encoder/cabac.c:1088` | CABAC syntax writer for one MB in scan order |
| `x264_macroblock_write_cavlc` | `encoder/cavlc.c:487` | CAVLC syntax writer for one MB in scan order |
| `cabac_mb_header_i` / `_p` / `_b` | `encoder/cabac.c:381/435/499` | per-slice-type MB header bins |
| `cabac_cbp_luma` / `cabac_cbp_chroma` | `encoder/cabac.c:118/129` | coded-block-pattern bins |
| `cabac_mvd` / `cabac_8x8_mvd` | `encoder/cabac.c:329/355` | motion-vector-difference bins |
| `x264_cabac_block_residual_c` | `encoder/cabac.c:750` | residual coefficient bins |
| `cavlc_block_residual_internal` | `encoder/cavlc.c:121` | residual coefficient token/bit writing for CAVLC |

### `headers` — WP7 SEI / slice header
| x264 function | source:line | one-line role |
|---|---|---|
| `x264_sei_write` | `encoder/set.c:80` | assembles SEI NAL payload |
| `x264_sei_version_write` | `encoder/set.c:593` | encoder version SEI payload |
| `x264_sei_recovery_point_write` | `encoder/set.c:574` | recovery-point SEI payload |

### `bitstream` — WP7 writer lifecycle
| x264 function | source:line | one-line role |
|---|---|---|
| `x264_bitstream_init` | `common/bitstream.c:106` | initialises architecture-specific bitstream writer tables |
| `bs_flush` / `bs_realign` | `common/bitstream.h:106/113` | pending-word flush / 32-bit realignment of the serial stream |
