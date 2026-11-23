# MB5 idx3 DDR dump is cross-pass — no change landed

Attempted to byte-compare x264's DDR prediction for MB5 idx3 against
the oracle's. x264's dump (pass-filtered frame 0 / I-slice) produced a
DDR prediction of `47,52,56,61,65,70,74,79` per row, while the oracle's
is `17,16,17,20,26,39,55,68`. The x264 values correspond to a bright edge
(~47-79), not MB5 idx3's own edge (all ~16 per the aligned XEDGE dump),
so the dump is again from a different `mb_analyse_intra` call.

This is the same cross-pass artifact that previously produced the false
1-LSB finding. Do not change DDR based on this dump. A unique per-call
identifier (not just frame/slice/mb/idx) is required before comparing
predictor outputs.

Current verified state (PR #238): MB0-5 all match; MB5 idx1 prediction
and reconstruction are byte-exact; NAL2 prefix 50/130; gates green;
audit 12/120; parity 2/10. MB6 remains the next divergence.
