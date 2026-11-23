# Next: compare MB5 idx1 prediction before residual

MB5 idx1 x264 FDEC first sample 19 (= source), oracle 18. Determine
whether prediction or residual is at fault:

1. Dump x264's `p_dst[0]` (prediction) before `x264_mb_encode_i8x8`
   for MB5 idx1, frame 0, I-slice, pass-tagged.
2. Dump oracle's `p8[0]` and raw DCT DC for MB5 idx1.
3. If predictions differ, fix `predict_8x8_vr` / filtered-edge inputs;
   if predictions match but recon differs, dump quant/dequant for
   MB5 idx1 (non-zero residual path) and fix that rounding.
4. Re-validate: `wholeframe_diff2.py` must show MB0-6 matching.
