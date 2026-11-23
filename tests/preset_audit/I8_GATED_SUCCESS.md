# Gated I8 decision path — first byte-level gain

Gated x264's filtered-edge x3+shortcut I8 decision on the **MB-level**
condition `mx>0 && my>0` (all three neighbours at MB level), rather than
the per-block neighbour class. Edge MBs (MB3/MB4) keep the original
baseline all-candidate path; only MB5+ use the exact path.

`build/wholeframe_diff2.py` validation:

```
MB0-4: matching (unchanged)
MB5:   x264 I8 modes 2,5,2,4 cbpL=10 cbpC=2
       oracle I8 modes 2,5,2,4 cbpL=10 cbpC=2   <-- MATCH
```

This is the first time MB5 is byte-exact at the decision level.

Results:
- CABAC decision-stream first divergence: #343 -> #356 (later)
- superfast NAL2 first byte diff: 49 -> **50** (first byte-level gain
  since the I4 work)
- All gates green: CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56,
  slicetype 13/16; audit 12/120.

Next: MB6 is the new earliest differing MB (type 1->0, modes differ).
