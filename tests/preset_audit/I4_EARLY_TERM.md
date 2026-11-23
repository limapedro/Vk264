# I4x4 early termination and analysis order (foundation)

## Evidence

Pass-aligned x264 cost dump (`build/dump_x264_i8cost.py`) for the 64x64
c004 clip shows `i_satd_i4x4 == COST_MAX` for MB2+, e.g.:

```
mb=5 type=1 i4=268435456 i8=348 i16=2559
```

x264 aborts I4x4 analysis early when the accumulated cost exceeds
`i_satd_thresh = MIN(i16, i8)` (`analyse.c:868`, `897`, `:951`), leaving
`i_satd_i4x4` at COST_MAX so I4 is not selected.

## Changes

- `vk_i4x4_decide()` takes a `thresh` argument; if `total > thresh`
  before block 15, it returns `VK_I4_COST_MAX`.
- Analysis order changed to x264's order: I16 → I8 → I4, so the I8 cost
  is available as the I4 early-termination threshold.
- Final type selection compares I4 / I8 / I16 as x264 does.

## Result

No stream change on the current clip: the oracle still computes an I8
cost (2150) far from x264's (348) for MB5, so I4 (470) wins and the
CABAC divergence remains at #343. The exact 8x8 predictors with filtered
edges were attempted but moved the divergence *earlier* (#303) and were
reverted. The real remaining issue is the I8x8 mode decision/cost.

Gates unchanged: 29/29, 65/65, 1360/1360, 56/56, slicetype 13/16;
audit 12/120; superfast NAL2 prefix still 49/130.
