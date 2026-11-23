# I8x8 macroblock path

## What was added

- `vk_i8x8_decide()` gained a `use_satd` parameter; superfast (subme=1)
  uses SAD, matching x264's `encoder/encoder.c:1411`.
- The CABAC IDR encoder now analyses I8x8 alongside I4/I16 when the PPS
  `transform_8x8_mode` is set (superfast and faster presets), using the
  existing 8x8 DCT/quant/dequant/IDCT primitives from `i8x8.c`.
- CABAC I8x8 macroblock coding: I4-style mb_type, `transform_size` bin
  (ctx `399 + neighbour_8x8_transform_count`), four intra8x8 pred modes
  with predictors from adjacent 8x8 blocks, luma/chroma CBP, qp_delta,
  and 8x8 residual coefficients (category `DCT_LUMA_8x8`). Unlike 4x4,
  x264 emits no CBF bin for 8x8 intra residuals.
- Real `mbtype_i4[]` and `mbts8[]` neighbour arrays drive the mb_type and
  transform-size contexts.

## Measured (superfast, c004.yuv, 64x64)

```
CABAC first divergence:  #297 -> #343
NAL2 first byte diff:    byte 43 -> byte 49
```

x264 MB types for the frame match the oracle's decisions for MB0-MB3:
I4, I4, I16, I8.

Gates remain green; audit still 12/120 — full preset parity is not yet
achieved.
