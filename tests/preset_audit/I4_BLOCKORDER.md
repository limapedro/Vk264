# I4 residual block ordering — the key CABAC fix

## Symptom

The superfast IDR NAL2 first diverged at byte 5 (oracle vs x264). After the
syntax fixes in PR #211 the divergence was at decision #20 (`cbp_luma` bit1).

## Root cause

x264 orders intra4x4 blocks in `block_idx_xy_fenc` (common/macroblock.h:225):

```
idx: (x,y) = (0,0),(4,0),(0,4),(4,4),(8,0),(12,0),(8,4),(12,4),
             (0,8),(4,8),(0,12),(4,12),(8,8),(12,8),(8,12),(12,12)
```

This is **quadrant-major**, not row-major. `vk_i4x4_decide()` already used
this order, but the residual loop in `cabac_enc.c` used `(i%4)*4,(i/4)*4`.
Result: mode 10 (DC_TOP) for decision index 2 was applied at (8,0) with no
top neighbour → prediction 128, instead of at (0,4) with a true top
neighbour → prediction ~16.

## Fixes required (all landed)

1. `I4XY[16][2]` table in the residual loop.
2. I4 luma uses `DCT_LUMA_4x4` (cat 2: CBF base 93, sig/last/level offsets
   134/195/248), not `DCT_LUMA_AC` (cat 1: 89/120/181/227).
3. CBF bin is the block's own nnz flag, not hardcoded 1.
4. CBF contexts use a 48-entry scan8 nnz cache; unavailable neighbours are
   0x80, within-MB blocks are plain 0/1.
5. `vk_i4x4_decide` candidate classes derive from `have_l/t/tl` rather
   than a fixed first-MB table.
6. x9 mode-bias is `3*lambda*(m!=pred)` (from `cost_i4x4_mode+8-pred`).

## Result

Gold x264 vs oracle decision stream: first divergence moved from #20 to
**#53**, with all of MB0 (including all 16 pred modes, CBP, qp_delta, CBF
and residual coefficient coding) matching exactly.

## Remaining

MB1's intra modes differ on textured content. SAD is required by the
`rewrite_i4x4_decide` fixture; x264's SATD path breaks it. A proper port
needs both the SATD cost and an updated fixture.
