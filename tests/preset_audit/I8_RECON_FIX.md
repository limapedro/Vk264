# I8 reconstruction fix: filtered-edge predictor + VR index correction

Prediction-vs-residual split for MB5 idx1 (frame 0 I-slice) showed:

```
x264 prediction: 25,45,63,70,70,70,69,69,20,35,54,66,70,70,69,69
oracle (before): 69,69,69,69,69,69,69,69,71,71,69,69,69,69,69,69
```

Two real bugs fixed:
1. Reconstruction used the old approximate `vk_predict8`, while mode
   decision used the exact filtered-edge predictors. Added exported
   `vk_predict8_filtered()` and use it for reconstruction — gated on
   MB-level `mx>0 && my>0` so edge MB3/MB4 keep their verified baseline
   reconstruction.
2. The VR predictor's odd-diagonal samples were shifted by one: x264
   uses `F2(LT,t0,t1)` for d=1 and `F2(t0,t1,t2)` for d=3; our code used
   `F2(t0,t1,t2)` and `F2(t1,t2,t3)`. Fixed to `k=(d-1)/2`,
   `F2(k?ET(k-1):ELT, ET(k), ET(k+1))`.

Result: MB5 idx1 prediction and reconstruction now match x264
byte-for-byte. Whole-frame harness: MB0-5 still all match. The NAL2
prefix remains 50/130 and the stream divergence #356 (MB6 still differs)
because MB5 idx3 has one residual LSB off (78 vs 75 at sample 7), which
feeds MB6's edge. Gates green: 29/29, 65/65, 1360/1360, 56/56, 13/16,
audit 12/120.
