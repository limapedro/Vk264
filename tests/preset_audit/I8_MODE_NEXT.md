# Next step: fix I8x8 mode decision cost

The I4 early-termination foundation is in place but cannot pay off until
the I8x8 cost is accurate. Measured on MB5 (frame 0, c004 clip):

```
x264:   i8=348  modes=2,5,2,4   (block1 VR=176, block3 DDR=131; blocks 0/2 ~0)
oracle: i8=2150 modes=8,4,1,1   (wrong modes -> high SAD)
```

Diagnostics:
- `build/dump_x264_i8cost.py` — per-MB i16/i8/i4 costs and i8 modes
- `build/dump_x264_i8blocks.py` — per-block x3/shortcut costs
- x264's I8 analysis uses `intra_sad_x3_8x8` over a **filtered** `edge[36]`
  buffer (`predict_8x8_filter`, `predict.c:627-675`), then a restricted
  candidate shortcut (`intra_analysis_shortcut`) with `-3*lambda` bias.

Attempting exact filtered-edge predictors in `vk_predict8` moved the
CABAC divergence earlier (#343 -> #303) because the oracle's
`vk_i8x8_decide` does not implement the x3 + shortcut path and tries all
12 modes; the combination produced worse modes for MB3. The correct port
must implement the filtered edge *and* x264's x3/shortcut decision path
together, then validate per-block costs against `X8C` dumps.
