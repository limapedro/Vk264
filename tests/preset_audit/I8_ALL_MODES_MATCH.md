# All I8 modes now match — with call-aligned x264 dumps

Added a shared `(frame, mb_xy, per-MB call counter)` identifier to every
x264 I8 dump (`build/dump_aligned_all.py`) and dumped the CABAC writer
state with the same key, removing all cross-pass ambiguity.

Aligned x264 encode-pass data for frame 0:

```
MB3 idx0: edge left=71, top=45,60,75,..., mode=9, FDEC=71
MB3 idx1: edge left=72, top=103,122,101,83,89,87,82,72, mode=9, FDEC=72
MB3 idx2: mode=2, FDEC=71 ; MB3 idx3: mode=1
MB5 idx0: mode=2 ; idx1: mode=5 ; idx2: mode=2 ; idx3: mode=4
```

Fixing the top-left `lt` for top-edge blocks to use the left neighbour
sample (x264's FDEC border is ~72, not 128) made the verbatim decision
path produce:

```
oracle MB3 modes: 9,9,2,1   = x264 EXACTLY
oracle MB5 modes: 2,5,2,4   = x264 EXACTLY
```

This is the first time all 8 I8 modes across both diagnostic MBs match.
However, the CABAC stream then diverged at #326 (I8 pred-mode bins)
instead of #343, with no NAL2 byte-prefix gain (still 49/130), so the
change was reverted. The pred-mode *context* derivation for I8
(`I8Lidx`/`I8Tidx` static tables) still differs from x264's cache-based
predictor — that is the next, narrowly-scoped item.
