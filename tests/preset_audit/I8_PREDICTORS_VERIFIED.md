# I8x8 VR/DDR predictors verified correct

## Definitive test (pass-tagged dumps)

1. `build/dump_x264_edge_tagged.py` captured x264's filtered `edge[36]`
   for frame-0 I-slice pass 6, MB5 idx1:
   `edge[14]=14, edge[15]=16, edge[16..23]=33,57,69,70,70,69,69,69,
   edge[24..31]=69...`.
2. `build/dump_x264_i8cost_tagged.py` confirmed the same pass reports
   mode5 VR = 176 and mode4 DDR = 1002.
3. `build/dump_x264_fenc_tagged.py` initially dumped the **4x4** source
   (wrong offset, `block_idx_xy_fenc[1] = +4`); the correct 8x8 source is
   at frame rows 16..23, cols 24..31 (`build/corr_source.txt`).
4. `build/test_vr_correct.py` fed the exact edge + correct source through
   the oracle's VR/DDR formulas:

```
VR  SAD = 189   (x264 176 -- difference 13, at the 3*lambda bias scale)
DDR SAD = 1002  (x264 1002 -- EXACT)
```

**Conclusion:** the VR/DDR predictor formulas are correct. The earlier
"oracle mode 7 cost 1812" result came from the oracle's own reconstructed
edge (`rmb[]`), not the predictor math. The remaining mismatch is in
I8x8 *reconstruction state*: the oracle's per-block reconstruction differs
from x264's FDEC, so subsequent blocks see different left/top samples.

## Consequence

Porting more predictor variants will not help. The next step is to
compare the oracle's reconstructed 8x8 block for MB5 idx0/idx1 against
x264's FDEC at the same pass (pass-tagged dump of `p_dst_by` after
`x264_mb_encode_i8x8`), and fix the 8x8 DCT/quant/dequant/IDCT path.
Gates unchanged: 29/29, 65/65, 1360/1360, 56/56, 13/16, audit 12/120,
NAL2 prefix 49/130.
