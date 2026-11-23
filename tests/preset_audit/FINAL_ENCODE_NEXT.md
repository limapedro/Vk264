# Next: two-phase FDEC analysis/final-encode state

x264 uses distinct FDEC neighbour states for `mb_analyse_intra` (analysis)
and `x264_macroblock_encode` (final encode). The oracle uses one
reconstruction pass, so it matches analysis but not the final frame.

Plan:
1. Confirm by dumping x264's final-encode p_dst for MB1 idx1 (hook in
   `x264_macroblock_encode` with skip-intra forced off, frame 0).
2. If final prediction differs from 67, identify which neighbouring
   reconstructed sample changed and where x264 updates FDEC between
   analysis and final encode.
3. Mirror that FDEC update in the oracle's CABAC I4/I8 encoder so
   prediction residuals match at final encode.
4. Validate with `build/compare_recon.py` (diff count must decrease)
   and `build/wholeframe_diff2.py`, then audit and full gates.
