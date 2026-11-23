# Exact x264 4x4 predictors — the MB1 unlock

## Why MB1 modes diverged

The oracle's `vk_predict4` used simplified formulas for modes 4-8:

```c
case 4: out = F2(t[i],t[i+1],t[i+2]);   // top-only
case 5: out = F2(t[i],t[i+1],t[i+2]);   // top-only
...
```

x264's `common/predict.c` mixes the left edge, top-left corner and top edge
with F1/F2 filters. For example DDR (`predict_4x4_ddr_c`):

```c
SRC(3,0) = F2(t3,t2,t1);
SRC(0,0) = SRC(1,1) = SRC(2,2) = SRC(3,3) = F2(t0,lt,l0);
SRC(0,1) = SRC(1,2) = SRC(2,3) = F2(lt,l0,l1);
...
```

The oracle also forced the top-left corner to 128 for blocks on the MB's
left/top edge, but x264 reads it from the existing neighbour MB.

## Fix

Ported `predict_4x4_ddl/ddr/vr/hd/vl/hu_c` verbatim into `vk_predict4`,
including the `SRC(x,y) -> out[y*4+x]` index mapping. Corrected the
top-left neighbour for edge blocks.

## Result

MB1 raw modes now match exactly:
```
x264:   9,9,4,5,9,9,2,2,2,5,2,4,2,2,5,0
oracle: 9,9,4,5,9,9,2,2,2,5,2,4,2,2,5,0
```
Decision-stream first divergence: #53 -> #88. Superfast NAL2 first byte
diff: byte 5 -> byte 17.

## Note on the x3 SAD investigation

Earlier readings of x264 x3 SADs (992/548/892) came from the **8x8**
analysis loop: the instrumentation anchor was shared by both loops. With
a unique 4x4-loop anchor, x264 frame0 IDR MB1 idx2 gives V=290, H=82,
DC=134 (post-bias), exactly matching the oracle. The SAD path was correct.
