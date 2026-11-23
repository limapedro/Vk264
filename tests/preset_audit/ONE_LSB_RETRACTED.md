# RETRACTION: the 1-LSB reconstruction difference was a cross-pass artifact

Earlier dumps compared x264 FDEC=15 (from `build/dump_x264_mb4fdec.py`,
filtered only by `i_frame==0`, no slice-type/pass tag) against oracle
rec8=16 and concluded a 1-LSB reconstruction bug.

A comprehensive same-pass dump with an explicit frame-0 I-slice filter
(`build/dump_x264_fullpipe2.py`) shows the complete pipeline for MB4 idx0:

```
XPIPE raw   dct0=64 pred0=15 src0=16
XPIPE quant dct0=1 nz=1 mf=745 bias=29
XPIPE deq   dct0=88 deqmf=352
XPIPE fdec  fdec0=16
```

x264's FDEC is **16**, identical to the oracle's rec8=16. Quant tables
(match: mf=745, bias=29, dequant=352) and IDCT arithmetic (source-identical
IDCT8_1D with +32 before >>6) are all correct.

There is no reconstruction bug. The earlier FDEC=15 came from a different
`mb_analyse_intra` call (x264 analyzes MBs in multiple passes). This
retracts `I8_1LSB_FINDING.md`, `DEQUANT8_16X_FINDING.md` (already corrected
by `DEQUANT8_CORRECTION.md`), and the premise of `I8_1LSB_NEXT.md`.

Actual remaining state (whole-frame harness): MB0-4 match, MB5 is the
first divergence (oracle I4 vs x264 I8 modes 2,5,2,4). The I8 decision
path still needs the per-block candidate/shortcut logic matched; that is
the real next step, with no reconstruction fix required.
