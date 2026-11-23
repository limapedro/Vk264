# Next: coupled dequant + mode-cost fix

The 8x8 dequant x16 scale bug is confirmed (`set.c:157`, `quant.c:130`).
Fixing it alone changes reconstructed samples (MB4 idx0 16→15, matching
x264) and therefore changes mode costs; the I8 mode decision currently
has compensating errors that relied on the wrong scale.

Recommended sequence:
1. Fix dequant scale (remove x16) and keep it.
2. Re-derive I8 mode decision costs from x264 with the corrected
   reconstruction: use `build/dump_i8_candidates.py` to get x264's x3
   costs and chosen modes, and compare with the oracle's per-mode SADs.
3. Validate with `build/wholeframe_diff2.py`: MB0-4 must remain matching
   while MB5 becomes I8 (2,5,2,4).
4. Only then re-run the full audit/gates.
