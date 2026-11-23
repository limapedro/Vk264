# Next: byte-compare MB5 FDEC vs oracle rec8

MB5's intra modes/CBP match x264, but MB6's H-mode x3 cost differs
(x264 90 vs oracle 202), suggesting MB5's reconstructed samples fed into
MB6's edge cache are not byte-exact.

1. Extend `build/dump_x264_fullpipe2.py` to dump MB5 idx0-3 FDEC after
   `x264_mb_encode_i8x8`, filtered by frame 0 / I-slice.
2. Dump oracle `rec8[]` for MB5 idx0-3 (instrument `cabac_enc.c`).
3. Byte-compare pass-tagged values. If they differ, fix the 8x8 residual
   reconstruction before touching MB6's decision logic.
4. Re-validate with `build/wholeframe_diff2.py`: MB0-5 stay matching,
   MB6 becomes I8 modes 2,2,5,2.
