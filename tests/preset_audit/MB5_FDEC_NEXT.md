# Next: prediction-vs-residual split for MB5

The MB5 FDEC byte-compare (`MB5_FDEC_RESULT.md`) shows mode-correct but
sample-different reconstruction for non-flat blocks (idx1/idx3). Before
changing any quant/IDCT code, determine which stage diverges:

1. Pass-tag x264's `p_dst[0]` immediately before `x264_mb_encode_i8x8`
   for MB5 idx1 (frame 0, I-slice).
2. Dump oracle's `p8[0]` and raw DCT DC for MB5 idx1.
3. Compare. If predictions differ, fix the 8x8 VR predictor / edge
   filtering; if predictions match but recon differs, dump post-quant
   DCT and dequant for that block.

Validate any fix with `build/wholeframe_diff2.py` (MB0-5 must stay
matching, MB6 should become I8 2,2,5,2).
