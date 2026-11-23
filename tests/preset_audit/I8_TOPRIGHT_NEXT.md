# Next: x3/shortcut decision path for I8x8

All I8x8 inputs are now aligned with x264 (edge samples, VR/DDR
predictors, FDEC reconstruction for flat blocks). The remaining mismatch
is in `mb_analyse_intra`'s I8 decision control flow:

- x264 computes x3 costs (V/H/DC) on the filtered edge, applies the
  `-3*lambda` bias to the predicted mode only if `< 3`, then uses
  `intra_analysis_shortcut[cand9][favor_vertical]`.
- Our combined path with this logic regressed the stream, so the exact
  candidate/bias interaction still differs.

Next: dump per-candidate `i_best`, `favor_vertical` and the chosen mode
from x264 pass 6 (frame 0) for MB3 and MB5 all 4 blocks, and replicate
the loop verbatim — including the `i_best >= 0 || i_mbrd >= 2` loop guard
and the `i_cost > i_satd_thresh` early break. Do not change predictors.
