# Origin of the initial 1-LSB neighbour drift (new method)

## Method: FDEC neighbour snapshot at MB6 analysis entry
Instead of tracing individual encoder passes, I dumped the **entire
FDEC neighbour arrays** at the entry to MB6's intra analysis on both
encoders (x264 `mb_analyse_intra`, oracle `vk_cabac_encode_idr_recon_frame`).

```
x264   left = 69,70,71,71,71,70,67,66,75,64,46,31,22,17,13,13
oracle left = 69,70,71,71,71,70,67,66,78,64,46,31,22,17,13,13
top rows identical (all 69)
```

Exactly **one** sample differs: left[8] = 75 vs 78, i.e. reconstructed
pixel (31,24). Every other neighbour byte matches. All downstream MB
divergence (MB6 54 bytes, MB10 153, MB14 72, MB15 75) cascades from
this single sample.

## Origin of that sample
- MB5 idx3 zigzag coefficients are **byte-identical** on both sides
  (`1,-1,1,0,-1,0,0,1,0,...`).
- x264's I8 analysis breaks before encoding idx3 (SIMD x9 path,
  analyse.c:778). Its final encode re-predicts idx3 with the
  mode-dependent neighbour mask (`x264_pred_i4x4_neighbors[4] = 11`,
  no TOPRIGHT). That final prediction at the right edge is **65**.
- The oracle's analysis reconstruction uses ALL_NEIGHBORS and predicts
  **68** there. With identical quantised residual (+10 after dequant)
  this gives exactly 75 vs 78.

So the 1-LSB drift originates in **x264's final-encode re-prediction
of MB5's last 8x8 block**, not in prediction, DCT, quant or CABAC
primitives.

## Why mirroring it did not help
Three variants were tested: (a) re-encode into rmb, (b) re-encode into
the decoded-frame output only, (c) re-encode with x264's coefficients
regenerated. All made the total recon diff worse (355 -> 418/424) and
changed MB6's chosen I8 mode away from x264. Crucially, with variant
(a) the MB6 entry neighbours matched x264 **exactly**, yet MB6's mode
still differed — so x264's following-MB analysis observes the
analysis-pass reconstruction, while its decoded frame observes the
final-encode reconstruction. A single reconstruction buffer in the
oracle cannot satisfy both; the two passes must be separated.

A further difference was measured in the I8 mode decision itself:
x264's x9 costs for MB6 idx3 are `81,113,69,81,79,74,304,81,399`
while the oracle's SAD costs are `185,81,65,2145,73,73,1048,1247,53`.
x264's scratch build has `mbcmp[0] != satd[0]` (subme=1), so it uses
the SAD x9 variant; the differing cost arrays indicate the SIMD x9
prediction/cost path is not yet modelled exactly. Replacing the
oracle's SAD with SATD or a hand-written SA8D was tested and broke
MB5/MB6 decisions, so it was reverted.

## Conclusion
The origin is identified, but no safe fix landed this turn: the oracle
needs two reconstruction states (analysis-pass for following-MB
prediction, final-encode for the decoded frame), plus exact modelling
of x264's SAD-x9 cost path. Verified baseline unchanged:
CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56, audit 12/120,
NAL2 prefix 67, recon 355/4096.
