# Phoenix function port index — CPU oracle → shader backend

Scope: **every function currently defined in `src/*.c` on `vk264-phoenix`**. 
This is a full working-tree scan of all 26 C files: **505 function definitions** 
(505 file/name pairs, 
499 unique names — duplicate names are intentional per-file static helpers). 
`src/phx_tables.c` defines data only (11 immutable arrays). All x264 references are relative to 
`reference/x264/`; the `file:line` given is the exact x264 definition the Phoenix function follows, 
as recorded in the port's own source comments and headers. Classification follows 
`docs/SHADER_MAPPING.md` (A = data-parallel, B = workgroup with neighbour state, C = strictly serial).

Counts: **185 × A, 109 × B, 211 × C**. 
Host lifecycle and one ordered output stream fall in C, since neither has a per-work-item mapping. 
The five predictor `*init` functions and `phx_quant_tables_init` are B (dispatch/table construction); 
`phx_rdo_init` is B for the same reason. The size-only RDO accumulator is B (per-MB local state), 
its table accessors are A, and the trellis state machine is C.

`docs/SHADER_NOTES.md` turns this index into the backend design; 
`docs/FUNCTION_INVENTORY.md` is the mechanical master list (1386 x264 functions, 
231 PORTED rows, 1155 MISSING rows).

---

## 1. Module `param` — `src/phx_param.c`

One-time host configuration. Every function is **C** by elimination from A and B: there is no per-pixel/per-block work item. Results feed UBO 0.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_param_default` | `common/base.c:344` | `phx_param_t *p` | writes `p` | explicit output pointer(s) `p`; no globals | constant result of parameters/dimensions; deterministic and recomputable at any point in a stream | **C** — host one-time configuration (UBO 0) |
| `phx_param_apply_preset` | `common/base.c:489` | `phx_param_t *p`, `const char *preset` | writes `p`; returns int | explicit output pointer(s) `p`; no globals | constant result of parameters/dimensions; deterministic and recomputable at any point in a stream | **C** — host one-time configuration (UBO 0) |
| `phx_param_finalize` | `encoder/encoder.c:1218-1231` | `phx_param_t *p` | writes `p` | explicit output pointer(s) `p`; no globals | constant result of parameters/dimensions; deterministic and recomputable at any point in a stream | **C** — host one-time configuration (UBO 0) |
| `phx_param_apply_profile` | `common/base.c:749` | `phx_param_t *p`, `const char *profile` | writes `p`; returns int | explicit output pointer(s) `p`; no globals | constant result of parameters/dimensions; deterministic and recomputable at any point in a stream | **C** — host one-time configuration (UBO 0) |

---

## 2. Module `tables` — `src/phx_tables.c`

No functions: this module is generated data (`tools/gen_tables.py`). `phx_tables.h:5` records `SHADER: A — immutable lookup tables, can live in constant memory`.

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
- **SHADER: A** — immutable constant/UBO data; the CABAC tables are read *by* a C stage, which does not change their own class.

---

## 3. Module `cost` — `src/phx_cost.c`

Cost-table construction and per-MB analysis setup from `encoder/analyse.c`. No hidden state remains after each function.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_bs_size_te` | `common/bitstream.h:301` | `int x`, `int val` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — cost-table setup |
| `phx_init_costs` | `encoder/analyse.c:143` | `phx_cost_tables_t *t`, `const float *logs`, `int qp`, `int mv_range`, `int me_method` | writes `t`; returns int | explicit output pointer(s) `t`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — cost-table setup |
| `phx_analyse_init_costs` | `encoder/analyse.c:179` | `phx_cost_tables_t *t`, `int mv_range`, `int me_method`, `int qp_min`, `int qp_max` | writes `t`; returns int | explicit output pointer(s) `t`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — cost-table setup |
| `phx_analyse_free_costs` | `encoder/analyse.c:204` | `phx_cost_tables_t *t`, `int mv_range` | writes `t` | explicit output pointer(s) `t`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — host teardown of the cost-table buffer |
| `phx_mb_analyse_init_qp` | `encoder/analyse.c:255` | `const phx_param_t *p`, `int qp`, `int chroma_qp_index_offset`, `int mbrd`, `phx_mb_analysis_t *a` | writes `a` | explicit output pointer(s) `a`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — cost-table setup |
| `phx_mb_analyse_load_costs` | `encoder/analyse.c:248` | `phx_cost_tables_t *t`, `phx_mb_analysis_t *a`, `int num_ref_l0`, `int num_ref_l1` | writes `a` | explicit output pointer(s) `a`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — cost-table setup |

---

## 4. Module `frame` — `src/phx_frame.c`

Frame allocation plus the lowres plane pipeline. Plane preparation is Class A; allocation/teardown is host lifecycle (C).

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_align_stride` | `common/frame.c:30` | `int x`, `int align`, `int disalign` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — lowres plane pass |
| `phx_align_plane_size` | `common/frame.c:38` | `int x`, `int disalign` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — lowres plane pass |
| `phx_plane_expand_border` | `common/frame.c:535` | `uint8_t *pix`, `int stride`, `int width`, `int height`, `int padh`, `int padv`, `int pad_top`, `int pad_bottom`, `int chroma` | writes `pix` | explicit output pointer(s) `pix`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — lowres plane pass |
| `phx_frame_expand_border_lowres` | `common/frame.c:627` | `phx_frame_t *f` | writes `f` | explicit output pointer(s) `f`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — lowres plane pass |
| `phx_frame_expand_border_chroma` | `common/frame.c:633-638` | `phx_frame_t *f` | writes `f` | explicit output pointer(s) `f`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — lowres plane pass |
| `phx_frame_init_lowres_core` | `common/mc.c:484` | `uint8_t *src0`, `uint8_t *dst0`, `uint8_t *dsth`, `uint8_t *dstv`, `uint8_t *dstc`, `int src_stride`, `int dst_stride`, `int width`, `int height` | writes `src0`, `dst0`, `dsth`, `dstv`, `dstc` | explicit output pointer(s) `src0`, `dst0`, `dsth`, `dstv`, `dstc`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — lowres plane pass |
| `phx_frame_init_lowres` | `common/mc.c:458` | `phx_frame_t *f` | writes `f` | explicit output pointer(s) `f`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — lowres plane pass |
| `phx_frame_new` | `common/frame.c:60` | `int width`, `int height`, `int bframes` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — host memory lifecycle |
| `phx_frame_delete` | `common/frame.c:312` | `phx_frame_t *f` | writes `f` | explicit output pointer(s) `f`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — host memory lifecycle |

---

## 5. Module `mbcache` — `src/phx_mbcache.c`

Per-MB neighbour state — the Class B wavefront cache (`SHADER_MAPPING.md:34`). Allocation/teardown is C.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_mb_state_new` | `common/frame.c:81 (x264_frame_new allocation pattern)` | `int mb_width`, `int mb_height` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — host allocation of SSBO 2 |
| `phx_mb_state_delete` | `common/frame.c:312 (x264_frame_delete pattern)` | `phx_mb_state_t *s` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — host allocation of SSBO 2 |
| `phx_macroblock_cache_load_neighbours` | `common/macroblock.c:673` | `phx_mb_state_t *s`, `phx_mb_cache_t *c`, `int mb_x`, `int mb_y`, `int b_mbaff` | writes `s`, `c` | explicit output pointer(s) `s`, `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup neighbour gather/write-back |
| `phx_macroblock_cache_load` | `common/macroblock.c:855` | `phx_mb_state_t *s`, `phx_mb_cache_t *c`, `int mb_x`, `int mb_y`, `int b_mbaff` | writes `s`, `c` | explicit output pointer(s) `s`, `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup neighbour gather/write-back |
| `phx_macroblock_backup_intra` | `common/macroblock.c:1638` | `phx_mb_state_t *s`, `int mb_x`, `int mb_y`, `int b_mbaff`, `const uint8_t *p_fdec`, `int stride` | writes `s` | explicit output pointer(s) `s`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup neighbour gather/write-back |
| `phx_macroblock_cache_save` | `common/macroblock.c:1680` | `phx_mb_state_t *s`, `phx_mb_cache_t *c`, `int b_mbaff` | writes `s`, `c` | explicit output pointer(s) `s`, `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup neighbour gather/write-back |

---

## 6. Module `bitstream` — `src/phx_bitstream.c`

All writers operate on one evolving output cursor (C); only the two size queries are pure (A).

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_bs_init` | `common/bitstream.h:86` | `phx_bs_t *s`, `void *data`, `int size` | writes `s`, `data` | explicit output pointer(s) `s`, `data`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_bs_pos` | `common/bitstream.h:100` | `const phx_bs_t *s` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_bs_write` | `common/bitstream.h:125` | `phx_bs_t *s`, `int count`, `uint32_t bits` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_bs_write1` | `common/bitstream.h:169` | `phx_bs_t *s`, `uint32_t bit` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_bs_align_0` | `common/bitstream.h:182` | `phx_bs_t *s` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_bs_align_10` | `common/bitstream.h:192` | `phx_bs_t *s` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_bs_size_ue` | `common/bitstream.h:278` | `unsigned val` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure size query in the serial stage |
| `phx_bs_size_se` | `common/bitstream.h:291` | `int val` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure size query in the serial stage |
| `phx_bs_write_ue_big` | `common/bitstream.h:221` | `phx_bs_t *s`, `unsigned val` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_bs_write_ue` | `common/bitstream.h:241` | `phx_bs_t *s`, `unsigned val` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_bs_write_se` | `common/bitstream.h:246` | `phx_bs_t *s`, `int val` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_bs_write_te` | `common/bitstream.h:264` | `phx_bs_t *s`, `int x`, `int val` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_bs_rbsp_trailing` | `common/bitstream.h:272` | `phx_bs_t *s` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_nal_escape` | `common/bitstream.c:29` | `uint8_t *dst`, `uint8_t *src`, `uint8_t *end` | writes `dst`, `src`, `end` | explicit output pointer(s) `dst`, `src`, `end`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |
| `phx_nal_encode` | `common/bitstream.c:55` | `uint8_t *dst`, `phx_nal_t *nal`, `int b_annexb` | writes `dst`, `nal` | explicit output pointer(s) `dst`, `nal`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial RBSP/NAL stage |

---

## 7. Module `cabac` — `src/phx_cabac.c`

The Class C exemplar. `phx_clz` is A; the five `phx_trace_*` helpers are compile-time-only debug observers inside `#ifdef PHX_CABAC_BIN_TRACE`.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_trace_open` | `common/cabac.c:69 (debug observation of cabac_putbyte)` | `void` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | read-only debug observation of the coder; compiled only with -DPHX_CABAC_BIN_TRACE, otherwise absent | **C** — serial arithmetic coder / compile-time debug observer; debug build only |
| `phx_trace_decision` | `common/cabac.c:113 (debug observation of encode_decision)` | `int ctx`, `int b`, `int st`, `int lo`, `int rg`, `int lo2`, `int rg2` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | read-only debug observation of the coder; compiled only with -DPHX_CABAC_BIN_TRACE, otherwise absent | **C** — serial arithmetic coder / compile-time debug observer; debug build only |
| `phx_trace_bypass` | `common/cabac.c:128 (debug observation of encode_bypass)` | `int b`, `int lo`, `int rg`, `int lo2`, `int rg2` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | read-only debug observation of the coder; compiled only with -DPHX_CABAC_BIN_TRACE, otherwise absent | **C** — serial arithmetic coder / compile-time debug observer; debug build only |
| `phx_trace_ue` | `common/cabac.c:142 (debug observation of encode_ue_bypass)` | `int exp_bits`, `int val`, `int nbins`, `int lo`, `int rg`, `int lo2`, `int rg2` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | read-only debug observation of the coder; compiled only with -DPHX_CABAC_BIN_TRACE, otherwise absent | **C** — serial arithmetic coder / compile-time debug observer; debug build only |
| `phx_trace_terminal` | `common/cabac.c:159 (debug observation of encode_terminal)` | `int lo`, `int rg`, `int lo2`, `int rg2` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | read-only debug observation of the coder; compiled only with -DPHX_CABAC_BIN_TRACE, otherwise absent | **C** — serial arithmetic coder / compile-time debug observer; debug build only |
| `phx_cabac_context_init` | `common/cabac.c:32` | `phx_cabac_t *cb`, `int slice_type`, `int qp`, `int model` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial arithmetic coder / compile-time debug observer |
| `phx_cabac_encode_init_core` | `common/cabac.c:53` | `phx_cabac_t *cb` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial arithmetic coder / compile-time debug observer |
| `phx_cabac_encode_init` | `common/cabac.c:61` | `phx_cabac_t *cb`, `uint8_t *data`, `uint8_t *end` | writes `cb`, `data`, `end` | explicit output pointer(s) `cb`, `data`, `end`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial arithmetic coder / compile-time debug observer |
| `phx_cabac_putbyte` | `common/cabac.c:69` | `phx_cabac_t *cb` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial arithmetic coder / compile-time debug observer |
| `phx_cabac_encode_renorm` | `common/cabac.c:101` | `phx_cabac_t *cb` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial arithmetic coder / compile-time debug observer |
| `phx_cabac_encode_decision` | `common/cabac.c:113` | `phx_cabac_t *cb`, `int ctx`, `int b` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial arithmetic coder / compile-time debug observer |
| `phx_cabac_encode_bypass` | `common/cabac.c:128` | `phx_cabac_t *cb`, `int b` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial arithmetic coder / compile-time debug observer |
| `phx_clz` | `common/osdep.h:514 (x264_clz)` | `uint32_t v` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure bit primitive inlined by the C stage |
| `phx_cabac_encode_ue_bypass` | `common/cabac.c:142` | `phx_cabac_t *cb`, `int exp_bits`, `int val` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial arithmetic coder / compile-time debug observer |
| `phx_cabac_encode_terminal` | `common/cabac.c:159` | `phx_cabac_t *cb` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial arithmetic coder / compile-time debug observer |
| `phx_cabac_encode_flush` | `common/cabac.c:165` | `phx_cabac_t *cb`, `int frame` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial arithmetic coder / compile-time debug observer |

---

## 8. Module `headers` — `src/phx_headers.c`

SPS/PPS init and write from `encoder/set.c`, plus the NAL assembly of `x264_encoder_headers`. All emit or prepare one ordered stream (C).

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_sps_init` | `common/set.c:101` | `phx_sps_t *s`, `const phx_param_t *p`, `int width`, `int height` | writes `s` | explicit output pointer(s) `s`; no globals | constant result of parameters/dimensions; deterministic and recomputable at any point in a stream | **C** — serial header RBSP / NAL assembly |
| `phx_pps_init` | `common/set.c:479` | `phx_pps_t *p`, `const phx_sps_t *s`, `const phx_param_t *param` | writes `p` | explicit output pointer(s) `p`; no globals | constant result of parameters/dimensions; deterministic and recomputable at any point in a stream | **C** — serial header RBSP / NAL assembly |
| `phx_sps_write` | `common/set.c:305` | `phx_bs_t *b`, `const phx_sps_t *s` | writes `b` | explicit output pointer(s) `b`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header RBSP / NAL assembly |
| `phx_pps_write` | `common/set.c:505` | `phx_bs_t *b`, `const phx_sps_t *s`, `const phx_pps_t *p` | writes `b` | explicit output pointer(s) `b`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header RBSP / NAL assembly |
| `phx_encoder_headers` | `encoder/api.c:159 (x264_encoder_headers)` | `uint8_t *dst`, `int capacity`, `const phx_param_t *p`, `int width`, `int height` | writes `dst`; returns int | explicit output pointer(s) `dst`; no globals | constant result of parameters/dimensions; deterministic and recomputable at any point in a stream | **C** — serial header RBSP / NAL assembly |

---

## 9. Module `dct` — `src/phx_dct.c`

`SHADER: A for every transform: one invocation per block, no hidden state` (`phx_dct.h:8`).

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_clip_pixel` | `common/common.h:145-148` | `int x` | returns uint8_t | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_pixel_sub_wxh` | `common/dct.c:145-155` | `phx_dctcoef *diff`, `int size`, `const uint8_t *pix1`, `int i_pix1`, `const uint8_t *pix2`, `int i_pix2` | writes `diff` | explicit output pointer(s) `diff`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_dct4x4dc` | `common/dct.c:47-76` | `phx_dctcoef d[16]` | writes `d` | explicit output pointer(s) `d`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_idct4x4dc` | `common/dct.c:78-107` | `phx_dctcoef d[16]` | writes `d` | explicit output pointer(s) `d`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_dct2x4dc` | `common/dct.c:109-143` | `phx_dctcoef dct[8]`, `phx_dctcoef dct4x4[8][16]` | writes `dct`, `dct4x4` | explicit output pointer(s) `dct`, `dct4x4`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_sub4x4_dct` | `common/dct.c:157-189` | `phx_dctcoef dct[16]`, `const uint8_t *pix1`, `int i_pix1`, `const uint8_t *pix2`, `int i_pix2` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_sub8x8_dct` | `common/dct.c:191-197` | `phx_dctcoef dct[4][16]`, `const uint8_t *pix1`, `int i_pix1`, `const uint8_t *pix2`, `int i_pix2` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_sub16x16_dct` | `common/dct.c:199-205` | `phx_dctcoef dct[16][16]`, `const uint8_t *pix1`, `int i_pix1`, `const uint8_t *pix2`, `int i_pix2` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_sub4x4_dct_dc` | `common/dct.c:207-214` | `const uint8_t *pix1`, `int i_pix1`, `const uint8_t *pix2`, `int i_pix2` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_sub8x8_dct_dc` | `common/dct.c:216-232` | `phx_dctcoef dct[4]`, `const uint8_t *pix1`, `int i_pix1`, `const uint8_t *pix2`, `int i_pix2` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_sub8x16_dct_dc` | `common/dct.c:234-270` | `phx_dctcoef dct[8]`, `const uint8_t *pix1`, `int i_pix1`, `const uint8_t *pix2`, `int i_pix2` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_add4x4_idct` | `common/dct.c:272-310` | `uint8_t *dst`, `int stride`, `const phx_dctcoef dct[16]` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_add8x8_idct` | `common/dct.c:312-318` | `uint8_t *dst`, `int stride`, `const phx_dctcoef dct[4][16]` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_add16x16_idct` | `common/dct.c:320-326` | `uint8_t *dst`, `int stride`, `const phx_dctcoef dct[16][16]` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_sub8x8_dct8` | `common/dct.c:359-378` | `phx_dctcoef dct[64]`, `const uint8_t *pix1`, `int i_pix1`, `const uint8_t *pix2`, `int i_pix2` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_sub16x16_dct8` | `common/dct.c:380-386` | `phx_dctcoef dct[4][64]`, `const uint8_t *pix1`, `int i_pix1`, `const uint8_t *pix2`, `int i_pix2` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_add8x8_idct8` | `common/dct.c:415-432` | `uint8_t *dst`, `int stride`, `phx_dctcoef dct[64]` | writes `dst`, `dct` | explicit output pointer(s) `dst`, `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_add16x16_idct8` | `common/dct.c:434-440` | `uint8_t *dst`, `int stride`, `phx_dctcoef dct[4][64]` | writes `dst`, `dct` | explicit output pointer(s) `dst`, `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_add4x4_idct_dc` | `common/dct.c:442-452` | `uint8_t *dst`, `int stride`, `phx_dctcoef dc` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_add8x8_idct_dc` | `common/dct.c:454-460` | `uint8_t *dst`, `int stride`, `const phx_dctcoef dct[4]` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |
| `phx_add16x16_idct_dc` | `common/dct.c:462-471` | `uint8_t *dst`, `int stride`, `const phx_dctcoef dct[16]` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per coefficient block |

---

## 10. Module `predict` — `src/phx_predict.c`

`SHADER: A for all predictors` (`phx_predict.h:14`); the five `*_init` functions fill dispatch tables (B).

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_clip_pixel` | `common/common.h:145-148` | `int x` | returns uint8_t | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_store4` | `common/predict.c:416-417` | `uint8_t *dst`, `const int pa[2]`, `const int pb[2]` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_16x16_dc` | `common/predict.c:67-79` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_16x16_dc_left` | `common/predict.c:80-89` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_16x16_dc_top` | `common/predict.c:90-99` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_16x16_dc_128` | `common/predict.c:100-103` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_16x16_h` | `common/predict.c:104-115` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_16x16_v` | `common/predict.c:116-131` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_16x16_p` | `common/predict.c:132-160` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8c_dc_128` | `common/predict.c:167-175` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8c_dc_left` | `common/predict.c:176-201` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8c_dc_top` | `common/predict.c:202-220` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8c_dc` | `common/predict.c:221-259` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8c_h` | `common/predict.c:260-269` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8c_v` | `common/predict.c:270-281` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8c_p` | `common/predict.c:282-308` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x16c_dc_128` | `common/predict.c:314-322` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x16c_dc_left` | `common/predict.c:323-341` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x16c_dc_top` | `common/predict.c:342-360` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x16c_dc` | `common/predict.c:361-420` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x16c_h` | `common/predict.c:421-430` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x16c_v` | `common/predict.c:431-442` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x16c_p` | `common/predict.c:443-469` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_dc_128` | `common/predict.c:481-484` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_dc_left` | `common/predict.c:485-489` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_dc_top` | `common/predict.c:490-494` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_dc` | `common/predict.c:495-500` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_h` | `common/predict.c:501-507` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_v` | `common/predict.c:508-511` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_ddl` | `common/predict.c:534-545` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_ddr` | `common/predict.c:546-558` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_vr` | `common/predict.c:560-575` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_hd` | `common/predict.c:577-592` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_vl` | `common/predict.c:594-608` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_4x4_hu` | `common/predict.c:610-621` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_filter` | `common/predict.c:627-675` | `uint8_t *dst`, `uint8_t edge[36]`, `int stride`, `int i_neighbor`, `int i_filters` | writes `dst`, `edge` | explicit output pointer(s) `dst`, `edge`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_fill` | `common/predict.c:693-698` | `uint8_t *dst`, `int stride`, `uint8_t v` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_dc_128` | `common/predict.c:700-703` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_dc_left` | `common/predict.c:704-709` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_dc_top` | `common/predict.c:710-715` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_dc` | `common/predict.c:716-722` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_h` | `common/predict.c:723-730` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_v` | `common/predict.c:731-740` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_ddl` | `common/predict.c:741-760` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_ddr` | `common/predict.c:761-782` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_vr` | `common/predict.c:783-810` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_hd` | `common/predict.c:811-837` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_vl` | `common/predict.c:838-864` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_8x8_hu` | `common/predict.c:865-884` | `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — one invocation per block |
| `phx_predict_16x16_init` | `common/predict.c:889-898` | `phx_predict_t pf[7]` | writes `pf` | explicit output pointer(s) `pf`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one-time dispatch table consumed by B stages |
| `phx_predict_8x8c_init` | `common/predict.c:936-945` | `phx_predict_t pf[7]` | writes `pf` | explicit output pointer(s) `pf`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one-time dispatch table consumed by B stages |
| `phx_predict_8x16c_init` | `common/predict.c:977-986` | `phx_predict_t pf[7]` | writes `pf` | explicit output pointer(s) `pf`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one-time dispatch table consumed by B stages |
| `phx_predict_4x4_init` | `common/predict.c:1042-1055` | `phx_predict_t pf[12]` | writes `pf` | explicit output pointer(s) `pf`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one-time dispatch table consumed by B stages |
| `phx_predict_8x8_init` | `common/predict.c:1000-1013` | `phx_predict8x8_t pf[12]`, `void (**filter)(uint8_t *, uint8_t *, int, int, int)` | writes `pf`, `int)` | explicit output pointer(s) `pf`, `int)`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one-time dispatch table consumed by B stages |

---

## 11. Module `quant` — `src/phx_quant.c`

Per-coefficient quantizers/dequantizers (A); `phx_shift`/`phx_div` are pure helpers; `phx_quant_tables_init` is B.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_shift` | `common/set.c:28` | `int x`, `int s` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_div` | `common/set.c:29` | `int n`, `int d` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_quant_tables_init` | `common/set.c:73-205` | `phx_quant_tables_t *t` | writes `t` | explicit output pointer(s) `t`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one-time flat-CQM table construction |
| `phx_quant_8x8` | `common/quant.c:59-65` | `phx_dctcoef dct[64]`, `const uint16_t mf[64]`, `const uint16_t bias[64]` | writes `dct`; returns int | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_quant_4x4` | `common/quant.c:67-73` | `phx_dctcoef dct[16]`, `const uint16_t mf[16]`, `const uint16_t bias[16]` | writes `dct`; returns int | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_quant_4x4x4` | `common/quant.c:75-86` | `phx_dctcoef dct[4][16]`, `const uint16_t mf[16]`, `const uint16_t bias[16]` | writes `dct`; returns int | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_quant_4x4_dc` | `common/quant.c:88-94` | `phx_dctcoef dct[16]`, `int mf`, `int bias` | writes `dct`; returns int | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_quant_2x2_dc` | `common/quant.c:96-104` | `phx_dctcoef dct[4]`, `int mf`, `int bias` | writes `dct`; returns int | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_dequant_4x4` | `common/quant.c:112-128` | `phx_dctcoef dct[16]`, `const int dequant_mf[6][16]`, `int i_qp` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_dequant_8x8` | `common/quant.c:130-146` | `phx_dctcoef dct[64]`, `const int dequant_mf[6][64]`, `int i_qp` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_dequant_4x4_dc` | `common/quant.c:148-165` | `phx_dctcoef dct[16]`, `const int dequant_mf[6][16]`, `int i_qp` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_idct_dequant_2x4_dc` | `common/quant.c:185-197` | `phx_dctcoef dct[8]`, `phx_dctcoef dct4x4[8][16]`, `const int dequant_mf[6][16]`, `int i_qp` | writes `dct`, `dct4x4` | explicit output pointer(s) `dct`, `dct4x4`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_idct_dequant_2x4_dconly` | `common/quant.c:199-211` | `phx_dctcoef dct[8]`, `const int dequant_mf[6][16]`, `int i_qp` | writes `dct` | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_optimize_chroma_idct_dequant_2x4` | `common/quant.c:213-224` | `phx_dctcoef out[8]`, `const phx_dctcoef dct[8]`, `int dmf` | writes `out` | explicit output pointer(s) `out`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_optimize_chroma_idct_dequant_2x2` | `common/quant.c:227-237` | `phx_dctcoef out[4]`, `const phx_dctcoef dct[4]`, `int dmf` | writes `out` | explicit output pointer(s) `out`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_optimize_chroma_round` | `common/quant.c:239-252` | `const phx_dctcoef *ref`, `const phx_dctcoef *dct`, `int dequant_mf`, `int chroma422` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_optimize_chroma_dc_internal` | `common/quant.c:254-292` | `phx_dctcoef *dct`, `int dequant_mf`, `int chroma422` | writes `dct`; returns int | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_optimize_chroma_2x2_dc` | `common/quant.c:294-297` | `phx_dctcoef dct[4]`, `int dequant_mf` | writes `dct`; returns int | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_optimize_chroma_2x4_dc` | `common/quant.c:299-302` | `phx_dctcoef dct[8]`, `int dequant_mf` | writes `dct`; returns int | explicit output pointer(s) `dct`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |
| `phx_denoise_dct` | `common/quant.c:304-315` | `phx_dctcoef *dct`, `uint32_t *sum`, `const phx_udctcoef *offset`, `int size` | writes `dct`, `sum` | explicit output pointer(s) `dct`, `sum`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per coefficient / per block |

---

## 12. Module `slice` — `src/phx_slice.c`

Slice-header setup and RBSP emission (C); `phx_slice_group_change_bits` is a pure SPS-derived bit count (A).

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_slice_header_init` | `encoder/encoder.c:122` | `phx_slice_header_t *sh`, `const phx_param_t *param`, `const phx_sps_t *sps`, `const phx_pps_t *pps`, `const phx_slice_frame_t *frame` | writes `sh` | explicit output pointer(s) `sh`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial slice header |
| `phx_slice_header_write` | `encoder/encoder.c:213` | `phx_bs_t *bs`, `const phx_param_t *param`, `const phx_slice_header_t *sh` | writes `bs` | explicit output pointer(s) `bs`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial slice header |
| `phx_slice_group_change_bits` | `encoder/encoder.c:213 (slice header group change)` | `const phx_sps_t *sps` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure SPS-derived bit count |

---

## 13. Module `cabac_syntax` — `src/phx_cabac_syntax.c`

Every function carries `SHADER: C` (file header `:16`); scan-order MB syntax bins through the single arithmetic coder.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_coeff_last4` | `common/quant.c:366-373 (x264_coeff_last4)` | `const phx_dctcoef_t *l` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_coeff_last8` | `common/quant.c:366-373 (x264_coeff_last8)` | `const phx_dctcoef_t *l` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_coeff_last15` | `common/quant.c:366-373 (x264_coeff_last15)` | `const phx_dctcoef_t *l` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_coeff_last16` | `common/quant.c:366-373 (x264_coeff_last16)` | `const phx_dctcoef_t *l` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_coeff_last64` | `common/quant.c:366-373 (x264_coeff_last64)` | `const phx_dctcoef_t *l` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_mb_init` | `encoder/cabac.c:35-67 (mb-type context setup)` | `phx_cabac_mb_t *mb`, `phx_mb_cache_t *cache` | writes `mb` | explicit output pointer(s) `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_predict_intra4x4_mode` | `common/macroblock.h:420-431` | `const phx_cabac_mb_t *mb`, `int idx` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_mb_type_intra` | `encoder/cabac.c:35-67` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int i_mb_type`, `int ctx0`, `int ctx1`, `int ctx2`, `int ctx3`, `int ctx4`, `int ctx5` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_intra4x4_pred_mode` | `encoder/cabac.c:83-96` | `phx_cabac_t *cb`, `int i_pred`, `int i_mode` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_intra_chroma_pred_mode` | `encoder/cabac.c:98-116` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_subpartition_p` | `encoder/cabac.c:198-213` | `phx_cabac_t *cb`, `int i_sub` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_subpartition_b` | `encoder/cabac.c:215-233` | `phx_cabac_t *cb`, `int i_sub` | writes `cb` | explicit output pointer(s) `cb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_transform_size` | `encoder/cabac.c:235-239` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_cbp_luma` | `encoder/cabac.c:118-127` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_cbp_chroma` | `encoder/cabac.c:129-148` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_qp_delta` | `encoder/cabac.c:150-186` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_mb_skip` | `encoder/cabac.c:189-195` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int b_skip` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_predict_mv` | `common/mvpred.c:30-127` | `const phx_cabac_mb_t *mb`, `int i_list`, `int idx`, `int i_width`, `int16_t mvp[2]` | writes `mvp` | explicit output pointer(s) `mvp`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_mvd_sum` | `common/base.h:259-266` | `const int16_t *left`, `const int16_t *top` | returns uint16_t | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cache_mvd` | `common/rectangle.h:137-144` | `phx_cabac_mb_t *mb`, `int idx`, `int width`, `int height`, `int i_list`, `int mdx`, `int mdy` | writes `mb` | explicit output pointer(s) `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_mvd_cpn` | `encoder/cabac.c:270-327` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int i_list`, `int idx`, `int l`, `int mvd`, `int ctx` | writes `cb`, `mb`; returns int | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_mvd` | `encoder/cabac.c:329-346` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int i_list`, `int idx`, `int width`, `int height` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_8x8_mvd` | `encoder/cabac.c:355-379` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int i` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_ref` | `encoder/cabac.c:241-259` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int i_list`, `int idx` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_mb_header_i` | `encoder/cabac.c:381-433` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int i_mb_type`, `int slice_type`, `int chroma` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_mb_header_p` | `encoder/cabac.c:435-497` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int i_mb_type`, `int chroma` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_mb_header_b` | `encoder/cabac.c:499-610` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int i_mb_type`, `int chroma` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_cbf_ctxidxinc` | `encoder/cabac.c:612-645` | `phx_cabac_mb_t *mb`, `int i_cat`, `int i_idx`, `int b_intra`, `int b_dc` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_block_residual` | `encoder/cabac.c:664-748` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int ctx_block_cat`, `const phx_dctcoef_t *l`, `int chroma422dc` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |
| `phx_cabac_block_residual_cbf` | `encoder/cabac.c:892-916` | `phx_cabac_t *cb`, `phx_cabac_mb_t *mb`, `int ctx_block_cat`, `int i_idx`, `const phx_dctcoef_t *l`, `int b_intra`, `int b_dc`, `int chroma422dc` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order syntax bins in the entropy stage |

---

## 14. Module `cavlc` — `src/phx_cavlc.c`

Every function carries `SHADER: C` (`phx_cavlc.c:2`); VLC tables are built once at init.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_clz` | `common/osdep.h:514-524` | `uint32_t x` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_coeff_last` | `common/quant.c:366-373` | `const int16_t *l`, `int num` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_coeff_level_run` | `common/quant.c:381-395` | `const int16_t *dct`, `int num`, `phx_cavlc_run_level_t *runlevel` | writes `runlevel`; returns int | explicit output pointer(s) `runlevel`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_init_level_token` | `common/vlc.c:35-72` | `void` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_init_run_before` | `common/vlc.c:74-99` | `void` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_init` | `common/vlc.c:33` | `void` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_write_vlc` | `encoder/cavlc.c:65` | `phx_bs_t *s`, `phx_vlc_t v` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_write_vlc_large` | `encoder/cavlc.c:65` | `phx_bs_t *s`, `phx_vlc_large_t v` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_block_residual_escape` | `encoder/cavlc.c:70-119` | `phx_cavlc_t *c`, `int i_suffix_length`, `int level` | writes `c`; returns int | explicit output pointer(s) `c`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_block_residual_internal` | `encoder/cavlc.c:121-196` | `phx_cavlc_t *c`, `int ctx_block_cat`, `const int16_t *l`, `int nC`, `int num_coeff` | writes `c`; returns int | explicit output pointer(s) `c`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_block_residual` | `encoder/cavlc.c:200-209` | `phx_cavlc_t *c`, `int ctx_block_cat`, `const int16_t *l`, `uint8_t *nnz`, `int nC`, `int num_coeff` | writes `c`, `nnz` | explicit output pointer(s) `c`, `nnz`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_coeff0` | `encoder/cavlc.c:206` | `int nC` | returns phx_vlc_t | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_coeff_token_at` | `encoder/cavlc.c:148` | `int nC`, `int total_minus1`, `int trailing` | returns phx_vlc_t | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_total_zeros_at` | `encoder/cavlc.c:190` | `int total_minus1`, `int zeros` | returns phx_vlc_t | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |
| `phx_cavlc_total_zeros_dc_at` | `encoder/cavlc.c:184-185` | `int total_minus1`, `int zeros`, `int chroma_2x4_dc` | returns phx_vlc_t | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial token writer / VLC table lookup |

---

## 15. Module `encoder` — `src/phx_encoder.c`

`SHADER: C — every entry point drives the serial, single-threaded host pipeline` (`phx_encoder.h:22`). This module now contains the frame loop.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_encoder_validate` | `encoder/encoder.c:423` | `const phx_param_t *p` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_release` | `encoder/encoder.c:4196` | `phx_t *h` | writes `h` | explicit output pointer(s) `h`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_open` | `api.c:76` | `const phx_param_t *p` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_open` | `api.c:76` | `const phx_param_t *p` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_headers` | `api.c:159` | `phx_t *h`, `phx_nal_t **out`, `int *nnal` | writes `h`, `out`, `nnal`; returns int | explicit output pointer(s) `h`, `out`, `nnal`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_header_bytes` | `encoder/encoder.c:2106-2133 (headers envelope)` | `phx_t *h`, `uint8_t *dst`, `int capacity` | writes `h`, `dst`; returns int | explicit output pointer(s) `h`, `dst`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_copy_picture` | `encoder/encoder.c:3372` | `phx_t *h`, `phx_frame_t *f`, `const phx_picture_t *pic` | writes `h`, `f`; returns int | explicit output pointer(s) `h`, `f`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_new_input_frame` | `encoder/encoder.c:3368` | `phx_t *h` | writes `h`; returns static | explicit output pointer(s) `h`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_queue_coded` | `encoder/encoder.c:3368` | `phx_t *h`, `int type`, `int frame_index` | writes `h`; returns int | explicit output pointer(s) `h`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_drain` | `encoder/encoder.c:3447` | `phx_t *h` | writes `h`; returns int | explicit output pointer(s) `h`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_slice_type` | `x264.h:272-280` | `int type` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_slice_qp` | `encoder/ratecontrol.c:821-825` | `const phx_t *h`, `int slice_type` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_dpb_remove` | `encoder/encoder.c:2533-2560` | `phx_t *h`, `int idx` | writes `h` | explicit output pointer(s) `h`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_dpb_refresh` | `encoder/encoder.c:2533-2560` | `phx_t *h` | writes `h` | explicit output pointer(s) `h`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_dpb_lookup` | `encoder/encoder.c:2186` | `const phx_t *h`, `int frame_num`, `int *poc`, `int *l0ref0` | writes `poc`, `l0ref0`; returns int | explicit output pointer(s) `poc`, `l0ref0`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_build_ref_lists` | `encoder/encoder.c:2295-2411` | `phx_t *h`, `phx_slice_frame_t *fc`, `int decided_type`, `int display_frame` | writes `h`, `fc` | explicit output pointer(s) `h`, `fc`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_frame_context` | `encoder/encoder.c:3499-3513` | `phx_t *h`, `int decided_type`, `int display_frame`, `phx_slice_frame_t *fc` | writes `h`, `fc` | explicit output pointer(s) `h`, `fc`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_bs_align_1` | `common/bitstream.h:187-191` | `phx_bs_t *bs` | writes `bs` | explicit output pointer(s) `bs`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_reset_mb_state` | `encoder/encoder.c:2783` | `phx_t *h`, `int slice_type` | writes `h` | explicit output pointer(s) `h`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_apply_decision` | `encoder/macroblock.c:974-982 (macroblock_encode boundary)` | `phx_t *h`, `const phx_mb_decision_t *d` | writes `h` | explicit output pointer(s) `h`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_block` | `common/macroblock.h:150 (scan8 coefficient layout)` | `const phx_mb_decision_t *d`, `int kind`, `int idx`, `int *ncoeff` | writes `ncoeff` | explicit output pointer(s) `ncoeff`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_block_nz` | `common/macroblock.h:150 (nnz layout)` | `const int16_t *coef`, `int ncoeff` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_mb_transform_8x8_allowed` | `common/macroblock.h:444-461` | `const phx_t *h`, `const phx_mb_decision_t *d` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_cabac_block` | `common/cabac.c:892-916` | `phx_t *h`, `phx_cabac_mb_t *mb`, `int cat`, `int idx`, `const int16_t *coef`, `int ncoeff`, `int b_intra`, `int b_dc` | writes `h`, `mb` | explicit output pointer(s) `h`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_cavlc_nc` | `common/macroblock.h:432-442` | `const phx_mb_cache_t *c`, `int idx` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_cavlc_block` | `common/cavlc.c:200-209` | `phx_t *h`, `phx_cavlc_t *cav`, `int cat`, `int idx`, `const int16_t *coef`, `int ncoeff` | writes `h`, `cav` | explicit output pointer(s) `h`, `cav`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_predict_i4` | `common/predict.h:45-49` | `const phx_mb_cache_t *c`, `int idx` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_predict_mv16` | `common/mvpred.c:129-163` | `const phx_mb_cache_t *c`, `int idx`, `int width`, `int16_t mvp[2]` | writes `mvp` | explicit output pointer(s) `mvp`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_cavlc_header_i` | `common/cavlc.c:305-335` | `phx_t *h`, `phx_bs_t *bs`, `const phx_mb_decision_t *d`, `int i_mb_i_offset` | writes `h`, `bs` | explicit output pointer(s) `h`, `bs`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_cavlc_qp_delta` | `common/cavlc.c:211-239` | `phx_bs_t *bs`, `int qp`, `int *last_qp` | writes `bs`, `last_qp` | explicit output pointer(s) `bs`, `last_qp`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_write_cavlc_mb` | `common/cavlc.c:487-595` | `phx_t *h`, `const phx_mb_decision_t *d`, `phx_cavlc_t *cav`, `int *last_qp`, `int slice_type` | writes `h`, `cav`, `last_qp` | explicit output pointer(s) `h`, `cav`, `last_qp`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_write_cabac_mb` | `common/cabac.c:917-1096` | `phx_t *h`, `phx_cabac_mb_t *mb`, `const phx_mb_decision_t *d`, `int mb_xy` | writes `h`, `mb` | explicit output pointer(s) `h`, `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encode` | `api.c:166` | `phx_t *h`, `const phx_picture_t *pic`, `phx_nal_t **out`, `int *nnal`, `uint8_t **recon` | writes `h`, `out`, `nnal`, `recon`; returns int | explicit output pointer(s) `h`, `out`, `nnal`, `recon`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_encode` | `api.c:166` | `phx_t *h`, `const phx_picture_t *pic`, `phx_nal_t **out`, `int *nnal`, `uint8_t **recon` | writes `h`, `out`, `nnal`, `recon`; returns int | explicit output pointer(s) `h`, `out`, `nnal`, `recon`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_encoder_close` | `api.c:130` | `phx_t *h` | writes `h` | explicit output pointer(s) `h`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |
| `phx_close` | `api.c:130` | `phx_t *h` | writes `h` | explicit output pointer(s) `h`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial host encoder driver / frame loop |

---

## 16. Module `me` — `src/phx_me.c`

`SHADER: A — pure, state-free per-MB search` (`phx_me.c:5`, `phx_me.h:33`). Full-pel DIA/HEX, no subpel.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_pixel_sad_16x16` | `common/pixel.c:55-70` | `const uint8_t *pix1`, `intptr_t i_stride_pix1`, `const uint8_t *pix2`, `intptr_t i_stride_pix2` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure state-free per-MB full-pel search |
| `phx_mv_cost_formula` | `encoder/analyse.c:186-188` | `int lambda`, `int delta` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure state-free per-MB full-pel search |
| `phx_resolve_cost_mv` | `encoder/analyse.c:143-177 (init_costs cost_mv pointer)` | `const phx_cost_tables_t *cost`, `int lambda` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure state-free per-MB full-pel search |
| `phx_check_mv_range` | `encoder/me.c:209` | `int mx`, `int my`, `uint32_t mv_min`, `uint32_t mv_max` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure state-free per-MB full-pel search |
| `phx_bits_mvd` | `encoder/me.c:60-61` | `const phx_me_ctx_t *c`, `int mx`, `int my` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure state-free per-MB full-pel search |
| `phx_cost_mv` | `encoder/me.c:63-70` | `phx_me_ctx_t *c`, `int mx`, `int my` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure state-free per-MB full-pel search |
| `phx_cost_mv_x4_dir` | `encoder/me.c:94-107` | `phx_me_ctx_t *c`, `const int off[4][2]`, `int costs[4]` | writes `costs` | explicit output pointer(s) `costs`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure state-free per-MB full-pel search |
| `phx_cost_mv_x3_dir` | `encoder/me.c:81-92` | `phx_me_ctx_t *c`, `const int off[3][2]`, `int costs[3]` | writes `costs` | explicit output pointer(s) `costs`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure state-free per-MB full-pel search |
| `phx_me16` | `encoder/me.c:182-419 (x264_me_search_ref; full-pel DIA/HEX :276-419)` | `int x`, `int y`, `const phx_frame_t *fenc`, `const phx_frame_t *fref`, `const phx_cost_tables_t *cost`, `int mv_range`, `int lambda`, `int method`, `const int16_t pred[2]`, `int *out_mv` | writes `out_mv`; returns int | explicit output pointer(s) `out_mv`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure state-free per-MB full-pel search |

---

## 17. Module `lowres` — `src/phx_lowres.c`

`SHADER: A` for per-8x8-block kernels and `C` for serial cost accumulation (`phx_lowres.h:30-33`). Slicetype frame/path costs, hpel/qpel search.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `lr_frame_clear` | `common/frame.c:285-288 (lowres cache clear)` | `phx_lr_frame_t *c`, `int ndist`, `int mb_count` | writes `c` | explicit output pointer(s) `c`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial frame/path cost accumulation |
| `lr_get_frame` | `common/frame.c:211-220,285-288 (lowres cache allocation)` | `phx_lowres_t *l`, `phx_frame_t *f` | writes `l`, `f`; returns static | explicit output pointer(s) `l`, `f`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial frame/path cost accumulation |
| `phx_lowres_set_mbtree` | `encoder/slicetype.c:823 (edge-MB scoring flag)` | `phx_lowres_t *l`, `int b_mb_tree` | writes `l` | explicit output pointer(s) `l`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial frame/path cost accumulation |
| `phx_lowres_frame_reset` | `common/mc.c:473-481 (cache invalidation)` | `phx_lowres_t *l`, `phx_frame_t *f` | writes `l`, `f` | explicit output pointer(s) `l`, `f`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial frame/path cost accumulation |
| `phx_lowres_new` | `encoder/slicetype.c:45-61 (lowres_context_init)` | `int mb_width`, `int mb_height`, `int mv_range`, `int bframes`, `int b_pyramid`, `int bframe_bias` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial frame/path cost accumulation |
| `phx_lowres_delete` | `common/frame.c:312 (frame teardown pattern)` | `phx_lowres_t *l` | writes `l` | explicit output pointer(s) `l`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial frame/path cost accumulation |
| `lr_sad8` | `common/pixel.c:49-71` | `const uint8_t *a`, `int sa`, `const uint8_t *b`, `int sb` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_satd_8x4` | `common/pixel.c:290-330` | `const uint8_t *a`, `int sa`, `const uint8_t *b`, `int sb` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_satd8` | `common/pixel.c:311-330` | `const uint8_t *a`, `int sa`, `const uint8_t *b`, `int sb` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_get_ref` | `common/mc.c:221-249` | `uint8_t *dst`, `int dst_stride`, `uint8_t *const src[4]`, `int src_stride`, `int mvx`, `int mvy` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_bidir_avg` | `common/mc.c:77-99` | `uint8_t *dst`, `const uint8_t *a`, `const uint8_t *b`, `int weight` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_copy_fenc` | `common/mc.c:77-99` | `uint8_t *dst`, `const phx_frame_t *f`, `int mb_x`, `int mb_y` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_bits_full` | `common/mc.c:77-99` | `const phx_lr_me_t *m`, `int mx`, `int my` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_bits_spel` | `common/mc.c:77-99` | `const phx_lr_me_t *m`, `int mx`, `int my` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_eval_full` | `encoder/me.c:63-70` | `const phx_lr_me_t *m`, `int mx`, `int my` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_eval_spel_sad` | `encoder/me.c:72-79` | `const phx_lr_me_t *m`, `int mx`, `int my` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_eval_spel_satd` | `encoder/me.c:826-833` | `const phx_lr_me_t *m`, `int mx`, `int my` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_in_fpel_range` | `encoder/me.c:826-833` | `const phx_lr_me_t *m`, `int mx`, `int my` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_init_me` | `encoder/me.c:826-833` | `phx_lr_me_t *m`, `const phx_lowres_t *l`, `const uint8_t *fenc`, `const phx_frame_t *fref`, `int mb_x`, `int mb_y`, `const int16_t mvp[2]` | writes `m` | explicit output pointer(s) `m`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_check_predictors` | `encoder/me.c:218-275` | `const phx_lr_me_t *m`, `int16_t (*mvc)[2]`, `int i_mvc`, `int me_range`, `int *best_mx`, `int *best_my`, `int *best_cost` | writes `(*mvc)[2`, `best_mx`, `best_my`, `best_cost` | explicit output pointer(s) `(*mvc)[2`, `best_mx`, `best_my`, `best_cost`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_fullpel_search` | `encoder/me.c:320-419` | `const phx_lr_me_t *m`, `int method`, `int me_range`, `int *bmx`, `int *bmy`, `int *bcost` | writes `bmx`, `bmy`, `bcost` | explicit output pointer(s) `bmx`, `bmy`, `bcost`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_refine_subpel` | `encoder/me.c:865-963` | `const phx_lr_me_t *m`, `int *bmx`, `int *bmy`, `int *bcost` | writes `bmx`, `bmy`, `bcost` | explicit output pointer(s) `bmx`, `bmy`, `bcost`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_motion_search` | `encoder/slicetype.c:654-709` | `const phx_lowres_t *l`, `const phx_lr_me_t *m`, `const int16_t *fenc_mv`, `int mb_width`, `int mb_height`, `int mb_x`, `int mb_y`, `int method`, `int me_range`, `int *out_mv`, `int *out_cost` | writes `out_mv`, `out_cost` | explicit output pointer(s) `out_mv`, `out_cost`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_try_bidir` | `encoder/slicetype.c:579-603` | `const uint8_t *fenc`, `uint8_t *src0[4]`, `uint8_t *src1[4]`, `int stride`, `const int mv0[2]`, `const int mv1[2]`, `int penalty`, `int weight`, `int *bcost`, `int *list_used` | writes `src0`, `src1`, `bcost`, `list_used` | explicit output pointer(s) `src0`, `src1`, `bcost`, `list_used`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-8x8-block kernel |
| `lr_mb_cost` | `encoder/slicetype.c:514-791` | `phx_lowres_t *l`, `phx_frame_t **frames`, `int p0`, `int p1`, `int b`, `int dist_scale_factor`, `const int do_search[2]`, `int bipred_weight`, `int mb_x`, `int mb_y`, `int *out_inter`, `int *out_intra_mbs`, `int *out_intra` | writes `l`, `out_inter`, `out_intra_mbs`, `out_intra` | explicit output pointer(s) `l`, `out_inter`, `out_intra_mbs`, `out_intra`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial frame/path cost accumulation |
| `phx_lowres_frame_cost` | `encoder/slicetype.c:836-995` | `phx_lowres_t *l`, `phx_frame_t **frames`, `int p0`, `int p1`, `int b` | writes `l`; returns int | explicit output pointer(s) `l`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial frame/path cost accumulation |
| `phx_lowres_path_cost` | `encoder/slicetype.c:1288-1327` | `phx_lowres_t *l`, `phx_frame_t **frames`, `const char *path`, `uint64_t threshold` | writes `l`; returns uint64_t | explicit output pointer(s) `l`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial frame/path cost accumulation |
| `phx_slicetype_frame_cost` | `encoder/slicetype.c:1288-1327` | `phx_frame_t **frames`, `int p0`, `int p1`, `int b`, `int b_pyramid` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial frame/path cost accumulation |

---

## 18. Module `lookahead` — `src/phx_lookahead.c`

`SHADER: C — serial lookahead` (`phx_lookahead.h:33`). Owns the ordered queue, scenecut and B/BREF decision.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_lh_frame_cost` | `encoder/slicetype.c:836-995` | `phx_lookahead_t *l`, `phx_lf_frame_t **frames`, `int p0`, `int p1`, `int b` | writes `l`, `frames`; returns int | explicit output pointer(s) `l`, `frames`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lh_path_cost` | `encoder/slicetype.c:1288-1327` | `phx_lookahead_t *l`, `phx_lf_frame_t **frames`, `const char *path`, `uint64_t threshold` | writes `l`, `frames`; returns uint64_t | explicit output pointer(s) `l`, `frames`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lh_scenecut_internal` | `encoder/slicetype.c:1384-1428` | `phx_lookahead_t *l`, `phx_lf_frame_t **frames`, `int p0`, `int p1`, `int real_scenecut` | writes `l`, `frames`; returns int | explicit output pointer(s) `l`, `frames`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lh_scenecut` | `encoder/slicetype.c:1430-1471` | `phx_lookahead_t *l`, `phx_lf_frame_t **frames`, `int p0`, `int p1`, `int real_scenecut`, `int num_frames`, `int i_max_search` | writes `l`, `frames`; returns int | explicit output pointer(s) `l`, `frames`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lh_slicetype_analyse` | `encoder/slicetype.c:1473-1742` | `phx_lookahead_t *l`, `int intra_minigop` | writes `l` | explicit output pointer(s) `l`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lh_slicetype_decide` | `encoder/slicetype.c:1745-1985` | `phx_lookahead_t *l` | writes `l` | explicit output pointer(s) `l`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lookahead_new` | `encoder/lookahead.c:110-150` | `const phx_param_t *p`, `int mb_width`, `int mb_height` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lookahead_put_frame` | `encoder/lookahead.c:192-197` | `phx_lookahead_t *l`, `phx_frame_t *f` | writes `l`, `f` | explicit output pointer(s) `l`, `f`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lh_shift_to_ofbuf` | `encoder/lookahead.c:42-57 (lookahead_shift)` | `phx_lookahead_t *l`, `int count` | writes `l` | explicit output pointer(s) `l`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lookahead_get_frames_full` | `encoder/lookahead.c:223-254` | `phx_lookahead_t *l`, `int *frame_types`, `int *frame_numbers`, `int n` | writes `l`, `frame_types`, `frame_numbers`; returns int | explicit output pointer(s) `l`, `frame_types`, `frame_numbers`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lookahead_get_frames_ex` | `encoder/lookahead.c:223-254 (x264_lookahead_get_frames)` | `phx_lookahead_t *l`, `int *frame_types`, `int n` | writes `l`, `frame_types`; returns int | explicit output pointer(s) `l`, `frame_types`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lookahead_get_frames` | `encoder/lookahead.c:223-254 (x264_lookahead_get_frames)` | `phx_lookahead_t *l`, `int *frame_types`, `int n` | writes `l`, `frame_types`; returns int | explicit output pointer(s) `l`, `frame_types`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |
| `phx_lookahead_delete` | `encoder/lookahead.c:177-187` | `phx_lookahead_t *l` | writes `l` | explicit output pointer(s) `l`; no globals | strictly serial lookahead queue; reproducible only from a lookahead-window boundary (`last_nonb` must be restored) | **C** — serial lookahead queue and B/BREF decision |

---

## 19. Module `analysis` — `src/phx_analysis.c`

`SHADER: B — per-MB workgroup stage` (`phx_analysis.h:22`). Fills one `phx_mb_decision_t` from neighbour cache + source pixels.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_clip3` | `common/common.h:93-108` | `int v`, `int lo`, `int hi` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_zigzag_scan4` | `common/dct.c:843-846` | `phx_dctcoef level[16]`, `const phx_dctcoef dct[16]` | writes `level` | explicit output pointer(s) `level`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_zigzag_scan8` | `common/dct.c:829-832 (x264_zigzag_scan8)` | `phx_dctcoef level[64]`, `const phx_dctcoef dct[64]` | writes `level` | explicit output pointer(s) `level`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_abs2` | `common/pixel.c:255-259` | `phx_sum2_t a` | returns phx_sum2_t | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_satd4` | `common/pixel.c:265-288` | `const uint8_t *p1`, `int s1`, `const uint8_t *p2`, `int s2` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_satd8x4` | `common/pixel.c:290-309` | `const uint8_t *p1`, `int s1`, `const uint8_t *p2`, `int s2` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_satd_4x4` | `common/pixel.c:311-332` | `const uint8_t *p1`, `int s1`, `const uint8_t *p2`, `int s2` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_satd_8x8` | `common/pixel.c:311-332` | `const uint8_t *p1`, `int s1`, `const uint8_t *p2`, `int s2` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_satd_16x16` | `common/pixel.c:311-332` | `const uint8_t *p1`, `int s1`, `const uint8_t *p2`, `int s2` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_sa8d_8x8` | `common/pixel.c:334-372` | `const uint8_t *p1`, `int s1`, `const uint8_t *p2`, `int s2` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_pixel_sad_16x16` | `common/pixel.c:55-73` | `const uint8_t *p1`, `int s1`, `const uint8_t *p2`, `int s2` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_inter_sad` | `encoder/me.h:55` | `const phx_analysis_ctx_t *c`, `const int mv[2]` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_median3` | `common/base.h:242 (x264_median_mv)` | `int a`, `int b`, `int c` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_predict_mv16` | `common/mvpred.c:129-163` | `const phx_mb_cache_t *cache`, `int i_ref`, `int16_t mvp[2]` | writes `mvp` | explicit output pointer(s) `mvp`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_predict_mv_pskip` | `common/mvpred.c:166-181` | `const phx_mb_cache_t *cache`, `int16_t mv[2]` | writes `mv` | explicit output pointer(s) `mv`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_i16_modes` | `encoder/analyse.c:530-535` | `int neighbour_intra` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_chroma_modes` | `encoder/analyse.c:537-542` | `int neighbour_intra` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_i8_modes` | `encoder/analyse.c:544-550` | `int neighbour8` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_i4_modes` | `encoder/analyse.c:552-558` | `int neighbour4` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_predict4_mode` | `common/macroblock.h:420-431` | `const phx_analysis_ctx_t *c`, `int idx` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_predict16` | `common/predict.c:67` | `int mode`, `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_predict4_v_fixed` | `common/predict.c:508-511` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_predict4` | `common/predict.c:481-610` | `int mode`, `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_predict8` | `common/predict.c:632-884` | `int mode`, `uint8_t *dst`, `const uint8_t edge[36]`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_init_neighbours` | `common/macroblock.c:489-498` | `phx_analysis_ctx_t *c` | writes `c` | explicit output pointer(s) `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_setup_fdec` | `common/macroblock.c:501-548` | `phx_analysis_ctx_t *c` | writes `c` | explicit output pointer(s) `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_analysis_quant4` | `common/quant.c:59-65 (quant_4x4)` | `phx_analysis_ctx_t *c`, `phx_dctcoef d[16]`, `int idx`, `int b_intra` | writes `c`, `d`; returns int | explicit output pointer(s) `c`, `d`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_encode_i4` | `encoder/macroblock.h:132-166` | `phx_analysis_ctx_t *c`, `int idx`, `phx_dctcoef out[16]` | writes `c`, `out`; returns int | explicit output pointer(s) `c`, `out`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_encode_i8` | `encoder/macroblock.h:168-213` | `phx_analysis_ctx_t *c`, `int idx`, `phx_dctcoef out[64]` | writes `c`, `out`; returns int | explicit output pointer(s) `c`, `out`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_encode_i16` | `encoder/macroblock.c:126-239` | `phx_analysis_ctx_t *c`, `phx_mb_decision_t *d` | writes `c`, `d` | explicit output pointer(s) `c`, `d`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_search_i16` | `encoder/analyse.c:688-742` | `phx_analysis_ctx_t *c` | writes `c` | explicit output pointer(s) `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_search_i8` | `encoder/analyse.c:745-862` | `phx_analysis_ctx_t *c` | writes `c` | explicit output pointer(s) `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_search_i4` | `encoder/analyse.c:864-979` | `phx_analysis_ctx_t *c` | writes `c` | explicit output pointer(s) `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_pixel_sad_4x4` | `common/pixel.c:55-73` | `const uint8_t *p1`, `int s1`, `const uint8_t *p2`, `int s2` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_pixel_ssd_4x4` | `common/pixel.c:85-110 (PIXEL_SSD_C)` | `const uint8_t *p1`, `int s1`, `const uint8_t *p2`, `int s2` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_rdo_satd_ac_4x4` | `encoder/rdo.c:94-112` | `const uint8_t *p`, `int stride` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_rdo_ssd4` | `encoder/rdo.c:123-148` | `const phx_analysis_ctx_t *c`, `const uint8_t *src`, `int ss`, `const uint8_t *dst`, `int ds` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_analysis_rd_i4` | `encoder/rdo.c:315-347` | `phx_analysis_ctx_t *c`, `int idx`, `int mode`, `const phx_dctcoef coef[16]` | writes `c`; returns uint64_t | explicit output pointer(s) `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_analysis_intra_rd_refine_i4` | `encoder/analyse.c:1086-1135` | `phx_analysis_ctx_t *c`, `phx_mb_decision_t *d` | writes `c`, `d` | explicit output pointer(s) `c`, `d`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_analysis_chroma_load` | `common/frame.c:89-99` | `const phx_analysis_ctx_t *c`, `uint8_t fenc[2][8*PHX_FENC_STRIDE]`, `uint8_t fdec[2][PHX_FDEC_STRIDE*10 + PHX_FDEC_STRIDE]` | writes `fenc`, `fdec` | explicit output pointer(s) `fenc`, `fdec`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_analysis_intra_chroma` | `encoder/analyse.c:586-665` | `phx_analysis_ctx_t *c`, `phx_mb_decision_t *d` | writes `c`, `d` | explicit output pointer(s) `c`, `d`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_analysis_inter_p16x16` | `encoder/analyse.c:1255-1332` | `phx_analysis_ctx_t *c`, `phx_inter_result_t *out` | writes `c`, `out`; returns int | explicit output pointer(s) `c`, `out`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_analysis_init` | `encoder/analyse.c:294-305` | `phx_analysis_ctx_t *c`, `int qp` | writes `c` | explicit output pointer(s) `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_analysis_finalise` | `encoder/analyse.c:2943-2948` | `phx_analysis_ctx_t *c`, `phx_mb_decision_t *d`, `const phx_inter_result_t *inter` | writes `c`, `d` | explicit output pointer(s) `c`, `d`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_macroblock_analyse_slice` | `encoder/analyse.c:2918-2952` | `const phx_param_t *p`, `const phx_frame_t *fenc`, `const phx_frame_t *fref`, `phx_mb_cache_t *cache`, `phx_cabac_t *cb`, `int mb_x`, `int mb_y`, `int qp`, `int slice_type`, `phx_mb_decision_t *d` | writes `d`; returns int | explicit output pointer(s) `d`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |
| `phx_macroblock_analyse` | `encoder/analyse.c:2918-2952` | `const phx_param_t *p`, `const phx_frame_t *f`, `phx_mb_cache_t *cache`, `int mb_x`, `int mb_y`, `int qp`, `phx_mb_decision_t *d` | writes `d`; returns int | explicit output pointer(s) `d`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB workgroup stage; writes one `phx_mb_decision_t` (SSBO 3) |

---

## 20. Module `mbencode` — `src/phx_mbencode.c`

`SHADER: B for every function in this file` (`phx_mbencode.c:8`), except `phx_predict4_v_fixed` (A). Produces the B-stage reconstruction record.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_mb_store_block` | `encoder/macroblock.h:150` | `phx_mb_decision_t *d`, `phx_mb_cache_t *c`, `int block`, `const phx_dctcoef coeff[16]`, `int nz` | writes `d`, `c` | explicit output pointer(s) `d`, `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_load_fdec_neighbours` | `common/frame.c:535-553` | `const phx_frame_t *f`, `int mb_x`, `int mb_y`, `uint8_t *p`, `int stride`, `const phx_mb_cache_t *cache` | writes `p` | explicit output pointer(s) `p`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_neighbour4` | `encoder/macroblock.c:761-763` | `const phx_mb_cache_t *c`, `int idx` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_encode_i16x16` | `encoder/macroblock.c:126-239` | `const phx_quant_tables_t *qt`, `const uint8_t *fenc`, `uint8_t *p_dst`, `int i_qp`, `int i_mode`, `phx_mb_decision_t *d`, `phx_mb_cache_t *c` | writes `p_dst`, `d`, `c` | explicit output pointer(s) `p_dst`, `d`, `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_predict4_v_fixed` | `common/predict.c:508-511` | `uint8_t *dst`, `int stride` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — per-MB reconstruction workgroup stage |
| `phx_mb_b_trellis` | `encoder/analyse.c:261` | `const phx_param_t *param` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_encode_i4x4` | `encoder/macroblock.h:132-166` | `const phx_param_t *param`, `const phx_quant_tables_t *qt`, `const uint8_t *fenc`, `uint8_t *p_dst`, `int i_qp`, `const phx_mb_decision_t *d`, `phx_mb_decision_t *d_out`, `phx_mb_cache_t *c`, `phx_cabac_t *cb` | writes `p_dst`, `d_out`, `c`, `cb` | explicit output pointer(s) `p_dst`, `d_out`, `c`, `cb`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_encode_pcm` | `encoder/macroblock.c:628-640` | `const phx_param_t *p`, `const uint8_t *fenc`, `uint8_t *p_dst`, `phx_mb_decision_t *d`, `phx_mb_cache_t *c` | writes `p_dst`, `d`, `c` | explicit output pointer(s) `p_dst`, `d`, `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_clear_inter_residual` | `encoder/macroblock.c:624` | `phx_mb_decision_t *d`, `phx_mb_cache_t *c` | writes `d`, `c` | explicit output pointer(s) `d`, `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_mc_l0_16x16` | `encoder/macroblock.c:654-667` | `const phx_frame_t *fref`, `uint8_t *p_dst`, `int mb_x`, `int mb_y`, `const phx_mb_decision_t *d`, `int *mvx_out`, `int *mvy_out` | writes `p_dst`, `mvx_out`, `mvy_out`; returns int | explicit output pointer(s) `p_dst`, `mvx_out`, `mvy_out`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_mc_chroma_l0` | `encoder/macroblock.c:669-681` | `const phx_frame_t *fref`, `phx_frame_t *f`, `int mb_x`, `int mb_y`, `int mvx`, `int mvy` | writes `f` | explicit output pointer(s) `f`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_macroblock_encode_ex` | `encoder/macroblock.c:974-982` | `const phx_param_t *p`, `phx_frame_t *f`, `const phx_frame_t *fref`, `phx_mb_cache_t *cache`, `int mb_x`, `int mb_y`, `phx_mb_decision_t *d`, `phx_cabac_t *cb` | writes `cache`, `d`, `cb`; returns int | explicit output pointer(s) `cache`, `d`, `cb`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_macroblock_encode` | `encoder/macroblock.c:974-982` | `const phx_param_t *p`, `phx_frame_t *f`, `phx_mb_cache_t *cache`, `int mb_x`, `int mb_y`, `phx_mb_decision_t *d` | writes `cache`, `d`; returns int | explicit output pointer(s) `cache`, `d`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_clip3i` | `encoder/macroblock.c:259-498` | `int v`, `int lo`, `int hi` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_chroma_qp` | `encoder/encoder.c:1585` | `const phx_param_t *p`, `int qp` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_chroma_mode_ok` | `encoder/analyse.c:485-492` | `int neighbour`, `int mode` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_zigzag2_dc` | `encoder/macroblock.c:34-41` | `phx_dctcoef level[4]`, `const phx_dctcoef dct[4]` | writes `level` | explicit output pointer(s) `level`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_dct2x2dc` | `encoder/macroblock.c:82-96` | `phx_dctcoef d[4]`, `phx_dctcoef dct4x4[4][16]` | writes `d`, `dct4x4` | explicit output pointer(s) `d`, `dct4x4`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_chroma_raw_dc` | `encoder/macroblock.c:343-379` | `const phx_frame_t *f`, `const phx_mb_cache_t *cache`, `int mb_x`, `int mb_y`, `int mode`, `phx_dctcoef raw[2][4]`, `phx_dctcoef ac[2][4][16]` | writes `raw`, `ac` | explicit output pointer(s) `raw`, `ac`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |
| `phx_mb_encode_chroma_trellis` | `encoder/macroblock.c:343-379` | `const phx_param_t *p`, `phx_frame_t *f`, `phx_mb_cache_t *cache`, `int mb_x`, `int mb_y`, `phx_mb_decision_t *d` | writes `f`, `cache`, `d`; returns int | explicit output pointer(s) `f`, `cache`, `d`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB reconstruction workgroup stage |

---

## 21. Module `chroma` — `src/phx_chroma.c`

`SHADER: B for every function in this file` (`phx_chroma.c:8`). 4:2:0 chroma DC/AC encode and B->C handoff.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_zigzag_scan_2x2_dc` | `encoder/macroblock.c:35-41` | `phx_dctcoef level[4]`, `const phx_dctcoef dct[4]` | writes `level` | explicit output pointer(s) `level`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB chroma workgroup stage; fills the chroma half of SSBO 3 |
| `phx_idct_dequant_2x2_dc` | `encoder/macroblock.c:56-70` | `phx_dctcoef dct[4]`, `phx_dctcoef dct4x4[4][16]`, `const int dequant_mf[6][16]`, `int i_qp` | writes `dct`, `dct4x4` | explicit output pointer(s) `dct`, `dct4x4`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB chroma workgroup stage; fills the chroma half of SSBO 3 |
| `phx_idct_dequant_2x2_dconly` | `encoder/macroblock.c:72-79` | `phx_dctcoef dct[4]`, `const int dequant_mf[6][16]`, `int i_qp` | writes `dct` | explicit output pointer(s) `dct`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB chroma workgroup stage; fills the chroma half of SSBO 3 |
| `phx_dct2x2dc` | `encoder/macroblock.c:82-96` | `phx_dctcoef d[4]`, `phx_dctcoef dct4x4[4][16]` | writes `d`, `dct4x4` | explicit output pointer(s) `d`, `dct4x4`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB chroma workgroup stage; fills the chroma half of SSBO 3 |
| `phx_chroma_mode_available` | `encoder/analyse.c:485-492` | `int neighbour`, `int mode` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB chroma workgroup stage; fills the chroma half of SSBO 3 |
| `phx_clip3` | `common/common.h:93-108 (x264_clip3)` | `int v`, `int lo`, `int hi` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB chroma workgroup stage; fills the chroma half of SSBO 3 |
| `phx_chroma_load_windows` | `common/frame.c:89-99 (fenc/fdec plane load)` | `const phx_frame_t *f`, `int mb_x`, `int mb_y`, `int cache_neighbour_intra`, `uint8_t fenc[2][8*PHX_FENC_STRIDE]`, `uint8_t *p_dst[2]` | writes `fenc`, `p_dst` | explicit output pointer(s) `fenc`, `p_dst`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB chroma workgroup stage; fills the chroma half of SSBO 3 |
| `phx_chroma_store` | `common/frame.c:89-99 (plane addressing)` | `phx_frame_t *f`, `int mb_x`, `int mb_y`, `uint8_t *p_dst[2]` | writes `f`, `p_dst` | explicit output pointer(s) `f`, `p_dst`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB chroma workgroup stage; fills the chroma half of SSBO 3 |
| `phx_macroblock_encode_chroma` | `encoder/macroblock.c:492-498` | `const phx_param_t *p`, `phx_frame_t *f`, `phx_mb_cache_t *cache`, `int mb_x`, `int mb_y`, `phx_mb_decision_t *d` | writes `cache`, `d`; returns int | explicit output pointer(s) `cache`, `d`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — per-MB chroma workgroup stage; fills the chroma half of SSBO 3 |

---

## 22. Module `rdo` — `src/phx_rdo.c`

`SHADER: C — the coefficient state machine and entropy-cost bookkeeping are strictly serial` (`phx_rdo.h:23`). The size-only accumulator is B; immutable table accessors are A.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_cabac_size_decision2` | `common/cabac.h:108-113 (cabac_size_decision)` | `uint8_t *state`, `int b` | writes `state`; returns int | explicit output pointer(s) `state`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_cabac_size_decision_noup2` | `common/cabac.h:121-124 (cabac_size_decision_noup)` | `const uint8_t *state`, `int b` | returns int | no caller-owned state mutated; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_init` | `encoder/rdo.c:384-415 (x264_rdo_init)` | `void` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_set_config` | `encoder/encoder.c:1585 (chroma QP offset) / analyse.c:257 (lambda)` | `const phx_rdo_config_t *cfg` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_rdo_get_config` | `encoder/encoder.c:1585 / analyse.c:257 (config mirror)` | `phx_rdo_config_t *cfg` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_rdo_quant4_mf` | `common/set.c:164-205 (flat CQM quant4_mf)` | `int list`, `int qp` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_quant4_bias0` | `common/set.c:164-205 (quant4_bias0)` | `int list`, `int qp` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_unquant4_mf` | `common/set.c:164-205 (unquant4_mf)` | `int list`, `int qp` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_quant8_mf` | `common/set.c:164-205 (quant8_mf)` | `int list`, `int qp` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_quant8_bias0` | `common/set.c:164-205 (quant8_bias0)` | `int list`, `int qp` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_unquant8_mf` | `common/set.c:164-205 (unquant8_mf)` | `int list`, `int qp` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_bs_size_ue_big` | `common/bitstream.h:283-289 (bs_size_ue_big)` | `unsigned int val` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_size_init` | `common/quant.c:878-882` | `phx_cabac_size_t *cb`, `const phx_cabac_t *live`, `int slice_type`, `int qp` | writes `cb` | explicit output pointer(s) `cb`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_size_decision` | `common/cabac.h:101-106 / encoder/rdo.c:31-38` | `phx_cabac_size_t *cb`, `int ctx`, `int b` | writes `cb` | explicit output pointer(s) `cb`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_size_decision_noup` | `common/cabac.h:115-119 / encoder/rdo.c:64-66` | `phx_cabac_size_t *cb`, `int ctx`, `int b` | writes `cb` | explicit output pointer(s) `cb`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_size_terminal` | `encoder/rdo.c:39-42 (size terminal, +7)` | `phx_cabac_size_t *cb` | writes `cb` | explicit output pointer(s) `cb`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_size_bypass` | `encoder/rdo.c:44-46 (size bypass, +256)` | `phx_cabac_size_t *cb` | writes `cb` | explicit output pointer(s) `cb`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_size_ue_bypass` | `encoder/rdo.c:48-50 (size ue_bypass)` | `phx_cabac_size_t *cb`, `int exp_bits`, `int val` | writes `cb` | explicit output pointer(s) `cb`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_size_unary` | `encoder/rdo.c:384-415 (size_unary table)` | `int prefix` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_transition_unary` | `encoder/rdo.c:384-415 (transition_unary table)` | `int prefix` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_size_5ones` | `encoder/rdo.c:384-415 (size_5ones table)` | `void` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_transition_5ones` | `encoder/rdo.c:384-415 (transition_5ones table)` | `void` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_size_intra4x4_pred_mode` | `encoder/cabac.c:83-96 (intra4x4_pred_mode)` | `phx_cabac_size_t *cb`, `int i_pred`, `int i_mode` | writes `cb` | explicit output pointer(s) `cb`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_size_block_residual` | `encoder/cabac.c:775-858 (block_residual, RDO_SKIP_BS)` | `phx_cabac_size_t *cb`, `int ctx_block_cat`, `const phx_dctcoef_t *l` | writes `cb` | explicit output pointer(s) `cb`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_size_block_residual_cbf` | `encoder/cabac.c:892-903 (block_residual_cbf)` | `phx_cabac_size_t *cb`, `phx_cabac_mb_t *mb`, `int ctx_block_cat`, `int i_idx`, `const phx_dctcoef_t *l`, `int b_intra` | writes `cb`, `mb` | explicit output pointer(s) `cb`, `mb`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_size_partition_i4x4` | `encoder/rdo.c:315-347 (rd_cost_i4x4 size)` | `const phx_cabac_t *live`, `phx_cabac_mb_t *mb`, `int i4`, `int i_pred`, `int i_mode`, `const phx_dctcoef_t *l`, `int slice_type`, `int qp` | writes `mb`; returns int | explicit output pointer(s) `mb`; no globals | deterministic given the seeded `state[460]`/`f8_bits_encoded`; one workgroup per MB, never carried across MBs | **B** — size-only per-MB accumulator |
| `phx_rdo_cavlc_predict_nc` | `common/macroblock.h:432-442 / encoder/cavlc.c:198` | `const phx_mb_cache_t *cache`, `int idx` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_coeff_last` | `common/quant.c:366-379 (x264_coeff_last4/8/15/16/64)` | `int ctx_block_cat`, `const int16_t *l` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_chroma_qp` | `encoder/analyse.c:257 (chroma QP table)` | `int qp` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_lambda2` | `encoder/analyse.c:264-267 (i_trellis_lambda2)` | `int b_chroma`, `int b_intra`, `int qp` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_trellis_dc_shortcut` | `encoder/rdo.c:462-491` | `int sign_coef`, `int quant_coef`, `int unquant_mf`, `int coef_weight`, `int lambda2`, `const uint8_t *cabac_state`, `int cost_sig` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_trellis_coef` | `encoder/rdo.c:493-531` | `int j`, `int const_level`, `int abs_level`, `int prefix`, `int suffix_cost`, `int node_ctx`, `int level1_ctx`, `int levelgt1_ctx`, `uint64_t ssd`, `int cost_siglast[3]`, `phx_trellis_node_t *nodes_cur`, `phx_trellis_node_t *nodes_prev`, `phx_trellis_level_t *level_tree`, `int levels_used`, `int lambda2`, `uint8_t *level_state` | writes `cost_siglast`, `nodes_cur`, `nodes_prev`, `level_tree`, `level_state`; returns int | explicit output pointer(s) `cost_siglast`, `nodes_cur`, `nodes_prev`, `level_tree`, `level_state`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_trellis_coef0_0` | `encoder/rdo.c:537-651` | `uint64_t ssd0`, `phx_trellis_node_t *nodes_cur`, `phx_trellis_node_t *nodes_prev`, `phx_trellis_level_t *level_tree`, `int levels_used` | writes `nodes_cur`, `nodes_prev`, `level_tree`; returns int | explicit output pointer(s) `nodes_cur`, `nodes_prev`, `level_tree`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_trellis_coef0_1` | `encoder/rdo.c:537-651` | `uint64_t ssd0`, `phx_trellis_node_t *nodes_cur`, `phx_trellis_node_t *nodes_prev`, `phx_trellis_level_t *level_tree`, `int levels_used` | writes `nodes_cur`, `nodes_prev`, `level_tree`; returns int | explicit output pointer(s) `nodes_cur`, `nodes_prev`, `level_tree`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_trellis_coef1_0` | `encoder/rdo.c:537-651` | `uint64_t ssd0`, `uint64_t ssd1`, `int cost_siglast[3]`, `phx_trellis_node_t *nodes_cur`, `phx_trellis_node_t *nodes_prev`, `phx_trellis_level_t *level_tree`, `int levels_used`, `int lambda2`, `uint8_t *level_state` | writes `cost_siglast`, `nodes_cur`, `nodes_prev`, `level_tree`, `level_state`; returns int | explicit output pointer(s) `cost_siglast`, `nodes_cur`, `nodes_prev`, `level_tree`, `level_state`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_trellis_coef1_1` | `encoder/rdo.c:537-651` | `uint64_t ssd0`, `uint64_t ssd1`, `int cost_siglast[3]`, `phx_trellis_node_t *nodes_cur`, `phx_trellis_node_t *nodes_prev`, `phx_trellis_level_t *level_tree`, `int levels_used`, `int lambda2`, `uint8_t *level_state` | writes `cost_siglast`, `nodes_cur`, `nodes_prev`, `level_tree`, `level_state`; returns int | explicit output pointer(s) `cost_siglast`, `nodes_cur`, `nodes_prev`, `level_tree`, `level_state`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_trellis_coefn_0` | `encoder/rdo.c:537-651` | `int abs_level`, `uint64_t ssd0`, `uint64_t ssd1`, `int cost_siglast[3]`, `phx_trellis_node_t *nodes_cur`, `phx_trellis_node_t *nodes_prev`, `phx_trellis_level_t *level_tree`, `int levels_used`, `int lambda2`, `uint8_t *level_state`, `int levelgt1_ctx` | writes `cost_siglast`, `nodes_cur`, `nodes_prev`, `level_tree`, `level_state`; returns int | explicit output pointer(s) `cost_siglast`, `nodes_cur`, `nodes_prev`, `level_tree`, `level_state`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_trellis_coefn_1` | `encoder/rdo.c:537-651` | `int abs_level`, `uint64_t ssd0`, `uint64_t ssd1`, `int cost_siglast[3]`, `phx_trellis_node_t *nodes_cur`, `phx_trellis_node_t *nodes_prev`, `phx_trellis_level_t *level_tree`, `int levels_used`, `int lambda2`, `uint8_t *level_state`, `int levelgt1_ctx` | writes `cost_siglast`, `nodes_cur`, `nodes_prev`, `level_tree`, `level_state`; returns int | explicit output pointer(s) `cost_siglast`, `nodes_cur`, `nodes_prev`, `level_tree`, `level_state`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_rdo_quant_trellis_cabac` | `encoder/rdo.c:641-895 (quant_trellis_cabac)` | `phx_cabac_t *cb`, `int16_t *dct`, `const uint16_t *quant_mf`, `const uint16_t *quant_bias0`, `const int *unquant_mf`, `const uint8_t *zigzag`, `int ctx_block_cat`, `int lambda2`, `int b_ac`, `int b_chroma`, `int dc`, `int num_coefs`, `int idx`, `int i_psy_trellis`, `const int16_t *psy_fenc`, `int b_interlaced` | writes `cb`, `dct`; returns int | explicit output pointer(s) `cb`, `dct`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_cavlc_count_bits` | `encoder/rdo.c:44-50 (CAVLC size-only count)` | `const int16_t *coefs`, `int ctx_block_cat`, `int nC`, `int num_coefs`, `int profile_idc`, `int chroma_2x4_dc` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — immutable table lookup |
| `phx_rdo_quant_trellis_cavlc` | `encoder/rdo.c:920-1096 (quant_trellis_cavlc)` | `int16_t *dct`, `const uint16_t *quant_mf`, `const int *unquant_mf`, `const uint8_t *zigzag`, `int ctx_block_cat`, `int lambda2`, `int b_ac`, `int b_chroma`, `int dc`, `int num_coefs`, `int idx`, `int b_8x8`, `int nC`, `int profile_idc`, `int chroma_2x4_dc` | writes `dct`; returns int | explicit output pointer(s) `dct`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_quant_4x4_trellis` | `encoder/rdo.c:1142-1157 (x264_quant_4x4_trellis)` | `phx_cabac_t *cb`, `int16_t *dct`, `int i_quant_cat`, `int i_qp`, `intptr_t ctx_block_cat`, `int b_intra` | writes `cb`, `dct`; returns int | explicit output pointer(s) `cb`, `dct`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_quant_8x8_trellis` | `encoder/rdo.c:1159-1184 (x264_quant_8x8_trellis)` | `phx_cabac_t *cb`, `int16_t *dct`, `int i_quant_cat`, `int i_qp`, `intptr_t ctx_block_cat`, `int b_intra` | writes `cb`, `dct`; returns int | explicit output pointer(s) `cb`, `dct`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_quant_luma_dc_trellis` | `encoder/rdo.c:1098-1109 (x264_quant_luma_dc_trellis)` | `phx_cabac_t *cb`, `int16_t *dct`, `int i_quant_cat`, `int i_qp`, `intptr_t ctx_block_cat`, `int b_intra` | writes `cb`, `dct`; returns int | explicit output pointer(s) `cb`, `dct`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |
| `phx_quant_chroma_dc_trellis` | `encoder/rdo.c:1114-1140 (x264_quant_chroma_dc_trellis)` | `phx_cabac_t *cb`, `int16_t *dct`, `int i_qp`, `intptr_t ctx_block_cat`, `int b_intra` | writes `cb`, `dct`; returns int | explicit output pointer(s) `cb`, `dct`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial trellis state machine / entropy bookkeeping |

---

## 23. Module `sei` — `src/phx_sei.c`

`SHADER: C — emitted once in the serial header stage` (`phx_sei.c:10`). Version SEI from the x264 parameter string.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_sei_clip3` | `common/common.h:93-108 (x264_clip3)` | `int v`, `int lo`, `int hi` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header stage (version SEI) |
| `phx_sei_cpu_num_processors` | `common/cpu.c:503-550 (x264_cpu_num_processors)` | `void` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header stage (version SEI) |
| `phx_sei_derive_runtime` | `common/base.c:1428-1445 (param string derivation)` | `phx_sei_runtime_t *rt`, `const phx_param_t *p`, `int width`, `int height` | writes `rt` | explicit output pointer(s) `rt`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header stage (version SEI) |
| `phx_sei_analyse_intra` | `encoder/base.c:191-195` | `int flags` | returns unsigned | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header stage (version SEI) |
| `phx_sei_analyse_inter` | `encoder/base.c:191-195` | `int flags` | returns unsigned | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header stage (version SEI) |
| `phx_sei_me_name` | `encoder/base.c:1453` | `int method` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header stage (version SEI) |
| `phx_sei_rc_name` | `encoder/base.c:1508-1510` | `const phx_param_t *p` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header stage (version SEI) |
| `phx_sei_param2string` | `encoder/base.c:1428-1567` | `const phx_param_t *p`, `const phx_sei_runtime_t *rt`, `char *s`, `int cap` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header stage (version SEI) |
| `phx_sei_write` | `encoder/set.c:80-99 (x264_sei_version_write)` | `phx_bs_t *s`, `const uint8_t *payload`, `int payload_size`, `int payload_type` | writes `s` | explicit output pointer(s) `s`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header stage (version SEI) |
| `phx_sei_version_write` | `encoder/set.c:80-99 (x264_sei_version_write)` | `uint8_t *dst`, `int cap`, `const phx_param_t *p`, `int width`, `int height` | writes `dst`; returns int | explicit output pointer(s) `dst`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial header stage (version SEI) |

---

## 24. Module `cavlc_syntax` — `src/phx_cavlc_syntax.c`

`SHADER: C — all writers are strictly serial; no parallel stage may reorder the bit stream` (`phx_cavlc_syntax.c:4`). CAVLC MB-header and MVD syntax, consuming `phx_mb_cache_t` as the neighbour boundary.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_cavlc_mb_init` | `encoder/cavlc.c:487 (x264_macroblock_write_cavlc context setup)` | `phx_cavlc_mb_t *mb`, `phx_bs_t *bs`, `phx_mb_cache_t *cache` | writes `mb`, `bs`, `cache` | explicit output pointer(s) `mb`, `bs`, `cache`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_cavlc_predict_mv` | `common/mvpred.c:30-127` | `const phx_cavlc_mb_t *mb`, `int i_list`, `int idx`, `int width`, `int16_t mvp[2]` | writes `mvp` | explicit output pointer(s) `mvp`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_cavlc_mvd` | `encoder/cavlc.c:241-248` | `phx_cavlc_mb_t *mb`, `int i_list`, `int idx`, `int width` | writes `mb`; returns int | explicit output pointer(s) `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_predict_intra4x4_mode` | `common/macroblock.h:420-431` | `const phx_cavlc_mb_t *mb`, `int idx` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_cavlc_mb_header_i` | `encoder/cavlc.c:305-335` | `phx_cavlc_mb_t *mb`, `int i_mb_i_offset` | writes `mb` | explicit output pointer(s) `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_cavlc_8x8_mvd` | `encoder/cavlc.c:250-272` | `phx_cavlc_mb_t *mb`, `int i` | writes `mb` | explicit output pointer(s) `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_cavlc_mb_header_p_l0` | `encoder/cavlc.c:340-372` | `phx_cavlc_mb_t *mb` | writes `mb` | explicit output pointer(s) `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_cavlc_mb_header_p_8x8` | `encoder/cavlc.c:373-406` | `phx_cavlc_mb_t *mb` | writes `mb` | explicit output pointer(s) `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_cavlc_mb_header_b_8x8` | `encoder/cavlc.c:414-439` | `phx_cavlc_mb_t *mb` | writes `mb` | explicit output pointer(s) `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_cavlc_mb_header_b_inter` | `encoder/cavlc.c:440-477` | `phx_cavlc_mb_t *mb` | writes `mb` | explicit output pointer(s) `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_cavlc_mb_header` | `encoder/cavlc.c:337-409` | `phx_cavlc_mb_t *mb` | writes `mb`; returns int | explicit output pointer(s) `mb`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |
| `phx_cavlc_mb_skip` | `encoder/encoder.c:2883-2890` | `phx_bs_t *s`, `int *skip_run`, `int b_skip`, `int b_last` | writes `s`, `skip_run`; returns int | explicit output pointer(s) `s`, `skip_run`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — scan-order CAVLC MB-header and MVD syntax in the entropy stage |

---

## 25. Module `mc` — `src/phx_mc.c`

`SHADER: B — one serial invocation per macroblock for all MC operations` (`phx_mc.c:4`). Scalar motion compensation and hpel filtering; the pure sample primitives are A.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_mc_clip_pixel` | `common/common.h:145-148` | `int x` | returns uint8_t | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-sample MC primitive |
| `phx_mc_hpel_filter` | `common/mc.c:174-196` | `uint8_t *dsth`, `uint8_t *dstv`, `uint8_t *dstc`, `const uint8_t *src`, `int stride`, `int width`, `int height`, `int16_t *buf` | writes `dsth`, `dstv`, `dstc`, `buf` | explicit output pointer(s) `dsth`, `dstv`, `dstc`, `buf`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup MC / hpel-filter stage |
| `phx_mc_hpel_filter_frame` | `common/mc.c:704-726` | `const uint8_t *plane`, `int stride`, `int width`, `int height`, `uint8_t *dsth`, `uint8_t *dstv`, `uint8_t *dstc`, `int16_t *buf` | writes `dsth`, `dstv`, `dstc`, `buf` | explicit output pointer(s) `dsth`, `dstv`, `dstc`, `buf`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup MC / hpel-filter stage |
| `phx_mc_pixel_avg` | `common/mc.c:63-75` | `uint8_t *dst`, `int i_dst`, `const uint8_t *src1`, `int i_src1`, `const uint8_t *src2`, `int i_src2`, `int width`, `int height` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-sample MC primitive |
| `phx_mc_copy` | `common/mc.c:162-171` | `const uint8_t *src`, `int i_src_stride`, `uint8_t *dst`, `int i_dst_stride`, `int width`, `int height` | writes `dst` | explicit output pointer(s) `dst`; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure per-sample MC primitive |
| `phx_mc_luma` | `common/mc.c:198-219` | `uint8_t *dst`, `int i_dst_stride`, `const uint8_t *src[4]`, `int i_src_stride`, `int mvx`, `int mvy`, `int width`, `int height` | writes `dst` | explicit output pointer(s) `dst`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup MC / hpel-filter stage |
| `phx_mc_chroma` | `common/mc.c:252-283` | `uint8_t *dstu`, `int i_dstu`, `uint8_t *dstv`, `int i_dstv`, `const uint8_t *src`, `int i_src_stride`, `int mvx`, `int mvy`, `int width`, `int height` | writes `dstu`, `dstv` | explicit output pointer(s) `dstu`, `dstv`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup MC / hpel-filter stage |
| `phx_mc_deinterleave_chroma` | `common/mc.c:328-337` | `uint8_t *dstu`, `int i_dstu`, `uint8_t *dstv`, `int i_dstv`, `const uint8_t *src`, `int i_src_stride`, `int width`, `int height` | writes `dstu`, `dstv` | explicit output pointer(s) `dstu`, `dstv`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup MC / hpel-filter stage |
| `phx_mc_hpel_invalidate` | `common/mc.c:704 (hpel plane cache lifecycle)` | `void` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup MC / hpel-filter stage |
| `phx_mc_luma_mb` | `common/macroblock.c:568-640` | `const phx_frame_t *fref`, `uint8_t *dst`, `int i_dst_stride`, `int mb_x`, `int mb_y`, `int mvx`, `int mvy`, `int width`, `int height` | writes `dst`; returns int | explicit output pointer(s) `dst`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — workgroup MC / hpel-filter stage |

---

## 26. Module `deblock` — `src/phx_deblock.c`

`SHADER: B — one invocation per MB; all eight edges of that MB are computed` (`phx_deblock.c:7`), with pure strength-table lookups (A) and the deblocking-strength cache (B).

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `phx_db_clip3` | `common/common.h:93-108` | `int v`, `int lo`, `int hi` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure strength/pixel lookup |
| `phx_db_clip_pixel` | `common/common.h:93-108` | `int v` | returns uint8_t | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure strength/pixel lookup |
| `phx_db_table_index` | `common/deblock.c:74-76` | `int x` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure strength/pixel lookup |
| `phx_deblock_alpha` | `common/deblock.c:74-76` | `int qp_plus_offset` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure strength/pixel lookup |
| `phx_deblock_beta` | `common/deblock.c:74-76` | `int qp_plus_offset` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure strength/pixel lookup |
| `phx_deblock_tc0` | `common/deblock.c:74-76` | `int qp_plus_offset`, `int bs` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure strength/pixel lookup |
| `phx_db_edge_luma_c` | `common/deblock.c:79-109` | `uint8_t *pix`, `intptr_t xstride`, `int alpha`, `int beta`, `int8_t tc0` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_luma_c` | `common/deblock.c:110-122` | `uint8_t *pix`, `intptr_t xstride`, `intptr_t ystride`, `int alpha`, `int beta`, `const int8_t *tc0` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_h_luma_mbaff_c` | `common/deblock.c:123-127` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta`, `const int8_t tc0[4]` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_v_luma_c` | `common/deblock.c:128-135` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta`, `const int8_t tc0[4]` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_h_luma_c` | `common/deblock.c:128-135` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta`, `const int8_t tc0[4]` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_edge_chroma_c` | `common/deblock.c:137-150` | `uint8_t *pix`, `intptr_t xstride`, `int alpha`, `int beta`, `int8_t tc` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_chroma_c` | `common/deblock.c:151-165` | `uint8_t *pix`, `int height`, `intptr_t xstride`, `intptr_t ystride`, `int alpha`, `int beta`, `const int8_t *tc0` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_h_chroma_mbaff_c` | `common/deblock.c:166-181` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta`, `const int8_t tc0[4]` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_v_chroma_c` | `common/deblock.c:166-181` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta`, `const int8_t tc0[4]` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_h_chroma_c` | `common/deblock.c:166-181` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta`, `const int8_t tc0[4]` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_edge_luma_intra_c` | `common/deblock.c:183-221` | `uint8_t *pix`, `intptr_t xstride`, `int alpha`, `int beta` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_luma_intra_c` | `common/deblock.c:222-239` | `uint8_t *pix`, `intptr_t xstride`, `intptr_t ystride`, `int alpha`, `int beta` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_h_luma_intra_mbaff_c` | `common/deblock.c:222-239` | `uint8_t *pix`, `intptr_t ystride`, `int alpha`, `int beta` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_v_luma_intra_c` | `common/deblock.c:222-239` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_h_luma_intra_c` | `common/deblock.c:222-239` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_edge_chroma_intra_c` | `common/deblock.c:241-275` | `uint8_t *pix`, `intptr_t xstride`, `int alpha`, `int beta` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_chroma_intra_c` | `common/deblock.c:241-275` | `uint8_t *pix`, `int width`, `int height`, `intptr_t xstride`, `intptr_t ystride`, `int alpha`, `int beta` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_h_chroma_intra_mbaff_c` | `common/deblock.c:241-275` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_v_chroma_intra_c` | `common/deblock.c:241-275` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_h_chroma_intra_c` | `common/deblock.c:241-275` | `uint8_t *pix`, `intptr_t stride`, `int alpha`, `int beta` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_strength` | `common/deblock.c:277-304` | `const uint8_t nnz[120]`, `const int8_t ref[2][40]`, `const int16_t mv[2][40][2]`, `uint8_t bs[2][8][4]`, `int mvy_limit`, `int bframe` | writes `bs` | explicit output pointer(s) `bs`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_strength_c` | `common/deblock.c:277-304` | `const phx_mb_cache_t *c`, `uint8_t bs[2][8][4]`, `int mvy_limit`, `int bframe` | writes `bs` | explicit output pointer(s) `bs`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_is_intra` | `common/deblock.c:277-304` | `int type` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure strength/pixel lookup |
| `phx_db_is_16x16` | `common/macroblock.h:139` | `int type` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure strength/pixel lookup |
| `phx_macroblock_deblock_strength` | `encoder/macroblock.c:1438-1622` | `const phx_mb_state_t *s`, `const phx_mb_cache_t *c`, `uint8_t bs[2][8][4]`, `int b_cabac`, `int disable_deblocking_filter_idc` | writes `bs` | explicit output pointer(s) `bs`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_deblock_edge` | `common/deblock.c:306-338` | `uint8_t *pix`, `intptr_t stride`, `const uint8_t bs[4]`, `int qp`, `int a`, `int b`, `int b_chroma`, `int intra`, `int dir` | writes `pix` | explicit output pointer(s) `pix`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_neighbours` | `common/deblock.c:340-376` | `const phx_mb_state_t *s`, `int mb_x`, `int mb_y`, `int disable` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_chroma_qp` | `common/macroblock.h:249-270` | `int qp`, `int chroma_offset` | returns int | no caller-owned state mutated; no globals | pure function of explicit arguments; deterministic, no frame history, seek-safe at any point | **A** — pure strength/pixel lookup |
| `phx_db_qp_at` | `common/macroblock.h:249-270` | `const phx_mb_state_t *s`, `const phx_deblock_cfg_t *cfg`, `int xy`, `int qp` | returns int | no caller-owned state mutated; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_db_filter_edge` | `common/deblock.c:417-447` | `uint8_t *pixy`, `uint8_t *pixuv`, `intptr_t stridey`, `intptr_t strideuv`, `int dir`, `int edge`, `const uint8_t bs[4]`, `int qp`, `int qpc`, `int a`, `int b`, `int intra`, `int transform_8x8` | writes `pixy`, `pixuv` | explicit output pointer(s) `pixy`, `pixuv`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_frame_deblock_ex` | `common/deblock.c:417-447` | `phx_frame_t *f`, `phx_mb_state_t *s`, `phx_mb_cache_t *c`, `int mb_x`, `int mb_y`, `int qp`, `int b_cabac`, `const phx_deblock_cfg_t *cfg` | writes `f`, `s`, `c`; returns int | explicit output pointer(s) `f`, `s`, `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |
| `phx_frame_deblock` | `common/deblock.c:417-447` | `phx_frame_t *f`, `phx_mb_state_t *s`, `phx_mb_cache_t *c`, `int mb_x`, `int mb_y`, `int qp`, `int b_cabac` | writes `f`, `s`, `c`; returns int | explicit output pointer(s) `f`, `s`, `c`; no globals | deterministic given already-completed left/top neighbour state; enter from a frame/slice boundary, not mid-MB | **B** — one MB workgroup, edge-parallel |

---

## 27. Module `ratecontrol` — `src/phx_ratecontrol.c`

`SHADER: C — serial per-frame state; AQ writes one independent result per MB` (`phx_ratecontrol.c:8`). Per-frame QP/CRF scalar derivation from `encoder/ratecontrol.c`.

| Phoenix function | x264 source | Inputs | Outputs | Mutates | Determinism / seek-safety | SHADER & rationale |
|---|---|---|---|---|---|---|
| `rc_clip3i` | `Phoenix helper (no standalone x264 definition)` | `int v`, `int lo`, `int hi` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `rc_clip3f` | `Phoenix helper (no standalone x264 definition)` | `double v`, `double lo`, `double hi` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `rc_max2` | `Phoenix helper (no standalone x264 definition)` | `double a`, `double b` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `rc_qp2qscale` | `encoder/ratecontrol.c:203-210` | `float qp` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `rc_qscale2qp` | `encoder/ratecontrol.c:203-210` | `float qscale` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `rc_exp2fix8` | `common/base.h:218-224` | `float x` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `rc_clz` | `common/base.h:226-230` | `uint32_t x` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `rc_log2_u32` | `common/base.h:226-230` | `uint32_t x` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `rc_clip_duration` | `encoder/ratecontrol.h:38-40` | `double d` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_init` | `encoder/ratecontrol.c:750-825` | `phx_ratecontrol_t **rc_out`, `const phx_param_t *p`, `int mb_count` | writes `rc_out`; returns int | explicit output pointer(s) `rc_out`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_delete` | `encoder/ratecontrol.c:750-825` | `phx_ratecontrol_t *rc` | writes `rc` | explicit output pointer(s) `rc`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_configure` | `encoder/ratecontrol.c:750-825` | `phx_ratecontrol_t *rc`, `const phx_ratecontrol_config_t *cfg` | writes `rc`; returns int | explicit output pointer(s) `rc`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_set_complexity` | `encoder/ratecontrol.c:750-825` | `phx_ratecontrol_t *rc`, `int satd` | writes `rc`; returns int | explicit output pointer(s) `rc`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_set_quant_offsets` | `encoder/ratecontrol.c:750-825` | `phx_ratecontrol_t *rc`, `const float *quant_offsets` | writes `rc`; returns int | explicit output pointer(s) `rc`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `rc_accum_p_qp_update` | `encoder/ratecontrol.c:1410-1420` | `phx_ratecontrol_t *rc`, `double qp`, `int slice_type` | writes `rc` | explicit output pointer(s) `rc`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_start` | `encoder/ratecontrol.c:1431-1538` | `phx_ratecontrol_t *rc`, `int slice_type`, `int frame_bits`, `int *qp_out` | writes `rc`, `qp_out`; returns int | explicit output pointer(s) `rc`, `qp_out`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_end` | `encoder/ratecontrol.c:1828-1928` | `phx_ratecontrol_t *rc`, `int slice_type`, `int bits` | writes `rc`; returns int | explicit output pointer(s) `rc`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `rc_ac_energy_mb` | `encoder/ratecontrol.c:225-257` | `phx_ratecontrol_t *rc`, `const phx_frame_t *f`, `int mb_x`, `int mb_y` | writes `rc` | explicit output pointer(s) `rc`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_aq_frame` | `encoder/ratecontrol.c:304-415` | `phx_ratecontrol_t *rc`, `phx_frame_t *f`, `const phx_param_t *p` | writes `rc`, `f`; returns int | explicit output pointer(s) `rc`, `f`; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_mb_qp` | `encoder/ratecontrol.c:1754-1768` | `const phx_ratecontrol_t *rc`, `int mb_xy`, `int frame_qp`, `int b_kept_as_ref` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_qp_offsets` | `encoder/ratecontrol.c:1754-1768` | `const phx_ratecontrol_t *rc` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_qp_offsets_aq` | `encoder/ratecontrol.c:1754-1768` | `const phx_ratecontrol_t *rc` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_inv_qscale` | `encoder/ratecontrol.c:1754-1768` | `const phx_ratecontrol_t *rc` | no caller-visible output (local/serial state only) | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_qp_constant` | `encoder/ratecontrol.c:1754-1768` | `const phx_ratecontrol_t *rc`, `int slice_type` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |
| `phx_ratecontrol_chroma_qp` | `common/common.h:58-64` | `const phx_param_t *p`, `int qp` | returns int | no caller-owned state mutated; no globals | strictly serial / ordered owner state; reproducible only from the documented reset boundary (slice start or NAL start) | **C** — serial per-frame QP/CRF derivation |

---

## NOT YET PORTED (from `docs/FUNCTION_INVENTORY.md` MISSING rows)

The inventory is regenerated mechanically by `tools/gen_function_inventory.py`. 
At this scan it indexes **1386** x264 C functions: **231 PORTED** (16.7%) and **1155 MISSING**. 
The tables below are taken directly from its MISSING rows; the source:line is the exact x264 definition site. 
A MISSING row means the same-named function does not exist in `src/` — it is not a claim about a replacement kernel.

| Inventory status | Rows | Share of 1386 |
|---|---:|---:|
| PORTED | 231 | 16.7% |
| MISSING | 1155 | 83.3% |

MISSING rows by top-level x264 directory:

| Directory | MISSING rows |
|---|---:|
| `common` | 792 |
| `encoder` | 322 |
| `autocomplete.c:231` | 1 |
| `autocomplete.c:240` | 1 |
| `autocomplete.c:246` | 1 |
| `autocomplete.c:256` | 1 |
| `autocomplete.c:268` | 1 |
| `autocomplete.c:293` | 1 |
| `example.c:45` | 1 |
| `x264.c:80` | 1 |
| `x264.c:88` | 1 |
| `x264.c:120` | 1 |
| `x264.c:266` | 1 |
| `x264.c:296` | 1 |
| `x264.c:306` | 1 |
| `x264.c:358` | 1 |
| `x264.c:415` | 1 |
| `x264.c:421` | 1 |
| `x264.c:440` | 1 |
| `x264.c:460` | 1 |
| `x264.c:480` | 1 |
| `x264.c:1185` | 1 |
| `x264.c:1224` | 1 |
| `x264.c:1262` | 1 |
| `x264.c:1271` | 1 |
| `x264.c:1280` | 1 |
| `x264.c:1301` | 1 |
| `x264.c:1364` | 1 |
| `x264.c:1375` | 1 |
| `x264.c:1386` | 1 |
| `x264.c:1729` | 1 |
| `x264.c:1801` | 1 |
| `x264.c:1851` | 1 |
| `x264.c:1871` | 1 |
| `x264.c:1899` | 1 |
| `x264.c:2041` | 1 |
| `x264.h:299` | 1 |
| `x264.h:333` | 1 |
| `x264.h:398` | 1 |
| `x264cli.h:50` | 1 |
| `x264cli.h:62` | 1 |
| `x264cli.h:67` | 1 |
| `x264dll.c:30` | 1 |

### Roadmap-relevant MISSING functions (`encoder/` rows)

These are the functions WP3–WP8 still has to supply. The full MISSING list, including 
the architecture-specific `common/*` DSP variants and CLI glue, lives in `FUNCTION_INVENTORY.md`.

| x264 function | source:line | Roadmap package |
|---|---|---|
| `x264_analyse_weight_frame` | `encoder/analyse.c:219` | WP7 — entropy / NAL |
| `predict_16x16_mode_available` | `encoder/analyse.c:530` | WP7 — entropy / NAL |
| `predict_8x8_mode_available` | `encoder/analyse.c:544` | WP7 — entropy / NAL |
| `predict_4x4_mode_available` | `encoder/analyse.c:552` | WP7 — entropy / NAL |
| `psy_trellis_init` | `encoder/analyse.c:561` | WP7 — entropy / NAL |
| `mb_init_fenc_cache` | `encoder/analyse.c:570` | WP7 — entropy / NAL |
| `mb_analyse_intra_chroma` | `encoder/analyse.c:586` | WP7 — entropy / NAL |
| `mb_analyse_intra` | `encoder/analyse.c:668` | WP7 — entropy / NAL |
| `now` | `encoder/analyse.c:833` | WP7 — entropy / NAL |
| `now` | `encoder/analyse.c:959` | WP7 — entropy / NAL |
| `intra_rd` | `encoder/analyse.c:982` | WP7 — entropy / NAL |
| `intra_rd` | `encoder/analyse.c:1060` | WP7 — entropy / NAL |
| `mb_analyse_inter_p16x16` | `encoder/analyse.c:1255` | WP7 — entropy / NAL |
| `mb_analyse_inter_p8x8_mixed_ref` | `encoder/analyse.c:1334` | WP7 — entropy / NAL |
| `mb_analyse_inter_p8x8` | `encoder/analyse.c:1425` | WP7 — entropy / NAL |
| `mb_analyse_inter_p16x8` | `encoder/analyse.c:1480` | WP7 — entropy / NAL |
| `mb_analyse_inter_p8x16` | `encoder/analyse.c:1546` | WP7 — entropy / NAL |
| `mb_analyse_inter_p4x4_chroma_internal` | `encoder/analyse.c:1611` | WP7 — entropy / NAL |
| `chromapix` | `encoder/analyse.c:1671` | WP7 — entropy / NAL |
| `mb_analyse_inter_p4x4_chroma` | `encoder/analyse.c:1675` | WP7 — entropy / NAL |
| `mb_analyse_inter_p4x4` | `encoder/analyse.c:1685` | WP7 — entropy / NAL |
| `mb_analyse_inter_p8x4` | `encoder/analyse.c:1724` | WP7 — entropy / NAL |
| `mb_analyse_inter_p4x8` | `encoder/analyse.c:1760` | WP7 — entropy / NAL |
| `analyse_bi_chroma` | `encoder/analyse.c:1796` | WP7 — entropy / NAL |
| `mb_analyse_inter_direct` | `encoder/analyse.c:1844` | WP7 — entropy / NAL |
| `PIXEL_8x8` | `encoder/analyse.c:1861` | WP7 — entropy / NAL |
| `chromapix` | `encoder/analyse.c:1884` | WP7 — entropy / NAL |
| `mb_analyse_inter_b16x16` | `encoder/analyse.c:1890` | WP7 — entropy / NAL |
| `PIXEL_16x16` | `encoder/analyse.c:2003` | WP7 — entropy / NAL |
| `PIXEL_16x16` | `encoder/analyse.c:2006` | WP7 — entropy / NAL |
| `PIXEL_16x16` | `encoder/analyse.c:2019` | WP7 — entropy / NAL |
| `load_deinterleave_chroma_fenc` | `encoder/analyse.c:2037` | WP7 — entropy / NAL |
| `chromapix` | `encoder/analyse.c:2055` | WP7 — entropy / NAL |
| `mb_cache_mv_p8x8` | `encoder/analyse.c:2076` | WP7 — entropy / NAL |
| `mb_load_mv_direct8x8` | `encoder/analyse.c:2106` | WP7 — entropy / NAL |
| `mb_cache_mv_b8x8` | `encoder/analyse.c:2142` | WP7 — entropy / NAL |
| `mb_cache_mv_b16x8` | `encoder/analyse.c:2161` | WP7 — entropy / NAL |
| `mb_cache_mv_b8x16` | `encoder/analyse.c:2165` | WP7 — entropy / NAL |
| `mb_analyse_inter_b8x8_mixed_ref` | `encoder/analyse.c:2171` | WP7 — entropy / NAL |
| `mb_analyse_inter_b8x8` | `encoder/analyse.c:2283` | WP7 — entropy / NAL |
| `mb_analyse_inter_b16x8` | `encoder/analyse.c:2360` | WP7 — entropy / NAL |
| `mb_analyse_inter_b8x16` | `encoder/analyse.c:2454` | WP7 — entropy / NAL |
| `mb_analyse_p_rd` | `encoder/analyse.c:2547` | WP7 — entropy / NAL |
| `mb_analyse_b_rd` | `encoder/analyse.c:2622` | WP7 — entropy / NAL |
| `refine_bidir` | `encoder/analyse.c:2692` | WP7 — entropy / NAL |
| `mb_analyse_transform` | `encoder/analyse.c:2735` | WP7 — entropy / NAL |
| `mb_analyse_transform_rd` | `encoder/analyse.c:2773` | WP7 — entropy / NAL |
| `mb_analyse_qp_rd` | `encoder/analyse.c:2810` | WP7 — entropy / NAL |
| `X264_MIN3` | `encoder/analyse.c:3201` | WP7 — entropy / NAL |
| `analyse_update_cache` | `encoder/analyse.c:3734` | WP7 — entropy / NAL |
| `x264_encoder_reconfig` | `encoder/api.c:145` | WP7 — entropy / NAL |
| `x264_encoder_parameters` | `encoder/api.c:152` | WP7 — entropy / NAL |
| `x264_encoder_delayed_frames` | `encoder/api.c:173` | WP7 — entropy / NAL |
| `x264_encoder_maximum_delayed_frames` | `encoder/api.c:180` | WP7 — entropy / NAL |
| `x264_encoder_intra_refresh` | `encoder/api.c:187` | WP7 — entropy / NAL |
| `x264_encoder_invalidate_reference` | `encoder/api.c:194` | WP7 — entropy / NAL |
| `cabac_field_decoding_flag` | `encoder/cabac.c:70` | WP7 — entropy / NAL |
| `modulo` | `encoder/cabac.c:176` | WP7 — entropy / NAL |
| `cabac_ref_p` | `encoder/cabac.c:261` | WP7 — entropy / NAL |
| `cabac_ref_b` | `encoder/cabac.c:265` | WP7 — entropy / NAL |
| `x264_cabac_block_residual_c` | `encoder/cabac.c:750` | WP7 — entropy / NAL |
| `cabac_block_residual_422_dc` | `encoder/cabac.c:763` | WP7 — entropy / NAL |
| `reversible` | `encoder/cabac.c:773` | WP7 — entropy / NAL |
| `this` | `encoder/cabac.c:774` | WP7 — entropy / NAL |
| `x264_cabac_block_residual_8x8_rd_c` | `encoder/cabac.c:860` | WP7 — entropy / NAL |
| `x264_cabac_block_residual_rd_c` | `encoder/cabac.c:864` | WP7 — entropy / NAL |
| `cabac_block_residual_8x8` | `encoder/cabac.c:869` | WP7 — entropy / NAL |
| `cabac_block_residual_422_dc` | `encoder/cabac.c:886` | WP7 — entropy / NAL |
| `macroblock_write_cabac_internal` | `encoder/cabac.c:917` | WP7 — entropy / NAL |
| `x264_macroblock_write_cabac` | `encoder/cabac.c:1088` | WP7 — entropy / NAL |
| `ref` | `encoder/cabac.c:1102` | WP7 — entropy / NAL |
| `partition_size_cabac` | `encoder/cabac.c:1106` | WP7 — entropy / NAL |
| `subpartition_size_cabac` | `encoder/cabac.c:1171` | WP7 — entropy / NAL |
| `partition_i8x8_size_cabac` | `encoder/cabac.c:1187` | WP7 — entropy / NAL |
| `partition_i4x4_size_cabac` | `encoder/cabac.c:1203` | WP7 — entropy / NAL |
| `chroma_size_cabac` | `encoder/cabac.c:1213` | WP7 — entropy / NAL |
| `cavlc_qp_delta` | `encoder/cavlc.c:211` | WP7 — entropy / NAL |
| `cavlc_mvd` | `encoder/cavlc.c:241` | WP7 — entropy / NAL |
| `cavlc_8x8_mvd` | `encoder/cavlc.c:250` | WP7 — entropy / NAL |
| `cavlc_macroblock_luma_residual` | `encoder/cavlc.c:274` | WP7 — entropy / NAL |
| `cavlc_partition_luma_residual` | `encoder/cavlc.c:293` | WP7 — entropy / NAL |
| `cavlc_mb_header_i` | `encoder/cavlc.c:305` | WP7 — entropy / NAL |
| `cavlc_mb_header_p` | `encoder/cavlc.c:337` | WP7 — entropy / NAL |
| `cavlc_mb_header_b` | `encoder/cavlc.c:411` | WP7 — entropy / NAL |
| `x264_macroblock_write_cavlc` | `encoder/cavlc.c:487` | WP7 — entropy / NAL |
| `ref` | `encoder/cavlc.c:601` | WP7 — entropy / NAL |
| `partition_size_cavlc` | `encoder/cavlc.c:605` | WP7 — entropy / NAL |
| `subpartition_size_cavlc` | `encoder/cavlc.c:661` | WP7 — entropy / NAL |
| `cavlc_intra4x4_pred_size` | `encoder/cavlc.c:677` | WP7 — entropy / NAL |
| `partition_i8x8_size_cavlc` | `encoder/cavlc.c:685` | WP7 — entropy / NAL |
| `partition_i4x4_size_cavlc` | `encoder/cavlc.c:695` | WP7 — entropy / NAL |
| `chroma_size_cavlc` | `encoder/cavlc.c:704` | WP7 — entropy / NAL |
| `calc_psnr` | `encoder/encoder.c:56` | WP7 — entropy / NAL |
| `calc_ssim_db` | `encoder/encoder.c:65` | WP7 — entropy / NAL |
| `threadpool_wait_all` | `encoder/encoder.c:74` | WP7 — entropy / NAL |
| `frame_dump` | `encoder/encoder.c:86` | WP7 — entropy / NAL |
| `bitstream_check_buffer_internal` | `encoder/encoder.c:368` | WP7 — entropy / NAL |
| `bitstream_check_buffer` | `encoder/encoder.c:403` | WP7 — entropy / NAL |
| `bitstream_check_buffer_filler` | `encoder/encoder.c:409` | WP7 — entropy / NAL |
| `mbcmp_init` | `encoder/encoder.c:1409` | WP7 — entropy / NAL |
| `chroma_dsp_init` | `encoder/encoder.c:1429` | WP7 — entropy / NAL |
| `set_aspect_ratio` | `encoder/encoder.c:1468` | WP7 — entropy / NAL |
| `encoder_try_reconfig` | `encoder/encoder.c:1862` | WP7 — entropy / NAL |
| `x264_encoder_reconfig_apply` | `encoder/encoder.c:1932` | WP7 — entropy / NAL |
| `x264_encoder_reconfig` | `encoder/encoder.c:1955` | WP7 — entropy / NAL |
| `x264_encoder_parameters` | `encoder/encoder.c:1974` | WP7 — entropy / NAL |
| `nal_start` | `encoder/encoder.c:1981` | WP7 — entropy / NAL |
| `nal_check_buffer` | `encoder/encoder.c:1995` | WP7 — entropy / NAL |
| `check_encapsulated_buffer` | `encoder/encoder.c:2025` | WP7 — entropy / NAL |
| `encoder_encapsulate_nals` | `encoder/encoder.c:2051` | WP7 — entropy / NAL |
| `reference_check_reorder` | `encoder/encoder.c:2138` | WP7 — entropy / NAL |
| `weighted_reference_duplicate` | `encoder/encoder.c:2163` | WP7 — entropy / NAL |
| `weighted_pred_init` | `encoder/encoder.c:2202` | WP7 — entropy / NAL |
| `reference_distance` | `encoder/encoder.c:2286` | WP7 — entropy / NAL |
| `reference_build_list` | `encoder/encoder.c:2295` | WP7 — entropy / NAL |
| `fdec_filter_row` | `encoder/encoder.c:2413` | WP7 — entropy / NAL |
| `x264_pixel_ssd_wxh` | `encoder/encoder.c:2499` | WP7 — entropy / NAL |
| `reference_update` | `encoder/encoder.c:2533` | WP7 — entropy / NAL |
| `reference_reset` | `encoder/encoder.c:2563` | WP7 — entropy / NAL |
| `reference_hierarchy_reset` | `encoder/encoder.c:2571` | WP7 — entropy / NAL |
| `slice_init` | `encoder/encoder.c:2608` | WP7 — entropy / NAL |
| `bitstream_backup` | `encoder/encoder.c:2689` | WP7 — entropy / NAL |
| `ended` | `encoder/encoder.c:2704` | WP7 — entropy / NAL |
| `bitstream_restore` | `encoder/encoder.c:2723` | WP7 — entropy / NAL |
| `slice_write` | `encoder/encoder.c:2752` | WP7 — entropy / NAL |
| `M16` | `encoder/encoder.c:3021` | WP7 — entropy / NAL |
| `thread_sync_context` | `encoder/encoder.c:3132` | WP7 — entropy / NAL |
| `thread_sync_stat` | `encoder/encoder.c:3153` | WP7 — entropy / NAL |
| `slices_write` | `encoder/encoder.c:3159` | WP7 — entropy / NAL |
| `i_mb_width` | `encoder/encoder.c:3179` | WP7 — entropy / NAL |
| `threaded_slices_write` | `encoder/encoder.c:3219` | WP7 — entropy / NAL |
| `x264_encoder_intra_refresh` | `encoder/encoder.c:3280` | WP7 — entropy / NAL |
| `x264_encoder_invalidate_reference` | `encoder/encoder.c:3286` | WP7 — entropy / NAL |
| `encoder_frame_end` | `encoder/encoder.c:3904` | WP7 — entropy / NAL |
| `print_intra` | `encoder/encoder.c:4182` | WP7 — entropy / NAL |
| `PIXEL_8x8` | `encoder/encoder.c:4305` | WP7 — entropy / NAL |
| `PIXEL_4x4` | `encoder/encoder.c:4307` | WP7 — entropy / NAL |
| `i_mb_cbp` | `encoder/encoder.c:4407` | WP7 — entropy / NAL |
| `i_mb_cbp` | `encoder/encoder.c:4408` | WP7 — entropy / NAL |
| `x264_encoder_delayed_frames` | `encoder/encoder.c:4579` | WP7 — entropy / NAL |
| `x264_encoder_maximum_delayed_frames` | `encoder/encoder.c:4600` | WP7 — entropy / NAL |
| `lookahead_shift` | `encoder/lookahead.c:42` | WP7 — entropy / NAL |
| `lookahead_update_last_nonb` | `encoder/lookahead.c:59` | WP7 — entropy / NAL |
| `lookahead_slicetype_decide` | `encoder/lookahead.c:68` | WP7 — entropy / NAL |
| `lookahead_thread` | `encoder/lookahead.c:90` | WP7 — entropy / NAL |
| `x264_lookahead_init` | `encoder/lookahead.c:132` | WP7 — entropy / NAL |
| `x264_lookahead_is_empty` | `encoder/lookahead.c:200` | WP7 — entropy / NAL |
| `lookahead_encoder_shift` | `encoder/lookahead.c:210` | WP7 — entropy / NAL |
| `zigzag_scan_2x4_dc` | `encoder/macroblock.c:44` | WP7 — entropy / NAL |
| `array_non_zero` | `encoder/macroblock.c:98` | WP7 — entropy / NAL |
| `mb_optimize_chroma_dc` | `encoder/macroblock.c:245` | WP7 — entropy / NAL |
| `mb_encode_chroma_internal` | `encoder/macroblock.c:259` | WP7 — entropy / NAL |
| `i_cbp_chroma` | `encoder/macroblock.c:488` | WP7 — entropy / NAL |
| `x264_mb_encode_chroma` | `encoder/macroblock.c:492` | WP7 — entropy / NAL |
| `macroblock_encode_skip` | `encoder/macroblock.c:500` | WP7 — entropy / NAL |
| `x264_predict_lossless_chroma` | `encoder/macroblock.c:526` | WP7 — entropy / NAL |
| `x264_predict_lossless_4x4` | `encoder/macroblock.c:555` | WP7 — entropy / NAL |
| `x264_predict_lossless_8x8` | `encoder/macroblock.c:575` | WP7 — entropy / NAL |
| `x264_predict_lossless_16x16` | `encoder/macroblock.c:595` | WP7 — entropy / NAL |
| `macroblock_encode_internal` | `encoder/macroblock.c:618` | WP7 — entropy / NAL |
| `mc_luma` | `encoder/macroblock.c:665` | WP7 — entropy / NAL |
| `macroblock_probe_skip_internal` | `encoder/macroblock.c:988` | WP7 — entropy / NAL |
| `mc_luma` | `encoder/macroblock.c:1005` | WP7 — entropy / NAL |
| `load_deinterleave_chroma_fdec` | `encoder/macroblock.c:1049` | WP7 — entropy / NAL |
| `x264_macroblock_probe_skip` | `encoder/macroblock.c:1129` | WP7 — entropy / NAL |
| `x264_noise_reduction_update` | `encoder/macroblock.c:1146` | WP7 — entropy / NAL |
| `macroblock_encode_p8x8_internal` | `encoder/macroblock.c:1179` | WP7 — entropy / NAL |
| `x264_macroblock_encode_p8x8` | `encoder/macroblock.c:1370` | WP7 — entropy / NAL |
| `macroblock_encode_p4x4_internal` | `encoder/macroblock.c:1385` | WP7 — entropy / NAL |
| `x264_macroblock_encode_p4x4` | `encoder/macroblock.c:1419` | WP7 — entropy / NAL |
| `x264_me_search_ref` | `encoder/me.c:182` | WP7 — entropy / NAL |
| `i_pixel` | `encoder/me.c:659` | WP7 — entropy / NAL |
| `i_pixel` | `encoder/me.c:668` | WP7 — entropy / NAL |
| `i_pixel` | `encoder/me.c:691` | WP7 — entropy / NAL |
| `x264_me_refine_qpel` | `encoder/me.c:801` | WP7 — entropy / NAL |
| `x264_me_refine_qpel_refdupe` | `encoder/me.c:812` | WP7 — entropy / NAL |
| `refine_subpel` | `encoder/me.c:865` | WP7 — entropy / NAL |
| `me_refine_bidir` | `encoder/me.c:1027` | WP7 — entropy / NAL |
| `i_pixel` | `encoder/me.c:1123` | WP7 — entropy / NAL |
| `x264_me_refine_bidir_satd` | `encoder/me.c:1180` | WP7 — entropy / NAL |
| `x264_me_refine_bidir_rd` | `encoder/me.c:1185` | WP7 — entropy / NAL |
| `x264_rd_cost_part` | `encoder/me.c:1228` | WP7 — entropy / NAL |
| `x264_me_refine_qpel_rd` | `encoder/me.c:1233` | WP7 — entropy / NAL |
| `change` | `encoder/ratecontrol.c:139` | WP7 — entropy / NAL |
| `qp2qscale` | `encoder/ratecontrol.c:203` | WP7 — entropy / NAL |
| `qscale2qp` | `encoder/ratecontrol.c:207` | WP7 — entropy / NAL |
| `qscale2bits` | `encoder/ratecontrol.c:216` | WP7 — entropy / NAL |
| `pow` | `encoder/ratecontrol.c:221` | WP7 — entropy / NAL |
| `ac_energy_var` | `encoder/ratecontrol.c:225` | WP7 — entropy / NAL |
| `ac_energy_plane` | `encoder/ratecontrol.c:237` | WP7 — entropy / NAL |
| `height` | `encoder/ratecontrol.c:242` | WP7 — entropy / NAL |
| `ac_energy_mb` | `encoder/ratecontrol.c:260` | WP7 — entropy / NAL |
| `x264_adaptive_quant_frame` | `encoder/ratecontrol.c:304` | WP7 — entropy / NAL |
| `macroblock_tree_rescale_init` | `encoder/ratecontrol.c:417` | WP7 — entropy / NAL |
| `macroblock_tree_rescale_destroy` | `encoder/ratecontrol.c:490` | WP7 — entropy / NAL |
| `tapfilter` | `encoder/ratecontrol.c:501` | WP7 — entropy / NAL |
| `macroblock_tree_rescale` | `encoder/ratecontrol.c:509` | WP7 — entropy / NAL |
| `x264_macroblock_tree_read` | `encoder/ratecontrol.c:541` | WP7 — entropy / NAL |
| `x264_reference_build_list_optimal` | `encoder/ratecontrol.c:585` | WP7 — entropy / NAL |
| `strcat_filename` | `encoder/ratecontrol.c:622` | WP7 — entropy / NAL |
| `x264_ratecontrol_init_reconfigurable` | `encoder/ratecontrol.c:632` | WP7 — entropy / NAL |
| `pow` | `encoder/ratecontrol.c:644` | WP7 — entropy / NAL |
| `x264_ratecontrol_new` | `encoder/ratecontrol.c:744` | WP7 — entropy / NAL |
| `parse_zone` | `encoder/ratecontrol.c:1219` | WP7 — entropy / NAL |
| `parse_zones` | `encoder/ratecontrol.c:1263` | WP7 — entropy / NAL |
| `get_zone` | `encoder/ratecontrol.c:1333` | WP7 — entropy / NAL |
| `x264_ratecontrol_summary` | `encoder/ratecontrol.c:1345` | WP7 — entropy / NAL |
| `x264_ratecontrol_delete` | `encoder/ratecontrol.c:1358` | WP7 — entropy / NAL |
| `accum_p_qp_update` | `encoder/ratecontrol.c:1410` | WP7 — entropy / NAL |
| `x264_ratecontrol_zone_init` | `encoder/ratecontrol.c:1422` | WP7 — entropy / NAL |
| `x264_ratecontrol_start` | `encoder/ratecontrol.c:1432` | WP7 — entropy / NAL |
| `predict_row_size` | `encoder/ratecontrol.c:1540` | WP7 — entropy / NAL |
| `row_bits_so_far` | `encoder/ratecontrol.c:1569` | WP7 — entropy / NAL |
| `predict_row_size_to_end` | `encoder/ratecontrol.c:1577` | WP7 — entropy / NAL |
| `x264_ratecontrol_mb` | `encoder/ratecontrol.c:1590` | WP7 — entropy / NAL |
| `x264_ratecontrol_qp` | `encoder/ratecontrol.c:1748` | WP7 — entropy / NAL |
| `x264_ratecontrol_mb_qp` | `encoder/ratecontrol.c:1754` | WP7 — entropy / NAL |
| `x264_ratecontrol_slice_type` | `encoder/ratecontrol.c:1771` | WP7 — entropy / NAL |
| `x264_ratecontrol_set_weights` | `encoder/ratecontrol.c:1812` | WP7 — entropy / NAL |
| `x264_ratecontrol_end` | `encoder/ratecontrol.c:1829` | WP7 — entropy / NAL |
| `nrt_first_access_unit` | `encoder/ratecontrol.c:1962` | WP7 — entropy / NAL |
| `get_qscale` | `encoder/ratecontrol.c:2003` | WP7 — entropy / NAL |
| `get_diff_limited_q` | `encoder/ratecontrol.c:2037` | WP7 — entropy / NAL |
| `predict_size` | `encoder/ratecontrol.c:2109` | WP7 — entropy / NAL |
| `update_predictor` | `encoder/ratecontrol.c:2114` | WP7 — entropy / NAL |
| `update_vbv` | `encoder/ratecontrol.c:2137` | WP7 — entropy / NAL |
| `x264_hrd_fullness` | `encoder/ratecontrol.c:2194` | WP7 — entropy / NAL |
| `update_vbv_plan` | `encoder/ratecontrol.c:2217` | WP7 — entropy / NAL |
| `clip_qscale` | `encoder/ratecontrol.c:2242` | WP7 — entropy / NAL |
| `vbv_pass1` | `encoder/ratecontrol.c:2266` | WP7 — entropy / NAL |
| `fenc_cpb_duration` | `encoder/ratecontrol.c:2275` | WP7 — entropy / NAL |
| `rate_estimate_qscale` | `encoder/ratecontrol.c:2400` | WP7 — entropy / NAL |
| `total_bits` | `encoder/ratecontrol.c:2406` | WP7 — entropy / NAL |
| `threads_normalize_predictors` | `encoder/ratecontrol.c:2665` | WP7 — entropy / NAL |
| `x264_threads_distribute_ratecontrol` | `encoder/ratecontrol.c:2675` | WP7 — entropy / NAL |
| `bits` | `encoder/ratecontrol.c:2714` | WP7 — entropy / NAL |
| `x264_threads_merge_ratecontrol` | `encoder/ratecontrol.c:2729` | WP7 — entropy / NAL |
| `x264_thread_sync_ratecontrol` | `encoder/ratecontrol.c:2754` | WP7 — entropy / NAL |
| `find_underflow` | `encoder/ratecontrol.c:2805` | WP7 — entropy / NAL |
| `fix_underflow` | `encoder/ratecontrol.c:2836` | WP7 — entropy / NAL |
| `count_expected_bits` | `encoder/ratecontrol.c:2855` | WP7 — entropy / NAL |
| `vbv_pass2` | `encoder/ratecontrol.c:2868` | WP7 — entropy / NAL |
| `init_pass2` | `encoder/ratecontrol.c:2932` | WP7 — entropy / NAL |
| `fps` | `encoder/ratecontrol.c:3111` | WP7 — entropy / NAL |
| `cached_hadamard` | `encoder/rdo.c:75` | WP7 — entropy / NAL |
| `cache_index` | `encoder/rdo.c:80` | WP7 — entropy / NAL |
| `cached_satd` | `encoder/rdo.c:94` | WP7 — entropy / NAL |
| `cache_index` | `encoder/rdo.c:99` | WP7 — entropy / NAL |
| `ssd_plane` | `encoder/rdo.c:123` | WP7 — entropy / NAL |
| `ssd_mb` | `encoder/rdo.c:150` | WP7 — entropy / NAL |
| `rd_cost_mb` | `encoder/rdo.c:162` | WP7 — entropy / NAL |
| `rd_cost_subpart` | `encoder/rdo.c:201` | WP7 — entropy / NAL |
| `ssd_plane` | `encoder/rdo.c:214` | WP7 — entropy / NAL |
| `x264_rd_cost_part` | `encoder/rdo.c:233` | WP7 — entropy / NAL |
| `ssd_plane` | `encoder/rdo.c:261` | WP7 — entropy / NAL |
| `rd_cost_i8x8` | `encoder/rdo.c:279` | WP7 — entropy / NAL |
| `ssd_plane` | `encoder/rdo.c:296` | WP7 — entropy / NAL |
| `ssd_plane` | `encoder/rdo.c:330` | WP7 — entropy / NAL |
| `rd_cost_chroma` | `encoder/rdo.c:349` | WP7 — entropy / NAL |
| `ssd_plane` | `encoder/rdo.c:357` | WP7 — entropy / NAL |
| `quant_trellis_cabac` | `encoder/rdo.c:642` | WP7 — entropy / NAL |
| `quant_trellis_cavlc` | `encoder/rdo.c:921` | WP7 — entropy / NAL |
| `roundings` | `encoder/rdo.c:966` | WP7 — entropy / NAL |
| `quant_trellis_cavlc` | `encoder/rdo.c:1137` | WP7 — entropy / NAL |
| `transpose` | `encoder/set.c:36` | WP7 — entropy / NAL |
| `scaling_list_write` | `encoder/set.c:43` | WP7 — entropy / NAL |
| `X264_MIN` | `encoder/set.c:151` | WP7 — entropy / NAL |
| `x264_sps_init_reconfigurable` | `encoder/set.c:249` | WP7 — entropy / NAL |
| `x264_sps_init_scaling_list` | `encoder/set.c:267` | WP7 — entropy / NAL |
| `x264_sei_recovery_point_write` | `encoder/set.c:574` | WP7 — entropy / NAL |
| `x264_sei_buffering_period_write` | `encoder/set.c:625` | WP7 — entropy / NAL |
| `x264_sei_pic_timing_write` | `encoder/set.c:647` | WP7 — entropy / NAL |
| `x264_sei_frame_packing_write` | `encoder/set.c:678` | WP7 — entropy / NAL |
| `x264_sei_mastering_display_write` | `encoder/set.c:720` | WP7 — entropy / NAL |
| `x264_sei_content_light_level_write` | `encoder/set.c:745` | WP7 — entropy / NAL |
| `x264_sei_alternative_transfer_write` | `encoder/set.c:762` | WP7 — entropy / NAL |
| `x264_filler_write` | `encoder/set.c:778` | WP7 — entropy / NAL |
| `x264_sei_dec_ref_pic_marking_write` | `encoder/set.c:789` | WP7 — entropy / NAL |
| `x264_sei_avcintra_umid_write` | `encoder/set.c:821` | WP7 — entropy / NAL |
| `x264_sei_avcintra_vanc_write` | `encoder/set.c:849` | WP7 — entropy / NAL |
| `x264_validate_levels` | `encoder/set.c:876` | WP7 — entropy / NAL |
| `x264_opencl_flush` | `encoder/slicetype-cl.c:58` | WP7 — entropy / NAL |
| `opencl_alloc_locked` | `encoder/slicetype-cl.c:72` | WP7 — entropy / NAL |
| `x264_opencl_lowres_init` | `encoder/slicetype-cl.c:82` | WP7 — entropy / NAL |
| `clCreateImage2D` | `encoder/slicetype-cl.c:95` | WP7 — entropy / NAL |
| `optimal_launch_dims` | `encoder/slicetype-cl.c:291` | WP7 — entropy / NAL |
| `x264_opencl_motionsearch` | `encoder/slicetype-cl.c:349` | WP7 — entropy / NAL |
| `x264_opencl_finalize_cost` | `encoder/slicetype-cl.c:540` | WP7 — entropy / NAL |
| `X264_MIN` | `encoder/slicetype-cl.c:554` | WP7 — entropy / NAL |
| `x264_opencl_slicetype_prep` | `encoder/slicetype-cl.c:653` | WP7 — entropy / NAL |
| `x264_opencl_slicetype_end` | `encoder/slicetype-cl.c:715` | WP7 — entropy / NAL |
| `x264_opencl_precalculate_frame_cost` | `encoder/slicetype-cl.c:730` | WP7 — entropy / NAL |
| `lowres_context_init` | `encoder/slicetype.c:45` | WP7 — entropy / NAL |
| `weight` | `encoder/slicetype.c:63` | WP7 — entropy / NAL |
| `weight_get_h264` | `encoder/slicetype.c:64` | WP7 — entropy / NAL |
| `weight_cost_init_luma` | `encoder/slicetype.c:77` | WP7 — entropy / NAL |
| `weight_cost_init_chroma` | `encoder/slicetype.c:111` | WP7 — entropy / NAL |
| `weight_cost_init_chroma444` | `encoder/slicetype.c:142` | WP7 — entropy / NAL |
| `weight_slice_header_cost` | `encoder/slicetype.c:170` | WP7 — entropy / NAL |
| `weight_cost_luma` | `encoder/slicetype.c:191` | WP7 — entropy / NAL |
| `weight_cost_chroma` | `encoder/slicetype.c:224` | WP7 — entropy / NAL |
| `weight_cost_chroma444` | `encoder/slicetype.c:257` | WP7 — entropy / NAL |
| `x264_weights_analyse` | `encoder/slicetype.c:284` | WP7 — entropy / NAL |
| `slicetype_mb_cost` | `encoder/slicetype.c:514` | WP7 — entropy / NAL |
| `slicetype_slice_cost` | `encoder/slicetype.c:814` | WP7 — entropy / NAL |
| `slicetype_frame_cost_recalculate` | `encoder/slicetype.c:999` | WP7 — entropy / NAL |
| `macroblock_tree_finish` | `encoder/slicetype.c:1029` | WP7 — entropy / NAL |
| `macroblock_tree_propagate` | `encoder/slicetype.c:1051` | WP7 — entropy / NAL |
| `mbtree_propagate_list` | `encoder/slicetype.c:1078` | WP7 — entropy / NAL |
| `vbv_frame_cost` | `encoder/slicetype.c:1186` | WP7 — entropy / NAL |
| `calculate_durations` | `encoder/slicetype.c:1199` | WP7 — entropy / NAL |
| `slicetype_path` | `encoder/slicetype.c:1333` | WP7 — entropy / NAL |
| `scenecut_internal` | `encoder/slicetype.c:1384` | WP7 — entropy / NAL |
| `pcost` | `encoder/slicetype.c:1424` | WP7 — entropy / NAL |
| `scenecut` | `encoder/slicetype.c:1430` | WP7 — entropy / NAL |
| `x264_slicetype_analyse` | `encoder/slicetype.c:1473` | WP7 — entropy / NAL |
| `scenecut` | `encoder/slicetype.c:1523` | WP7 — entropy / NAL |
| `scenecut` | `encoder/slicetype.c:1657` | WP7 — entropy / NAL |
| `x264_slicetype_decide` | `encoder/slicetype.c:1745` | WP7 — entropy / NAL |
| `f_duration` | `encoder/slicetype.c:1769` | WP7 — entropy / NAL |
| `i_frame_reference` | `encoder/slicetype.c:1820` | WP7 — entropy / NAL |
| `x264_rc_analyse_slice` | `encoder/slicetype.c:1976` | WP7 — entropy / NAL |

### Other notable MISSING groups

- `common/ratecontrol.c` does not exist; rate control lives under `encoder/ratecontrol.c` above.
- `common/*` MISSING rows are mostly SIMD variants (`x86`, `aarch64`, `ppc`, `mips`) plus 
  `common/mc.c` interpolation filters not used by the 8-bit progressive scalar path.
- CLI/application glue (`x264.c`, `x264cli.h`, `example.c`, `autocomplete.c`) is outside the shader backend.

---

## Appendix — full function-name index by class (name coverage check)

Every one of the 505 function definitions in `src/*.c` appears exactly once below. 
Compound rows above use one row per function; duplicate static names are listed once per defining file.

### Class A — 185 definitions

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

### Class B — 109 definitions

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

### Class C — 211 definitions

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

## Consequences for the remaining work

- `phx_mb_decision_t` is now written by `phx_analysis.c`, `phx_mbencode.c` and `phx_chroma.c` 
  (B stages) and read only by the C entropy/frame-loop stages.
- The centred `cost_mv` pointers and the `i_row_satds` pointer arrays still need flattening before an SSBO can hold them.
- Keep the `SHADER: A|B|C` line on every new function, as required by `docs/SHADER_MAPPING.md:72–80` and `README.md` rule 2; 
  regenerate `docs/FUNCTION_INVENTORY.md` after each port so this index stays complete.

