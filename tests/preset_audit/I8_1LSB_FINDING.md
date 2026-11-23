# I8 mismatch localised to 1-LSB reconstruction difference

Using the whole-frame harness and per-mode cost dumps:

- x264's I8 candidate data for MB4 idx3: `nb8=11, cand9=1, favor=0,
  x3=(V=0,H=0,DC=-12), chosen mode=2` (DC, raw cost ~3).
- Oracle's class-4 path for the same block sees `l0=17, lt=17` where
  x264's filtered edge has `edge[14]=16, edge[15]=16` — a 1-LSB
  reconstruction difference in MB4's earlier blocks (`rmb[]` 17 vs
  x264 FDEC 16). That flips the mode-bias comparison: oracle DC cost
  116 vs H cost 8, so it picks H/mode8 instead of DC/mode2.

This is why every class-4 decision patch fixes MB5 but breaks MB4 (and
vice versa): the decision logic is now correct, but the *input samples*
differ by 1 LSB for some MBs. Full parity requires byte-exact 8x8
reconstruction for all MBs (we verified MB3 idx0 is exact; MB4 is not).

Baseline whole-frame table (committed HEAD): MB0-4 match, MB5 is first
divergence (I4 vs x264 I8 2,5,2,4). The harness is committed at
`build/wholeframe_diff2.py` and remains the validation tool.
No byte-prefix gain (49/130); all changes reverted; gates green.

Next: byte-compare x264 FDEC and oracle `rec8[]` for MB4 idx0/idx1/idx2
(the blocks feeding idx3) and fix the 1-LSB DCT/quant/IDCT difference.
