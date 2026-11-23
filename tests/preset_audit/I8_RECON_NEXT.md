# Next: pass-tagged FDEC reconstruction dump for I8x8

The VR/DDR predictors are verified correct
(`tests/preset_audit/I8_PREDICTORS_VERIFIED.md`). The remaining I8x8
difference is reconstruction state.

1. Patch `encoder/macroblock.c::x264_mb_encode_i8x8` to dump the 8x8
   reconstructed `p_dst` (FDEC) after `add8x8_idct8`, tagged with
   pass/frame/slice/mb/idx, for frame-0 I-slice MB5 blocks 0..3.
2. Dump the oracle's `rmb[]` for the same MB5 blocks (reconstruction is
   done inline in `cabac_enc.c` after `vk_ref_dequant8_idct_add`).
3. Compare byte-for-byte. Mismatch localises the bug to 8x8
   DCT/quant/dequant/IDCT or the residual scan.
4. Fix that primitive, then re-land the combined filtered-edge + x3 +
   shortcut mode-decision path (kept ready in prior diagnostics).

Tools: `build/dump_x264_edge_tagged.py`,
`build/dump_x264_i8cost_tagged.py`, `build/test_vr_correct.py`.
