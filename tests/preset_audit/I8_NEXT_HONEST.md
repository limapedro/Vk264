# Honest assessment of I8x8 work

Across many turns, every hypothesised I8 defect has been tested and
ruled out with pass-aligned, call-aligned dumps:

- VR/DDR predictors: DDR SAD 1002 exact; VR 189 vs 176 (bias scale). Correct.
- Edge inputs (t8 +bx, tr8 top): fixed and verified against x264 edge.
- 8x8 reconstruction (MB3 idx0): byte-exact (all 72).
- All 8 I8 modes: matched exactly with the verbatim path.
- Pred-mode cache: all x264 preds are 2; our tables already yield 2.

Yet the overall CABAC stream never improved past byte 49; every
re-landing of the verbatim path moved the first divergence earlier
without a byte-prefix gain. This strongly suggests the remaining
difference is not in I8 mode analysis at all, but in a later CABAC
symbol-stream detail (e.g. CBP/residual bin order for I8 MBs) that is
masked by the divergent I8 decisions.

Recommendation: stop porting analysis primitives. Instead, take a
byte-exact I8 MB (e.g. MB3 once the verbatim path is landed) and diff its
CABAC decision stream against x264 from the first bin, fixing residual
coding order/contexts — the same differential method that worked for I4.
