# Full reconstructed-frame byte-compare (frame 0, c004)

Used x264's native `--dump-yuv` and the oracle's `ORACLE_DUMP_RECON` to
dump both complete reconstructed luma frames after frame 0, then
byte-compared with `build/compare_recon.py`:

```
differing luma bytes: 1460 / 4096
earliest: pixel (22,0) x264=68 oracle=67
          pixel (24,0) x264=69 oracle=70
per-MB diff counts: MB0=2, MB1=72, MB2=58, MB5=111, MB6=134, ... MB23=42
```

This is the definitive comparison (no per-MB FDEC rebasing, no analysis
pass ambiguity). Key conclusions:

1. Even MB0/MB1 — whose *decision-level* state (type, modes, CBP) matches
   in the whole-frame harness — differ by 1 LSB in reconstruction.
2. MB6 is not uniquely wrong; reconstruction diverges broadly across the
   frame. Fixing one edge sample will not produce byte-exact NAL2.
3. The whole-frame decision harness (`wholeframe_diff2.py`) is therefore
   only a partial proxy: matching decisions does not imply matching
   reconstructed pixels.

Remaining work is a systematic byte-exact reconstruction pass (intra
prediction filtering, quant/dequant rounding, IDCT) across all MB types,
validated against this full-frame comparison — not isolated MB6 fixes.

No source change landed. Gates green; audit 12/120; NAL2 prefix 50/130;
parity 2/10.
