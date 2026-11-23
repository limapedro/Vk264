# MB5 FDEC byte-compare result

Pass-tagged frame-0 I-slice dumps of MB5 (my=1, mx=1):

```
x264 X5FD idx=1 mode=5 rec=19,52,67,72,73,70,70,69,18,36,60,70,70,71,70,70
oracle O5FD idx=1 m=5   rec=18,50,68,67,72,74,68,74,11,36,66,71,69,70,62,69
x264 X5FD idx=3 mode=4 rec=18,17,17,20,28,43,63,75,17,18,16,17,22,30,47,64
oracle O5FD idx=3 m=4   rec=15,19,14,17,30,51,67,70,10,17,22,20,21,36,52,70
```

Modes match (5 and 4), but reconstructed samples differ by 1-12 LSB.
For idx1 the first sample is 19 in x264 (equal to source) vs 18 in the
oracle, so the 8x8 intra prediction/residual reconstruction for MB5 is
not byte-exact even though its mode and CBP decisions are.

This explains MB6: its H/DC x3 costs use left-edge samples taken from
MB5's reconstruction in `rmb[]`, which differ from x264's FDEC
(x264 MB6 idx0 H=90 vs oracle 202).

Earlier pass-tagged checks showed MB4 idx0 reconstruction matching
(x264 FDEC 16 == oracle rec8 16), so the discrepancy is specific to
non-flat 8x8 residuals (MB5 idx1/idx3 have nonzero coefficients).

No code change landed this turn; MB0-5 decisions still match and the
superfast NAL2 prefix remains 50/130. All gates green; audit 12/120.
