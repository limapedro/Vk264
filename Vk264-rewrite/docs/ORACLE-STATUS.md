# Oracle status (evidence log)

## Verified byte-exact cases vs x264

All comparisons are Annex-B NAL-by-NAL (SPS, PPS, IDR slice), SEI skipped.
Reproduce with `ctest --test-dir Vk264-rewrite/build -C Release`.

| Case | Result | Source |
|---|---|---|
| 32 flat gray values (0..248 step 8) | 32/32 exact | `tests/test_flat_sweep.py` |
| 5 real 1080p 16x16 crops | 5/5 exact | `tests/test_real_crops.py` |
| 32x16 real crop (2 MBs) | slice 75/75 B exact | `tests/test_multimb.py` |
| 16x32 real crop (2 MBs) | slice 63/63 B exact | `tests/test_multimb.py` |
| 32x32 real crop (4 MBs) | slice 89/89 B exact | `tests/test_multimb.py` |

Gate snapshot (this commit):
```
ctest --test-dir build -C Release              -> 65/65 passed
ctest --test-dir Vk264-rewrite/build -C Release -> 10/10 passed
```

## Known gap

64x64 real crop (16 MBs) mismatches at slice byte ~180/195. x264's per-mode
cost dump (from the scratch probe build, NOT the read-only reference tree)
showed I16 mode divergences at MB 11 and MB 14, where x264 selects I16 mode 3
(P / plane prediction). Fix in progress.

## x264 reference tree is read-only

Never edit, write, build, or run git write operations in `references/x264-master/`.
See `docs/REFERENCE-X264-READ-ONLY.md`. Instrumentation copies source into
`build/x264-ref-src/` and builds there; `VK264_X264_EXE` selects the binary.

## Retired work

PRs #63 (P-frame investigation) and #67 (intra-in-P) were closed as superseded
by this rewrite. The old `Vk264/` tree remains only as the legacy gate suite.
