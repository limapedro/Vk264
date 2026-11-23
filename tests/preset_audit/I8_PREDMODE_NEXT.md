# Next: I8 CABAC pred-mode context derivation

With all I8 modes matching (9,9,2,1 / 2,5,2,4), the CABAC stream diverges
at #326 on the intra pred-mode bins. x264 uses
`x264_mb_predict_intra4x4_mode(h, 4*idx)` reading
`h->mb.cache.intra4x4_pred_mode[x264_scan8[4*idx]-1]` (left) and `-8` (top),
where 8x8 modes were written by `x264_macroblock_cache_intra8x8_pred`.

Our `I8Lidx`/`I8Tidx` static tables assume a=2 when the neighbour is
outside the MB; x264 instead uses the adjacent MB's cached sub-block mode.
For MB3 idx3 x264's predictor is 1 while ours is 2.

1. Dump `h->mb.cache.intra4x4_pred_mode[x264_scan8[4*idx]-1/-8]` for MB3
   and MB5 from `macroblock_write_cabac_internal` (call-aligned).
2. Replace `I8Lidx`/`I8Tidx` with an actual 8x8-mode neighbour cache
   (`mb8mode[mw*mh][4]`) storing fixed modes per MB, plus left/top MB
   edge modes.
3. Re-run the stream comparison; with modes already correct this should
   move the first divergence past the I8 pred-mode region.
