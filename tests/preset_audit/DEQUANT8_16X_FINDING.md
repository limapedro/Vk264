# 8x8 dequant has a 16x scale discrepancy vs x264

Byte-compared x264 FDEC against oracle `rec8[]` for MB4 idx0/1/2:

```
x264 FDEC:   idx0=15, idx1=16, idx2=16
oracle rec8: idx0=16, idx1=16, idx2=16   (baseline)
```

Both have identical inputs: pred0=15, src0=16, DCT DC=64. So the
difference is in quant/dequant. Inspected x264 `common/set.c:157`:
`dequant8_mf[i][q] = def_dequant8[q][i] * scaling_list` with
`def_dequant8[q][i] = dequant8_scale[q][j]` — **no x16**. Our
`vk_ref_dequant8_idct_add` uses `dequant8_scale[q][j] * 16`, a 16x scale
error. `common/quant.c:130 dequant_8x8` confirms no hidden x16.

Changing the dequant to remove x16 made MB4 idx0 reconstruct to 15,
exactly matching x264. However, it also shifted the I8 mode decisions for
MB3/MB4/MB6 etc. (the mode cost depends on reconstruction), and the
whole-frame table got worse. No NAL2 byte-prefix gain (49/130), so the
change was reverted.

Conclusion: the dequant scale bug is real, but fixing it requires
simultaneously aligning the I8 mode-decision cost model, which currently
has compensating errors. Treat as a coupled fix, not an isolated one.
Gates green; audit 12/120; parity 2/10.
