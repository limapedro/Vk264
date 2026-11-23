# Systematic reconstruction pass — starting point

Verified metric: `build/compare_recon.py` (full frame-0 luma, x264
`--dump-yuv --deblock 0:0` vs oracle `ORACLE_DUMP_RECON`).

Result: 1460/4096 luma bytes differ; identical with deblock enabled and
disabled, so this is genuine pre-deblock reconstruction difference, not
an artifact of x264's in-loop filter.

Earliest differences (row 0):
```
pixel (22,0) src=68  x264=68  oracle=67
pixel (24,0) src=69  x264=69  oracle=70
```

At MB1 idx1 (mode 9 DC_LEFT), the oracle's prediction is 67 while the
source is 68, so a +1 residual is quantized away; x264 reconstructs the
source exactly. A per-block dump of x264's prediction could not be
captured (`x264_mb_encode_i4x4` is skipped in the analysis pass for this
MB), so the exact quant/rounding difference is still unresolved.

Next: instrument `x264_macroblock_encode` (not the analysis path) to
dump p_dst/p_src/dct for MB1 idx1, and compare with the oracle's
`vk_i4x4_decide` residual path. Fix the shared rounding primitive and
re-measure the full-frame diff count (target: monotonically decreasing).

No source change landed. Gates green; audit 12/120; NAL2 50/130; parity 2/10.
