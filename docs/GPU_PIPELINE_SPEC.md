# Vk264 GPU shader backend — concrete pipeline specification

Status: **design specification, no shader code exists yet**. Evidence base: the CPU port in
`src/*.h` / `src/*.c`, the vendored x264 in `reference/x264/`, and the project documents
`docs/SHADER_MAPPING.md`, `docs/SHADER_NOTES.md`, `docs/DATA_STRUCTURES.md`,
`docs/FUNCTION_PORTS.md`, `docs/PORTING_ROADMAP.md`.

Working tree: branch `vk264-phoenix`. Line numbers refer to this working tree unless the
path is `reference/x264/...`, in which case they refer to the vendored source. Some line
numbers quoted in earlier docs have drifted (e.g. `docs/SHADER_NOTES.md:341` cites
`src/phx_cabac.c:146` for the `frame & 31` term; the actual line is
`src/phx_cabac.c:218`, corresponding to `reference/x264/common/cabac.c:174`). Where they
disagree, the source file is authoritative and the drift is recorded in §14.

This document is deliberately concrete: entry-point names, workgroup sizes, dispatch
dimensions, buffer offsets and per-field provenance are given so an engineer can implement
the backend without guessing. Anything not present in the CPU port is labelled
**UNKNOWN**, **NOT PORTED** or **deferred** — in particular there is no Vulkan code, no
SPIR-V, no GLSL and no Vulkan target in `CMakeLists.txt` at the time of writing.

---

## 1. Contract and vocabulary

### 1.1 The three classes

The classification is inherited from `docs/SHADER_MAPPING.md:7-56` and made actionable in
`docs/SHADER_NOTES.md:11-19`. It is a property of a **function**, not of the buffer it
writes. `docs/FUNCTION_PORTS.md:1-16` records the current scan: 505 definitions across
27 `src/*.c` modules — 185 Class A, 109 Class B, 211 Class C. Re-scan before using the
counts as a gate (`docs/SHADER_NOTES.md:19`).

| Class | Rule | Backend interpretation |
|---|---|---|
| **A** | "one invocation per work item, no cross-item state … inputs passed explicitly, outputs written explicitly, no hidden state" (`docs/SHADER_MAPPING.md:9-25`) | Pure compute shader. All inputs are UBO/SSBO reads, all outputs are indexed SSBO writes. No inter-invocation barrier is needed. Atomics are unnecessary. |
| **B** | "ordered but local … neighbour information within a frame/MB but not across frames … workgroup with shared memory or a two-pass reduce" (`docs/SHADER_MAPPING.md:27-38`) | One workgroup per MB *row* (§4.3), with the `phx_mb_cache_t` mirrored in workgroup shared memory. Ordering comes from the row loop and inter-dispatch barriers. State dies at frame end. |
| **C** | "cannot be naively parallelized because they mutate a single evolving state … dedicated serial stage" (`docs/SHADER_MAPPING.md:40-56`) | One logical owner: the host serial encoder loop in the primary plan (`src/phx_encoder.c:1274-1553`). A single-invocation GPU shader is a *later, optional* port (§12, phase P7), not part of the initial design. |

Two host lifecycle facts matter: allocation/free and preset application are Class C only
because they have no work item, while the data they produce is read by Class A shaders
(`docs/SHADER_NOTES.md:21-24`). For example `phx_frame_new` is C (`src/phx_frame.h:58`) but
`phx_frame_init_lowres` is A (`src/phx_frame.h:82`).

### 1.2 Non-negotiable invariants

1. **Every piece of state is explicit and owned by a named structure.** The old oracle's
   `vk_cur_*` globals are forbidden (`docs/DATA_STRUCTURES.md:3-6, 83-90`).
2. **One-way boundary.** The `phx_mb_decision_t[]` array (SSBO 3) is written by A/B stages
   and read only by the serial stage. The serial stage never writes it
   (`src/phx_mb_decision.h:1-8`, `docs/SHADER_MAPPING.md:67-70`).
3. **No hidden frame history in A/B kernels.** If a kernel needs a previous frame (ME
   reference, deblocked reconstruction), that frame is an explicit SSBO binding.
4. **Byte-exactness over speed.** A shader stage is accepted only when its outputs are
   byte-identical to the CPU function it replaces for the full golden corpus
   (`docs/PORTING_ROADMAP.md:102-140`, especially :138-140).
5. **No new syntax elements, no new tools.** This spec introduces no function absent from
   `docs/FUNCTION_PORTS.md` and no capabilitiy beyond core Vulkan compute plus the buffer
   types explicitly discussed below. Optional 8/16-bit storage features are called out as
   optimizations, never assumed.

---

## 2. Stage DAG (raw YUV → Annex-B)

### 2.1 Graph

```
                           ┌──────────────────────────────────────────────────────┐
                           │ HOST / C: param_default, apply_preset, finalize,   │
                           │ apply_profile, sps_init, pps_init, sei_version     │
                           │ → UBO 0, UBO 1, UBO 2                              │
                           └──────────────────────────────────────────────────────┘
                                          │ immutable for whole encode
   raw YUV (pinned host picture) ─────────┤
                                          ▼
   ┌─────────────────────────── A: frame setup ───────────────────────────────┐
   │ copy picture→SSBO 1 (host/transfer); duplicate last row/col;             │
   │ cs_lowres_core (2x2 filter, 4 planes); cs_lowres_border;                 │
   │ cs_lowres_clear (i_cost_est, i_row_satds[..][0], lowres_mvs);             │
   │ cs_cost_tables_init (SSBO 0) [once per encode]; cs_rdo_tables_init       │
   └──────────────────────────────────────────────────────────────────────────┘
                                          │ lowres planes (SSBO 1), cost tables (SSBO 0)
                                          ▼
   ┌──────────────── A: lowres cost kernels ──────────────────────────────────┐
   │ lr_sad8 / lr_satd_8x4 / lr_satd8, lr_get_ref / lr_bidir_avg /            │
   │ lr_copy_fenc, lr_eval_full / lr_eval_spel_*, lr_check_predictors,        │
   │ lr_fullpel_search / lr_refine_subpel / lr_motion_search / lr_try_bidir    │
   │ → SSBO 5 (per-8x8 MV/cost mirrors), SSBO 6 (winner per MB)               │
   └──────────────────────────────────────────────────────────────────────────┘
                                          │ per-block costs/MVs
                                          ▼
   ┌────────────────────── C: lookahead / slicetype ──────────────────────────┐
   │ put_frame → lowres frame/path cost → scenecut → analyse → decide →      │
   │ get_frames_ex (last_nonb, i_last_keyframe, B/BREF tag, frame ordering)   │
   │ → frame type per picture in coded order                                 │
   └──────────────────────────────────────────────────────────────────────────┘
                                          │ decided type
                                          ▼
   ┌────────────────────── C: frame context / rate control ──────────────────┐
   │ frame_num / POC / nal_ref_idc / ref lists / MMCO (phx_encoder_frame_context)
   │ ratecontrol_start(frame_bits) → frame QP; ratecontrol_aq_frame → per-MB QP│
   │ → UBO 3 + SSBO 9                                                        │
   └──────────────────────────────────────────────────────────────────────────┘
                                          │
                                          ▼
   ┌─────────────── C: slice header (same RBSP as MB data) ──────────────────┐
   │ phx_slice_header_init / _write → RBSP; CABAC context init + encode_init  │
   └──────────────────────────────────────────────────────────────────────────┘
                                          │
                                          ▼
   ┌─────────────── A: full-pel ME (P slices) ───────────────────────────────┐
   │ phx_me16 + SAD + COST_MV + BITS_MVD → SSBO 6 (one result per MB)         │
   └──────────────────────────────────────────────────────────────────────────┘
                                          │
                                          ▼
   ┌─────────────── B: per-MB row wavefront ─────────────────────────────────┐
   │ cache_load_neighbours → cache_load (SSBO 2 → shared phx_mb_cache_t)      │
   │ analysis (predictors/DCT/SAD/SATD), quant, reconstruct, chroma, backup   │
   │ cache_save → SSBO 2; publish phx_mb_decision_t → SSBO 3                  │
   │ size-only RDO accumulator (SSBO 7) used inside per-MB analysis           │
   └──────────────────────────────────────────────────────────────────────────┘
                                          │
                                          ▼
   ┌─────────────── B: in-loop deblock (per MB row) ──────────────────────────┐
   │ strength bs[2][8][4] (SSBO 8), alpha/beta/tc0, 8 edge filters/MB         │
   │ NOTE: not currently wired into phx_encode() — see §14 item 6             │
   └──────────────────────────────────────────────────────────────────────────┘
                                          │
                    ╔═════════════════════▼════════════════════════════════╗
                    ║ BOUNDARY: SSBO 3 = phx_mb_decision_t[n], read-only   ║
                    ║ + SSBO 2 neighbour state snapshot (§8)               ║
                    ╚═════════════════════╦════════════════════════════════╝
                                          ▼
   ┌─────────────── C: serial entropy (host) ────────────────────────────────┐
   │ CABAC: context bins, mvd/ref/cbp/qp_delta, block residual bins, flush    │
   │ CAVLC: mb_skip_run, mb_type/MVD, cbp, qp_delta, coefficient tokens        │
   │ one RBSP cursor; trellis QNS coefficient state machine (Class C)          │
   └──────────────────────────────────────────────────────────────────────────┘
                                          │
                                          ▼
   ┌─────────────── C: NAL assembly ─────────────────────────────────────────┐
   │ rbsp_trailing (CAVLC) / cabac flush, nal_escape (2-byte lookbehind),      │
   │ nal_encode (start code + header), SPS/PPS/SEI NALs, Annex-B byte stream  │
   └──────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Stage classification table

| Stage | Class | Primary source | Producer | Consumer |
|---|---|---|---|---|
| Parameter/set-up | C (data A) | `phx_param.h:24-88`; `phx_tables.h:18-28` | host | all shaders |
| Cost-table build | A (build B by annotation) | `phx_cost.h:23-41`; `src/phx_cost.c:26-93` | `cs_cost_tables_init` | ME, analysis |
| Picture copy / plane padding | A | `phx_frame.h:24-49, 63-73` | transfer + `cs_lowres_*` | lowres kernels |
| Lowres plane construction | A | `src/phx_frame.c:76-79, 106-135` (`mc.c:484`) | `cs_lowres_core` | lookahead |
| Lowres SAD/SATD/hpel search | A | `phx_lowres.h:30-33, 66-85` | `cs_lowres_me` | lookahead cost |
| Lookahead / slice type | C | `phx_lookahead.h:33-34, 72-104`; `slicetype.c:1473/1745` | host | frame context |
| Frame context / frame_num / POC | C | `src/phx_encoder.c:695-778` | host | slice header |
| Rate control (frame + per-MB QP) | C | `phx_ratecontrol.h:54-137`; `src/phx_encoder.c:1335-1343` | host | UBO 3 / SSBO 9 |
| Slice header | C | `phx_slice.h:82-124` | host | RBSP |
| Full-pel ME | A | `phx_me.h:33-38, 76-78` | `cs_me_fullpel` | SSBO 6 → analysis |
| MB analysis | B (A helpers) | `phx_analysis.h:22-23, 39-99` | `cs_mb_row` | SSBO 3 |
| Predict / DCT / quant / reconstruct | A per block, B per MB | `phx_predict.h`, `phx_dct.h`, `phx_quant.h`, `phx_mbencode.h:39-40` | `cs_mb_row` | SSBO 2/3 |
| Chroma DC/AC | B | `phx_mbencode.h:88-97`; `phx_chroma.h` | `cs_mb_row` | SSBO 3 |
| Deblock | B (A helpers) | `phx_deblock.h:20-33, 59-163` | `cs_deblock` | reconstruction / next frame |
| Trellis QNS | C (size API B) | `phx_rdo.h:23-30, 136-198` | host entropy + in-MB size accumulator | SSBO 3 coefficients |
| CABAC / CAVLC syntax | C | `phx_cabac_syntax.h:9`, `phx_cavlc_syntax.h:29-31` | host | RBSP |
| RBSP / NAL / Annex-B | C | `phx_bitstream.h:12, 20-53`; `src/phx_bitstream.c:111-140` | host | output |

### 2.3 Ordering facts that constrain the DAG

1. The lookahead decides frame types in **coded order** and advances `last_nonb`
   (`reference/x264/encoder/lookahead.c:59`; `phx_lookahead.h:22-25`). The frame context
   consumes that decision before any slice data is written (`src/phx_encoder.c:1315-1325`).
2. Rate control starts **after** the type decision and **before** the slice header
   (`src/phx_encoder.c:1327-1343`), and ends **after** NAL size is known
   (`src/phx_encoder.c:1542-1546`). This is a feedback edge; it cannot be hoisted into the
   parallel half.
3. The slice header and CABAC context initialisation precede the MB loop
   (`src/phx_encoder.c:1362-1376`).
4. Analysis → encode → chroma → decision publish → entropy → cache save is the per-MB
   order in the CPU port (`src/phx_encoder.c:1392-1521`). The GPU B stage must produce the
   *same* decision and the *same* neighbour state before the serial stage reads it.
5. Deblocking is an in-loop filter; in x264 it runs before reconstructed frames are reused
   as references. The Phoenix CPU encoder does **not** currently call it (§14 item 6), so
   the GPU stage must match the CPU baseline (no call) until the CPU port wires it.

---

## 3. Vulkan dispatch plan

### 3.1 Global assumptions

- One graphics/transfer queue for picture upload; one compute queue for all shaders; one
  host thread acting as the serial entropy owner. The CPU entropy stage runs concurrently
  with nothing that writes SSBO 3 for the frame it is consuming.
- All shaders are compute shaders. No graphics pipeline, no render passes, no subgroups,
  no atomics, no `VK_KHR_8bit_storage` / `VK_KHR_16bit_storage` requirement in the baseline
  (§5.2 explains the 32-bit canonical layout).
- All dispatches are recorded into one command buffer per frame except where an explicit
  host wait is required (`cs_decision_readback` → host entropy → next frame).
- Between dispatches that share a buffer, insert `vkCmdPipelineBarrier` with
  `srcStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`,
  `dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`,
  `srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT`,
  `dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT`, and a buffer
  memory barrier covering the affected range. A dispatch/submission boundary alone is
  **not** a memory dependency.
- `n = mb_width * mb_height` with `mb_width = i_width/16`, `mb_height = i_height/16`
  (`phx_mbcache.h:42`, `src/phx_mbcache.c:38-41`). Width/height are 16-pixel multiples
  (`src/phx_frame.h:57-58`).

### 3.2 Entry-point table

"Local" is GLSL `local_size_x/y/z`. "Groups" is the `vkCmdDispatch` argument. `ceil_div(a,b) = (a+b-1)/b`.

| # | Shader entry point | Class | Local | Groups | Reads | Writes |
|---|---|---|---|---|---|---|
| S0 | `cs_cost_tables_init` | A | (64,1,1) | `(ceil_div(70*(2*4*mv_range+1),64), 4, 1)` (four tables) | UBO 0/1 | SSBO 0 |
| S1 | `cs_lowres_core` | A | (8,8,1) | `(ceil_div(i_width_lowres,8), ceil_div(i_lines_lowres,8), 1)` | SSBO 1 fenc luma | SSBO 1 lowres[0..3] |
| S2 | `cs_lowres_border` | A | (64,1,1) | `(ceil_div(i_stride_lowres,64), i_lines_lowres+2*PHX_PADV, 4)` | SSBO 1 lowres | SSBO 1 padded lowres |
| S3 | `cs_lowres_clear` | A | (64,1,1) | `(ceil_div(n,64),1,1)` | UBO 0 (`bframes`) | SSBO 1 cost caches, SSBO 5 |
| S4 | `cs_lowres_me` | A | (8,8,1) | `(ceil_div(2*mb_width,8), ceil_div(2*mb_height,8), 1)` per `(p0,p1,b)` triple | SSBO 1 lowres, SSBO 0/5 | SSBO 5 winner slots |
| S5 | `cs_lookahead_decide` | C | (1,1,1) | `(1,1,1)` per minigop; **host recommended** | SSBO 5/6 | frame-type array (host) |
| S6 | `cs_me_fullpel` | A | (64,1,1) | `(ceil_div(n,64),1,1)` | SSBO 1 fenc/fref luma, SSBO 0 cost_mv, UBO 0 | SSBO 6 one result/MB |
| S7 | `cs_mb_row` | B | (64,1,1) | `(1, mb_height, 1)` — one workgroup per MB row | SSBO 1/2/5/6/9, UBO 0/1/2/3 | SSBO 2 state, SSBO 3 decision, SSBO 7 accumulator |
| S8 | `cs_deblock` | B | (64,1,1) | `(1, mb_height, 1)` (not wired — §14) | SSBO 1 recon, SSBO 2, SSBO 8 | SSBO 1 recon, SSBO 8 |
| S9 | `cs_decision_readback` | transfer (copy) | n/a | `vkCmdCopyBuffer` SSBO 3 (+ SSBO 2) → host-visible staging | SSBO 2/3 | staging |
| S10 | `cs_entropy_single_thread` | C (optional, P7) | (1,1,1) | `(1,1,1)` per slice | SSBO 2/3/4 | RBSP SSBO |
| S11 | `cs_nal_escape` | C (optional, P7) | (1,1,1) | `(1,1,1)` per NAL | RBSP SSBO | Annex-B SSBO |

Notes on the table:

- **S0** is once per encode (or per parameter change), not per frame. The table count uses
  `cost_mv[70]` centred at `±2*4*mv_range` (`src/phx_cost.c:32-36`), so the flattened SSBO
  slot is `cost_mv_flat[qp][i + 2*4*mv_range]`. `cost_mv_fpel[qp][j][i] =
  cost_mv[qp][i*4+j]` (`src/phx_cost.c:55`). `cost_ref` is `[70][3][33]` and
  `cost_i4x4_mode` is `[70][17]` (`src/phx_cost.h:26-27`).
- **S1** corresponds to `phx_frame_init_lowres_core` (`src/phx_frame.h:75-79`,
  `reference/x264/common/mc.c:484`). The duplicate-last-row/column step
  (`src/phx_frame.c:114-118`) is a separate tiny kernel or a transfer-region copy; it must
  happen before S1.
- **S4** is dispatched once per reference triple by the serial lookahead owner. That is
  intentional: the *kernels* are A, but selecting which `(p0,p1,b)` triples to evaluate is
  the Class C frame/path cost walk (`phx_lowres.h:66-77`).
- **S5** is listed for completeness. `phx_lookahead_*` and `phx_lh_*` are all Class C
  (`phx_lookahead.h:33-34`; `docs/SHADER_NOTES.md:316-327`). A single-invocation shader is
  legal only if it reproduces the queue order exactly; the primary plan keeps it on the
  host.
- **S7** is the critical stage. One workgroup per MB row avoids a global barrier: all 64
  invocations cooperate on MB `(0,row)`, then `(1,row)`, …, with `barrier()` between MBs.
  The top neighbour comes from SSBO 2, which is guaranteed complete for row `y-1` by the
  inter-dispatch barrier. This is the implementation of the "left/top MBs must be
  complete" rule (`docs/SHADER_MAPPING.md:34`, `docs/SHADER_NOTES.md:290-298`).
- **S9** also copies the SSBO 2 snapshot because the serial entropy writer derives `nC`,
  intra predictions and MV predictors from neighbour cache slots
  (`src/phx_encoder.c:923-964, 970-1010`).

### 3.3 Alternative B wavefront (documented, not selected)

A per-MB workgroup with local size (16,1,1) and `vkCmdDispatch(ceil_div(mb_width, k),
ceil_div(mb_height, k), 1)` can process anti-diagonal `k` in one dispatch, with a full
buffer barrier between diagonals. It preserves the same dependency rule but costs
`mb_width + mb_height - 1` dispatches per stage and needs an extra scratch generation
counter for double-buffered neighbour state. It is **not** the baseline because the number
of submissions is large and the ordering proof is harder. If it is ever adopted, the
acceptance gate is the same SSBO 3 byte comparison as §12 phase P4.

### 3.4 Per-frame command stream (baseline)

```
# once per encode
cs_cost_tables_init                 ; rdo tables built by host (phx_rdo_init, rdo.c:384-415)
barrier(SSBO0 write -> read)

# per input picture
transfer: copy picture bytes -> SSBO 1 (fenc luma + NV12 plane)
barrier(transfer write -> compute read)
cs_lowres_core
barrier(SSBO1 lowres write -> read)
cs_lowres_border
barrier
cs_lowres_clear
barrier
host: phx_lookahead_put_frame ; serial queue + lowres cost/path cost + decide
host: frame context, ratecontrol_start, slice header, CABAC context init
host: upload UBO 3 / SSBO 9 (per-MB QP offsets)
loop over (p0,p1,b) triples required by this minigop:
    cs_lowres_me
    barrier(SSBO5 write -> read)
    host: phx_lh_frame_cost / path_cost read SSBO 5 in frame order
barrier(all lookahead reads complete -> SSBO1/5 reuse)
cs_me_fullpel                       ; only for P decisions with a list-0 reference
barrier(SSBO6 write -> read)
cs_mb_row                           ; SSBO 2 + SSBO 3 + SSBO 7
barrier(SSBO2/3 write -> read)
[cs_deblock]                        ; only when the CPU port wires deblocking
barrier(SSBO1 write -> read)
copy SSBO 2 + SSBO 3 -> staging buffer
submit + host wait (fence)
host: serial CABAC/CAVLC MB loop, flush, nal_escape, nal_encode
host: ratecontrol_end(actual NAL bytes)
```

**Why the host wait is at the boundary.** The CPU entropy loop reads `phx_mb_decision_t`
in scan order and mutates its own `phx_mb_cache_t` / `phx_mb_state_t` copies. It cannot
start before the whole B stage has published SSBO 3 (§8). The wait is one per frame, not
one per MB.

---

## 4. Workgroup internals (Class B)

### 4.1 Shared-memory image of `phx_mb_cache_t`

`phx_mb_cache_t` is declared at `src/phx_mbcache.h:61-79`. The shader workgroup keeps the
arrays in `workgroup` memory with exactly these shapes:

| Field | CPU declaration | Workgroup declaration | Size (bytes, 32-bit ints) |
|---|---|---|---|
| `intra4x4_pred_mode` | `int8_t[40]` (`:74`) | `int[40]` | 160 |
| `non_zero_count` | `uint8_t[120]` (`:75`) | `uint[120]` | 480 |
| `ref[2]` | `int8_t[2][40]` (`:76`) | `int[2][40]` | 320 |
| `mv[2]` | `int16_t[2][40][2]` (`:77`) | `ivec2[2][40]` | 640 |
| `mvd[2]` | `int16_t[2][40][2]` (`:78`) | `ivec2[2][40]` | 640 |
| neighbour scalars | `i_mb_x/y/xy`, `i_b8_xy`, `i_b4_xy`, `top`, `left[2]`, `topleft`, `topright`, `i_neighbour*`, `i_cbp_*`, `b_allow_skip`, … (`:62-71`) | per-workgroup scalars/arrays | ~256 |

Total ≈ 2.5 KiB of workgroup memory, well under the 16 KiB minimum guaranteed by Vulkan.
8-bit/16-bit storage extensions could shrink this but are not required.

`phx_scan8[51]` (`src/phx_mbcache.c:9-23`, `reference/x264/common/base.h:180`) and
`phx_left_indices[4]` (`src/phx_mbcache.c:26-31`, `reference/x264/common/macroblock.c:662`)
are copied verbatim into UBO 1. `phx_left_indices[3]` is the progressive entry selected at
`src/phx_mbcache.c:96`. `DATA_STRUCTURES.md:77-81` records that earlier off-by-one defects
(E83–E95) came from this layout; **do not re-derive it**.

### 4.2 Row workgroup algorithm

```
% gl_WorkGroupID.y == row
shared PhxMbCache cache;
for (mb_x = 0; mb_x < mb_width; ++mb_x) {
    load_neighbours(cache, SSBO2, mb_x, row);   // phx_mbcache.c:75-96, 132-...
    load_cache(cache, SSBO2, mb_x, row);        // macroblock.c:855 in x264
    barrier();
    decide_and_encode(cache, SSBO1, SSBO6, UBO0..3, decision);
    // predictors / DCT / quant from A helpers; chroma from phx_chroma.h
    size_only_rdo(cache, SSBO7[xy]);            // phx_rdo.h:153-198
    barrier();
    backup_intra(cache, SSBO1.recon, row);      // phx_mbcache.h:93-95
    cache_save(cache, SSBO2, xy);               // phx_mbcache.c:434-515
    publish_decision(SSBO3[xy], decision);      // §8
    barrier();                                  // before next MB reads SSBO2/left
}
```

Ordering inside `decide_and_encode` must match `src/phx_encoder.c:1401-1481`: decision is
zeroed, QP is taken from `phx_ratecontrol_mb_qp`, analysis runs, unsupported modes fall
back to `PHX_I_16x16`, encode fills coefficients and cache nnz, chroma is encoded only for
non-inter decisions, then `phx_encoder_apply_decision` publishes the decision values into
the cache. Any deviation changes CABAC bins.

### 4.3 MBAFF caveat

MBAFF neighbour selection is present in the CPU port
(`src/phx_mbcache.c:105-129, 179-214, 360-408`) and must be preserved if `b_mbaff` is ever
enabled. The spec's baseline is progressive only: `phx_encoder.c:1399` passes `b_mbaff=0`,
`phx_encoder.c:751` sets `fc.b_mbaff = 0`, and deblock's MBAFF entry points are not wired
(`phx_deblock.h:25-33`). **UNKNOWN**: no shader dispatch for field pairs is specified until
a progressive-byte-exact gate passes.

---

## 5. Buffer / UBO / SSBO layouts

### 5.1 Canonical 32-bit representation

GLSL SSBOs are declared `layout(std430, binding = N)`. Every field is a 32-bit `int`,
`uint`, `float`, `ivec2` or array thereof. CPU `int8_t`/`uint8_t` values are zero-extended
(for unsigned) or sign-extended (for signed) on upload; `int16_t` values are sign-extended.
This avoids a dependency on `VK_KHR_8bit_storage` / `VK_KHR_16bit_storage` and makes
byte-level comparison with the CPU port a simple masking/sign-extension test. An optional
packed variant is permitted only if it is behind an exactness gate.

### 5.2 Binding map

| Binding | Kind | Object | Lifetime |
|---|---|---|---|
| UBO 0 | `layout(std140)` | `PhxParam` (`phx_param.h:24-75`) | whole encode, immutable after `phx_param_finalize` |
| UBO 1 | `layout(std140)` | tables + `phx_scan8` + `phx_left_indices` (`phx_tables.h:18-28`; `phx_mbcache.h:31-38`) | whole encode, immutable |
| UBO 2 | `layout(std140)` | `PhxRdoConfig` (`phx_rdo.h:80-87`) | per dispatch, set by `phx_rdo_set_config` |
| UBO 3 | `layout(std140)` | `PhxRcFrame` frame scalars | per frame, after `phx_ratecontrol_start` |
| SSBO 0 | `std430` | `PhxCostTables` | whole encode; rebuild on preset/profile change |
| SSBO 1 | `std430` | `PhxFrameBuffers` + dims | per frame; recycled after lookahead releases frame |
| SSBO 2 | `std430` | `PhxMbState` (SoA) | per frame; zeroed at slice start (`src/phx_encoder.c:794-816`) |
| SSBO 3 | `std430` | `PhxMbDecision[n]` | per frame; fully overwritten before entropy reads |
| SSBO 4 | `std430` | `PhxCabacState` + RBSP bytes | per slice; reset at slice start |
| SSBO 5 | `std430` | `PhxLowresMirrors` | per frame/minigop; cleared by S3 / `phx_lowres_frame_reset` |
| SSBO 6 | `std430` | `PhxMeResult[n]` | per frame; one slot per MB |
| SSBO 7 | `std430` | `PhxCabacSize[n]` | per MB workgroup, reseeded each MB |
| SSBO 8 | `std430` | `PhxDeblockBs[n]` | per frame; recomputed per MB |
| SSBO 9 | `std430` | per-MB QP offsets / inv qscale | per frame, after AQ |

### 5.3 UBO 0 — `PhxParam`

Direct mirror of `src/phx_param.h:24-75`, all fields `int` except `f_psy_rd`,
`f_psy_trellis`, `f_rf_constant`, `f_ip_factor`, `f_pb_factor`, `f_aq_strength` (`float`),
and the enums `i_me_method`, `i_bframe_adaptive`, `rc_method` (`int` with the exact values
from `phx_param.h:12-22`). Add derived scalars at the end:

```glsl
layout(std140, binding = 0) uniform PhxParam {
    int  i_csp, i_width, i_height, i_log2_max_frame_num;
    int  i_frame_reference, i_keyint_max, i_keyint_min, i_bframe;
    int  i_scenecut_threshold, i_bframe_adaptive, b_open_gop;
    int  b_cabac, b_deblocking_filter, b_transform_8x8;
    int  i_me_method, i_me_range, i_subpel_refine, b_mixed_references;
    int  b_fast_pskip, i_direct_mv_pred, b_weighted_bipred, i_weighted_pred;
    int  i_trellis, b_psy, i_chroma_qp_offset, b_dct_decimate;
    int  analyse_intra, analyse_inter;
    int  rc_method, i_qp_constant, i_aq_mode, i_lookahead, b_mb_tree;
    float f_psy_rd, f_psy_trellis, f_rf_constant, f_ip_factor, f_pb_factor, f_aq_strength;
    int  i_chroma_qp_index_offset;
    // derived by host, not in phx_param_t:
    int  mb_width, mb_height, mb_count, bframes;
    int  i_stride, i_width_lowres, i_lines_lowres, i_stride_lowres;
    ivec2 pad0;
} param;
```

`i_chroma_qp_index_offset` is derived at runtime (`phx_param.h:73-74`). `bframes` mirrors
`phx_frame_t::bframes` (`src/phx_frame.h:37`). All stride/size values come from
`phx_frame.h:25-27`.

### 5.4 UBO 1 — immutable tables

Exact declarations from `src/phx_tables.h:18-28` and `src/phx_mbcache.h:31-38`:

```glsl
layout(std140, binding = 1) uniform PhxTables {
    uint  lambda_tab[82];             // phx_lambda_tab, tables.c:97
    int   lambda2_tab[82];            // tables.c:114
    int   trellis_lambda2_tab[2][82];  // tables.c:131
    uint  chroma_lambda2_offset_tab[37]; // tables.c:168
    uint  ue_size_tab[256];           // bitstream.h:201
    uint  chroma_qp_base[76];         // macroblock.h:249 (8-bit entries)
    uint  cabac_range_lps[64][4];     // tables.c:1668
    uint  cabac_transition[128][2];   // tables.c:1688
    uint  cabac_renorm_shift[64];     // tables.c:1708
    int   cabac_context_init_I[1024][2];   // tables.c:456
    int   cabac_context_init_PB[3][1024][2]; // tables.c:768
    uint  scan8[51];                  // base.h:180, phx_mbcache.c:9-23
    LeftTable left_indices[4];        // macroblock.c:662, phx_mbcache.c:26-31
} tables;
```

`LeftTable` is `{ int intra[4], nnz[4], nnz_chroma[4], mv[4], ref[4]; }`
(`src/phx_mbcache.h:35-37`). std140 array-of-struct stride is 80 bytes (20 ints); pad
explicitly if the compiler disagrees. These tables are Class A data; one bind survives
every frame and every seek (`docs/SHADER_NOTES.md:82-91`).

### 5.5 UBO 2 — `PhxRdoConfig`

Mirror of `phx_rdo_config_t` (`src/phx_rdo.h:80-87`):

```glsl
layout(std140, binding = 2) uniform PhxRdoConfig {
    int chroma_qp_index_offset;
    int i_psy_trellis;      // FIX8(f_psy_trellis/4)
    int b_interlaced;
    int idx;                // block index used by psy-trellis
    // fenc_dct4/fenc_dct8 are pointers in the CPU API (rdo.h:85-86): replace with
    // an SSBO 1 offset + block kind, never a pointer.
    int fenc_dct4_offset, fenc_dct8_offset;
    ivec2 pad0;
} rdo_cfg;
```

`phx_rdo_set_config()` is documented as "change before calling the wrappers, not
concurrently" (`src/phx_rdo.h:76-78`), which satisfies the immutability requirement during
one dispatch.

### 5.6 UBO 3 + SSBO 9 — rate control

`phx_ratecontrol_t` is an **opaque** typedef (`src/phx_ratecontrol.h:54`): its internal
accumulators are not exported, so they cannot be mirrored. Only its *outputs* cross the
boundary.

```glsl
layout(std140, binding = 3) uniform PhxRcFrame {
    int frame_qp;             // phx_ratecontrol_start(..., &qp_out), ratecontrol.h:105
    int chroma_qp_index_offset;
    int aq_mode;              // PHX_RC_AQ_*, ratecontrol.h:44-47
    int mb_count;
    int b_kept_as_ref;        // selects f_qp_offset vs f_qp_offset_aq, ratecontrol.h:120-123
    int inv_qscale_base;      // from phx_ratecontrol_inv_qscale(), ratecontrol.h:128
    ivec2 pad0;
} rc_frame;

layout(std430, binding = 9) buffer PhxRcBuffers {
    float qp_offset[];        // phx_ratecontrol_qp_offsets(), ratecontrol.h:126
    float qp_offset_aq[];     // phx_ratecontrol_qp_offsets_aq(), ratecontrol.h:127
    uint  inv_qscale[];       // phx_ratecontrol_inv_qscale(), ratecontrol.h:128
};
```

Array offsets: `qp_offset_aq` begins at `mb_count` floats, `inv_qscale` at `2*mb_count`
floats. Per-MB QP is `phx_ratecontrol_mb_qp(rc, mb_xy, frame_qp, b_kept_as_ref)`
(`src/phx_ratecontrol.h:123`, call site `src/phx_encoder.c:1405-1407`). The AQ pass is
serial by annotation (`src/phx_ratecontrol.h:22-24, 112-118`), so UBO 3/SSBO 9 must be
finalised before S7.

### 5.7 SSBO 0 — `PhxCostTables`

```glsl
layout(std430, binding = 0) buffer PhxCostTables {
    uint cost_mv_flat[];        // 70 * (2*4*mv_range+1) entries, centred i=0 at index 2*4*mv_range
    uint cost_mv_fpel[];        // 70 * 4 * (2*4*mv_range+1)
    uint cost_ref[];            // 70 * 3 * 33  (phx_cost.h:26)
    uint cost_i4x4_mode[];      // 70 * 17      (phx_cost.h:27)
};
```

Base offsets (all in `uint` slots):

| Section | Offset | Length |
|---|---|---|
| `cost_mv_flat` | 0 | `70 * (8*mv_range+1)` |
| `cost_mv_fpel` | `70*(8*mv_range+1)` | `70*4*(8*mv_range+1)` |
| `cost_ref` | previous + `280*(8*mv_range+1)` | `70*3*33 = 6930` |
| `cost_i4x4_mode` | previous + 6930 | `70*17 = 1190` |

The CPU representation uses centred pointers (`src/phx_cost.c:32-36, 92-93`) and
`phx_mb_analysis_t::p_cost_mv` is a pointer (`src/phx_cost.h:39`); **flatten in the owning
module before upload**, never in the shader. `phx_mb_analyse_load_costs`
(`src/phx_cost.h:62-63`, `src/phx_cost.c:138`) becomes a UBO scalar pair identifying the QP
and the active reference count.

### 5.8 SSBO 1 — frame planes

Mirror of `phx_frame_t` (`src/phx_frame.h:24-49`). Dimensions are in UBO 0; the buffer
holds packed 32-bit words (four luma bytes per word):

```glsl
layout(std430, binding = 1) buffer PhxFrameBuffers {
    uint luma[];        // stride i_stride bytes; PHX_PADV=32 rows top/bottom (frame.h:17)
    uint chroma[];      // NV12 U/V pairs (frame.h:39-48), stride i_stride_planes[1]
    uint lowres0[];     // i_stride_lowres, i_lines_lowres + 2*PHX_PADV rows
    uint lowres1[];     // H
    uint lowres2[];     // V
    uint lowres3[];     // HV
    uint cost_est[];    // 18*18 ints (frame.h:33, PHX_BFRAME_MAX+2)
    uint row_satds[];   // 18*18*mb_width ints, only [0] initialised (src/phx_frame.c:128-130)
    int  lowres_mvs[];  // 2*17*2 (frame.h:35), sentinel 0x7FFF (src/phx_frame.c:132-134)
};
```

`i_row_satds` is a host pointer array (`src/phx_frame.h:34`); flatten to a single array
with stride `mb_width` per `[y][x]` cell. `lowres[0..3]` order is Orig/H/V/HV
(`src/phx_frame.h:31`, `reference/x264/common/mc.c:281`). A reconstructed-plane view
(`h->recon`, FDEC_STRIDE layout, `src/phx_encoder.c:1396-1398`) must be a distinct range
within SSBO 1 so deblock can write it without aliasing fenc.

### 5.9 SSBO 2 — `PhxMbState` (SoA)

Mirror of `src/phx_mbcache.h:41-58`; allocation sizes from `src/phx_mbcache.c:33-59` and
reset values from `src/phx_encoder.c:794-816`.

```glsl
layout(std430, binding = 2) buffer PhxMbState {
    uint  mb_type[];            // uint8 in CPU (mbcache.h:43); reset 0
    int   intra4x4_pred_mode[]; // [n][8]  (mbcache.h:44); reset 2
    uint  non_zero_count[];     // [n][48] (mbcache.h:45); reset 0
    int   cbp[];                // [n]     (mbcache.h:46); reset 0; packed (cbp_chroma<<4)|cbp_luma|0x1700
    uint  mb_transform_size[];  // [n]     (mbcache.h:47); reset 0
    int   ref0[];               // [n][4]  (mbcache.h:48); reset -1
    int   ref1[];               // [n][4]
    ivec2 mv0[];                // [n][16] (mbcache.h:49); reset 0
    ivec2 mv1[];                // [n][16]
    ivec2 mvd0[];               // [n][8]  (mbcache.h:50); reset 0; CABAC only
    ivec2 mvd1[];               // [n][8]
    uint  slice_table[];        // [n]     (mbcache.h:51); reset 0
    uint  field[];              // [n]     (mbcache.h:52)
    uint  intra_border_backup[];// [3][3][16] (mbcache.h:53)
    uint  threadslice_start;
    uint  first_mb;
    uint  b_constrained_intra, b_cabac, slice_type; // mbcache.h:54-57
};
```

`mb_stride`, `b8_stride`, `b4_stride` are derived (`mb_stride = mb_width`,
`b8_stride = 2*mb_width`, `b4_stride = 4*mb_width`, `src/phx_mbcache.c:38-41`) and live in
UBO 0. `cache_save` (`src/phx_mbcache.c:434-515`) is the sole writer of these arrays in the
baseline; `cache_load_neighbours` / `cache_load` (`src/phx_mbcache.c:75-96`, `macroblock.c:855`)
are the readers.

Key `cache_save` behaviours to reproduce exactly:

- `slice_table[xy] = first_mb` (`:455`).
- I_4x4 intra4x4_pred_mode packing via `phx_scan8[10]`, `[5]`, `[7]`, `[13]` (`:458-464`).
- Other intra types fill 2, constrained-intra inter fills -1 (`:465-471`).
- I_PCM sets cbp with `0x1700` and nnz to 1/16 (`:473-478`).
- nnz store uses `nz_idx[] = {0,2,8,10,16,18,32,34}` → `nz_dst[] = {0,4,8,12,16,20,32,36}`
  (`:481-484`, from `reference/x264/common/macroblock.c:1761-1768`).
- `b_transform_8x8` is cleared when `cbp_luma == 0` and type is not I_8x8 (`:486-488`).
- ref/mv stores use `phx_scan8[4*k]` and `phx_scan8[k]` (`:492-511`); B slices also store
  list-1 refs (`:504-505`).
- CABAC mvd store copies `c->mvd[0][phx_scan8[0..7]]` (`:513-514`).

### 5.10 SSBO 3 — `PhxMbDecision[n]`

The CPU struct exists at `src/phx_mb_decision.h:14-40`; use it as the semantic definition.
A std430-safe GPU form (all fields 32-bit, `ivec2` for MVs) is:

```glsl
struct PhxMbDecision {
    int   mb_type;                 // 0  : PHX_I_4x4..PHX_B_SKIP (mbcache.h:22-29, cabac_syntax.h:19-30)
    int   i_partition;             // 4  : PHX_D_* (cabac_syntax.h:33-49)
    int   i_qp;                    // 8
    int   b_transform_8x8;         // 12
    int   cbp_luma;                // 16
    int   cbp_chroma;              // 20
    int   i_cbp;                   // 24
    int   intra16_pred_mode;       // 28 : raw 0..6
    int   chroma_pred_mode;        // 32 : raw 0..6
    int   skip;                    // 36 : P_SKIP / B_SKIP
    int   intra8x8_pred_mode[4];   // 40 : reserved (see §8.3)
    int   intra4x4_pred_mode[16];  // 56 : raw 0..11, block-indexed k
    ivec2 mv[2][16];               // 120 : qpel, [list][block k]
    int   ref[2][16];              // 376 : [list][block k]
    uint  nnz[48];                 // 504 : block-indexed k (see §8.2)
    int   dct[48][16];             // 696 : zigzag quantised coefficients, scan8 slot indexed
    int   chroma_dct[2][5][16];    // 3768 : [U/V][DC + 4 AC][16]
    uint  chroma_nnz[2][5];        // 4408 : [U/V][DC + 4 AC]
};                                 // stride 4448 bytes, base alignment 8
layout(std430, binding = 3) buffer PhxDecisions { PhxMbDecision decision[]; };
```

Offset table (bytes):

| Field | Offset | Size |
|---|---|---|
| scalar header (`mb_type` … `skip`) | 0 | 40 |
| `intra8x8_pred_mode[4]` | 40 | 16 |
| `intra4x4_pred_mode[16]` | 56 | 64 |
| `mv[2][16]` | 120 | 256 |
| `ref[2][16]` | 376 | 128 |
| `nnz[48]` | 504 | 192 |
| `dct[48][16]` | 696 | 3072 |
| `chroma_dct[2][5][16]` | 3768 | 640 |
| `chroma_nnz[2][5]` | 4408 | 40 |
| **total stride** | — | **4448** |

The CPU struct is pointer-free but uses C `int` / `int8_t` / `int16_t` / `uint8_t`, so a
host packing routine is required; it must sign-extend `mv`/`ref`/`dct` and zero-extend
`nnz`/`chroma_nnz`. Gate this packing routine with a byte comparison against a CPU dump
before any shader is written (§12, P1).

### 5.11 SSBO 4 — `PhxCabacState` and RBSP

`phx_cabac_t` (`src/phx_cabac.h:20-26`):

```glsl
layout(std430, binding = 4) buffer PhxCabacState {
    int  i_low, i_range, i_queue, i_bytes_outstanding;
    int  p_start_offset, p_offset, p_end_offset;   // replace pointers with RBSP offsets
    uint state[1024];                              // 460 active, PHX_CABAC_CTX_COUNT (cabac.h:18)
    uint rbsp[];                                   // separate range; nal_encode rewrites p_payload (bitstream.c:138-139)
};
```

The baseline keeps this state on the CPU (`phx_encoder.c:1367-1373, 1483-1513, 1524-1527`);
the SSBO declaration is the target for the optional P7 single-invocation port. The RBSP
byte buffer and `phx_nal_t` descriptors (`src/phx_bitstream.h:20-32`) must be a separate
range because `phx_nal_encode` updates `p_payload` and `i_payload` in place
(`src/phx_bitstream.c:138-139`).

### 5.12 SSBO 5 — lowres mirrors

Mirror of the per-frame context described in `docs/SHADER_NOTES.md:187-197` and
`src/phx_lowres.h:23-28`:

```glsl
layout(std430, binding = 5) buffer PhxLowresMirrors {
    ivec2 mvs[2];            // [list][mb_count*4] lowres 8x8 blocks
    int   mv_costs[2];       // [list][mb_count*4]
    int   lowres_costs[];    // (bframes+2)^2, frame-level
    int   intra_cost[];      // mb_count*4, alias of lowres_costs per frame.h:135
    uint  b_intra_calculated;
};
```

In GLSL these are variable-length arrays (`mvs[2][]` is not allowed in one declaration);
declare three separate arrays with host-computed base offsets. `phx_lowres_frame_reset()`
(`src/phx_lowres.h:56-59`, `reference/x264/common/mc.c:473-481`) is the explicit
invalidation entry point before reuse.

### 5.13 SSBO 6 — ME result

```glsl
struct PhxMeResult { ivec2 mv; int sad; int cost; int ref; }; // 32 bytes
layout(std430, binding = 6) buffer PhxMeResults { PhxMeResult result[]; };
```

Producer `cs_me_fullpel` implements `phx_me16` (`src/phx_me.h:76-78`, source lines
`me.c:60-107, 182-419`). One result per MB (`n` entries); `mv` is in quarter-pel units
(`phx_me.h:67-68`), `cost` has `SAD + lambda*MVbits` semantics (`phx_me.h:70`), `-1` is the
error sentinel (`phx_me.h:50-51, 70-71`).

### 5.14 SSBO 7 — size-only RDO accumulator

`phx_cabac_size_t` (`src/phx_rdo.h:153-156`) is exactly the two fields x264's `COPY_CABAC`
copies (`reference/x264/encoder/rdo.c:71-73`): `state[0..459]` and `f8_bits_encoded`.

```glsl
struct PhxCabacSize { uint state[460]; int f8_bits_encoded; }; // 1844 -> pad to 1848
layout(std430, binding = 7) buffer PhxRdoSizes { PhxCabacSize size_acc[]; };
```

One per MB workgroup, reseeded by `phx_rdo_size_init` (`src/phx_rdo.h:161-162`). It is
**not** an arithmetic coder: no `i_low`, `i_range`, queue or output bytes
(`phx_rdo.h:146-151`). Trellis node arrays (`phx_rdo.h:218-220`, `src/phx_rdo.c:586-834`)
remain invocation-local, i.e. Class C inside the entropy stage.

### 5.15 SSBO 8 — deblock strength

```glsl
layout(std430, binding = 8) buffer PhxDeblockBs { uint bs[]; }; // [n][2][8][4] uints = 256 B/MB
```

Matches `phx_deblock_strength_c(const phx_mb_cache_t*, uint8_t bs[2][8][4], ...)`
(`src/phx_deblock.h:61-62`): dir 0 = vertical, dir 1 = horizontal; edge 0..3 are 4x4
rows/columns (`phx_deblock.h:53-59`). `phx_macroblock_deblock_strength`
(`src/phx_deblock.h:72-76`, `reference/x264/common/macroblock.c:1438-1622`) is the producer
actually used by x264.

### 5.16 Alignment and lifetime rules

1. All SSBO arrays begin at a 16-byte-aligned offset; each `PhxMbDecision` begins on an
   8-byte boundary (4448 is divisible by 8).
2. Per-frame buffers are allocated once and reused; a frame's slot is invalid until S3/S7
   have run for that frame. There is no implicit zeroing: `cs_lowres_clear` and the host
   equivalent of `phx_encoder_reset_mb_state` are explicit writes
   (`src/phx_frame.c:126-134`, `src/phx_encoder.c:801-815`).
3. SSBO 3 is single-generation: it is overwritten for frame `f` before frame `f`'s entropy
   read, and never carried across frames. No double buffering is required because the
   host waits at S9 before starting entropy.
4. SSBO 2 is shared between B and C: B writes it, C reads it for neighbour contexts, and C
   never writes it back in the GPU plan (the CPU reference writes `type[xy]` and saves the
   cache inside the same loop at `src/phx_encoder.c:1519-1520`; B already published the
   equivalent state).
5. UBO 0/1/SSBO 0 survive seek; UBO 3/SSBO 2/3/5/6/7/8/9 do not.
6. Host-visible staging is used only for S9 and for initial uploads; device-local buffers
   are used otherwise.

---

## 6. The A/B → C boundary in detail

This is the core of the design. `docs/SHADER_MAPPING.md:67-70` requires an explicit array
of mode, MV, CBP, quantised coefficients and nnz flags. `src/phx_mb_decision.h:14-40` is
the existing type. The serial consumer code is `src/phx_encoder.c:824-845` (decision →
cache), `:858-874` (coefficient views), `:1080-1260` (CAVLC/CABAC writers).

### 6.1 What the B stage writes (per MB `xy`)

| Decision field | Producer in CPU port | Exact value / encoding |
|---|---|---|
| `mb_type` | `phx_macroblock_analyse_slice`, `src/phx_encoder.c:1412-1415` | One of `PHX_I_4x4..PHX_B_SKIP` (`phx_mbcache.h:22-29`, `phx_cabac_syntax.h:19-30`). Unsupported modes fall back to `PHX_I_16x16` (`:1425-1438`). |
| `i_partition` | analysis (`:1409`) | `PHX_D_*`, `phx_cabac_syntax.h:33-49`. |
| `i_qp` | `phx_ratecontrol_mb_qp`, `:1405-1407` | Frame QP + per-MB AQ offset; CQP path uses `sh.i_qp`. |
| `b_transform_8x8` | forced 0 at `:1444` | Entrypoints gated by `x264_mb_transform_8x8_allowed` (`:895-907`). |
| `cbp_luma`, `cbp_chroma`, `i_cbp` | encode + chroma, `:1445-1472` | 4-bit luma / 2-bit chroma; `i_cbp = (chroma<<4)|luma|0x1700`-style packed value (see `src/phx_mbcache.c:474, 490`). |
| `intra4x4_pred_mode[16]` | analysis, `phx_analysis.h:52-55` | Raw 0..11 **block-indexed k**; FIX4 mapping applied by entropy (`:958-964`, `:1043-1053`). |
| `intra8x8_pred_mode[4]` | analysis | **Reserved**: no serial reader in the current code (`phx_encoder.c` never reads it). Write zero and gate; do not invent syntax. |
| `intra16_pred_mode` | analysis | Raw 0..6; FIX16 mapping `{0,1,2,3,2,2,2}` at `:1027`. |
| `chroma_pred_mode` | chroma analysis | Raw 0..6; FIXC mapping `{0,1,2,3,0,0,0}` at `:1028`. |
| `mv[2][16][2]` | `phx_me16` → analysis | Quarter-pel, block-indexed `k`; entropy maps to cache slots via `phx_scan8[k]` (`:832-839`). |
| `ref[2][16]` | analysis | Block-indexed `k`; `-1` for intra/non-ref; mapped via `phx_scan8[k]` (`:833-834`). |
| `nnz[48]` | encode (`phx_mbencode.h:30-37`) | Canonical layout §6.2. Also written to SSBO 2 via `cache_save`. |
| `dct[48][16]` | quant/encode, `phx_mbencode.h:30-37` | Zigzag-scanned quantised coefficients; slot `phx_scan8[k]`, I16 DC in slot 0. |
| `skip` | analysis | `P_SKIP`/`B_SKIP`; `src/phx_encoder.c:1430` avoids fabricated skips. |
| `chroma_dct[2][5][16]` | `phx_macroblock_encode_chroma`, `:1468-1472` | Plane 0=U, 1=V; block 0 = 2x2 DC (4 coefficients), blocks 1..4 = 4x4 AC (15 coefficients). |
| `chroma_nnz[2][5]` | chroma encode | 0/1 flags per chroma block; slots 0 copied to `phx_scan8[49]/[50]` (`:843-844`). |

### 6.2 `scan8` layout and nnz indexing

`phx_scan8[51]` (`src/phx_mbcache.c:9-23`) maps a 4x4 block number to a cache slot in a
5-row × 8-column layout (`X264_SCAN8_LUMA_SIZE = 5*8`, `X264_SCAN8_SIZE = 3*40`,
`reference/x264/common/base.h:153-154`):

| Block index `k` | `phx_scan8[k]` | Meaning |
|---|---|---|
| 0..15 | 12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39 | 16 luma 4x4 blocks |
| 16..19 | 52,53,60,61 | U chroma 4x4 AC blocks |
| 32..35 | 92,93,100,101 | V chroma 4x4 AC blocks |
| 48 | 0 | luma I16 DC Hadamard alias (`PHX_LUMA_DC = 48`, `phx_cabac_syntax.h:68`) |
| 49 | 40 | U chroma DC (`PHX_CHROMA_DC = 49`, `phx_cabac_syntax.h:69`) |
| 50 | 80 | V chroma DC |

Slots 20..23, 24..27, 46..47, 54..63, etc. are neighbours (`-1`, `-8` lookups) or unused.
**Canonical decision rule** for this spec:

- `decision.nnz[k]` (k = 0..47) is **block-indexed**, i.e. `nnz[k] = cache.non_zero_count[phx_scan8[k]]`. This matches `phx_analysis.h:45-46` and, crucially,
  the serial consumer at `src/phx_encoder.c:841-842`.
- `decision.chroma_nnz[ch][0]` feeds `cache.non_zero_count[phx_scan8[49 or 50]]`
  (`src/phx_encoder.c:843-844`).
- `decision.dct[phx_scan8[k]]` holds luma block k's coefficients, **not** `dct[k]`
  (`phx_analysis.h:47-51`, `phx_mbencode.h:30-37`). The serial views are:
  - I16 DC: `d->dct[0]`, 4 coefficients (`:862`, `scan8[48] == 0`).
  - I16 AC block i: `d->dct[phx_scan8[i]] + 1`, 15 coefficients (`:863`).
  - 4x4 luma block idx: `d->dct[phx_scan8[idx]]`, 16 coefficients (`:864`).
  - Chroma DC plane ch: `d->chroma_dct[ch][0]`, 4 coefficients (`:867`).
  - Chroma AC idx (16..19 U, 32..35 V): `d->chroma_dct[(idx>>4)-1][1+(idx&3)]`,
    15 coefficients (`:869-871`).

**Known inconsistency — must be resolved by gate P4.** `src/phx_mbencode.h:34-35` describes
`d->nnz[slot]` (slot-indexed), whereas the serial consumer at
`src/phx_encoder.c:841-842` treats it as block-indexed. This spec adopts the serial
consumer's interpretation because the entropy stage is the authoritative reader. The P4
gate compares the whole SSBO 3 dump against a CPU dump; any disagreement is a gate failure,
not a shader bug to paper over.

### 6.3 Chroma DC slots

4:2:0 chroma is explicit in the decision (`src/phx_mb_decision.h:35-39`) because the shared
48-entry luma arrays cannot represent `scan8[49]`/`scan8[50]`:

```
U DC : decision.chroma_nnz[0][0] → cache.non_zero_count[phx_scan8[49] = 40]
       decision.chroma_dct[0][0][0..3] → 4 quantised DC coefficients
V DC : decision.chroma_nnz[1][0] → cache.non_zero_count[phx_scan8[50] = 80]
       decision.chroma_dct[1][0][0..3] → 4 quantised DC coefficients
U AC : decision.chroma_dct[0][1..4] ↔ phx_scan8[16..19]
V AC : decision.chroma_dct[1][1..4] ↔ phx_scan8[32..35]
```

`phx_encoder_block` returns `ncoeff = 4` for DC and `15` for AC (`:867-871`), and the
residual writers use those counts (`:1141-1173`, `:1220-1257`). The entropy stage also
derives nnz from the coefficient values themselves via `phx_encoder_block_nz`
(`:881-887`) so that CBF and the written residual stay consistent; the shader must fill
both arrays consistently.

### 6.4 What the serial stage reads, and what it writes back

The serial stage reads:

1. `SSBO 3[xy]` in raster scan order, one MB at a time (`src/phx_encoder.c:1392-1521`).
2. `SSBO 2` neighbour fields to build `phx_cabac_mb_t` / `phx_cavlc_mb_t`:
   - `prev_type`, `prev_cbp` from `type[xy-1]`, `cbp[xy-1]` (`:1494-1495`).
   - left/top ref and MV via the borrowed `phx_mb_cache_t` (`phx_cabac_syntax.h:73-75`,
     `phx_cavlc_syntax.h:81-84`, `phx_encoder.c:923-1010`).
   - `non_zero_count[phx_scan8[idx]-1]` and `-8` for nC (`phx_encoder.c:928-930`).
3. Frame-level values that do **not** live in SSBO 3: `slice_type`, `frame_num`, `i_poc`,
   `nal_ref_idc`, `i_frame`, `qp`, `i_cabac_init_idc`, deblock offsets, ref lists, weights,
   MMCO (`phx_slice.h:83-111`). These come from the C frame context.
4. `h->i_frame` for the CABAC flush tie-break bit (`src/phx_cabac.c:218`).

The serial stage writes back only to its own state: `phx_cabac_t` (`i_low`, `i_range`,
`i_queue`, `state[]`, RBSP cursor), `phx_bs_t`, `last_qp`/`last_dqp`, `skip_run`, and the
NAL descriptors. It never writes SSBO 3. In the CPU reference the serial loop also calls
`phx_encoder_apply_decision` and `phx_macroblock_cache_save` per MB (`:1481, 1519-1520`);
in the GPU plan those writes are already in SSBO 2 from S7, which is why the host wait
happens before entropy starts.

### 6.5 Why this boundary is sufficient

Every syntax writer in `phx_cabac_syntax.h` / `phx_cavlc_syntax.h` takes either a
`phx_cabac_t` + `phx_cabac_mb_t` + coefficient pointer, or a `phx_cavlc_mb_t` + `phx_bs_t`
(`phx_cabac_syntax.h:100-159`, `phx_cavlc_syntax.h:103-137`). With SSBO 2 + SSBO 3 + the
frame context, all of those inputs are reconstructable. No syntax writer reads fenc pixels
directly; the only pixel-derived data are the quantised coefficients already in the
decision (the I_PCM raw-sample path is a documented TODO at `:1194-1204` and `:1092-1101`).

---

## 7. Determinism and seek rules

### 7.1 CABAC state

CABAC is an arithmetic coder with a single evolving cursor and 460 active contexts:
`i_low`, `i_range`, `i_queue`, `i_bytes_outstanding`, `state[1024]`, `p_start/p/p_end`
(`src/phx_cabac.h:18-26`). `phx_cabac_encode_decision` updates both the context state and
the range/low (`src/phx_cabac.c:120-134`), bypass shifts the low and queue (`:138-149`),
and flush consumes `frame & 31` (`:218`, x264 `reference/x264/common/cabac.c:174`).

Parallelising MB entropy would require: (a) a deterministic per-MB prefix of context
states, (b) a prefix sum of byte positions, and (c) exact tie-break preservation across
renormalisation and outstanding `0xFF` bytes. None of that exists in the CPU port, and
introducing it would change the emitted bins unless the sequential order is reproduced
bit-for-bit. Therefore: **one owner, slice start reset, scan order, no exceptions**.

Reset sequence (from `src/phx_encoder.c:1367-1373`):

```
phx_cabac_context_init(cb, slice_type, qp, i_cabac_init_idc)
phx_cabac_encode_init(cb, rbsp + header_bits, rbsp_end)
for each MB in scan order: terminal (except first) → header → CBP → qp_delta → residual
phx_cabac_encode_flush(cb, frame)   // frame = h->i_frame, not display frame
```

### 7.2 Lookahead `last_nonb`

`last_nonb` is the boundary between decided and undecided frames in the lookahead window
(`reference/x264/encoder/lookahead.c:59`; ownership documented at `src/phx_lookahead.h:22-25`).
Frame/path costs are computed for triples `p0 <= b <= p1` relative to that boundary
(`src/phx_lowres.h:66-77`), scenecut flags accumulate per frame
(`src/phx_lookahead.h:10-13`, `reference/x264/encoder/slicetype.c:1384/1430`), and
`slicetype_decide` assigns types plus BREF in coded order (`:1745`; BREF insertion
`:1892-1895`, reorder `:1950-1964`). Evaluating frames in parallel changes which frames
are references, changes the B/BREF pattern, and therefore changes **slice headers and
CABAC context initialisation** for every later frame. This is a bitstream change, not a
performance optimisation.

### 7.3 `frame_num` and POC

`frame_num` is advanced only for reference pictures
(`src/phx_encoder.c:772-777`, x264 `encoder.c:3882`); IDR resets it and sets the POC anchor
(`:707-710`); POC is `2*(display_frame - last_idr)` (`:717`). These values are written into
the slice header (`phx_slice.h:90, 93`) and feed ref lists/MMCO (`phx_slice.h:99-104`,
`src/phx_encoder.c:757`). Two more subtle dependencies:

- `i_nal_ref_idc` is derived from the decided type, including BREF (`:731-741`).
- The CABAC flush tie-break uses `frame & 31` (`src/phx_cabac.c:218`), where `frame` is the
  coded frame counter (`src/phx_encoder.c:1526`).

A parallel frame-type decision would therefore alter both header bytes and slice bytes.
It cannot be done without changing the bitstream.

### 7.4 Rate-control feedback

`phx_ratecontrol_start` consumes the decided slice type and planned bits before the slice
header (`src/phx_encoder.c:1335-1343`); `phx_ratecontrol_end` consumes the actual NAL size
after NAL assembly (`:1545-1546`). QP changes cascade into `mb.qp_delta` bins, trellis
coefficients and CAVLC token choices. Serial by construction
(`src/phx_ratecontrol.h:22-24, 99-110`).

### 7.5 Reset table

| Stage | Safe entry point | Required reset / recompute |
|---|---|---|
| Params, tables, cost tables, RDO tables | any time | re-run C config builders; table data immutable |
| Frame / lowres A stages | any frame boundary | `phx_frame_new` + `phx_frame_init_lowres` clears all caches (`src/phx_frame.c:107-135`); `phx_lowres_frame_reset` for mirrors (`phx_lowres.h:56-59`) |
| B neighbour cache | frame or slice start | zeroed `phx_mb_state_t` (`src/phx_encoder.c:794-816`), `first_mb`/`threadslice_start` bound the wavefront (`phx_mbcache.h:54-55`) |
| Lookahead C | lookahead-window boundary | re-prime `i_lookahead` frames (default 40, `phx_param.h:70`), restore `last_nonb` / `i_last_keyframe` |
| CABAC C | slice start | `context_init` + `encode_init_core`/`encode_init`; supply `i_frame` for flush |
| CAVLC / slice header C | slice start, then MB in scan order | `phx_cavlc_mb_init`, `last_qp`, `skip_run`; never resume mid-MB |
| RBSP / NAL C | NAL start | `phx_bs_init`; `nal_escape` needs two preceding bytes (`src/phx_bitstream.c:112-121`) |
| SSBO 3 | same frame as entropy | fully regenerated by S7; never cached across frames |
| ME / lowres A | any frame with valid fenc/fref | re-run S3/S4; no hidden search state (`phx_me.h:33-38`) |
| Size-only RDO accumulator | one MB workgroup | reseed `state[460]` + `f8_bits_encoded` (`phx_rdo.h:161-162`) |
| Trellis / rate control | MB in scan order / frame boundary | set UBO 2 before dispatch; RC `start`/`end` once per frame |

The rule: **A/B stages are seek-safe because their inputs are explicit and frame-local; C
stages are seek-safe only at their documented reset boundary.** This is why hidden globals
are forbidden (`docs/DATA_STRUCTURES.md:3-6`).

---

## 8. CPU function → shader mapping

The authoritative per-function register is `docs/FUNCTION_PORTS.md` (505 definitions).
The tables below group the functions that matter for the dispatch plan. `SHADER: A/B/C`
tags come from the source annotations; where a family mixes classes, both are shown.

### 8.1 Setup and A-stage kernels → `cs_cost_tables_init`, `cs_lowres_*`, `cs_me_fullpel`

| CPU function(s) | Source | Shader | Notes |
|---|---|---|---|
| `phx_param_*` (4) | `src/phx_param.c` | host only | C; UBO 0 |
| `phx_init_costs`, `phx_analyse_init_costs`, `phx_mb_analyse_init_qp`, `phx_mb_analyse_load_costs`, `phx_bs_size_te` | `src/phx_cost.c` | `cs_cost_tables_init` / host | A; SSBO 0; flatten centred pointers (`src/phx_cost.c:32`) |
| `phx_align_stride`, `phx_align_plane_size` | `src/phx_frame.c` | host | A data, computed before dispatch |
| `phx_frame_init_lowres_core` | `src/phx_frame.c:76-104` | `cs_lowres_core` | A; one 2x2 source block per invocation |
| `phx_plane_expand_border`, `phx_frame_expand_border_lowres`, `phx_frame_expand_border_chroma` | `src/phx_frame.c` | `cs_lowres_border` | A; NV12 pairs replicated as units (`phx_frame.h:71-73`) |
| `phx_frame_init_lowres` | `src/phx_frame.c:107-135` | S1+S2+S3 orchestration | A; cache clears at `:126-134` |
| `lr_sad8`, `lr_satd_8x4`, `lr_satd8`, `lr_bits_full`, `lr_bits_spel`, `lr_eval_full`, `lr_eval_spel_sad/_satd`, `lr_in_fpel_range`, `lr_init_me`, `lr_check_predictors`, `lr_fullpel_search`, `lr_refine_subpel`, `lr_motion_search`, `lr_try_bidir`, `lr_get_ref`, `lr_bidir_avg`, `lr_copy_fenc` | `src/phx_lowres.c`, `phx_lowres.h:30-31` | `cs_lowres_me` | A; one lowres 8x8 block per invocation |
| `phx_me16`, `phx_pixel_sad_16x16`, `phx_cost_mv*`, `phx_bits_mvd`, `phx_check_mv_range`, `phx_mv_cost_formula`, `phx_resolve_cost_mv` | `src/phx_me.c`, `phx_me.h:33-38` | `cs_me_fullpel` | A; no subpel/multi-ref/UMH (`:26-31`) |
| `phx_rdo_quant4/8_mf`, `*_bias0`, `*_unquant*`, `phx_rdo_size_unary`, `phx_rdo_transition_unary`, `phx_rdo_size_5ones`, `phx_rdo_transition_5ones`, `phx_slice_group_change_bits` | `docs/SHADER_NOTES.md:502-511` | UBO 1 lookup | A; immutable after `phx_rdo_init` |
| `phx_deblock_alpha/beta/tc0`, `phx_db_clip3` | `src/phx_deblock.h:46-51` | UBO 1 lookup | A |

### 8.2 B-stage kernels → `cs_mb_row` / `cs_deblock`

| CPU family | Source | Shader | Notes |
|---|---|---|---|
| `phx_macroblock_cache_load[_neighbours]`, `_save`, `phx_macroblock_backup_intra` | `src/phx_mbcache.c:75-515` | `cs_mb_row` shared cache | B; verbatim MBAFF tables |
| predictors (49), `phx_predict_8x8_filter/_fill` | `src/phx_predict.c`, `docs/SHADER_NOTES.md:252-260` | `cs_mb_row` inner per-block | A helpers; dispatch tables become UBO 1 mode indices; `edge[36]` is invocation-local (`SHADER_NOTES.md:184-185`) |
| DCT/IDCT/Hadamard (21) | `src/phx_dct.c` | `cs_mb_row` inner per-block | A; copy coefficients to registers before in-place IDCT (`SHADER_NOTES.md:257-258`) |
| quant/dequant/chroma-DC/denoise (19) | `src/phx_quant.c` | `cs_mb_row` inner per-block | A; `phx_quant_tables_init` is B setup |
| `phx_analysis_*`, `phx_search_i16/i8/i4`, `phx_macroblock_analyse[_slice]`, `phx_analysis_intra_chroma`, `phx_analysis_inter_p16x16` | `src/phx_analysis.h:39-99`, `docs/SHADER_NOTES.md:474-476` | `cs_mb_row` decision producer | B; writes SSBO 3 |
| `phx_mb_encode_*`, `phx_macroblock_encode[_ex]`, `phx_mb_store_block`, `phx_mb_mc_*` | `src/phx_mbencode.h:54-97` | `cs_mb_row` reconstruction | B; writes SSBO 2/3 |
| chroma DC/AC (`phx_chroma_*`, `phx_mb_encode_chroma_trellis`) | `src/phx_chroma.h`, `phx_mbencode.h:88-97` | `cs_mb_row` chroma slot writer | B; fills `chroma_dct`/`chroma_nnz` |
| `phx_mc_*` (hpel, luma, chroma, avg/copy) | `src/phx_mc.h` | `cs_mb_row` / reference prep | B/A; hpel planes uploaded as SSBO 1 views (`SHADER_NOTES.md:205-207`) |
| `phx_rdo_size_*` (11) | `src/phx_rdo.h:161-198` | `cs_mb_row` SSBO 7 | B; not a second coder |
| `phx_deblock_*`, `phx_frame_deblock[_ex]` | `src/phx_deblock.h:59-163` | `cs_deblock` | B; eight edges/MB; progressive only |
| `phx_macroblock_deblock_strength` | `src/phx_deblock.h:64-76` | `cs_deblock` strength pass | B; `bs[2][8][4]` |

### 8.3 C-stage functions that stay on the CPU (baseline)

| CPU family | Source | Why serial |
|---|---|---|
| `phx_lookahead_new/_delete/_put_frame/_get_frames[_ex/_full]`, `phx_lh_*` | `src/phx_lookahead.h:69-109` | queue + `last_nonb` (§7.2) |
| `lr_mb_cost`, `phx_lowres_frame_cost`, `phx_lowres_path_cost`, `phx_slicetype_frame_cost`, `lr_frame_clear`, `lr_get_frame`, `phx_lowres_*` lifecycle | `src/phx_lowres.h:52-85` | prefix sum + B discount (`slicetype.c:983-987`) |
| `phx_ratecontrol_*`, `rc_*` | `src/phx_ratecontrol.h:74-137` | frame history + feedback (§7.4) |
| `phx_cabac_context_init`, `phx_cabac_encode_init[_core]`, `phx_cabac_encode_decision/_bypass/_ue_bypass/_terminal/_flush`, `phx_cabac_putbyte`, `phx_cabac_encode_renorm` | `src/phx_cabac.h:28-40`, `src/phx_cabac.c` | arithmetic coder state (§7.1) |
| `phx_cabac_mb_*` / `phx_cabac_block_residual*` (30) | `src/phx_cabac_syntax.h:100-159` | bin order in one RBSP |
| `phx_cavlc_*` token writers (15) | `src/phx_cavlc.h` | single bit cursor |
| `phx_cavlc_mb_init/_predict_mv/_mvd/_8x8_mvd/_mb_header*` | `src/phx_cavlc_syntax.h:103-137` | MVD/header bit order |
| `phx_bs_*` writers, `phx_nal_escape`, `phx_nal_encode` | `src/phx_bitstream.h:34-53`, `src/phx_bitstream.c:104-140` | single output stream; 2-byte lookbehind |
| `phx_slice_header_init/_write`, `phx_slice_group_change_bits` (C host part) | `src/phx_slice.h:116-127` | slice syntax before MB data |
| `phx_sps_init/_write`, `phx_pps_init/_write`, `phx_encoder_headers`, `phx_sei_*` | `src/phx_headers.h`, `src/phx_sei.h` | parameter-set/SEI NAL assembly |
| `phx_rdo_quant_trellis_cabac/cavlc`, `phx_quant_*_trellis`, `phx_trellis_*` | `src/phx_rdo.h:99-118`, `src/phx_rdo.c` | coefficient state machine in scan order |
| `phx_encoder_*` envelope (open, frame context, MB loop, close) | `src/phx_encoder.c:1274-1573` | orchestration of all C stages |

### 8.4 Functions explicitly *not* mapped

These are absent from the CPU port and therefore absent from the GPU plan:

- MB-tree propagation (`encoder/slicetype.c:1091`) — `src/phx_lookahead.h:27-30` lists it as
  not ported; only the flag `b_mb_tree` exists (`phx_param.h:71`).
- Sub-pel / HPEL / QPEL ME refinement, chroma ME, weighted prediction, multi-ref, B-slice
  ME (`phx_me.h:26-31`).
- UMH/ESA/TESA search methods (`phx_param.h:13`, `phx_me.h:53-55`).
- I_PCM raw sample payload (`src/phx_encoder.c:1092-1101, 1194-1204` is a TODO).
- Non-flat CQM and QP > 51 trellis (`src/phx_rdo.h:213-214`).
- 8x8 CABAC trellis bit-exactness and 8x8 CAVLC split path (`src/phx_rdo.h:204-211`);
  `I_8x8` is rejected by the encoder (`src/phx_encoder.c:1419-1421`).
- MBAFF deblock wiring and cross-slice neighbour reload (`src/phx_deblock.h:25-33`).
- Multi-slice / sliced threads (`src/phx_slice.h:64`, `src/phx_encoder.c:750` sets
  `b_sliced_threads = 0`).
- Bitstream CLI / bitrate mode at 640x360 (`docs/PORTING_ROADMAP.md:128-132`).

---

## 9. Implementation phases and acceptance gates

Each phase is gated against the **CPU implementation first**, then against the shader.
The current baseline is documented at `docs/PORTING_ROADMAP.md:104-140`: 24/24 tests pass,
20/20 SPS+PPS byte-exact, MB0 CABAC bin trace 715/715 exact, 100/100 encoder robustness,
but **full-stream parity is 0/100** (`:112`, `:123-127`). That number is a divergence to
fix, not a success metric.

### P0 — Freeze the byte-exact CPU reference (gate before any shader)

- Record golden vectors for: cost tables, lowres planes and caches, ME results, one
  `phx_mb_decision_t` array per golden frame, deblocked reconstruction, CABAC bin trace and
  final Annex-B bytes. Use existing harnesses: `tools/parity_harness.py`,
  `tools/cabac_bin_diff.py`, `tools/compare_streams.py`, `tools/first_frame_probe.py`,
  `tests/compare_headers.py`.
- Gate: all registered tests green (`python tools/run_tests.py`, currently 24/24); golden
  dumps are deterministic across two runs and across a seek/re-start.
- Exit criterion: a named corpus and dump format that phases P1–P6 compare against.

### P1 — Layout and packing freeze

- Implement CPU-side pack/unpack for every struct in §5, including the 4448-byte decision
  stride, scan8/nnz canonicalization (§6.2) and chroma DC slots (§6.3).
- Gate: pack → unpack → `memcmp` against the original structs is byte-equal for every
  golden MB; the nnz inconsistency at `phx_mbencode.h:34` vs `phx_encoder.c:842` is resolved
  in favour of the serial reader and recorded in the test.
- Exit criterion: SSBO/UBO offsets are frozen; no shader may change a struct size.

### P2 — A setup and lowres shaders (`cs_cost_tables_init`, S1–S3)

- Gate: `cs_lowres_core` output planes byte-equal to `phx_frame_init_lowres_core` for all
  golden frames; border padding byte-equal to `phx_frame_expand_border_lowres`;
  `i_cost_est`/`i_row_satds[..][0]`/`lowres_mvs` equal to `src/phx_frame.c:126-134`;
  SSBO 0 tables byte-equal to `phx_analyse_init_costs`.
- Exit criterion: lookahead host code produces identical frame types using GPU-generated
  lowres data.

### P3 — A search shaders (`cs_lowres_me`, `cs_me_fullpel`)

- Gate: per-block `mv`, `sad`, `cost`, `ref` byte-equal to `lr_motion_search`/`phx_me16`
  for every golden candidate; error sentinel behaviour matches (`phx_me.h:50-51`).
- Any candidate-splitting optimisation must reproduce tie-breaking exactly; otherwise it
  is rejected.

### P4 — B MB-row shader (`cs_mb_row`)

- Gate: entire SSBO 3 dump byte-equal to the CPU `phx_mb_decision_t[]` for the golden
  corpus, including `dct[phx_scan8[k]]`, `nnz[k]`, `chroma_dct`, `chroma_nnz`, `mv`, `ref`,
  `i_qp` and fallback modes. SSBO 2 dump byte-equal to `phx_mb_state_t` after cache save.
- Gate: per-frame gate must pass for I, P and B slices separately; B slices are gated only
  after the lookahead decision matches (P2/P3).
- Exit criterion: the boundary of §6 is considered solid.

### P5 — Deblock shader (`cs_deblock`)

- Gate: reconstructed plane byte-equal to `phx_frame_deblock_ex` for every golden MB and
  configuration from `tests/test_phx_deblock.c`; strength bytes equal to
  `phx_macroblock_deblock_strength`.
- Integration gate: only enable it in the encoder when the CPU port also calls it;
  otherwise GPU stream parity would diverge from the CPU reference by construction
  (§14 item 6).

### P6 — Boundary readback + serial entropy integration

- Gate: SSBO 2/3 staging copies byte-equal to CPU structs; CABAC bin trace still 715/715
  on MB0 and extended to all golden MBs; header bytes remain 20/20 exact.
- Gate: NAL bytes and Annex-B stream byte-exact on the short corpus, then the extended
  corpus. Target from `docs/PORTING_ROADMAP.md:84-85, 112`: 120/120 short and 240/240
  extended cases, 0 NAL-count mismatches, full-stream parity greater than 0/100 and then
  100/100.
- Exit criterion: the shader backend produces byte-identical streams to the CPU port and
  to x264 for the golden corpus.

### P7 — Optional GPU single-invocation entropy (`S10`, `S11`)

- Only now port `phx_cabac_*` / `phx_bs_*` / `phx_nal_*` to a **single** compute invocation
  per slice/NAL. Gate: identical RBSP and NAL bytes to P6 for every case.
- **Do not** attempt multi-thread entropy: it would require prefix-state snapshots and
  changes the bitstream unless the sequential order is exactly preserved (§7.1). This
  phase is optional and is not required by the project goal.

### Gate discipline

A phase is not complete until its gate passes **and** all previous gates stay green;
metrics that "move a number" without golden evidence are forbidden
(`docs/PORTING_ROADMAP.md:1-5, 138-140`, `docs/DATA_STRUCTURES.md:83-90`).

---

## 10. Explicit unknowns and deferred items

1. **No Vulkan implementation exists.** No `.comp`/`.glsl`/`.spv` files and no Vulkan
   target in `CMakeLists.txt`. All entry-point names in §3 are proposed names, not existing
   code.
2. **`phx_mb_decision_t` drift.** `docs/SHADER_NOTES.md:141-159` describes a proposed
   struct (`i_type`, `mvd`, `coeff`, …) that does not match the real type at
   `src/phx_mb_decision.h:14-40`. This spec uses the real type. MVD is not stored; it is
   computed from `mv` minus the predictor by the serial writers
   (`phx_cabac_syntax.h:139-143`, `phx_cavlc_syntax.h:117-121`).
3. **nnz indexing conflict** between `src/phx_mbencode.h:34-35` and
   `src/phx_encoder.c:841-844`; resolved as §6.2 but must be proven by P4.
4. **`intra8x8_pred_mode[4]`** has no reader in the entropy stage; its exact semantics are
   **UNKNOWN**. Write zero and gate; do not invent behaviour.
5. **8x8 transform / trellis** is not bit-exact (`src/phx_rdo.h:204-211`) and `I_8x8` is
   rejected by the encoder (`src/phx_encoder.c:1419-1421`). The four-16-coefficient-slot
   layout in `phx_analysis.h:50-51` is therefore a reserved representation, not a verified
   path.
6. **Deblock is not called by `phx_encode()`** (verified: no call site in `src/phx_encoder.c`).
   The stage is specified because it exists in the CPU port, but enabling it changes the
   CPU baseline; sequence with P5 accordingly.
7. **Reference frames are input frames, not reconstructed DPB frames** in the current
   encoder (`src/phx_encoder.c:1346-1356`, TODO at `:1350`). GPU ME parity is limited by
   that; multi-frame reconstructed-reference plumbing is **UNKNOWN**.
8. **MBAFF** is partially ported in caches but disabled in the encoder; no field-pair
   dispatch is specified.
9. **Multi-slice / sliced threads** are not present (`src/phx_encoder.c:750`); S5/S7
   assume one slice covering all MBs.
10. **I_PCM** writes zero samples in the CPU port (`src/phx_encoder.c:1092-1101, 1194-1204`);
    no shader path is specified because there are no fenc samples in the decision.
11. **Rate-control internal state** is opaque (`src/phx_ratecontrol.h:54`); only outputs can
    cross to the GPU. VBV and two-pass are not ported (`src/phx_encoder.c:1272`).
12. **Frame types report 16/16 agreement but full streams 0/100**
    (`docs/PORTING_ROADMAP.md:110-112`); the remaining divergences (IDR slice payload byte
    76, stream bytes 35–39 at `:125-126`) are outside this spec and must be fixed in the
    CPU port before shader gates can be meaningful.
13. **8-bit/16-bit storage extensions** are deliberately not assumed. If a packed variant
    is adopted, its upload/download conversion needs its own gate.
14. **std140 padding** for UBO 1's `LeftTable` (§5.4) and for `PhxParam`'s mixed
    int/float fields must be verified with the compiler's reflection data; offsets in this
    document are the intended logical layout, not a substitute for reflection.
15. **Source line drift** in earlier docs (e.g. `docs/SHADER_NOTES.md:341` vs
    `src/phx_cabac.c:218`) is recorded; always cite the source file when implementing.

---

## 11. Citations index (derivation)

### 11.1 Project sources

- Class contract and boundary: `docs/SHADER_MAPPING.md:7-25, 27-38, 40-56, 58-80`.
- State ownership and forbidden globals: `docs/DATA_STRUCTURES.md:3-6, 24-42, 44-59, 77-81, 83-90`.
- Shader design precedent: `docs/SHADER_NOTES.md:11-19, 26-58, 69-232, 234-360`.
- Per-function classification: `docs/FUNCTION_PORTS.md:1-16` and module sections.
- Roadmap/gates: `docs/PORTING_ROADMAP.md:14-140`.
- Frame/plane layout: `src/phx_frame.h:16-49, 51-82`; `src/phx_frame.c:76-135`.
- MB state/cache: `src/phx_mbcache.h:17-105`; `src/phx_mbcache.c:9-31, 33-59, 75-96, 434-515`.
- Decision struct: `src/phx_mb_decision.h:1-42`.
- Cost tables: `src/phx_cost.h:21-64`; `src/phx_cost.c:26-93, 138`.
- CABAC: `src/phx_cabac.h:15-40`; `src/phx_cabac.c:120-218`.
- CABAC syntax: `src/phx_cabac_syntax.h:18-159`.
- CAVLC syntax: `src/phx_cavlc_syntax.h:39-137`.
- Bitstream/NAL: `src/phx_bitstream.h:20-53`; `src/phx_bitstream.c:104-140`.
- Slice header: `src/phx_slice.h:27-127`.
- Lookahead: `src/phx_lookahead.h:10-109`.
- Lowres: `src/phx_lowres.h:23-85`.
- ME: `src/phx_me.h:26-78`.
- RDO: `src/phx_rdo.h:23-31, 71-90, 136-198, 204-217`.
- Rate control: `src/phx_ratecontrol.h:22-24, 54, 62-137`.
- Analysis/encode/deblock: `src/phx_analysis.h:1-99`; `src/phx_mbencode.h:1-97`;
  `src/phx_deblock.h:1-163`.
- Encoder orchestration: `src/phx_encoder.c:695-778, 790-845, 858-1260, 1274-1553`.

### 11.2 Vendored x264

- `reference/x264/common/base.h:153-155, 180` — scan8 layout and sizes.
- `reference/x264/common/macroblock.c:855` (static `macroblock_cache_load`), `:1354/1359`
  (progressive/interlaced wrappers), `:1680` (`macroblock_cache_save`).
- `reference/x264/common/cabac.c:165-183` (`x264_cabac_encode_flush`; frame tie-break at
  `:174`).
- `reference/x264/common/bitstream.c:29-39` (`nal_escape_c`), `:55-102` (`nal_encode`).
- `reference/x264/encoder/lookahead.c:59` (`lookahead_update_last_nonb`).
- `reference/x264/encoder/slicetype.c:836` (frame cost), `:983-987` (B discount),
  `:1288-1327` (path cost), `:1384/1430` (scenecut), `:1473` (`slicetype_analyse`),
  `:1745` (`slicetype_decide`), `:1892-1895` (BREF), `:1950-1964` (reorder).
- `reference/x264/encoder/rdo.c:71-73` (`COPY_CABAC`: only `state[0..459]` and
  `f8_bits_encoded`).
- `reference/x264/encoder/encoder.c:3323` (`x264_encoder_encode`), `:3882` (frame_num
  advance for reference pictures).
- `reference/x264/common/mc.c:458/484` (lowres construction), `:473-481` (cache clears),
  `:221-249/252-283` (hpel/chroma MC).
- `reference/x264/encoder/me.c:60-107, 182-419` (search), `:774-963` (subpel, not ported).

---

## 12. One-page implementation checklist

1. `[ ]` Freeze CPU goldens (P0) and confirm the 0/100 stream divergence is tracked separately.
2. `[ ]` Implement and byte-verify struct packing (P1), including the 4448-byte decision stride (SSBO 3).
3. `[ ]` Implement S0–S3 and prove lowres parity (P2).
4. `[ ]` Implement S4/S6 and prove MV/cost parity (P3).
5. `[ ]` Implement S7 (`cs_mb_row`) and dump SSBO 2 + SSBO 3 for byte comparison (P4).
6. `[ ]` Implement S8 only after the CPU deblock wiring decision (P5).
7. `[ ]` Add S9 readback; re-run all existing tests and CABAC bin trace (P6).
8. `[ ]` Only then consider S10/S11 (P7); never parallelise entropy.
9. `[ ]` Re-run `tools/gen_function_inventory.py` and keep every `SHADER: A|B|C` annotation
       intact (`docs/SHADER_MAPPING.md:72-80`).
