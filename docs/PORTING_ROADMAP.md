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
plus ref/mv/mvd caches for the progressive path. Interlaced (MBAFF) paths and
the deblock-strength cache are still pending.

Gate: `python tools/run_tests.py` → 4/4 passing, including a load/save
round-trip over neighbour arrays (`tests/test_mbcache.c`). Byte-exact MB
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

## Current state (for reviewers)

- WP0 complete; WP1 in progress (parameters ported and tested); WP2–WP9 pending.
- Historical baseline from the discarded incremental oracle: 12/120 short,
  0/240 extended, first-diff 84/30/41/42. Its harnesses are no longer in this
  repository; parity gates must be rebuilt under `tests/` per work package.
- Do not start a later WP before the earlier gate passes — the E70/E75/E90/E95
  history shows that partial ports create misleading metrics.
