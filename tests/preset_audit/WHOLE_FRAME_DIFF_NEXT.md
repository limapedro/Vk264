# Next: whole-frame per-MB differential harness

One-MB-at-a-time I8 fixes keep shifting divergences between MBs without
improving the NAL2 byte prefix (49/130). Build a single harness that, for
frame 0 of c004, compares for every MB:

1. MB type (I4/I8/I16) — x264 CABAC writer vs oracle `MBTYPE` debug.
2. I4/I8 intra modes — x264 `a->i_predict4x4[16]` / `a->i_predict8x8[4]`
   vs oracle `modes4[]` / `modes8[]`.
3. CBP luma/chroma and qp_delta bins.
4. Residual coefficient streams.

Emit a table `mb | x264_type | oracle_type | x264_modes | oracle_modes |
first_diff_bin` and fix the earliest differing MB completely (type +
modes + residual) before moving on. This avoids the whack-a-mole of
isolated symbol fixes.

Tools already available: `build/dump_all_mbtypes.py`,
`build/dump_aligned_all.py`, `build/dump_oracle_trace.py`,
`build/progress_metrics.py`.
