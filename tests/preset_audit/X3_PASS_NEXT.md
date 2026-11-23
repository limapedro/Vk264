# Next step for the remaining MB1 mode mismatch

## Observed

With the gold `--frames 2` setup, x264's MB1 idx2 x3 costs are
V/H/DC = 992/548/892, while the oracle computes 290/82/146 from the
source and prediction samples that were verified identical in a separate
dump.

Direct dumps from instrumented x264 runs showed *different* fenc buffers
for the same `(mb_xy=1, idx=2)` depending on `--frames` (textured
`16,15,25,55,...` vs flat `16,16,16,15,...`). x264 calls
`mb_analyse_intra` more than once per MB (lookahead + encode), so the two
dumps were from different passes.

## Concrete next step

1. Add a pass identifier to the x264 trace: dump `h->sh.i_type`,
   `h->i_frame`, and a static call counter alongside `X4X3`/`X4MODE`.
2. Compare only the pass whose `(mb_xy, idx, fenc)` matches the oracle's
   frame-0 IDR MB1 input.
3. If costs then agree, port the remaining pass-specific behaviour;
   if not, compare the 16 prediction samples written by
   `intra_sad_x3_4x4` into `p_dst_by` (the macro at `common/pixel.c:542`)
   against `vk_predict4` modes V/H/DC.

## Reproduction tools (in build/, scratch)

- `dump_x264_x3.py`, `dump_x264_x3b.py` — x3 costs
- `dump_x264_x3in.py` — fenc/fdec inputs to x3
- `dump_x264_pred.py` — fdec neighbours at analysis entry
- `dump_x264_srcblock.py` — 16 source samples for MB1 idx2
