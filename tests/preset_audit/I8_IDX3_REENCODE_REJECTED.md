# Rejected: re-encoding I8 idx3 in the oracle (with evidence)

## Hypothesis
x264's I8 analysis uses its SIMD `intra_mbcmp_x9_8x8` path. For idx3
the `i_cost > i_satd_thresh` / `idx == 3` break fires **before** the
`x264_mb_encode_i8x8` call (analyse.c:778), so idx3's reconstruction
comes from the final encode pass (`x264_macroblock_encode`), which
re-predicts with the mode-dependent neighbour mask
`x264_pred_i4x4_neighbors[i_mode]` (no TOPRIGHT for DDR/VR/HD, mask 11
for MB5 idx3 mode 4). The oracle instead reconstructs idx3 during
analysis with ALL_NEIGHBORS.

## Confirmed by instrumentation
- x264 final-encode prediction for MB5 idx3:
  `17,16,17,20,26,39,55,65,...`
- oracle analysis prediction (same block/mode 4):
  `17,16,17,20,26,39,55,68,...`
  Only the right-edge sample differs (65 vs 68); that is exactly the
  known 1-byte recon difference at (31,24) (x264 75, oracle 78).
- Zigzag coefficients for MB5 idx3 are byte-identical on both sides,
  so the divergence is purely in the prediction used for reconstruction.

## Tested variants (all rejected)
1. Re-encode idx3 into `rmb` (affects both output and neighbours):
   recon 355 -> 424/4096, NAL2 first diff 67 -> 64, MB5 byte-exact,
   but MB6's mode changed away from x264 (idx3 mode 2 -> 4), and
   MB7/15 got worse.
2. Keep analysis recon in `rmb` for neighbours, put re-encoded idx3
   only in the decoded-frame output: recon 418/4096, NAL2 prefix back
   to 67 — no net gain.
3. Both were also tested with/without the TOPRIGHT mask; the mask is
   correct (x264 mask = 11 for this block), so the mismatch is not
   the filter mask.

## Conclusion
The analysis-pass reconstruction of idx3 is what x264's following-MB
analysis actually observes (MB6 decisions matched only with it), and
it gives the lower total recon diff (355 vs 424). The re-encode was
reverted. The remaining MB6+ divergence is therefore **not** the idx3
re-predict; it is the earlier 1-LSB neighbour drift whose origin is
still unresolved.

No source change beyond comment cleanup. Gates: CTest 29/29 + 65/65,
CAVLC 1360/1360, MP4 56/56. Audit 12/120, NAL2 prefix 67, recon 355.
