# Next: byte-compare MB4 FDEC to find the 1-LSB difference

1. Extend `build/dump_x264_mb3fdec.py` to dump MB4 idx0/idx1/idx2 FDEC
   after `x264_mb_encode_i8x8` with pass tags.
2. Dump oracle `rec8[]` for the same blocks (instrument
   `vk_ref_dequant8_idct_add` in `cabac_enc.c`).
3. Byte-compare. The mode decision for MB4 idx3 sees `l0=17` in the
   oracle vs 16 in x264 — find which sample in which earlier block
   differs and fix the 8x8 quant/dequant/IDCT rounding.
4. Validate with `build/wholeframe_diff2.py`: MB0-4 must stay matching
   while MB5 becomes I8 (2,5,2,4) from the class-4 path.
