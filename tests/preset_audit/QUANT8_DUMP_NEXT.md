# Next: dump x264 quant8_mf/bias for MB4 idx0

The dequant x16 factor is correct (flat CQM scaling_list = 16). The
remaining 1-LSB reconstruction difference (oracle MB4 idx0 recon 16 vs
x264 FDEC 15, with identical pred=15/src=16/DCT-DC=64) must come from
quantization or IDCT rounding.

1. Patch `common/set.c` after `x264_cqm_init` to dump, for qp=25:
   `quant8_mf[CQM_8IY][25][0]`, `quant8_bias[CQM_8IY][25][0]`,
   `dequant8_mf[CQM_8IY][1][0]`.
2. Dump x264's post-quant `dct8x8[0]` for MB4 idx0 in
   `encoder/macroblock.h::x264_mb_encode_i8x8` (pass-tagged).
3. Dump oracle's `vk_ref_quant_tables8` mf/bias and post-quant d8[0]
   (instrument `cabac_enc.c`).
4. Byte-compare and fix the actual differing table entry.
