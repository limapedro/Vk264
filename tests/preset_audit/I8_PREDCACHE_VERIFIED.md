# I8 CABAC pred-mode cache — verified already correct

Dumped `h->mb.cache.intra4x4_pred_mode[x264_scan8[4*idx]-1/-8]` and
`x264_mb_predict_intra4x4_mode` from `macroblock_write_cabac_internal`
for frame-0 I-slice I8 MBs (`build/dump_x264_predcache.py`):

```
MB3: idx0[a=2,b=-1,pred=2] idx1[a=9,b=-1,pred=2]
     idx2[a=2,b=9,pred=2] idx3[a=2,b=9,pred=2]
MB5: idx0[a=2,b=2,pred=2] idx1[a=2,b=5,pred=2]
     idx2[a=2,b=2,pred=2] idx3[a=2,b=5,pred=2]
```

`x264_mb_pred_mode4x4_fix()` maps 9/10/11 -> 2, so every predictor is 2.

Our `I8Lidx`/`I8Tidx` static tables with `vk_i4x4_fix()` produce exactly
the same pred=2 for all four blocks of both MBs (verified with the
`I8PRED` debug: pr=2 for k=0..3 in both MBs). Therefore the hypothesized
pred-mode-context bug does **not** exist — no real per-MB 8x8-mode cache
is needed; x264's cache happens to yield all-2 predictors here.

The earlier #326 divergence (pred-mode bins) is thus not explained by
pred context. With `vk_i4x4_fix()` already applied to modes 9/10/11 and
all preds equal to 2, the bins should match; the discrepancy is most
likely another artefact of comparing dumps from non-identical encoder
states. Do not change pred-mode coding on this basis.

Current committed state: no I8 changes, all gates green, audit 12/120,
NAL2 prefix 49/130, parity 2/10.
