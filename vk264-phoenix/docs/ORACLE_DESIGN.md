# Phoenix oracle design — how the new oracle must work

## Principle

The Phoenix oracle is a **function-for-function CPU port** of x264. It is not
a reimplementation of the bitstream, and not a collection of heuristic
shortcuts. For every x264 function we port:

1. copy the signature and state semantics;
2. copy the table/constant it uses (from `reference/x264/`);
3. document inputs, outputs, mutated state, and shader classification;
4. prove equivalence with a small deterministic test before integrating.

## Module layout (target)

```
src/
  param.c/h          x264_param_t defaults + preset deltas + profile checks
  frame.c/h          frame allocation, lowres planes, frame caches
  cost.c/h           init_costs, mode-bit tables, lambda tables
  slicetype.c/h      frame cost, scenecut, b-adapt, MB-tree, lookahead state
  analysis.c/h       mb_analyse_intra/inter/chroma/transform RD
  predict.c/h        4x4/8x8/16x16/chroma predictors (incl. filtered edges)
  dct.c/h            DCT/Hadamard/IDCT paths
  quant.c/h          deadzone quant, trellis (CABAC + CAVLC QNS)
  mbcache.c/h        x264_scan8 layout, neighbour load/save
  mbencode.c/h       per-MB encode dispatch
  entropy.c/h        CABAC + CAVAC syntax and arithmetic coder
  slice.c/h          slice header, frame loop, NAL assembly
  ratecontrol.c/h    QP/CRF/AQ/MB-tree QP offsets
  oracle.c/h         stable public API used by the CLI and tests
```

Naming: keep x264's names where possible (`phx_mb_analyse_intra8x8`) so the
port can be diffed against `reference/x264/encoder/analyse.c`.

## Public API

```c
phx_t *phx_open(const phx_param_t *p);
int    phx_headers(phx_t *h, phx_nal_t **out);
int    phx_encode(phx_t *h, const phx_picture_t *pic,
                  phx_nal_t **out, int *nnal, uint8_t **recon);
void   phx_close(phx_t *h);
```

`phx_encode` must perform the stages in x264's order:

```
put_frame -> lookahead_get_frames (slicetype_decide) ->
ratecontrol_start -> slice_write -> macroblock_analyse ->
macroblock_encode -> macroblock_write_{cabac,cavlc} ->
ratecontrol_end -> NAL assembly
```

The CLI becomes a thin wrapper: read YUV, call `phx_encode`, write Annex-B.
No encoding logic in the CLI (the current oracle has ~400 lines of it).

## Determinism rules

- **Single-threaded order**: x264's CABAC state and lookahead `last_nonb` are
  sequential. Phoenix must not parallelize them until the CPU port is exact.
- **Fixed tables**: all quant/dequant/zigzag/VLC tables come from
  `reference/x264/common/tables.c`, generated into headers at build time.
- **No env-var feature switches** except explicit A/B experiments that are
  never landed (E45/E47/E48/E49 established the value-alignment rule).
- **State ownership**: every state field lives in `phx_t`, `phx_frame_t` or
  `phx_mb_t`. A function may only mutate what x264 mutates.

## Error detection during the port

For each ported function add a test that compares against x264's reference
build directly:

- pixel/DCT/quant/predict functions: random-vector unit tests with the same
  inputs, compare outputs bit-exactly;
- mode decisions: dump x264's per-block costs (like `build/i8_zigzag_probe.py`)
  and compare arrays, not just the chosen mode;
- CABAC/CAVLC: compare the bin/VLC stream (like `build/bincompare_faster.py`,
  but with **matched presets** — E87 showed a mismatched-preset harness
  misled us for several turns).

## Gates that protect the port

Keep the existing harnesses working while Phoenix grows:

```
build/audit_presets.py          short 120-case parity
build/extended_audit.py         240-case parity (the real target)
build/progress_metrics.py       first-diff bytes
build/recon_pair.py c004        reconstruction equality
build/compare_slicetype.py      16/16 frame types
build/cavlc_parity_sweep.py     1360/1360
ctest (both build trees)        29/29 + 65/65
```

A work package is not complete until all of these are green **and** its new
unit tests pass.
