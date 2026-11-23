# Next after quant-table verification

x264 and oracle quant8 tables are identical (mf=745, bias=29,
dequant8_mf=352 at qp25). Raw DCT DC=64 quantizes to 1 in both. The
1-LSB reconstruction difference must therefore be isolated by dumping,
in the SAME pass:

1. x264's post-IDCT FDEC bytes for MB4 idx0 (already have: all 15).
2. Oracle's post-IDCT rec8 bytes (already have: all 16).
3. x264's `add8x8_idct8` intermediate dct[0] before/after +32 and the
   final `(rhs)>>6` per pixel.
4. Oracle's corresponding `vk_ref_idct8x8_add` values.

Since both IDCT8_1D macros and the +32 placement match source-for-source,
a likely cause is that x264's FDEC was dumped from a different encoder
pass than the quant values (the earlier XQV dump lacked a slice-type
filter). Use the pass-tagged `build/dump_aligned_all.py` pattern for all
dumps before concluding the IDCT differs.
