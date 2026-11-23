# Unique per-call ID implemented — encode-pass dumps

`build/dump_with_callid.py` adds a global `vkd_call` counter incremented
per `x264_macroblock_analyse` call and tags every I8 dump, plus dumps the
CABAC writer's FDEC for the same MB. Results for frame 0:

```
XCID call=6 mb=5 idx=0 mode=2 pred0=16 edge15=16 edge14=16
XCID call=6 mb=5 idx=1 mode=5 pred0=25 (25,45,63,70,70,70,69,69)
XCID call=6 mb=5 idx=2 mode=2 pred0=16
XWR  mb=5 type=1 fdec0=16
XCID call=7 mb=6 idx=0 mode=2 pred0=69 edge15=69 edge14=69
XCID call=7 mb=6 idx=1 mode=2 pred0=69 edge15=69 edge14=69
XCID call=7 mb=6 idx=2 mode=5 pred0=69 edge15=69 edge14=70
XWR  mb=6 type=1 fdec0=69
```

Key findings:
1. The earlier cross-pass DDR value (47,52,56,...) is confirmed wrong;
   the real encode-pass MB6 prediction is flat 69 (edge all 69).
2. Oracle MB6 idx0 sees `l0=67` where x264's encode-pass edge[14]=69 —
   an edge-sample difference of 2, coming from MB5's reconstructed
   rightmost column. This is why the oracle's H-mode x3 cost is high and
   it picks I4 instead of I8.
3. MB5 idx1 prediction (25,45,63,...) and FDEC match the oracle exactly
   (from the earlier VR/recon fix).

No source change landed this turn; MB0-5 still match and NAL2 prefix is
50/130. Next: compare MB5's rightmost reconstructed column against
x264's FDEC at the exact encode-pass call and fix that sample.
