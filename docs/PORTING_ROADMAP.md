# Phoenix porting roadmap — ordered work packages

Each package has an input (what must exist first), an output (what is ported),
and an acceptance gate (how we prove it). A package is not done until its gate
passes **and** all existing gates stay green.

## WP0 — Branch + authoritative reference ✓

- Branch `vk264-phoenix`; copy `build/x264-ref-src/` → `reference/x264/`.
- Generate `docs/FUNCTION_INVENTORY.md` (1386 functions, mechanical).
- Write pipeline / data-structure / design / shader docs.
- Gate: PR merged; inventory reproducible via `tools/gen_function_inventory.py`.

## WP1 — Parameters, cost tables, frame plumbing ✓

Ported: `x264_param_default`, `param_apply_preset`, `x264_param_apply_profile`,
`init_costs`, `x264_analyse_init_costs`, `x264_analyse_free_costs`,
`mb_analyse_load_costs`, `mb_analyse_init_qp`, `bs_size_te`,
`align_stride`, `align_plane_size`, `frame_new`, `x264_frame_delete`,
`plane_expand_border`, `x264_frame_expand_border_lowres`,
`frame_init_lowres_core`, `x264_frame_init_lowres`. x264 tables are generated
mechanically by `tools/gen_tables.py`.

Gate: `python tools/run_tests.py` → 3/3 passing (10 presets, cost tables vs
x264 goldens, lowres planes vs independent filter computation). Remaining gate:
SPS/PPS byte-exact for all 20 preset×entropy cases once the encoder exists;
`tools/parity_harness.py` implements that comparison.

## WP2 — Neighbour caches and MB state (progressive path ✓)

Ported: `x264_scan8`, `x264_left_table_t`/`left_indices`,
`macroblock_cache_load_neighbours`, `macroblock_cache_load`,
`macroblock_cache_save`, `STORE_8x8_NNZ`, intra4x4 pred-mode and nnz caches,
ref/mv/mvd caches for both progressive and MBAFF paths (field-pair neighbour
selection, MAP_MVS, `b_allow_skip`, `i_neighbour_skip`) plus
`macroblock_backup_intra`. Still pending: `macroblock_store_pic` (reconstructed
pixel write-back) and the deblock-strength cache.

Gate: `python tools/run_tests.py` → 4/4 passing, including a load/save
round-trip over neighbour arrays (`tests/test_mbcache.c`) and the
bitstream/NAL writer (`tests/test_bitstream.c`) and the CABAC coder
against x264 golden bytes (`tests/test_cabac.c`). SPS+PPS is byte-exact 20/20 (`tests/compare_headers.py`). Byte-exact MB
decisions remain gated on WP4–WP7; the rebuild parity harness is described in
`docs/PARITY_HARNESS.md`.

## WP3 — Lookahead / slice-type state machine

Port: `x264_lookahead_put_frame`, `_get_frames`, `x264_slicetype_frame_cost`,
`scenecut_internal`, `scenecut`, `x264_slicetype_analyse`,
`x264_slicetype_decide`, `macroblock_tree`.

Gate: NAL-count mismatches 6 → 0 on the short audit and 120 → 0 on the
extended map; frame-type agreement stays 16/16.

## WP4 — Intra analysis (I4/I8/I16/chroma)

Port: `x264_mb_analyse_intra`, `mb_analyse_intra4x4/8x8/16x16`,
`mb_analyse_intra_chroma`, mode-availability tables, analysis shortcuts.

Gate: per-block cost arrays match x264 (not just chosen modes); veryfast
first-diff 84 → grows; chroma DC coefficient arrays match (E100).

## WP5 — Inter analysis and ME

Port: `x264_mb_analyse_inter_p16x16`, ME methods DIA/HEX/UMH/TESA, subpel
refinement, B-slice analysis.

Gate: all 10 presets' ME candidates match x264's dumped candidates; B-slice
NALs emitted where x264 emits them.

## WP6 — Predictors, DCT, quantization

Port: all predictor functions with filtered edges, DCT/Hadamard/IDCT,
`quant_4x4/8x8/2x2_dc`, trellis (CABAC + bounded CAVLC QNS).

Gate: pixel-level unit tests against x264; faster/fast CAVLC first-diff grow;
write-time trellis state matches.

## WP7 — Entropy and NAL output

Port: CABAC/CAVLC syntax, arithmetic coder, coefficient tokens, SPS/PPS/SEI,
emulation prevention.

Gate: **byte-exact** streams for all 120 short and 240 extended cases; 100%
NAL2 pairs; 0 NAL-count mismatches.

## WP8 — Rate control / CRF

Port: `x264_ratecontrol_start/end`, AQ, MB-tree QP offsets, CRF.

Gate: non-fixed-QP audits pass; then the 100% target covers real encode
settings, not just `--qp 28`.

## WP9 — Shader handoff

For every ported function, ensure the `SHADER: A/B/C` annotation exists and
`phx_mb_decision_t` is the explicit boundary between Class A/B and Class C.

Gate: a generated index of all functions with their shader class; a design
review document for the shader backend derived from it.

## Current state (measured)

| Gate | Measured result | Evidence |
|---|---|---|
| `ctest` (all registered tests) | **24/24 passing** | `python tools/run_tests.py` / CMake ctest |
| Function inventory | **273 PORTED of 1386 (19.7%)**; 1113 MISSING | `docs/FUNCTION_INVENTORY.md` |
| Header parity | **byte-exact 20/20** SPS+PPS; version SEI byte-exact | `python tests/compare_headers.py`, `tools/cabac_bin_diff.py` |
| MB0 CABAC bin trace | **715/715 bins exact**; first divergence bin 715 (MB1 mode) | `python tools/cabac_bin_diff.py --all` |
| Lookahead | B/BREF emitted from real lowres frame/path costs | `tests/test_phx_lookahead.c`, `tests/test_phx_lowres.c` |
| Encoder robustness | **100/100** cases `rc=0`, 0 crashes (frame_num/MMCO fix) | `python tools/parity_harness.py` |
| Decoder modules | CABAC arithmetic + CAVLC primitives + slice/MB driver ported, 3 new CTest targets | `src/phx_cabac_dec.*`, `src/phx_cavlc_dec.*`, `src/phx_decode.*` |
| Real-video encoding | 2 videos decoded to 640x360/60f, x264 6000 kbps 40/40 streams; Phoenix 640x360 opens and encodes all 10 presets | `build/video_encode_test/`, `build/video_parity/` |
| Full-stream parity | **byte-exact 0/100** ? NOT achieved | `build/parity/parity_report.json` |

- WP0?WP2 are complete: x264 reference, parameters/cost tables/frame plumbing,
  progressive + MBAFF neighbour caches, SPS/PPS/SEI, CABAC/CAVLC primitives.
  The parallel-worker batches added ME (`phx_me`), lowres slicetype costs
  (`phx_lowres`), trellis QNS + size-only RDO (`phx_rdo`), chroma DC/AC
  (`phx_chroma`), version SEI, B/BREF lookahead, deblocking (`phx_deblock`),
  CAVLC inter syntax (`phx_cavlc_syntax`), CRF/AQ rate control
  (`phx_ratecontrol`) and subpel/chroma MC (`phx_mc`). `src/*.c` defines 27
  modules; `docs/FUNCTION_PORTS.md` is the per-function register and
  `docs/SHADER_NOTES.md` the shader-stage/SSBO design.
- **Full-stream parity is not achieved.** 0/100 means zero of the 100 saved
  x264 streams are byte-identical; it is a divergence, not a success metric.
  First divergences: IDR slice payload byte 76 (MB1 intra4x4 mode decisions /
  analysis-vs-encode reconstruction), stream bytes 35?39 across the corpus.
  715/715 exact MB0 bins and 20/20 exact headers are necessary, not sufficient.
- Two real `D:\Videos` MP4s were decoded to 640x360/60f and encoded by x264 at
  `--bitrate 6000` for all 10 presets x CABAC/CAVLC: **40/40 streams** in
  `build/video_encode_test/`. Phoenix currently fails to open at 640x360 and
  has no bitrate CLI mode yet; on the 60-frame clips x264's achieved average is
  far below 6000 kbps because ABR has not converged. Both gaps are being worked.
- Historical baseline from the discarded incremental oracle: 12/120 short,
  0/240 extended, first-diff 84/30/41/42. Its harnesses are no longer in this
  repository; parity gates were rebuilt under `tests/` (`compare_headers.py`,
  `compare_streams.py`) and `tools/` (`parity_harness.py`, `first_frame_probe.py`,
  `cabac_bin_diff.py`, `nightly_gates.py`, `extended_video_probe.py`).
- Evidence-first rule: a work package is complete only with a golden test and
- All ten preset audits are in `docs/preset_audit/`; their synthesis is `docs/PARITY_GAP_MATRIX.md` (I4 modes, I16 luma DC, chroma-DC trellis, P/B headers, high-preset ME/RD).
  a byte-exact gate, never with a metric-driven approximation. Only the
  byte-exact count is a parity claim (`docs/NIGHTLY_STATUS.md`).
