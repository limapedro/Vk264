# x264 superfast intra mode decision is SAD, not SATD

## Evidence

`encoder/encoder.c:1411`:

```c
int satd = !h->mb.b_lossless && h->param.analyse.i_subpel_refine > 1;
memcpy( h->pixf.mbcmp, satd ? h->pixf.satd : h->pixf.sad_aligned, ... );
h->pixf.intra_mbcmp_x3_4x4 = satd ? ...satd_x3... : ...sad_x3...;
```

The `--preset superfast` uses `subme=1`, so `i_subpel_refine=1` and
`satd=0`. Therefore `mbcmp` and `intra_mbcmp_x3_4x4` are **SAD**. This was
verified on an instrumented scratch build of the exact x264 used for the
gold traces.

## Consequence for the oracle

`vk_i4x4_decide()` correctly uses SAD. An experiment switching it to SATD
broke the `rewrite_i4x4_decide` fixture and was reverted.

## Also ported

Instrumentation showed for MB1: `mbrd=0`, `b_fast_intra=0`,
`intra_mbcmp_x9_4x4=NULL`. The oracle now replicates x264's
`intra_sad_x3_4x4` + `intra_analysis_shortcut` path for candidate sets of
6+ modes, keeping the full loop for smaller sets (the fixture requires it).

A separate out-of-bounds read was fixed: `cabac_enc.c` indexed `rmb[-1]`
for MB-edge neighbours; these now come from the padded `rbase` buffer.

## Status

Gates green. First decision-stream divergence remains at #53 (MB1 mode
choice); x264's MB1 x3 SADs (992/548/892) still differ from the oracle's
(290/82/146), so the remaining work is on the analysis input/reconstruction
state, not CABAC syntax.
