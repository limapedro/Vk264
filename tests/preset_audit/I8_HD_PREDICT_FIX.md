# Fixed: I8 HD predictor and predicted-mode bias

## Two real bugs in the oracle's I8 path

1. **HD (mode 6) predictor** was implemented as `d=2*y-x` with a
   `v=128` fallback. x264's `predict_8x8_hd_c` packs F1/F2 pairs along
   `d = x + 2*(7-y)`. Ported literally: the 22 packed pair values are
   indexed as `out[y*8+x] = pair[14-2*y+x]`. For MB10 idx2 this cost
   the oracle SAD 1904 where x264's x9 cost was 73; after the fix the
   mode matches (6) and MB10 reconstruction became byte-exact.

2. **Predicted-mode bias** was hardcoded to `2` (DC) in
   `vk_i8x8_decide`'s interior-MB shortcut. x264 discounts
   `x264_mb_predict_intra4x4_mode()`, which for MB15 idx1 is `0` (V)
   from the adjacent 8x8 mode. Added `pred8` to `vk_i8x8_decide_ex`,
   computed from the adjacent 8x8 modes with the same left/top rule
   x264 uses.

## Verified effect
```
c004 frame-0 luma recon:   277 -> 0 / 4096   (byte-exact!)
wholeframe_diff2:          all 16 MB decisions match
c004 IDR NAL2:             130/130 bytes = byte-exact
audit NAL2 byte-exact pairs: 2/20 -> 3/20  (superfast now exact)
audit full byte-exact:      12/120 (superfast warzone NAL2 now exact;
                            divergence moved to NAL3 P-slice byte 2)
CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56
```
Remaining superfast divergence is in P-slice coding (NAL3), not the
I-slice encoder.
