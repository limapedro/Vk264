# Writer-pass MB6 dump

x264 CABAC writer (`macroblock_write_cabac_internal`) for frame-0 MB6:

```
WM6 leftcol = 69,70,71,71,71,70,67,66
WM6 fdec0   = 69,69,69,69,69,69,69,69 (first 16 samples all 69)
```

The left column matches the earlier analysis-pass X6E edge, so that edge
was correct. The oracle's MB6 `l8[0]=67` differs from x264's 69 at the
first sample (and the columns differ further down: 66 vs 67).

However, the same writer dump for MB5 showed FDEC all 16, which is
inconsistent with MB6 reading 69 from MB5's rightmost column — the
per-MB FDEC pointer is rebased/scratch-cleared between MBs, so a single
writer snapshot cannot be used as the neighbour source. To locate the
real divergence we need the oracle's and x264's full reconstructed
frame (not per-MB FDEC) at the end of frame 0.

No source change landed. Verified state preserved: MB0-5 match, NAL2
prefix 50/130, all gates green, audit 12/120, parity 2/10.
