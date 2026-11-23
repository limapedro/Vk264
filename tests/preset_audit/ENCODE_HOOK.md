# Instrument x264_macroblock_encode for I4 dumps

The analysis path (`mb_analyse_intra`) skips blocks after early
termination, so `x264_mb_encode_i4x4` there cannot dump every block.
The definitive encode path is `encoder/macroblock.c::x264_macroblock_encode`,
which calls `x264_mb_encode_i4x4(h, 0, i, i_qp, i_pred, b_predict=1)`
for every I4 sub-block. Patch there to dump, for frame 0 / MB1 idx1:

- `h->mb.i_intra4x4_pred_mode[x264_scan8[1]]`
- `p_dst[0..15]` (prediction after intra predictor)
- `p_src[0..15]`
- raw DCT DC after `sub4x4_dct`

Compare with the oracle's `vk_i4x4_decide` prediction/DCT for the same
block. This avoids the analysis-pass early-break problem.
