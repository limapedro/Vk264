# wx264-vulkan → libx264 parity roadmap (2026-11-19)

Goal: make the Vulkan x264 port functionally equivalent to libx264 for the
presets wx264 accepts, starting with the ones whose x264 counterparts are
already "wired" (ultrafast…fast) and expanding the supported set as features
land. Inspired by the gap analysis in `docs/presets.md`.

This document is the working plan of record. Each phase lists the slice of
libx264 it ports, the files it touches, and its acceptance test. Phases are
ordered by (impact ÷ size) with hard dependencies on later phases marked.

## Ground rules

- The resident pipeline (whole-frame `cabac_frame`, single command buffer, no
  host round-trips between stages) is the design constraint. Host-side fixes
  that require per-macroblock readback are rejected unless isolated behind a
  fixture (like progressive CABAC tests) — they belong on GPU.
- Every phase lands with (a) a `ctest` case or parity fixture, (b) a 1080p60
  re-encode of the six-clip experiment with FFmpeg-verified reconstruction,
  (c) updated numbers in `docs/presets.md` + `results/encoded.md`.
- `harness/check.py --vulkan [--entropy cavlc]` and `ctest` stay green
  throughout; no phase may weaken byte-exact recon.
- The recon suite is the gate: byte-identical reconstruction vs FFmpeg is
  the ship criterion for every phase.

## Priority order (impact ÷ size, honouring dependencies)

| # | Phase | x264 surface | Size | Payoff | Depends on |
|---|-------|--------------|------|--------|------------|
| 1 | AQ (variance, mode 1) + per-MB QP delta plumbing | `x264_adaptive_quant_frame`, CABAC QP deltas | M (kernel L done) | High | — |
| 2 | 8×8 transform (luma inter + intra) + zigzag/scan wiring completion | `x264_8x8_dct`, `quant/dequant`, `zigzag_scan8` | M | High | — |
| 3 | Full sub-pixel refine (16×16/8×8) + chroma ME + psy_rd weighting | `me.c`, `pixel_subpel_*` | L | High | — |
| 4 | Trellis quantization (per-level RD with DP) | `x264_quant_trellis` | L | High | 2 |
| 5 | Lookahead + mb-tree (low-quality analysis pass → propagated QP deltas) | `x264 lookahead`, `mbtree` | L+ | High (RC quality) | 1 |
| 6 | B-frames (`b_pyramid=2`, adaptive fast `b-adapt=fast`, direct=temporal then spatial) | `encoder.c` reorder, `analyse_b` | XL | Very high | 3 (needs bipred MC), 5 (sensible with lookahead; can land b-adapt=none/P-only start) |
| 7 | Weighted prediction | `weightp`, `weightb` | M | Medium | 6 |
| 8 | Adaptive direct prediction (temporal + spatial fallback) | `macroblock_direct` | M | Medium | 6 |
| 9 | Expand accepted presets (veryslow/placebo feature gates) | `param_apply_preset` | S | — | 2,3,4,5,6,8 |

Phases 6 and 7 are intentionally split; B-frame support is the only phase
that restructures the encode loop (encode order ≠ display order, explicit
reordering, DPB with 2+ refs), so it is planned last among the big items.

## Non-goals (for now)

- Psy-RD / psy-trellis, grain, tuning (ssim/psnr) — x264 psychological
  defaults stay unmapped; wired presets keep `psy_rd=0` equivalent.
- Fast QP-aware multi-threaded CTU slicing / frame-slices — n/a on the
  resident pipeline.
- 10-bit and CQM — out of scope.
- Weighted bipred (`wB`) — veryslow/placebo only; not on the parity path.

## Harness impact per phase

| phase | harness addition |
|---|---|
| 1 | `--aq 0|1` already exists; extend with `--aq-strength` ; recon-gate stays; new fixture: flat/grid with AQ on, byte-exact recon |
| 2 | `--partitions 0|1` gains 8×8 inter path; new recon/PSNR check at RD with 8×8 on; preset table flips `inter_8x8_transform` to "active" |
| 3 | `--subpel` gains richer weights; new `--chroma-me 0|1` flag; PSNR-Y/U/V printed |
| 4 | `--trellis 0|1|2` flag; report deadzone behaviour; recon requires transform requantization divergence is still byte-exact |
| 5 | `--lookahead N` flag; RC reports mbtree deltas; summary.json gains `gpu_runs[].settings.lookahead/mbtree` |
| 6 | new `--bframes N`, `--b-pyramid 2`, `--b-adapt fast|none`; verify_stream checks I/B/P frame order (h264 `slice_type` via ffprobe `frame=pict_type`); recon must stay byte-exact with bipred |
| 7 | `--weightp 0|1` |
| 8 | `--direct auto|spatial|temporal` |
| 9 | flip `supported=true` per preset; re-run full preset sweep (Evals-03+) |

## Verification ladder (per phase)

1. `ctest --test-dir build -C Release --output-on-failure` — all pass, no
   regressions; new fixtures add coverage.
2. `harness/check.py --vulkan` / `--entropy cavlc` — PASS.
3. `--check-recon` on all 109 test-matrix clips (testsrc2, mandelbrot,
   4×NVENC game 240f) — byte-identical recon required.
4. Re-run the preset sweep; report must show no quality regressions at same
   settings; VMAF must not regress.

## Current state of this document

Skeleton replaced (2026-11-19) by verified facts from a full codebase pass.

### Phase 1 groundwork — DONE (this session)

`Wx264\vulkan\aq.comp` — fixture-capable variance kernel matching
`x264_adaptive_quant_frame(X264_AQ_AUTOVARIANCE)` in
`x264-vulkan/encoder/ratecontrol.c:304` exactly (luma var2 shift=8, chroma
shift=6, `energy = luma + cb + cr`). `verifyAqVariance()` in `dct_parity.cpp`
checks 1024 fixtures (flat/gradient/checkerboard/random) against a host
reference translated bit-for-bit from `common/pixel.c`'s `pixel_var_16x16` /
`ac_energy_var`. Registered as `x264_aq_parity`; ctest 46/46.

### Confirmed architecture (from reading the encode path, not the docs)

- QP is **per-slice only**: `slice.qp` (`dct_parity.cpp:5540`), CABAC ring
  entry via `encodeQpDelta(f, qp, qp, ...)` with **delta hard-wired to 0**
  at `cabac_syntax.glsl:201,270`. No per-MB QP path exists anywhere.
- `chromaQpMap[52]` is **host-only**, passed into the resident frame as a
  resolved scalar (`chromaQpMap[qp]` → `filterInput[3]`, `qc.qp`).
- Resident path (`encodeResidentCabacFrame`, `dct_parity.cpp:4675`) is a
  single command buffer: fixture staging → motion search (wavefront order)
  → per-512-MB `residual()` (bridge prepare → `intra16_frame` 4×4 →
  `inter8x8_frame` → `intra_chroma_frame` → CABAC-residual join) →
  `cabac_frame` → deblock → optional reconstruction readback.
- **8×8 inter transform is live** in the resident path (docs claiming
  otherwise were stale): SA8D-vs-SATD choice in `inter_frame.comp:78–99`,
  `inter8x8_frame.spv` dispatched only when `eight` is set at
  `dct_parity.cpp:4898–4902`. Intra 8×8 and chroma 8×8 are **not**.
- **AQ is in the legacy CAVLC backend only** (`Wx264/shaders/residual.comp:35–56`,
  heuristic `offset=clamp(round(1.25*(log2(max(1,var))-6)),-6,5)` — not x264's
  algorithm). Not wired into the CABAC path; `--aq 0|1` is rejected for
  `--backend vulkan` at `src/main.cpp:~111`.
- **Lookahead/mb-tree and B-frames do not exist as infrastructure**: single
  pass, display order, 2-deep modulo-2 DPB, `i_num_ref_frames=1`,
  `i_poc_type=2`, hardwired `l0/l1_num_ref_idx=1` (SPS), loop-carried
  reconstruction slot. B-frames need: POC type change, DPB N slots,
  `SLICE_TYPE_B` CABAC models (`cabac.comp` `initialContext(model,...)`
  keyed on `model` already supports `0/1` only), bipred MC wiring
  (`motion_bidir.comp` is a fixture, not wired), direct-prediction, GOP/RC
  keyed off `i%gop`, `b_pyramid` hierarchical structure.
- Trellis: only `[[noreturn]]` throw-stubs (`dct_parity.cpp:373,431–444`).
  AQ hooks for trellis RD would sit in `quant.comp`/`intra16.comp` cost path.

### Phase 1 integration plan (the actual plumbing)

1. Dispatch `aq.spv` inside the resident command buffer **before** motion
   search (source plane is already uploaded; outputs `mbStats` indexed by
   mb index → storage buffer in the arena).
2. One readback of `mbStats` (8160×4 B ≈ 32 KiB, one frame) + host loop to
   compute `f_qp_offset[mb]` exactly as x264 does (mean of
   `pow(energy+1, 0.125)`, mean of its square, `strength = aq_strength * avg_adj`,
   `avg_adj -= 0.5*(avg_adj_pow2-14)/avg_adj`, per-MB
   `strength*(qp_adj-avg_adj)`), or a second tiny 64-thread kernel.
   Upload `mbQp` (uint8 offsets or per-MB chroma-style QP pairs).
3. Read `mbQp[mb]` instead of the frame `qp` in every stage that caches
   QP per macroblock: residual transform/quant fixtures created per MB
   (intra16/inter8x8/intra_chroma kernel FFmpeg paths consume
   fixture-level `qp`), `motion_search.comp` stride words are per-frame
   QP-agnostic already (uses `computeLambda(qp)` — check), deblock
   strength (`deblock_frame.comp` edge alpha/beta per MB).
4. **CABAC**: `encodeQpDelta(f, mbQp, lastQp)` per macroblock with running
   `lastQp` state per slice (the `qpDelta` op exists; contexts/bins done);
   initial QP delta vs slice init comes out as the first non-zero delta.
   x264 writes `mb_qp_delta` as se(v) — wired `encodeQpDelta` handles this.
5. **RC feedback**: after the frame, feed the **frame-average** of mb QPs
   back into `rate_control.spv` (keep RC frame-level for now; per-MB QP
   deltas are AQ offsets on top of the RC-chosen frame QP). Note: true
   parity needs the RC to see the AQ-adjusted QP; acceptable approximation
   for phase 1, documented.
6. `qp_delta` bins are decoded by x264 anyway; this phase keeps
   byte-exact decode as the gate — FFmpeg must reconstruct.
7. CLI: drop `--aq` from the Vulkan rejection list in `main.cpp`; harness
   gains `--aq-strength`
   (default 1.0, x264-equivalent); `summary.json` gains `gpu_runs[].aq` truth.
8. Verify: ctest green, `harness/check.py --vulkan` recon byte-exact,
   sweep re-run on the 6 clips, VMAF must not regress.

Known risks/scope cuts:
- chroma AQ plane weighting (x264 adds chroma variances into the same MB
  energy; our kernel already does luma+Cb+Cr together — port on/off flag
  to skip chroma like high-motion-toollow-complexity cases).
- mb-tree is NOT required for AQ mode 1 (AQ is per-MB self-contained).
- Per-MB QP in CABAC changes bin counts; recon will only pass once
  CABAC correctly distinguishes mean base QP vs deltas — this is the long
  pole of phase 1. Budget: 2-4 kernels + syntax writer edits.

Estimate honestly: kernel work ≈ days (worked examples of this size before:
CABAC resident took weeks). Phases 5-6 (mb-tree, B-frames) remain larger.
