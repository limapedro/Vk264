# P-slice I8 port, second attempt: exact bin differential

## Method
With correctly tagged bin dumps (x264's MB tag set at
`x264_macroblock_cache_load_progressive`, before the skip flag),
x264 frame-1 MB1 (I_8x8 in a P slice) produces:

```
T, D12=0, D14=1, D17=0, D399=1,
D68=1, D68=1, D68=0 D69=0 D69=0 D69=0, D68=1,
D64=0, D73=1, D73=0, D73=0, D76=0, D78=1, D81=0, D60=0,
D402=1, D417=1, D427=0, B0, ...
```

The P-slice I8 port reproduced the header exactly through
`transform_size` and the first three pred modes:

```
T, D12=0, D14=1, D17=0, D399=1,
D68=1, D68=1, D68=0 D69=0 D69=0 D69=0,
D68=0 D69=1 D69=0 D69=0, ...
```

The first divergence is the **fourth 8x8 pred mode**: x264 chooses
mode 0 (V, matching the predicted mode → single `D68=1`), the
oracle chooses mode 2 (DC → `D68=0` plus three `D69` bins).

Mode dumps confirmed `x264 modes = 9,9,0,0` vs
`oracle modes = 9,9,0,2`; the predicted mode is 0 on both sides, so
this is a `vk_i8x8_decide_ex` cost-model difference (the oracle's
x3+shortcut path vs x264's SIMD x9 path), not a header syntax bug.

## Result
NAL3 stayed at byte 7 (length 57 vs 61), audit stayed 12/120,
and 8f PSNR worsened 21.71 -> 20.75. Per the task's requirement to
validate with the bin differential before keeping the change, it
was reverted.

## Next
The P-slice I8 port needs x264's exact x9 mode costs for the fourth
block; the header syntax itself is now verified correct.

Tree clean at 04d08f9. Gates green: CTest 29/29 + 65/65,
CAVLC 1360/1360, MP4 56/56. Audit 12/120, NAL2 3/20, recon 0/4096.
