# Reconstruction diagnostics summary

The systematic reconstruction pass is blocked on obtaining x264's
per-block I4 prediction for early MBs: both the analysis path (early
termination) and the encode path (skip-intra optimization) skip the
relevant block. This is a harness limitation, not a product bug.

What is verified:
- Full-frame luma diff 1460/4096 with deblocking on and off (real
  pre-deblock reconstruction difference).
- Earliest difference: pixel (22,0) src=68, x264=68, oracle=67.
- Oracle MB1 idx1 DC_LEFT prediction = 67 while source = 68, so the
  +1 residual is quantized away; x264 reconstructs the source exactly.
- 4x4 IDCT, quant tables (mf=745, bias=29, dequant=352 at qp25) match
  x264 source exactly.

The likely remaining primitive difference is in 4x4 intra prediction
neighbour selection/filtering for DC_LEFT (or in quant bias), but a
reliable per-block dump requires disabling skip-intra for diagnosis.

All verified gains preserved; no user input required.
