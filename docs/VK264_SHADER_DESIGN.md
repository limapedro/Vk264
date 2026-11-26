# Vk264 as a shader-based x264 encoder: design, fidelity, and performance

**Status**: technical design / feasibility study. No shader code, SPIR-V, GLSL, Vulkan
target, or benchmark exists in this repository at the time of writing. Every name of a
shader entry point below is a **proposed label** for work already described in
`docs/GPU_PIPELINE_SPEC.md`, not an implemented artifact
(`docs/GPU_PIPELINE_SPEC.md:3-19`, `:1124-1126`).

**Branch**: `vk264-phoenix`. All `reference/x264/...:N` citations refer to the vendored
x264 in this checkout and were checked against the files; all `docs/...:N` and `src/...:N`
citations refer to this working tree. Some earlier docs carry stale line numbers; where
they disagree the source file wins, and the drift is recorded in
`docs/GPU_PIPELINE_SPEC.md:10-13`, `:1163-1164`.

---

## 0. How to read this document: facts, estimates, and confidence

This document deliberately separates three kinds of statement:

| Kind | Meaning | How marked |
|---|---|---|
| **Fact (repo)** | Reproducible from this checkout or a cited file | Asserted with a `reference/x264/...:N`, `docs/...:N`, or `src/...:N` citation |
| **Fact (external)** | Public product specification, recalled by the author, not measured here | Tagged `~` and `spec-only`; confidence column; **must be re-verified against a vendor datasheet before it is used as a gate** |
| **Estimate** | Analytical model output, not a measurement | Tagged `model`, with explicit inputs and a confidence level (H/M/L) |

**No measured GPU speedup is claimed anywhere in this document.** There is no Vulkan
implementation and no GPU timing data in the repository. Likewise, the repo contains no
measured x264 fps benchmark; statements about CPU speed are model estimates, not
measurements. The only measured numbers used here are the parity/test counts in §1 and
`docs/PORTING_ROADMAP.md:102-143`.

`SHADER: A / B / C` tags are the repository's own classification, restated in §2.1 and
taken from `docs/SHADER_MAPPING.md:7-56` and `docs/FUNCTION_PORTS.md:1-16`. They describe
a **function**, not a buffer (`docs/SHADER_NOTES.md:21-24`).

---

## 1. The authoritative baseline: what "faithful" means here

The CPU port is the specification for the shader backend
(`docs/SHADER_MAPPING.md:1-5`). x264 is the oracle; a plausible decode is not parity.

| Measured fact | Value | Evidence |
|---|---|---|
| x264 reference cases generated | 100/100 `rc=0` (5 fixtures × 10 presets × 2 entropies) | `docs/PARITY_GAP_MATRIX.md:9` |
| Phoenix encodes | 100/100 `rc=0`, no crashes | `docs/PARITY_GAP_MATRIX.md:10`, `docs/PORTING_ROADMAP.md:111` |
| Header parity | SPS/PPS byte-exact 20/20; version SEI exact; start codes match | `docs/PARITY_GAP_MATRIX.md:11-14`, `docs/PORTING_ROADMAP.md:108` |
| IDR slice header | byte-exact for every preset | `docs/PARITY_GAP_MATRIX.md:13` |
| MB0 CABAC bin trace | 715/715 bins exact; first divergence at MB1 | `docs/PORTING_ROADMAP.md:109` |
| **Full-stream byte-exact parity** | **0/100** | `docs/PARITY_GAP_MATRIX.md:15`, `docs/PORTING_ROADMAP.md:114`, `docs/NIGHTLY_STATUS.md:41-50` |
| First divergent byte (post-fix) | 586 (ultrafast) … 677 (placebo), median 672 | `docs/PARITY_GAP_MATRIX.md:16` |
| Function inventory | 1386 x264 C functions indexed; 273 PORTED (19.7%), 1113 MISSING | `docs/PORTING_ROADMAP.md:107`, `docs/FUNCTION_INVENTORY.md:10` |
| Working-tree definitions | 505 across 27 modules: 185 A, 109 B, 211 C | `docs/SHADER_NOTES.md:19`, `docs/FUNCTION_INVENTORY.md:5-8` |
| Real-video stream blowup | Phoenix 9469 B vs x264 2475 B on a 64×64/30-frame CABAC crop; first P frame 292 B vs 6 B | `docs/BYTE_EXACT_FUNCTIONS.md:29-44` |

**Why 0/100 matters for the shader design.** A shader kernel is accepted only when its
outputs are byte-identical to the CPU function it replaces (`docs/GPU_PIPELINE_SPEC.md:53-55`).
If the CPU reference is itself divergent from x264, a byte-exact shader would faithfully
reproduce a wrong stream. That is why `docs/GPU_PIPELINE_SPEC.md` P0 requires frozen CPU
goldens *first* (`:1041-1050`) and why `docs/NIGHTLY_STATUS.md:41-50` declares the byte-exact
count to be the only parity claim. The gates in §8 are ordered accordingly: **fix the CPU
oracle before writing shaders**.

Two nuances that limit the apparent size of the porting job:

* The 1113 "MISSING" functions include large numbers of ARM/AArch64/MIPS/LoongArch SIMD
  variants (e.g. `docs/FUNCTION_INVENTORY.md:16-34`, `:243-282`, `:364-454`). Those are
  irrelevant to a Vulkan backend, which re-implements the *C reference semantics*, not the
  CPU SIMD dispatch. Do not treat 19.7% as a shader-completion percentage.
* A "chosen mode matches" result is not a golden: `docs/BYTE_EXACT_FUNCTIONS.md:3` requires
  exact inputs *and* outputs of every contracted function.

---

## 2. Architecture: the complete shader pipeline

### 2.1 The three classes, restated

| Class | Contract | Vulkan interpretation | Source |
|---|---|---|---|
| **A** | One invocation per work item; explicit inputs/outputs; no hidden state | Pure compute; UBO/SSBO reads and indexed SSBO writes; no atomics or inter-item barriers | `docs/SHADER_MAPPING.md:7-25` |
| **B** | Ordered but local: neighbour data inside a frame/MB, not across frames | Workgroup shared memory (`phx_mb_cache_t`, ~2.5 KiB) + inter-dispatch barriers; state dies at frame end | `docs/SHADER_MAPPING.md:27-38`, `docs/GPU_PIPELINE_SPEC.md:313-328` |
| **C** | Mutates one evolving state | One logical owner: the host serial loop in the baseline; a single-invocation GPU shader only as an optional later port | `docs/SHADER_MAPPING.md:40-56`, `docs/GPU_PIPELINE_SPEC.md:37` |

### 2.2 Stage DAG (raw YUV → Annex-B)

The baseline DAG is reproduced from `docs/GPU_PIPELINE_SPEC.md:67-145` and grouped by owner.

```
HOST / C — immutable for whole encode
  param_default, apply_preset, finalize, apply_profile, sps/pps/sei  → UBO 0/1/2
  cost tables (S0), RDO tables (host, rdo.c:384-415)                → SSBO 0

A — frame setup (per input picture)
  upload/copy picture → SSBO 1; duplicate last row/col
  cs_lowres_core (FILTER recurrence, mc.c:484-507) → 4 lowres planes
  cs_lowres_border; cs_lowres_clear (sentinels -1 / 0x7FFF)

A — lowres cost kernels
  lr_sad8 / lr_satd_8x4 / lr_satd8 / lr_get_ref / lr_bidir_avg
  lr_eval_full / lr_eval_spel_* / lr_fullpel_search / lr_refine_subpel
  lr_check_predictors / lr_try_bidir                              → SSBO 5/6

C — lookahead / slice type (host)
  put_frame → frame cost → path cost → scenecut → analyse → decide
  last_nonb, last_keyframe, B/BREF, coded-order frame_num/POC

C — frame context / rate control (host)
  DPB insert/MMCO, ref lists, frame_num/POC, nal_ref_idc
  ratecontrol_start → UBO 3 + SSBO 9 (per-MB QP / AQ offsets)
  slice header + CABAC context init

A — fullpel ME (P slices)
  cs_me_fullpel (DIA/HEX, SAD + λ·bits)                           → SSBO 6

B — per-MB row wavefront
  cache_load_neighbours/cache_load → shared cache
  analysis (I4/I8/I16/chroma, inter), DCT/Hadamard, quant, MC,
  reconstruct, chroma, backup_intra, cache_save                    → SSBO 2/3/7

B — in-loop deblock (per MB row)                                   → SSBO 1 recon / SSBO 8
  NOTE: not wired into phx_encode() in the CPU port (§2.6)

=== BOUNDARY: SSBO 3 = phx_mb_decision_t[n], read-only below ===

C — serial entropy (host)
  CABAC bins (mvd/ref/cbp/qp_delta/residual) or CAVLC tokens
  trellis QNS coefficient state machine; flush; one RBSP cursor

C — NAL assembly
  rbsp_trailing / cabac flush; nal_escape (2-byte lookbehind);
  nal_encode; Annex-B
```

The ordering constraints are not stylistic: lookahead assigns frame types in **coded
order** and advances `last_nonb` (`reference/x264/encoder/lookahead.c:59`,
`docs/GPU_PIPELINE_SPEC.md:169-186`); rate control starts after the type decision and ends
after the NAL size is known (`src/phx_encoder.c:1338-1346`, `:1563-1564`); the CABAC flush
bit depends on `h->i_frame & 31` (`reference/x264/common/cabac.c:174`, verified). Any
parallel reordering of these stages changes bytes, not just timing.

### 2.3 Vulkan dispatch plan (baseline and alternative)

The entry-point table below is the spec's baseline (`docs/GPU_PIPELINE_SPEC.md:214-230`);
`ceil_div(a,b) = (a+b-1)/b`, `n = mb_width*mb_height`.

| # | Proposed entry point | Class | Local size | Groups | Reads → Writes |
|---|---|---|---|---|---|
| S0 | `cs_cost_tables_init` | A | (64,1,1) | `(ceil_div(70*(2*4*mv_range+1),64),4,1)` ×4 tables | UBO 0/1 → SSBO 0 |
| S1 | `cs_lowres_core` | A | (8,8,1) | `(ceil_div(w_lowres,8),ceil_div(h_lowres,8),1)` | SSBO 1 fenc → SSBO 1 lowres[0..3] |
| S2 | `cs_lowres_border` | A | (64,1,1) | `(ceil_div(stride_lowres,64), lines+2*PHX_PADV,4)` | SSBO 1 → padded lowres |
| S3 | `cs_lowres_clear` | A | (64,1,1) | `(ceil_div(n,64),1,1)` | UBO 0 → SSBO 1/5 sentinels |
| S4 | `cs_lowres_me` | A | (8,8,1) | per `(p0,p1,b)` triple, one 8×8 block per item | SSBO 1/0/5 → SSBO 5 |
| S5 | `cs_lookahead_decide` | C | (1,1,1) | (1,1,1) per minigop; **host recommended** | SSBO 5/6 → frame types |
| S6 | `cs_me_fullpel` | A | (64,1,1) | `(ceil_div(n,64),1,1)` | SSBO 1/0 → SSBO 6 |
| S7 | `cs_mb_row` | B | (64,1,1) | `(1,mb_height,1)` — one workgroup per MB row | SSBO 1/2/5/6/9 → SSBO 2/3/7 |
| S8 | `cs_deblock` | B | (64,1,1) | `(1,mb_height,1)` (not wired) | SSBO 1/2/8 → SSBO 1/8 |
| S9 | `cs_decision_readback` | transfer | n/a | `vkCmdCopyBuffer` SSBO 2+3 → staging | SSBO 2/3 → host |
| S10 | `cs_entropy_single_thread` | C (optional) | (1,1,1) | (1,1,1) per slice | SSBO 2/3/4 → RBSP |
| S11 | `cs_nal_escape` | C (optional) | (1,1,1) | (1,1,1) per NAL | RBSP → Annex-B |

Barriers between dependent dispatches are explicit
`vkCmdPipelineBarrier(COMPUTE→COMPUTE, SHADER_WRITE→SHADER_READ|WRITE)` plus a buffer
memory barrier; a submission boundary alone is **not** a memory dependency
(`docs/GPU_PIPELINE_SPEC.md:201-207`). The per-frame command stream is
`docs/GPU_PIPELINE_SPEC.md:268-302`: upload → S1/S2/S3 → host lookahead/frame context →
S4 per triple → S6 → S7 → [S8] → S9 copy → **host wait** → serial entropy → RC end.
The host wait is one per frame, because the entropy loop consumes SSBO 3 in scan order
(`docs/GPU_PIPELINE_SPEC.md:304-307`).

An alternative B layout is documented but not selected: per-MB workgroups of 16 threads
on anti-diagonals, with a full barrier per diagonal and a generation counter for double
buffered neighbour state (`docs/GPU_PIPELINE_SPEC.md:258-266`). It costs
`mb_width+mb_height-1` dispatches per stage. §6.3 shows why the selected row layout is
correctness-friendly but throughput-poor.

### 2.4 Buffers and shared memory

Canonical representation: all SSBO fields are 32-bit (`int`, `uint`, `float`, `ivec2`);
CPU `int8/uint8/int16` are sign/zero-extended on upload, avoiding any dependency on
`VK_KHR_8bit_storage`/`VK_KHR_16bit_storage` (`docs/GPU_PIPELINE_SPEC.md:375-382`).

| Binding | Object | Lifetime / role |
|---|---|---|
| UBO 0 | `PhxParam` (`src/phx_param.h:24-75`) | whole encode, immutable after finalize |
| UBO 1 | immutable tables + `phx_scan8[51]` + `phx_left_indices[4]` | whole encode |
| UBO 2 | `PhxRdoConfig` (`src/phx_rdo.h:80-87`) | per dispatch |
| UBO 3 | `PhxRcFrame` frame scalars | per frame, after `ratecontrol_start` |
| SSBO 0 | `PhxCostTables` (`cost_mv`, `cost_mv_fpel`, `cost_ref`, `cost_i4x4_mode`) | whole encode |
| SSBO 1 | planes, lowres[0..3], `i_cost_est`, `i_row_satds`, `lowres_mvs`, recon view | per frame |
| SSBO 2 | `PhxMbState` SoA: type, pred modes, nnz, cbp, ref/mv/mvd | per frame |
| SSBO 3 | `PhxMbDecision[n]`, **4448-byte stride** | per frame, written by B, read-only by C |
| SSBO 4 | `PhxCabacState` + RBSP (`src/phx_cabac.h:20-26`) | per slice; baseline stays on host |
| SSBO 5 | lowres per-8×8 MV/cost mirrors, frame costs, intra costs | per frame/minigop |
| SSBO 6 | `PhxMeResult[n]` (mv, sad, cost, ref; 32 B) | per frame |
| SSBO 7 | `PhxCabacSize[n]` (state[460] + `f8_bits_encoded`, ~1848 B) | one per MB workgroup |
| SSBO 8 | `PhxDeblockBs[n]` (`bs[2][8][4]`, 256 B/MB) | per frame |
| SSBO 9 | per-MB QP offsets / inv qscale | per frame, after AQ |

Full declarations: `docs/GPU_PIPELINE_SPEC.md:403-755`. Workgroup shared memory mirrors
`phx_mb_cache_t` (`src/phx_mbcache.h:61-79`): `intra4x4_pred_mode[40]`,
`non_zero_count[120]`, `ref[2][40]`, `mv[2][40][2]`, `mvd[2][40][2]` and neighbour
scalars ≈2.5 KiB (`docs/GPU_PIPELINE_SPEC.md:313-328`). `phx_scan8` and
`phx_left_indices` must be copied verbatim: earlier off-by-one defects E83–E95 came from
this layout (`docs/DATA_STRUCTURES.md:77-81`).

Two layout facts that a shader must not "simplify": (a) `decision.nnz[k]` is
block-indexed while `decision.dct[phx_scan8[k]]` is slot-indexed, a conflict resolved in
favour of the serial reader and gated by P4 (`docs/GPU_PIPELINE_SPEC.md:785-821`); (b)
chroma DC lives in separate `chroma_dct[2][5][16]` / `chroma_nnz[2][5]` arrays because
`scan8[49]/[50]` cannot be represented in the 48-entry luma arrays
(`docs/GPU_PIPELINE_SPEC.md:823-841`).

### 2.5 The 19 byte-exact function contracts mapped to shader stages

All 19 rows of `docs/BYTE_EXACT_FUNCTIONS.md:7-27`, with the proposed shader home. A row
is "matched" only against instrumented-x264 input/output goldens
(`docs/BYTE_EXACT_FUNCTIONS.md:3`).

| # | x264 function (source) | Owner / shader kernel | Class | Current state (doc) |
|---:|---|---|---|---|
| 1 | `mbcmp_init` `encoder/encoder.c:1409-1410`; `x264_pixel_sad_16x16` `common/pixel.c:73`, `satd` `common/pixel.c:234-330` | `phx_analysis.c`; `cs_sad_block` / `cs_satd_block` | A | superfast bin 5 proves SAD-vs-SATD mismatch |
| 2 | `mb_analyse_intra` I4 x3/x9 shortcuts `encoder/analyse.c:864-979` | `phx_analysis.c`; `cs_analysis_i4` | B | veryfast/faster bin 23, slow/placebo bin 5 |
| 3 | `intra_rd`, `intra_rd_refine`, `rd_cost_i4x4` `analyse.c:982-1135`, `rdo.c:162-197,315-347`, `COPY_CABAC rdo.c:71-73` | `phx_analysis.c`+`phx_rdo.c`; `cs_rd_cost` + host size API | B/C | slow/slower/veryslow/placebo |
| 4 | `x264_me_search_ref` DIA/HEX/UMH `encoder/me.c:182-419` (search `:276-419`), mv limits `:178-212` | `phx_me.c`; `cs_me_fullpel` | A | DIA/HEX pass; UMH rejected (`phx_me.c:170`) |
| 5 | subpel hpel/qpel `encoder/me.c:801-992`, `common/mc.c:173-283` | `phx_me.c`; `cs_me_subpel` (future) | A | not ported |
| 6 | `mb_analyse_intra_chroma` `encoder/analyse.c:586-665` | `phx_chroma.c`; `cs_analysis_chroma` | B | zigzag fixed; mode/coeff trace pending |
| 7 | `quant_4x4{,dc,x4}` `common/quant.c:50-109`, `common/set.c:130-205` | `phx_quant.c`; `cs_quant_dequant` | A | tables identical; I16 DC count fixed |
| 8 | `x264_quant_4x4_trellis` / `quant_trellis_cabac` `encoder/rdo.c:642-895,1142-1157` | `phx_rdo.c`; serial trellis inside S10/host | C | 10 vectors verified isolated |
| 9 | `x264_quant_chroma_dc_trellis` `encoder/rdo.c:1114-1140` | `phx_rdo.c`; serial chroma-DC trellis | C | `val=7` vs `14` at raw record 1623 |
| 10 | `cabac_block_residual_internal` `encoder/cabac.c:664-748`, `common/quant.c:878-882`, `common/tables.c:1758-1792` | `phx_cabac_syntax.c`; S10/host | C | coeff_last16 fixed; bin 932 next |
| 11 | `x264_macroblock_cache_load`/`_save` `common/macroblock.c:855,1680` | `phx_mbcache.c`; `cs_mb_row` shared cache | B | progressive+MBAFF ported; golden per MB pending |
| 12 | `x264_slice_header_write` P/B `encoder/encoder.c:213-364` | `phx_slice.c`; host C | C | IDR exact; P/B from slice #2 differ |
| 13 | `x264_slicetype_decide`, scenecut, B-adapt `encoder/slicetype.c:1384-1985` | `phx_lookahead.c`; S5/host | C | all-P/BREF gap, TRELLIS TODO |
| 14 | `x264_weights_analyse` `encoder/slicetype.c:284`, `encoder/encoder.c:2163-2284` | `phx_lookahead.c`; host C | C | not ported |
| 15 | `x264_ratecontrol_start`/`_end`, AQ, chroma QP `encoder/ratecontrol.c:1432-1538,1829-1928,304-415` | `phx_ratecontrol.c`; host C | C | CQP aligned; AQ/MB-tree incomplete |
| 16 | `x264_frame_deblock_row` `common/deblock.c:378-606`, `macroblock.c:1438-1622` | `phx_deblock.c`; S8 | B | byte-exact vs objects; not wired into encoder |
| 17 | `x264_macroblock_probe_pskip`+fast skip `encoder/analyse.c:2953-3024,1294-1307`, `common/mvpred.c:166-181` | `phx_analysis.c` + syntax writers | B/C | `d.skip=0` hard-coded at `phx_encoder.c:1438` |
| 18 | inter MB encode with residual `encoder/macroblock.c:654-775`, `common/mc.c:251-283` | `phx_mbencode.c`; `cs_residual_encode` | B | only `P_L0/D_16x16`, `cbp==0` accepted |
| 19 | I_8x8 transform/quant `encoder/macroblock.c:379-417`, `encoder/rdo.c:890` | `phx_mbencode.c`; `cs_dct8x8` + quant | B | rejected: no 64-coefficient slot |

The single highest-leverage gap for real content is row 17: hard-coding `skip=0` makes
the first P frame 292 B where x264 emits 6 B (`docs/BYTE_EXACT_FUNCTIONS.md:25`, `:39-44`).
A byte-exact GPU path cannot compensate for that in shaders; it is a CPU decision bug.

### 2.6 Deliberate omissions in the baseline GPU plan

Explicitly out of scope (each is a bitstream-visible state machine, not cleanup):
MB-tree propagation `encoder/slicetype.c:1091` (not ported;
`src/phx_lookahead.h:27`), subpel/multi-ref/B ME and UMH/ESA/TESA
(`src/phx_me.h:26-31`, `reference/x264/encoder/me.c:422-798`), I_PCM samples, non-flat
CQM, 8×8 trellis exactness (`src/phx_rdo.h:204-211`), MBAFF dispatch, multi-slice/sliced
threads (`src/phx_encoder.c:750`), VBV/two-pass (`docs/GPU_PIPELINE_SPEC.md:1013-1029`,
`:1140-1154`). In-loop deblock is specified but **not called** by `phx_encode()`
(`docs/GPU_PIPELINE_SPEC.md:1140-1141`); enabling it on GPU alone would break CPU parity
by construction.

---

## 3. Fidelity analysis

### 3.1 What can be ported essentially verbatim

These are the SIMD-friendly, no-hidden-state families. They are Class A or A-helpers
inside B workgroups, and their golden is a byte comparison of their output arrays.

| Family | x264 source | Why faithful | Shader tag |
|---|---|---|---|
| Lowres 2×2 filter + border padding | `common/mc.c:458-507` | Fixed integer `FILTER` recurrence, explicit neighbours; Phoenix copies it at `src/phx_frame.c:81-106` | A |
| SAD / SATD / SA8D 4×4…16×16 | `common/pixel.c:55-73,234-330` | Pure reductions over two explicit blocks; one invocation per candidate/mode (`docs/SHADER_MAPPING.md:15`) | A |
| Predictors (16×16, 8×8, 8×8c, 8×16c, 4×4, filtered edges) | `common/predict.c:67-886` | Neighbour samples via loads, `edge[36]` invocation-local; 49 ported functions (`docs/SHADER_NOTES.md:252-258`) | A |
| 4×4/8×8 DCT, Hadamard DC, IDCT, zigzag | `common/dct.c:47-326,829-958` | Butterflies on registers; in-place coefficient mutation must be copied to registers first (`docs/SHADER_NOTES.md:257-258`) | A |
| Deadzone quant/dequant, chroma-DC optimizers | `common/quant.c:50-146`, `common/set.c:73-206` | Per-coefficient; bias/deadzone tables immutable in UBO 1 | A |
| Fullpel DIA/HEX ME candidate evaluation | `encoder/me.c:182-419` | Each candidate is independent given fenc/fref and MVP; ties must be broken exactly (`docs/GPU_PIPELINE_SPEC.md:1070-1075`) | A |
| Cache load/save | `common/macroblock.c:855,1680` | Ordered only within one MB row; `scan8` tables copied verbatim | B |
| Deblock strength + edge filters | `common/deblock.c:277-687` | Eight independent edges per MB; state dies at row end | B/A |
| Trellis *candidate score* | `encoder/rdo.c:642-895` | Per-coefficient candidate cost is vectorizable; the state machine is not | A helper |

Caveat: "verbatim" means bit-exact *semantics*, including rounding
(`common/mc.c:493-499`), sentinel poisoning (`common/mc.c:473-481`), scan8 addressing
(`common/base.h:153-195`), λ vs λ² separation (`encoder/analyse.c:258-259`,
`encoder/rdo.c:162-197`), and the `0x35a4e4f5 >> (i_frame & 31)` flush bit
(`common/cabac.c:174`). A single changed rounding constant moves the first divergent bin.

### 3.2 What is hard: strictly serial state

| Serial stage | State that cannot be reordered | Source |
|---|---|---|
| CABAC arithmetic coder | `i_low`, `i_range`, `i_queue`, `i_bytes_outstanding`, `state[460..1024]`, byte cursor; each bin depends on prior renormalisation; flush depends on frame counter | `common/cabac.h:30-52`, `common/cabac.c:113-183`, `docs/GPU_PIPELINE_SPEC.md:878-890` |
| CABAC/CAVLC MB syntax | One RBSP in scan order; `mb_skip_run`, `last_qp`, `last_dqp` accumulate across MBs | `encoder/cabac.c:917-1105`, `encoder/cavlc.c:487-595`, `docs/X264_INTERNALS.md:1332-1357` |
| RDO trellis QNS | Viterbi nodes in reverse zigzag with four CABAC states per node; size-only `COPY_CABAC` must not advance the live coder | `encoder/rdo.c:71-73,642-895`, `docs/GPU_PIPELINE_SPEC.md:709-722` |
| Lookahead / slice type | `last_nonb`, per-frame `b_scenecut`, lowres MV predictors, B/BREF insertion, coded-order reorder | `encoder/lookahead.c:59`, `encoder/slicetype.c:1384-1985`, `docs/GPU_PIPELINE_SPEC.md:901-912` |
| DPB, MMCO, ref lists, POC, frame_num | `frame_num` advances only for reference pictures; POC anchored at last IDR; MMCO and B-pyramid reorder lists | `encoder/encoder.c:2533-2660,3512-3513,3881-3882`, `docs/X264_INTERNALS.md:1297-1326` |
| Rate control + rollback | `qpm`, VBV `buffer_fill`, row predictors; CAVLC overflow and VBV row failure restore a bitstream snapshot and re-encode the MB | `encoder/ratecontrol.c:72-179,1590-1746`, `encoder.c:2689-2751,2894-2983` |
| NAL assembly / emulation prevention | `00 00 ≤03` insertion depends on the two preceding output bytes | `common/bitstream.c:29-40,55-104` |

The consequences for a byte-exact GPU path are concrete:

1. **Parallel entropy is not available.** It would require per-MB prefix snapshots of 460
   contexts plus a prefix sum of byte positions, and exact tie-break preservation across
   renormalisation and outstanding `0xFF` bytes. None of that exists in the CPU port
   (`docs/GPU_PIPELINE_SPEC.md:886-890`). Slice-splitting or independent entropy segments
   change CABAC context evolution and therefore the bitstream (`docs/X264_INTERNALS.md:1386-1388`).
2. **Frame-level lookahead cannot be freely parallelised.** Reordering frame evaluation
   changes which frames are references, the B/BREF pattern, slice headers and CABAC
   context initialisation for later frames (`docs/GPU_PIPELINE_SPEC.md:909-912`).
3. **The A/B→C boundary must be one-way.** SSBO 3 is written by A/B and read only by C;
   the serial stage never writes it (`docs/SHADER_MAPPING.md:67-70`,
   `docs/GPU_PIPELINE_SPEC.md:46-51`).

### 3.3 Byte-exact GPU path vs "visually equivalent but not byte-identical"

| | Byte-exact GPU path | Visually-equivalent path |
|---|---|---|
| Decision rule | A kernel is accepted only on exact input/output equality with the CPU function for the full golden corpus (`docs/GPU_PIPELINE_SPEC.md:53-55`) | Accept if decoded PSNR/SSIM and bitrate are within a tolerance; no golden identity |
| Entropy | Single serial owner, scan order, exact context init and flush; optional single-invocation GPU port (S10) gated byte-for-byte (`docs/GPU_PIPELINE_SPEC.md:1106-1112`) | Could split slices/MBs across lanes, use fixed context prefixes, or use CAVLC-only; changes bytes |
| Lookahead/DPB | Preserve `last_nonb`, BREF, MMCO, POC/frame_num exactly (`docs/GPU_PIPELINE_SPEC.md:901-927`) | May approximate frame types, skip MB-tree, use simpler GOP structure |
| ME/RD | Exact tie-breaking and cost formulas; UMH/TESA and subpel required for high presets | SAD-only, fewer candidates, approximate λ·bits acceptable |
| Quant/trellis | Exact bias tables, deadzone, Viterbi states; chroma DC must produce `val=7`, not `14` | Approximate RDO; visual difference small, stream different |
| Speed ceiling | Limited by the serial stages (§6.5) | Potentially much faster because entropy and lookahead can be parallelised |
| Contract with x264 | Stream-identical for the covered corpus; **currently unreachable** because CPU oracle is 0/100 | Not a parity claim; must never be reported as one (`docs/NIGHTLY_STATUS.md:41-50`) |

The project goal explicitly chooses the first column: byte-exactness over speed
(`docs/GPU_PIPELINE_SPEC.md:53-55`). The second column is documented here only to make the
trade-off explicit; it would be a *different product*, and every "visually equivalent"
claim must be labelled as such.

### 3.4 Staged fidelity ladder with measurable gates

The phases are inherited from `docs/GPU_PIPELINE_SPEC.md:1033-1118`; the gates below add
the CPU-oracle precondition that 0/100 forces.

| Level | What must be true | Measurement / gate | Confidence that it is achievable |
|---|---|---|---|
| L0 — CPU oracle frozen | Byte-exact CPU streams exist for the golden corpus; 0/100 raised to >0, then 100/100 on the short corpus | `tools/parity_harness.py` byte-exact count; `tools/nightly_gates.py` report (`docs/NIGHTLY_STATUS.md:29-54`) | Low today; requires rows 1–19 fixes, especially P_SKIP |
| L1 — struct packing | Pack→unpack→`memcmp` byte-equal for all structs, including 4448-B decision stride and scan8/nnz canonicalization | `docs/GPU_PIPELINE_SPEC.md:1052-1059` P1 | High (mechanical) |
| L2 — A setup/lowres | `cs_lowres_core` planes, borders and sentinels byte-equal to `src/phx_frame.c:107-135`; cost tables equal | `docs/GPU_PIPELINE_SPEC.md:1061-1068` P2 | High |
| L3 — A search | per-block `mv/sad/cost/ref` byte-equal to `lr_motion_search`/`phx_me16`, sentinel behaviour included | P3 (`:1070-1075`) | Medium; UMH/subpel are not ported |
| L4 — B MB row | whole SSBO 2+3 dumps byte-equal to CPU for I, P and B separately | P4 (`:1077-1084`) | Medium-low until rows 17–19 land |
| L5 — deblock + boundary | reconstructed plane byte-equal to `phx_frame_deblock_ex`; staging copies byte-equal | P5/P6 (`:1085-1104`) | Medium; deblock wiring must be sequenced |
| L6 — full stream | NAL + Annex-B byte-exact on short then extended corpus; 0 NAL-count mismatches | P6 target 120/120 short, 240/240 extended (`:1095-1104`) | Low until L0 passes |
| L7 — optional GPU entropy | S10/S11 RBSP and NAL bytes identical to L6; **no multi-thread entropy** | P7 (`:1106-1112`) | Low speed value (§6.5); medium correctness |

A level is not complete until its gate passes **and** all lower gates stay green
(`docs/GPU_PIPELINE_SPEC.md:1114-1118`).

---

## 4. Concrete shader function inventory

All names below are proposed implementation labels. They group functions that already
exist in `src/`; they introduce no new syntax element and no capability beyond the spec
(`docs/GPU_PIPELINE_SPEC.md:56-59`). `SHADER` tags are the repository's own annotations.

| Proposed kernel | SHADER | Thread mapping | Inputs → outputs | Mirrors (x264 source) |
|---|---|---|---|---|
| `cs_cost_tables_init` | A | 64 lanes/slot × 4 tables | UBO 0/1 → SSBO 0 | `encoder/analyse.c:143-202`; `src/phx_cost.c:26-93` |
| `cs_lowres_core` | A | 1 thread per 2×2 source block | SSBO 1 fenc → 4 lowres planes | `common/mc.c:458-507` (`FILTER` at `:494-499`); `src/phx_frame.c:81-106` |
| `cs_lowres_border` | A | 1 thread per lowres byte/word | lowres → padded lowres (NV12 pairs as units) | `common/frame.c:627`, `:535`; `src/phx_frame.c` border helpers |
| `cs_lowres_clear` | A | 1 thread per MB + per cost cell | UBO 0 → SSBO 1/5 sentinels (`-1`, `0x7FFF`) | `common/mc.c:473-481`; `src/phx_frame.c:126-134` |
| `cs_lr_sad8` / `cs_lr_satd_8x4` / `cs_lr_satd8` | A | 1 invocation per 8×8 candidate | lowres planes → cost | `common/pixel.c:49-71,265-332`; `src/phx_lowres.h:30-31` |
| `cs_lr_motion_search` (incl. `lr_get_ref`, `lr_bidir_avg`, `lr_eval_*`, `lr_check_predictors`, `lr_try_bidir`) | A | 1 per lowres 8×8 block | SSBO 1/0/5 → SSBO 5 winner | `encoder/slicetype.c:514-791`, `encoder/me.c:63-107,218-419`; `src/phx_lowres.c:437-511` |
| `cs_lookahead_decide` | C | 1 invocation per minigop (host recommended) | SSBO 5/6 → frame types | `encoder/slicetype.c:1384-1985`, `encoder/lookahead.c:59,192-254` |
| `cs_me_fullpel` (`phx_me16`, `phx_cost_mv*`, `phx_bits_mvd`, `phx_pixel_sad_16x16`) | A | 1 thread (or 16-lane lane group) per MB/candidate | SSBO 1/0 → SSBO 6 | `encoder/me.c:182-419`; `src/phx_me.h:33-38,76-78` |
| `cs_me_subpel` *(future, not ported)* | A | 1 per partition per hpel/qpel candidate | SSBO 1/6 → refined MV/cost | `encoder/me.c:801-992`, `common/mc.c:173-283` |
| `cs_mb_cache_load` / `cs_mb_cache_save` | B | 64-lane workgroup, one per MB row | SSBO 2 ↔ workgroup shared cache | `common/macroblock.c:673-846,855-1353,1680-1880`; `src/phx_mbcache.c:75-515` |
| `cs_predict_16x16` / `_8x8c` / `_8x16c` / `_4x4` / `_8x8` | A | 1 invocation per block | neighbour samples → predicted block | `common/predict.c:67-886`; `src/phx_predict.c:40-854` |
| `cs_sad_block` / `cs_satd_block` / `cs_sa8d_block` | A | 1 invocation per candidate/mode, reduce over 16/64 lanes | two blocks → cost | `common/pixel.c:55-73,234-330`; `src/phx_analysis.c` A helpers |
| `cs_analysis_i16` / `cs_analysis_i8` / `cs_analysis_i4` | B | 1 MB workgroup; inner modes A | cache + fenc → mode arrays/costs → SSBO 3 | `encoder/analyse.c:668-979` (I4 at `:864-979`) |
| `cs_analysis_chroma` | B | 1 MB workgroup | U/V planes → mode + DC/AC arrays | `encoder/analyse.c:586-665`; `src/phx_chroma.c:207` |
| `cs_analysis_inter` (P16x16 trial; future sub-partitions/B) | B | 1 MB workgroup | SSBO 1/6 + cache → decision | `encoder/analyse.c:2953-3733`; only P L0 16×16 in `src/phx_analysis.c:1056` |
| `cs_rd_cost` (size-only `phx_cabac_size_*`) | B (A helpers) | 1 per mode; `state[460]` invocation-local | decision candidate → bit-count accumulator SSBO 7 | `encoder/rdo.c:71-73,162-373`; `src/phx_rdo.h:153-198` |
| `cs_dct4x4` / `cs_dct8x8` / `cs_hadamard_dc` / `cs_idct*` | A | 1 invocation per 4×4/8×8/DC block | residual ↔ coefficients | `common/dct.c:47-326,829-958`; `src/phx_dct.c:30-413` |
| `cs_quant_dequant` (deadzone + chroma DC) | A | 1 thread per coefficient / 4×4 block | coefficients + QP → quantized + recon coefficients | `common/quant.c:50-146`, `common/set.c:73-206`; `src/phx_quant.c:137-387` |
| `cs_mc_luma` / `cs_mc_chroma` / `cs_hpel_filter` | A/B | 1 per 16×16/8×8 block; hpel planes prebuilt | ref plane + MV → samples | `common/mc.c:173-283,704`; `src/phx_mc.h` |
| `cs_residual_encode` | B | 1 MB workgroup | decision + fenc → coefficients, nnz, recon → SSBO 2/3 | `encoder/macroblock.c:618-987`; `src/phx_mbencode.c:475` |
| `cs_deblock_strength` | B | 1 per MB row (8 edges) | SSBO 2 → SSBO 8 `bs[2][8][4]` | `common/deblock.c:277-304`, `common/macroblock.c:1438-1622` |
| `cs_deblock_edges` | B/A helpers | 1 workgroup per row, edges filtered in order | SSBO 1/8 → filtered recon | `common/deblock.c:378-687`; `src/phx_deblock.c:352-689` |
| `cs_decision_readback` | transfer | `vkCmdCopyBuffer` | SSBO 2/3 → host staging | `docs/GPU_PIPELINE_SPEC.md:227,256` |
| `cs_entropy_single_thread` *(optional P7)* | C | 1 invocation per slice | SSBO 2/3/4 → RBSP | `common/cabac.c:32-183`, `encoder/cabac.c:917-1105`, `encoder/cavlc.c:487-595`, `encoder/rdo.c:642-1096` |
| `cs_nal_escape` *(optional P7)* | C | 1 invocation per NAL | RBSP → Annex-B | `common/bitstream.c:29-104` |
| Host: `ratecontrol_start/end`, `mb_qp`, AQ | C | 1 per frame / per MB scalar | frame bits → UBO 3 / SSBO 9 | `encoder/ratecontrol.c:304-415,1432-1538,1754-1768,1829-1994` |
| Host: `slice_header_init/write` | C | 1 per slice | frame context → RBSP header | `encoder/encoder.c:122-364` |
| Host: DPB/MMCO/ref lists/POC/frame_num | C | 1 per frame | frame type → context | `encoder/encoder.c:2286-2660,3512-3513,3881-3882` |

Two entries are explicitly marked future/not ported: `cs_me_subpel` and full B/partition
analysis. Their absence is a parity gap, not a shader design shortcut
(`docs/GPU_PIPELINE_SPEC.md:1019-1021`).

---

## 5. Hardware survey (spec-only inputs to the model)

All figures in this section are **~ spec-only, not measured**, and are the author's
recall of public product specifications for parts sold 2018–2026. Intended accuracy is
±20–30%; treat them as scenario inputs only. `FP32` is dense throughput. For integer
codec work the model assumes **INT32 ≈ FP32** on NVIDIA Turing/Ampere/Ada/Blackwell and
Intel Xe (with a *assumed* INT32 penalty for Pascal, see A4), and notes the RDNA3 caveat
below. Confidence: **H** for long-shipped parts, **M** for 2024–2025 parts, **L** for
announced/uncertain figures.

### 5.1 Discrete GPUs (~34)

| GPU | ~Year | Arch | ~FP32/INT32 TFLOPS | ~BW GB/s | ~Board W | Conf. |
|---|---:|---|---:|---:|---:|:--:|
| GTX 1050 Ti | 2016/18 | Pascal GP107 | 2.1 | 112 | 75 | H |
| GTX 1060 6GB | 2016/18 | Pascal GP106 | 4.4 | 192 | 120 | H |
| GTX 1070 | 2016/18 | Pascal GP104 | 6.5 | 256 | 150 | H |
| GTX 1070 Ti | 2017/18 | Pascal GP104 | 8.2 | 256 | 180 | H |
| GTX 1080 | 2016/18 | Pascal GP104 | 9.3 | 320 | 180 | H |
| GTX 1080 Ti | 2017/18 | Pascal GP102 | 11.9 | 484 | 250 | H |
| GTX 1650 | 2019 | Turing TU117 | 2.9 | 128 | 75 | H |
| GTX 1650 Super | 2019 | Turing TU116 | 4.4 | 192 | 100 | H |
| GTX 1660 | 2019 | Turing TU116 | 5.0 | 192 | 120 | H |
| GTX 1660 Super | 2019 | Turing TU116 | 5.0 | 336 | 125 | H |
| GTX 1660 Ti | 2019 | Turing TU116 | 5.5 | 288 | 120 | H |
| RTX 2060 | 2019 | Turing TU106 | 6.5 | 336 | 160 | H |
| RTX 2060 Super | 2019 | Turing TU106 | 7.2 | 448 | 175 | H |
| RTX 2070 | 2018 | Turing TU106 | 7.5 | 448 | 175 | H |
| RTX 2070 Super | 2019 | Turing TU104 | 9.0 | 448 | 215 | H |
| RTX 2080 | 2018 | Turing TU104 | 10.6 | 448 | 215 | H |
| RTX 2080 Super | 2019 | Turing TU104 | 11.3 | 496 | 250 | H |
| RTX 2080 Ti | 2018 | Turing TU102 | 13.4 | 616 | 250 | H |
| RX 560 | 2017/18 | GCN5 | 2.6 | 112 | 60–80 | H |
| RX 580 | 2017/18 | GCN5 Polaris | 6.3 | 256 | 185 | H |
| RX 590 | 2018 | GCN5 Polaris | 7.1 | 256 | 225 | H |
| Vega 56 | 2017/18 | GCN5 HBM2 | 10.6 | 410 | 210 | H |
| Vega 64 | 2017/18 | GCN5 HBM2 | 12.7 | 484 | 295 | H |
| Radeon VII | 2019 | GCN5 HBM2 | 13.4 | ~1000 | 300 | M |
| RX 5500 XT | 2019 | RDNA1 Navi 14 | 5.0 | 224 | 130 | H |
| RX 5600 XT | 2020 | RDNA1 Navi 10 | 7.2 | 288 | 150 | H |
| RX 5700 | 2019 | RDNA1 Navi 10 | 8.0 | 448 | 180 | H |
| RX 5700 XT | 2019 | RDNA1 Navi 10 | 10.1 | 448 | 225 | H |
| RX 6500 XT | 2022 | RDNA2 Navi 24 | 5.7 | 144 | 107 | H |
| RX 6600 | 2021 | RDNA2 Navi 23 | 8.9 | 224 | 132 | H |
| RX 6600 XT | 2021 | RDNA2 Navi 23 | 10.6 | 256 | 160 | H |
| RX 6700 XT | 2021 | RDNA2 Navi 22 | 13.2 | 384 | 230 | H |
| RX 6800 / 6800 XT | 2020 | RDNA2 Navi 21 | 16.2 / 20.7 | 512 | 250 / 300 | H |
| RX 6900 XT | 2020 | RDNA2 Navi 21 | 23.1 | 512 | 300 | H |
| RX 7600 / 7600 XT | 2023/24 | RDNA3 Navi 33 | ~21.8 / ~22.5 adv.¹ | 288 / 320 | 165 / 190 | M |
| RX 7700 XT / 7800 XT | 2023 | RDNA3 Navi 32 | ~35 / ~37 adv.¹ | 432 / 624 | 245 / 263 | M |
| RX 7900 XT / XTX | 2022 | RDNA3 Navi 31 | ~52 / ~61 adv.¹ | 800 / 960 | 315 / 355 | M |
| RX 9070 / 9070 XT | 2025 | RDNA4 | ~24–30 effective¹ | ~640 | 220 / 304 | L |
| RTX 3050 / 3060 | 2022/21 | Ampere GA106 | 9.1 / 12.7 | 224 / 360 | 130 / 170 | H |
| RTX 3060 Ti / 3070 | 2021/20 | Ampere GA104 | 16.2 / 20.3 | 448 | 200 / 220 | H |
| RTX 3070 Ti / 3080 | 2021/20 | Ampere GA104/GA102 | 21.7 / 29.8 | 608 / 760 | 290 / 320 | H |
| RTX 3080 Ti / 3090 | 2021/20 | Ampere GA102 | 34 / 35.6 | 912 / 936 | 350 | H |
| RTX 4060 / 4060 Ti | 2023 | Ada AD107/AD106 | 15.1 / 22.1 | 272 / 288 | 115 / 165 | H |
| RTX 4070 / 4070 Super | 2023/24 | Ada AD104 | 29 / ~35 | 504 | 200 / 220 | H |
| RTX 4070 Ti / Ti Super | 2023/24 | Ada AD104 | ~40 / ~44 | 504 / 672 | 285 | H |
| RTX 4080 / 4090 | 2022 | Ada AD103/AD102 | 48.7 / 82.6 | 717 / 1008 | 320 / 450 | H |
| RTX 5060 Ti / 5070 | 2025 | Blackwell GB106/GB? | ~22 / ~31 | ~448 / ~672 | 180 / 250 | L |
| RTX 5070 Ti / 5080 | 2025 | Blackwell GB? | ~44 / ~56 | ~896 / ~960 | 300 / 360 | L |
| RTX 5090 | 2025 | Blackwell GB202 | ~104 | ~1792 | 575 | L |

¹ RDNA3 advertises up to 2 FP32 ops/clk/SP; INT32 remains ~1/clk. For codec kernels
(model uses integer ops) use roughly **half** the advertised FP32 figure. Verify per SKU.

### 5.2 APUs / iGPUs (~22)

| APU / iGPU | ~Year | GPU | ~FP32 TFLOPS | ~BW GB/s | ~Package W | Conf. |
|---|---:|---|---:|---:|---:|:--:|
| Ryzen 5 2400G | 2018 | Vega 11 (704 SP) | 1.2 | ~38 (DDR4-2400 dual) | 65 | H |
| Ryzen 5 3400G | 2019 | Vega 11 | 1.4 | ~51 (DDR4-3200) | 65 | H |
| Ryzen 7 3700U / 3500U | 2019 | Vega 10 / 8 | 1.15 / 0.9 | ~38 | 15 | H |
| Ryzen 7 4700U / 4800U | 2020 | RDNA2 7–8 CU | 1.3 / 1.8 | ~51 | 15–25 | H |
| Ryzen 7 5700G / 5800H | 2021 | Vega 8 (512 SP) | 1.8 / 1.7 | ~51 | 65 / 45 | H |
| Ryzen 7 6600H / 6800H | 2022 | RDNA2 680M 12 CU | 3.4 | ~102 (LPDDR5-6400) | 45 | H |
| Ryzen 7 6800U | 2022 | RDNA2 680M 12 CU | 3.4 | ~102 | 15–28 | H |
| Ryzen 7 7840HS / 7640HS | 2023 | RDNA3 780M 12 CU | ~4 effective¹ | ~120 (LPDDR5X-7500) | 35–54 | M |
| Ryzen 7 8700G / 8840U | 2024 | RDNA3 12 CU | ~4 effective¹ | ~83 / ~120 | 65 / 28 | M |
| Ryzen AI 9 HX 370 | 2024 | RDNA3.5 890M 16 CU | ~6–8 effective¹ | ~120 | 28–54 | L |
| Intel UHD 620 | 2017/19 | Gen9.5 192 EU | 0.4 | ~38 (DDR4-2400 dual) | 15 | H |
| Intel UHD 630 | 2018/20 | Gen9.5 192–230 EU | 0.45 | ~38–42 | 15–65 | H |
| Intel Iris Plus 655 | 2019 | Ice Lake 64 EU | 1.1 | ~60 (LPDDR4X-3733) | 15 | H |
| Intel Iris Xe 80 EU | 2020/21 | Tiger Lake Xe | 1.6 | ~68 (LPDDR4X-4266) | 15–28 | H |
| Intel Iris Xe 96 EU | 2020/21 | Tiger Lake Xe | 2.0 | ~68 | 28 | H |
| Intel UHD 770 | 2021+ | Alder Lake 32 EU | 0.8 | ~51–77 (DDR4/DDR5) | 65 | H |
| Intel Arc A380 | 2022 | Alchemist | 6.2 | 140 (GDDR6) | 75 | H |
| Intel Arc A580 / A750 / A770 | 2023/22 | Alchemist | ~10 / ~17 / ~20 | 320 / 448 / 560 | 185 / 225 / 225 | M |
| Intel Arc B580 | 2024 | Xe2 Battlemage | ~13 | ~456 | 190 | L |
| Apple M1 | 2020 | 8 GPU cores | 2.6 | 68 | ~10–15 (GPU) | M |
| Apple M1 Pro / Max | 2021 | 14–32 cores | ~5–11 | ~200–400 | ~30–70 | M |
| Apple M2 / M3 / M4 | 2022–24 | 8–10 cores | ~3.6 / ~4.4 / ~6 | ~100–150 | ~10–25 | L |

**Apple caveat**: no Vulkan driver exists for Apple silicon; these are Metal-only. They
are listed as *reference points* for cross-architecture comparison and are **not**
Vk264 deployment targets. Arc parts are discrete but included here because they share
the Xe architecture with Iris Xe. iGPU bandwidth is shared with the CPU and is often the
binding constraint; a 51 GB/s Ryzen Vega APU cannot feed high-preset ME even if it had
enough ALUs.

---

## 6. Analytical performance model

### 6.1 Workload definition (explicit assumptions)

Fix 1080p: `W=1920`, `H=1080`, padded `H'=1088`, `mb_width=120`, `mb_height=68`,
`n=8160` MBs (`docs/GPU_PIPELINE_SPEC.md:208-210`). Assume 4:2:0 8-bit, one slice per
frame, CABAC unless noted, CQP or CRF at nominal quality. Preset parameters are taken
from the vendored source, not guessed: defaults at `reference/x264/common/base.c:384-455`
(ref 3, bframes 3, HEX, me_range 16, subme 7, trellis 1, AQ variance, lookahead 40,
mb_tree 1, 8×8dct 1), preset overrides at `base.c:496-602` (ultrafast DIA/subme0/no
CABAC/no deblock/CAVLC; slow subme8/ref5/trellis2; slower UMH/subme9/ref8/B-adapt
TRELLIS; veryslow UMH/subme10/ref16/bframes8; placebo TESA/subme11/bframes16).

**Operation model** (`model`, confidence **M** for shape, **L** for absolute scale;
an integer op ≈ one integer add/sub/abs/compare, not a CPU instruction):

* SAD 16×16 ≈ 256 abs + 255 adds ≈ **0.5 kops**; SATD 16×16 ≈ 16 row + 16 column
  Hadamards (~768 adds) + 256 abs/acc ≈ **1 kop**; SATD 8×8 ≈ 0.25 kop; SATD 4×4 ≈
  0.08 kop; SA8D 8×8 ≈ 4×SATD4×4 + combine ≈ 0.34 kop.
* 4×4 DCT or IDCT ≈ 16 rows + 16 cols × 24 adds ≈ **0.77 kop**; 16 blocks/MB →
  ~12 kops per DCT+IDCT pass.
* Trellis Viterbi: ~16 coefficients × 2 candidate levels × ~30 ops ≈ ~1 kop per 4×4
  block; 16 blocks → ~16 kops/MB at trellis 1–2.
* Intra analysis: I16 3–7 modes × 1 kop; I4 16 blocks × ~3–9 modes × 0.08 kop;
  I8 4 × ~9 × 0.34 kop; chroma ~7 × 0.25 kop × 2 planes. With mbrd≥2, RD re-evaluates
  each retained mode with DCT+quant+IDCT+SSD+size bins ≈ ~2 kops/mode, so I4 RD alone
  can reach ~0.2–0.3 **Mops/MB**.
* ME candidates (per MB, `model`): DIA ≈ 4×me_range (64 at R=16); HEX ≈ 6×R+8
  (~104); UMH ≈ 200–400; TESA ≈ (2R+1)² (up to 2401 at R=24). Multiply by refs and
  by partition count (P16x16, 2×16x8, 2×8x16, 4×8x8, optional 8 sub-8×8), with smaller
  SAD sizes for smaller partitions. Subme≥3 adds ~16–32 hpel/qpel candidates per
  partition.
* CABAC bins: assume 150–500 bins/MB → **1.2–4 M bins/frame**.

Resulting **per-MB work estimates** (`model`):

| Preset | Dominant term | ~kops/MB | ~Gops/frame | Confidence |
|---|---|---:|---:|:--:|
| ultrafast / CAVLC | 1 ref DIA16 + I4 shortcuts + deadzone | 50–100 | 0.4–0.8 | M |
| veryfast / fast | DIA/HEX, 1–2 refs, no trellis | 150–300 | 1.2–2.5 | M |
| medium / CABAC | HEX16 ×3 refs + subme7 + partitions + trellis1 | 400–700 | 3.3–5.7 | M-L |
| slow / slower | subme8–9, 5–8 refs, trellis2, mbrd2, UMH for slower | 1500–3000 | 12–25 | L |
| veryslow / placebo | UMH/TESA, 16 refs, mbrd2, subpel RD | 3000–8000 | 24–65 | L |

Memory traffic (`model`, confidence M): input frame ≈3.1 MB, lowres planes ≈2.1 MB,
decision array ≈4448 B × 8160 ≈36 MB/frame, MB state ≈~4 MB/frame. ME window reuse is
high (neighbouring candidates overlap), so with a 2–8 MB L2 most candidate reads are
L2 hits; assume **50–200 MB/frame DRAM** for medium, **0.5–2 GB/frame** for veryslow.
Arithmetic intensity is therefore ~25–45 ops/byte, comparable to the compute/bandwidth
balance of a modern dGPU (~30 TFLOPS / ~1 TB/s) and of a high-end iGPU (~4 TFLOPS /
~100 GB/s): **the model predicts compute/integer bound, not bandwidth bound**, on both.
This is an assumption, not a measurement.

### 6.2 Throughput model

For each frame:

```
T_parallel = max( W_ops / (P_int * eta), W_bytes / BW_eff, T_dispatch )
T_serial   = T_lookahead + T_ratecontrol + T_entropy + T_readback + T_host_wait
T_frame    = T_parallel + T_serial                 (baseline, additive)
           ≈ max(T_parallel, T_serial)             (only with cross-frame pipelining)
fps        = 1 / T_frame
```

Where `P_int` is INT32 TFLOPS from §5 (converted to ops/s), `eta` is kernel efficiency
(branchy, divergent, small-workgroup code), `BW_eff` is ~50–70% of peak for scattered
window reads, and `T_dispatch ≈ 20–40 dispatches × (2–10 µs CPU + 1–5 µs GPU barrier)`
≈ **0.06–0.6 ms/frame** (`model`, L; derived from S0–S9 count in
`docs/GPU_PIPELINE_SPEC.md:216-230,268-302`).

`T_entropy` (`model`, confidence **L**): 1.2–4 M bins at an assumed 50–300 M bins/s for
one serial coder → **4–80 ms/frame**. This is the single most uncertain number in the
model and must be measured before any speed claim (gate E4 in §8).

### 6.3 Occupancy: the baseline row layout is not a throughput layout

S7 dispatches `(1, mb_height, 1) = (1,68,1)` workgroups of 64 threads = 2 warps each.
On a mid-range GPU with 40–80 SMs that is **68 CTAs, ≤2 warps per SM**; on an 8-CU iGPU
(64 shaders × ~8 warps) it is even further from hiding latency. All 120 MBs of a row are
processed sequentially inside one workgroup with `barrier()` per MB
(`docs/GPU_PIPELINE_SPEC.md:249-253`, `:336-354`). This layout was chosen to avoid a
global barrier and make the neighbour proof simple; it is a **correctness-first** layout.
Model consequences (`model`, M):

* `eta` for S7 is likely **2–10%**, not 20–50%, because only 2 warps/SM are resident and
  every MB ends in a workgroup barrier.
* The documented alternative — 16-thread workgroups on anti-diagonals with a barrier
  per diagonal (`docs/GPU_PIPELINE_SPEC.md:258-266`) — raises CTA count to ~8160 but
  costs `mb_width+mb_height-1 = 187` dispatches per stage; dispatch overhead then grows
  to ~1–8 ms/frame and may erase the occupancy gain for short kernels.
* A throughput-oriented variant (not yet specified) is a persistent workgroup handling
  several rows with a lane-per-4×4-block mapping (256 threads/MB), double-buffered
  neighbour state, and one barrier per MB row. This needs its own correctness proof; it
  is listed as an experiment (E3), not adopted here.

### 6.4 Scenario fps (`model`, not measured)

Assumptions for the table: `eta = 0.20` for A kernels and `0.05` for S7 with the baseline
row layout; entropy on one host core at 150 M bins/s with 2.5 M bins/frame →
`T_serial ≈ 17 ms` nominal (range 5–50 ms); lookahead+RC+readback ≈ 3–10 ms. Numbers are
rounded ranges across the hardware classes of §5, not predictions for a specific SKU.

| Preset | Hardware class | `T_parallel` ms | `T_serial` ms | Baseline fps (additive) | Pipelined fps (max) | Conf. |
|---|---|---:|---:|---:|---:|:--:|
| ultrafast/CAVLC | Iris Xe / Vega 8 APU | 2–8 | 5–25 | 30–100 | 60–200 | L |
| ultrafast/CAVLC | GTX 1650 / RX 6600 | 0.5–3 | 5–25 | 35–180 | 80–250 | L |
| medium/CABAC | Iris Xe / Vega 8 APU | 8–25 | 15–40 | 18–42 | 30–70 | L |
| medium/CABAC | mid dGPU (RTX 3060 / RX 6700 XT) | 1.5–6 | 15–40 | 18–45 | 40–90 | L |
| medium/CABAC | high dGPU (RTX 4090 / RX 7900 XTX) | 0.4–2 | 15–40 | 20–50 | 45–100 | L |
| slow/CABAC | mid dGPU | 8–25 | 20–60 | 10–25 | 20–50 | L |
| veryslow/CABAC | mid dGPU | 25–90 | 25–80 | 5–14 | 10–30 | L |
| veryslow/CABAC | high dGPU | 6–25 | 25–80 | 8–22 | 15–40 | L |

Read these as *scenario envelopes*, not forecasts. The dominant term in nearly every
row is `T_serial`; adding GPU power beyond a mid-range part changes little in the
byte-exact baseline. Where the GPU does help is the A-stage candidate search: low
presets become serial-bound, and the parallel portion shrinks toward zero.

### 6.5 Bottleneck analysis

**Entropy (CABAC) is the hard ceiling.** It is one evolving arithmetic coder with 460
active contexts, scan-order bins and a frame-dependent flush bit
(`common/cabac.c:113-183`, `docs/GPU_PIPELINE_SPEC.md:878-890`). A single GPU invocation
(S10) would run at a fraction of a CPU core's effective rate for branchy table-driven
bin coding (`model`, L). It is useful for *integration* (removing a host round-trip), not
for a speedup. Real speed requires multiple independent entropy streams (slices, tiles,
or WPP-like splitting), each of which changes context evolution and hence the bitstream —
explicitly forbidden by the byte-exact contract
(`docs/X264_INTERNALS.md:1386-1388`, `docs/GPU_PIPELINE_SPEC.md:1110-1112`).

**DPB/lookahead is the second ceiling.** `last_nonb`, BREF insertion and coded-order
reordering determine slice headers and CABAC context init for all later frames
(`docs/GPU_PIPELINE_SPEC.md:901-927`). The baseline cannot start frame N+1's entropy
until frame N's RC feedback and frame-type decisions are settled. Cross-frame pipelining
is therefore limited to a window, and even then the RC feedback edge
(`src/phx_encoder.c:1542-1564`) forces serialization of QP-affecting sizes.

**Where a GPU can realistically beat a CPU x264:**

* Lowres frame/path cost evaluation across lookahead frames — x264 itself already
  offloads this to OpenCL and lookahead threads (`encoder/slicetype.c:878-944`,
  `docs/X264_INTERNALS.md:1361-1364`), so this is a proven parallel region.
* Fullpel ME candidate evaluation across MBs and references: hundreds of independent
  SAD/SATD candidates per MB, ideal for many ALUs, provided fenc/fref windows are cached.
* Intra mode cost sweeps (I4/I8/I16/chroma) and 8×8 trellis candidate scoring.
* DCT/Hadamard/quant/dequant and deblock edge filters over many MBs concurrently.
* Batch workloads with many frames in flight, where A/B latency can be amortised and the
  serial entropy lane can be kept busy.

**Where it cannot:**

* Single-slice CABAC/CAVLC emission at byte-exact parity (one lane by construction).
* Frame_num/POC/MMCO/DPB and slice-header decisions (strict frame order).
* Rate-control feedback and CAVLC-overflow/VBV-row rollback
  (`encoder.c:2689-2751,2894-2983`): a GPU design that cannot roll back must either fix
  QP early (losing parity) or buffer MB-local streams and concatenate (changing CABAC
  context evolution) — `docs/X264_INTERNALS.md:1386-1388`.
* Byte-exact full-stream parity today, because the CPU oracle is 0/100 regardless of
  where the kernels run.

---

## 7. Viability verdict

| Target | Verdict | Why |
|---|---|---|
| Interactive / live 1080p at 30 fps, **byte-exact**, low preset (ultrafast/veryfast), CAVLC or CABAC | **Plausible on mid-range dGPU** (`model`, L) once L0–L6 pass; parallel work is small and entropy cap (~17 ms) fits the 33 ms budget | §6.4; baseline additive path |
| Real-time 1080p 30 fps, byte-exact, medium preset | **Marginal** (`model`, L): nominal ~18–45 fps, entirely entropy/lookahead-limited; an iGPU is unlikely to sustain it | §6.4 |
| Real-time 1080p high preset (slow/veryslow), byte-exact | **Not realistic** on current hardware with one serial entropy lane; parallel work is large but `T_serial` still dominates | §6.2, §6.5 |
| High frame rate (>100 fps) 1080p, byte-exact CABAC | **Not achievable** with one serial entropy owner; needs bitstream-changing parallelism | `docs/GPU_PIPELINE_SPEC.md:886-890,1110-1112` |
| Batch encoding with x264-equivalent output for a *covered corpus* | **Viable after the CPU oracle reaches 100/100**; GPU then accelerates the A/B stages; throughput gain is bounded by entropy | L0 precondition, §1 |
| Full x264 feature parity (all presets, MBAFF, MB-tree, weighted prediction, VBV, 2-pass, multi-slice) | **Not viable on this roadmap**; each is a bitstream-visible state machine and most are not ported at all | §2.6, `docs/X264_INTERNALS.md:1461-1476` |
| "Visually equivalent" encoder | Achievable sooner, but it is a **different product** and cannot be called x264 parity | §3.3 |

**Bottom line.** As a *correctness-first* engineering target, a shader-based x264 is
coherent: the A/B stages have a clean, well-documented boundary to a serial C stage, and
the repo already specifies buffers, dispatches, classes and gates down to byte offsets.
As a *performance* target, the design wins where x264 already parallelises (lowres
lookahead) and in candidate-heavy ME/intra/DCT work, but the byte-exact contract keeps
CABAC, DPB and rate control on one lane, so end-to-end speed is capped by those serial
stages. The largest immediate risk is not GPU performance: it is that the CPU oracle is
still 0/100 byte-exact, which makes any shader golden meaningless until fixed.

---

## 8. Experiments and gates needed to prove it

All experiments are additive to `docs/GPU_PIPELINE_SPEC.md:1033-1118` P0–P7.

| ID | Experiment | Method | Pass criterion |
|---|---|---|---|
| E0 | Raise CPU oracle above 0/100 | `tools/parity_harness.py`, `tools/first_frame_probe.py`, `tools/cabac_bin_diff.py`, `tools/nightly_gates.py` (`docs/NIGHTLY_STATUS.md:29-54`); fix rows 1–19 in owner order, P_SKIP first | >0 then 100/100 byte-exact short corpus; golden dumps deterministic across two runs and a seek |
| E1 | Packing gate | pack→unpack→`memcmp` on every struct, incl. 4448-B decision stride and scan8/nnz canonicalization | byte-equal on every golden MB; resolve `phx_mbencode.h:34` vs `phx_encoder.c:842` in favour of serial reader |
| E2 | Kernel microbenchmarks | GPU timer queries on SAD/SATD/DCT/quant/deblock; compare to CPU SIMD reference; report ops/s and achieved occupancy | reproducible numbers on ≥3 hardware classes; **no speedup claim until measured** |
| E3 | S7 layout sweep | row layout vs anti-diagonal vs lane-per-4×4 persistent variant; dump SSBO 2+3 for each | all variants byte-equal to CPU dump; report dispatch overhead and occupancy |
| E4 | Serial entropy throughput | measure bins/s of `phx_cabac_*` on one host core and of S10 on GPU; include flush tie-break case | measured `T_entropy` replaces the model's 4–80 ms assumption |
| E5 | End-to-end parity + fps | S0–S9 + host entropy on the golden corpus; timestamp every dispatch and the host wait | NAL/Annex-B byte-exact; full per-frame timing breakdown |
| E6 | Hardware matrix | rerun E2/E5 on the §5 devices after verifying vendor datasheets | published table with measured values, replacing all `~ spec-only` and `model` tags |
| E7 | Negative controls | deliberately reorder one lookahead frame, one CABAC context init and one deblock edge; confirm the gate fails | proves the gates actually detect bitstream changes |

**Using the CPU oracle's goldens as shader test vectors.** Each frozen dump becomes a
kernel input/output pair: cost tables (SSBO 0), lowres planes and sentinels (SSBO 1), ME
results (SSBO 6), `phx_mb_decision_t[]` and MB state (SSBO 2/3), deblocked reconstruction
(SSBO 1 recon), CABAC bin trace and final Annex-B bytes. The corpus must cover all ten
presets × both entropies × 5 fixtures (`docs/PARITY_GAP_MATRIX.md:9`), plus the real-video
64×64/30-frame CABAC case that exposes the P_SKIP blowup
(`docs/BYTE_EXACT_FUNCTIONS.md:29-44`). A shader that matches only MB0's 715 bins has not
passed anything (`docs/PORTING_ROADMAP.md:109,125-129`).

---

## 9. Assumptions and confidence register

| # | Assumption | Confidence | How to invalidate |
|---:|---|---|---|
| A1 | x264 is the oracle; byte-exactness outranks speed | H | Documented contract (`docs/GPU_PIPELINE_SPEC.md:44-59`) |
| A2 | CPU goldens can be made 100/100 before shader work | L | E0 |
| A3 | SSBO 3 layout (4448 B) and scan8/nnz interpretation are correct | M | E1 / P4; known conflict at `docs/GPU_PIPELINE_SPEC.md:816-821` |
| A4 | INT32 ≈ FP32 on Turing/Ampere/Ada/Blackwell, Xe, GCN/RDNA; Pascal assumed ~½ FP32; RDNA3 uses conservative FP32/2 | M | Verify per SKU in E6 |
| A5 | Op counts in §6.1 (0.5/1/0.25/0.08 kop per SAD/SATD size, etc.) | M shape, L absolute | Instrument x264 and count actual integer ops |
| A6 | ME candidate counts (DIA 4R, HEX 6R+8, UMH 200–400, TESA (2R+1)²) | L | Dump candidate traces for all presets |
| A7 | 1.2–4 M CABAC bins/frame at 50–300 M bins/s | L | E4 |
| A8 | `eta` 0.20 for A kernels, 0.05 for baseline S7 | L | E2/E3 |
| A9 | ~20–40 dispatches/frame at 2–10 µs each | M | timestamp command buffers in E5 |
| A10 | 50–200 MB/frame (medium) DRAM traffic due to L2 reuse | M | GPU memory counters in E2 |
| A11 | All hardware figures in §5 (±20–30%) | H/M/L per row | datasheet verification in E6 |
| A12 | Apple silicon excluded from Vulkan targets (Metal only) | H | external driver fact; no repo claim |

---

## 10. Citation index (primary)

**Repo contract and gates**: `docs/BYTE_EXACT_FUNCTIONS.md:3-59`; `docs/FUNCTION_INVENTORY.md:1-10`;
`docs/FUNCTION_PORTS.md:1-16`; `docs/SHADER_MAPPING.md:7-80`; `docs/SHADER_NOTES.md:11-24,384-500`;
`docs/GPU_PIPELINE_SPEC.md:1-59,67-145,147-186,189-302,313-370,375-755,757-874,876-955,959-1029,1033-1118,1122-1165`;
`docs/X264_INTERNALS.md:34-77,155-277,280-485,487-664,666-813,815-956,958-1126,1128-1234,1236-1328,1330-1476`;
`docs/PARITY_GAP_MATRIX.md:5-45`; `docs/PORTING_ROADMAP.md:94-143`; `docs/NIGHTLY_STATUS.md:29-54`;
`docs/DATA_STRUCTURES.md:3-6,77-90`.

**Phoenix sources**: `src/phx_param.h:24-88`; `src/phx_frame.h:16-82`; `src/phx_frame.c:76-135`;
`src/phx_mbcache.h:17-105`; `src/phx_mbcache.c:9-31,75-96,434-515`;
`src/phx_mb_decision.h:1-42`; `src/phx_analysis.h:1-99`; `src/phx_me.h:26-78`;
`src/phx_lowres.h:23-85`; `src/phx_rdo.h:23-31,71-90,136-217`; `src/phx_ratecontrol.h:22-137`;
`src/phx_deblock.h:20-163`; `src/phx_encoder.c:695-845,858-1260,1274-1573`.

**Vendored x264 (verified anchors)**: `common/base.c:384-455,489-602`;
`common/pixel.c:73,55-73,234-330`; `common/mc.c:458-507,173-283,704`;
`common/dct.c:47-326,829-958`; `common/quant.c:50-146`; `common/set.c:73-206`;
`common/predict.c:67-886`; `common/deblock.c:277-687`;
`common/macroblock.c:673-846,855,1438-1622,1680-1880`;
`common/cabac.c:32-183` (flush bit at `:174`); `common/bitstream.c:29-104`;
`common/tables.c:1668-1792`; `encoder/analyse.c:143-318,586-979,982-1135,2918-3734`;
`encoder/me.c:182-419,801-992`; `encoder/rdo.c:71-73,162-373,642-895,921-1096,1114-1167`;
`encoder/cabac.c:917-1105`; `encoder/cavlc.c:70-704`;
`encoder/slicetype.c:284,514-995,1091-1184,1288-1382,1384-1985,878-944`;
`encoder/ratecontrol.c:304-415,1432-1538,1590-1768,1829-1994`;
`encoder/encoder.c:122-364,1507-1859,2051-2134,2286-3130,3323-3902`;
`encoder/lookahead.c:59,192-254`; `encoder/macroblock.c:126-987`;
`common/mvpred.c:129-181,183-516`; `common/common.h:270-763`.

---

*End of document. All performance numbers are model outputs; all hardware specifications
are unverified public-spec estimates; the sole measured parity fact used throughout is
**byte-exact 0/100**.*
