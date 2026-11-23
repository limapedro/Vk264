# Whole-frame per-MB differential harness — result

Built the harness from `WHOLE_FRAME_DIFF_NEXT.md`:

- `build/wholeframe_x264.py` patches x264's CABAC writer to dump, for
  every frame-0 MB: `type`, `i16`, `cbp_luma`, `cbp_chroma` and all 16
  intra4x4 cache modes (`XMB` lines).
- `build/wholeframe_oracle.py` instruments `cabac_enc.c` to emit the same
  `XMB` format from the oracle.
- `build/wholeframe_diff2.py` produces a per-MB comparison table.

## Baseline result (committed HEAD)

```
mb  x264 orcl  cbpL    cbpC   detail
 0    0    0   1/1     0/0    MATCH
 1    0    0   15/15   2/2    MATCH
 2    2    2   0/0     1/1    MATCH
 3    1    1   1/1     1/1    MATCH (I8 modes 9,9,2,1)
 4    1    1   1/1     0/0    MATCH (I8 modes 10,2,10,2)
 5    1    0   10/10   2/2    FIRST DIFF: type 1->0, modes 2,5,2,4 vs 2,2,2,2
 6    1    0   14/4    2/2    type 1->0
 10   1    0   13/15   2/2    type 1->0
 14   1    0   2/10    2/2    type 1->0
 15   1    0   4/4     2/2    type 1->0
```

So the first five MBs are fully byte-exact; the first divergence is MB5
choosing I4 instead of I8.

## Surgical class-4 attempt

Applied x264's filtered-edge x3+shortcut only for all-neighbour blocks
(MB5) while keeping edge-class baseline logic. MB5 then matched exactly
(type=1, modes 2,5,2,4, cbp 10/2), but MB4 idx3 and other blocks changed
because x264's class-4 shortcut also applies to within-MB blocks of
edge MBs. Divergence moved to #329 with no byte-prefix gain (49/130).
Reverted.

The harness is retained and gives a precise per-MB target list for the
next fix: MB5 (all-neighbour I4->I8) then MB6/10/14/15.
