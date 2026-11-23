# Note on x264 two-pass intra encode

x264's `mb_analyse_intra` writes a prediction into p_dst and quantizes
it to choose modes; later `x264_macroblock_encode` re-writes the
prediction and re-quantizes with `b_predict=1`, using FDEC neighbours
that have been updated by the final reconstruction of earlier MBs.

The oracle's `vk_i4x4_decide` does both jobs in one pass. To reach byte
parity the oracle needs to mirror x264's FDEC update between the analysis
and final encode phases — i.e. reconstruct each MB twice, with the
second pass using the final neighbour samples. This is a structural
change to the CABAC I-slice encoder, not a rounding fix.
