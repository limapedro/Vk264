# RETRACTION: two-phase FDEC hypothesis is wrong

`build/check_skipintra.py` dumped x264's `h->mb.i_skip_intra` for frame-0
MB1 in the unmodified encoder:

```
XSK initial skip_intra=1
XSK final   skip_intra=1 mode1=9
```

With `i_skip_intra=1`, `x264_macroblock_encode` copies the analysis
reconstruction (`i4x4_fdec_buf`) into the final FDEC rather than
re-encoding (`encoder/macroblock.c:742-752`). Therefore x264's final
reconstruction equals its analysis reconstruction — there is no
two-phase FDEC divergence. The hypothesis in `TWO_PASS_NOTE.md` and
`FINAL_ENCODE_STATE.md` is retracted.

Earlier "final prediction 68 vs analysis 67" measurements were taken
with a modified encoder (early-terminate disabled), which changed the
analysis itself; they are not valid evidence.

Remaining verified facts:
- Full-frame luma diff 1460/4096 (deblock on/off identical).
- Row 0 cols 22-23: x264 68, oracle 67; cols 24-26: x264 69, oracle 70.
- 4x4/8x8 IDCT, quant tables and predictors match x264 source.
- MB0-5 decisions match in the whole-frame harness; NAL2 prefix 50/130.

The reconstruction difference is in analysis-pass mode/residual details
that repeated cross-pass dumps have not pinned down. No source change
landed; gates green; audit 12/120; parity 2/10.
