# 8x8 reconstruction verified byte-exact (MB3 idx0)

Dumped x264 FDEC after `x264_mb_encode_i8x8` for frame-0 I-slice MB3 idx0
(mode 9, DC_LEFT) with pass tags (`build/dump_x264_mb3fdec.py`) and the
oracle's `rec8[]` for the same block.

```
x264 X3FD: all 64 samples = 72
oracle O3FD: all 64 samples = 72
=> byte-for-byte identical
```

So the 8x8 DCT / quant / dequant / IDCT path is correct. The earlier
hypothesis that 8x8 reconstruction caused MB3 idx1's mode difference is
ruled out.

The remaining MB3 idx1 question is a dump inconsistency: x264's XCHO
reported mode 9 with `best=-12` while the left neighbour is 72 and the
source is ~69-70 (DC_LEFT cost should be ~160, not ~3). This suggests the
XCHO cost dump and the FDEC/candidate dumps are not perfectly aligned.
Do not change reconstruction or predictors on this basis.

MB5 modes 2,5,2,4 already matched exactly with the verbatim decision loop
(`I8_DECISION_FINAL.md`); re-landing that loop is the next candidate once
the MB3 dump inconsistency is resolved.

Gates unchanged: 29/29, 65/65, 1360/1360, 56/56, 13/16, audit 12/120,
NAL2 prefix 49/130.
