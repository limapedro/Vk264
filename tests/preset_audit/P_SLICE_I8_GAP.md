# P-slice NAL3 byte-7 divergence: missing intra I4/I8 support

## Measurement
Full CABAC bin dump (decisions + bypass + terminal + ue_bypass) for
the superfast warzone frame-1 P slice, after the P-slice header fixes:

```
x264   first bins: D11=0 D14=0 D15=0 D16=0 D40=0 D47=0 D73=0
                   D74=1 D75=0 D74=1 D77=1 D81=0 D399=0 D60=0 ...
oracle first bins: D11=0 D14=0 D15=0 D16=0 D40=0 D47=0 D73=0
                   D74=1 D75=0 D74=1 D77=1 D81=0 D60=0 D93=1 ...
```
The sequences are identical for 12 bins, then x264 emits
`transform_size` (ctx 399) where the oracle goes straight to
`qp_delta` (ctx 60).

## Cause
x264 frame 1 MB types (superfast, c004):
```
mb0 type=4 (P_L0), mb1 type=1 (I_8x8, transform_8x8=1)
```
The oracle's P-slice intra branch in `vk_cabac_encode_p_recon_frame`
(cabac_enc.c:1475) only implements **I16** macroblocks
(`vk_encode_i16_mb`). It has no I4/I8 intra coding for P slices, so
it cannot emit the transform_size bin or the I8 prediction/residual
syntax that x264 writes for MB1.

## Conclusion
This is a missing feature, not a small parameter mismatch: P-slice
I4/I8 intra coding (mb_type prefix, transform_size, 4 pred modes,
8x8 residual) must be ported from the already byte-exact I-slice
path into `vk_cabac_encode_p_recon_frame`. The header is now
byte-exact (bytes 0..6 match), so this is the next concrete task.

No source change this turn beyond the merged P-slice header fixes.
Gates green; audit 12/120; NAL2 pairs 3/20; recon 0/4096.
