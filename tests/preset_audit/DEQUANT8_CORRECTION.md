# CORRECTION: the 8x8 dequant x16 factor is CORRECT

PR #231 claimed a 16x dequant scale bug based on `set.c:157` showing
`dequant8_mf = def_dequant8 * scaling_list` with no explicit x16.
That conclusion was WRONG: x264's flat-CQM `scaling_list` is
`x264_cqm_flat16` (encoder/set.c:273), i.e. every entry is 16. So
`dequant8_mf = def_dequant8 * 16`, exactly matching our
`vk_ref_dequant8_idct_add`'s `dequant8_scale[q][j] * 16`.

Removing the x16 (as PR #231 suggested) broke MB3/MB4 mode decisions and
gave no byte-prefix gain — consistent with the factor being required.

The earlier "1-LSB difference" (oracle MB4 idx0 recon 16 vs x264 FDEC
15) therefore is NOT a dequant scale bug. With identical pred=15,
src=16 and raw DCT DC=64 in both encoders, the difference must be in
quant bias/mf or IDCT rounding, which needs a fresh byte-level dump of
x264's quant8_mf/quant8_bias and the post-quant dct[0] for MB4 idx0.

All PR #231 code changes were reverted; gates green (29/29, 65/65),
audit 12/120, NAL2 prefix 49/130. This document supersedes
`DEQUANT8_16X_FINDING.md` and `DEQUANT8_NEXT.md`.
