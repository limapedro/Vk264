# Next: fix MB6's left edge sample (67 vs 69)

Encode-pass dumps show x264 MB6 idx0 filtered `edge[14]=69` while the
oracle's `l8[0]=67`. x264's edge[14] = F2(lt, l0, l1) where l0,l1 are
MB5's reconstructed rightmost column at rows 16,17. Oracle's rbase has
67 there.

1. Dump x264's MB5 FDEC column 7 of idx3 (rows 0..7 of the block) with
   the unique call ID, and oracle's `rmb[(by+y)*16+bx+7]` for MB5 idx3.
2. Compare byte-for-byte; fix the residual/reconstruction difference.
3. Re-validate: `build/wholeframe_diff2.py` must show MB0-6 matching
   (MB6 I8 modes 2,2,5,2).
