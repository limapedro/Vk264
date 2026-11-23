# Chroma CBP / zigzag / nnz borders — progress to byte 43

## Fixes

1. **I4 chroma CBP**: was hardcoded `d 77 0`. Now uses x264's
   `cabac_cbp_chroma` (77+ctx, 81+ctx2) and packs chroma CBP in `mbcbp`
   bits 4-5 as x264's `(i_cbp_chroma << 4) | i_cbp_luma`.
2. **nnz cache borders**: I4 CBF contexts now read left/top neighbour MB
   nnz values from the previous MB instead of a blanket 0x80.
3. **Zigzag**: I4 coefficients are scanned with `ZIGZAG4_FRAME`
   `{0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15}` before CABAC coding.
4. **Top-left corner**: `lt4` in the CABAC residual loop now reads the
   existing left/top neighbour MB (x264 `SRC(-1,-1)`) rather than 128.
5. **MB type contexts**: real `mbtype_i4[]` tracking replaces the CBP
   proxy for I4/I16 mb_type context derivation.

## Measured (superfast, c004.yuv)

```
CABAC first divergence:  #138 -> #297
NAL2 first byte diff:    17 -> 43
```

The first 43 bytes of the superfast IDR slice are now byte-identical to
x264 (previously 17). The next divergence is `transform_size=1` (I8x8),
which the oracle does not implement yet.

Gates all green; audit still 12/120.
