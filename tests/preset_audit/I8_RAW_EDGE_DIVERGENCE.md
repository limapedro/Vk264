# I8 raw edge divergence inside MB6 (new evidence)

## Measurement
Dumped the raw left/top FDEC samples used for MB6 idx3's I8
prediction on both encoders, with identical mode decisions:

```
x264   raw left = 65,68,70,70,69,68,71,75   lt=68   top = 70*8
oracle raw left = 63,67,71,71,69,67,67,69   lt=69   top = 70*8
```

These left samples are MB6 idx2's rightmost reconstructed column,
so MB6 idx2's **reconstruction** differs by up to 6 LSB although its
chosen I8 mode (5) matches x264. The resulting filtered edges also
differ (`x264 edge[7..14]=74,74,71,69,69,70,70,68,67` vs
`oracle edge[7..14]=69,69,68,68,69,71,70,67,66`).

## Consequences measured
- x264's x9 costs for MB6 idx3: `81,113,69,81,79,74,304,81,399`.
- Oracle SAD costs (clean baseline): `185,81,65,2145,73,73,1048,1247,53`.
- x264's scratch build has `mbcmp[0] != satd[0]` for superfast
  (`subpel_refine=1`), so it uses the SAD x9 variant; the oracle's
  plain SAD is the right metric, but the differing reconstructed
  edges produce different costs.
- Replacing SAD with SATD or a hand-written 8x8 SA8D was tested and
  broke MB5/MB6 decisions; reverted.

## Conclusion
The drift is generated *inside* MB6's I8 reconstruction chain, not
only at the (31,24) neighbour. Each I8 block's residual reconstruction
is fed into the next block's prediction, so a small quant/dequant
difference compounds. Fixing this requires matching x264's exact
per-block reconstructed samples, which in turn requires the two
reconstruction states (analysis vs final) and the exact SAD-x9
prediction path — a structural change not yet landed.

No source change. Gates green: CTest 29/29 + 65/65, CAVLC 1360/1360,
MP4 56/56. Audit 12/120, NAL2 67, recon 355/4096.
