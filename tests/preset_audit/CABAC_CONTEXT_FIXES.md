# CABAC context fixes (bin-level differential)

## Method
Hooked `x264_cabac_encode_decision_c` (scratch x264 is pure C) and
`vk_cabac_decision` (oracle) to dump every CABAC decision bin with an
MB tag, then compared the sequences bin-by-bin. This eliminates the
byte-alignment ambiguity of comparing compressed bytes directly.

## Fixes in `Vk264-rewrite/src/ref/cabac_enc.c`

1. **8x8DCT coefficient contexts.** `cabac_coeffs()` used `ctx_sig+i` /
   `ctx_last+i` for every category. x264 uses dedicated 64-entry tables
   `significant_coeff_flag_offset_8x8` / `last_coeff_flag_offset_8x8`
   when `DCT_LUMA_8x8` (cat 5). The oracle already had byte-identical
   copies of both tables but never used them. Now used for cat 5.
   Effect: c004 NAL2 length 147 -> 132 (x264 = 130).

2. **Cached CBP for I4 and I8 MBs.** After reconstruction the oracle
   left `cbp_luma` at its I16-derived value (often 0) while x264 caches
   `i_cbp_luma = i4cbp` / `i8cbp`. This changed `cabac_cbp_luma`
   contexts in all following MBs. Now set correctly.

3. **CBP bit 8 (luma DC cbf).** The `nz` flag packed into `mbcbp` bit 8
   was derived from "any nonzero luma"; x264 has it clear for I4/I8
   MBs. Set to 0 for both.

## Verified effect
```
c004 IDR NAL2:  first-diff byte 50 -> 67; length 147 -> 132 (x264 130)
CABAC bin diff: first mismatch bin 318 (MB5 CBP) -> bin 470 (MB6 residual)
                = the whole MB0..MB5 header/residual prefix now matches
audit:          12/120 (unchanged), NAL2 first diff superfast 50 -> 67
recon:          355/4096 (unchanged; these are bitstream-context fixes)
gates:          CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56
```
The remaining first mismatch is in MB6's 8x8 residual significance,
consistent with the known 1-LSB neighbour reconstruction differences
(`{6:54, 10:153, 14:72, 15:75}` luma bytes), not a CABAC bug.
