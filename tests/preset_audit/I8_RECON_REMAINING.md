# Remaining I8 reconstruction divergence (after PR #249)

Correct metric: `x264 --no-deblock` with `qp_i = qp_p - 3`
(x264 CQP's default I-frame QP offset). `--deblock 0:0` does NOT
disable the in-loop filter and produced a misleading 1460-byte
"root cause" that was actually a deblocking artifact.

Current baseline (frame 0 of `c004.yuv`, 64x64):
```
ultrafast: recon diff 0/4096
superfast: recon diff 355/4096
per-MB: {5:1, 6:54, 10:153, 14:72, 15:75}
```
`wholeframe_diff2.py` (regenerated after the fix) shows decisions
matching through MB6; remaining decision diffs are MB10 idx2
(x264 mode 6 vs oracle 1) and MB15 idx1/idx3.

## Unambiguous dump for MB6 idx2 (both sides agree on inputs)
- I8 mode: 5 (VR) on both
- Left neighbour samples: x264 [75,64,46,31,22,17,13,13],
  oracle [78,64,46,31,22,17,13,13] (one 1-LSB difference from MB5)
- Top samples: x264 row of 69s, oracle row of 70s (1 LSB)
- x264 filtered edge: `edge[7..15]=13,13,14,17,23,33,47,62,70,69`

The predictions nevertheless differ in the placement of their
diagonal samples (x264's VR features are in rows 5-7, oracle's in
rows 0-4). Both are derived from mode 5 (VR, `2x-y` diagonals), but
x264 uses its SIMD `intra_sad_x9_8x8` path for interior MBs
(`predict_mode[8] >= 0`), whose predictor/edge handling differs in
detail from the scalar `predict_8x8_vr_c` the oracle models.

Pre-quant DCT for MB6 idx2:
```
x264:   -145,154,-77,23,23,-2,-12,2
oracle: -157,159,-75,26,23,-1,-10,4
```
This is consistent with the differing predictions, not with a
quant-table bug (quant tables and `QUANT_ONE` were verified to match
x264 exactly).

A tested variant that removed the `+3*lambda` I8 mode-bit cost
(x264's SIMD x9 path omits the scalar path's explicit term) made
things worse (recon 355 -> 770, MB2 regression), so it was reverted.

## Next
Model x264's `intra_sad_x9_8x8` predictor exactly (checkasm semantics:
it leaves the winning prediction in `p_dst`), or disable x264's x9
path in the oracle reference build with an explicit flag and port the
equiv, then recheck `check_recon_fix.py` (target < 355).
