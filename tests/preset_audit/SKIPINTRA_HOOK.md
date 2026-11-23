# Diagnostic hook: disable skip-intra to dump I4 predictions

To obtain x264's per-block I4 prediction for MB1 idx1, patch
`encoder/macroblock.c::x264_macroblock_encode` to set
`h->mb.i_skip_intra = 0` before the intra encode loop for frame 0, then
dump `p_dst[0..15]` and `i_mode` at each `x264_mb_encode_i4x4` call.
Also patch `mb_analyse_intra` to disable `b_early_terminate` so all
16 blocks are analysed.

Caveat: these changes alter encoder state, so dumped values are
diagnostic only (prediction/quant formulas remain valid, but the exact
bitstream changes). Use them only to compare the prediction primitive,
not as byte-exact stream values.
