# Next: resolve MB0 idx3 DC prediction rounding

Validated root: MB0 rightmost column pixels (15,3) and (15,4) are x264=14,
oracle=15; MB0 idx3/idx7 are I4 DC (mode 2). This is the earliest
reconstruction divergence and it propagates to all later MBs.

Plan:
1. Dump oracle's `l4[]`/`t4[]` and DC value for MB0 idx3 at the point of
   `vk_predict4` in `cabac_enc.c`, with the exact audit flags.
2. Dump x264's `x264_predict_4x4_dc_c` inputs for MB0 idx3 by patching
   `predict.c` (unmodified encoder, skip-intra already on), reading
   `SRC(-1,0..3)` and `SRC(0..3,-1)` directly at function entry.
3. Compare the two 8-sample sums and the `(sum+4)>>3` rounding. Fix the
   differing neighbour sample or rounding, then re-run
   `build/validate_recon_method.py` (target: diff count decreasing).
