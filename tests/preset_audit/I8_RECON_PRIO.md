# Priority: byte-exact 8x8 reconstruction

Diagnosis now converges: I8 mode-decision control flow, predictors, and
edge inputs are all reproduced. The only remaining I8 differences come
from reconstructed neighbour samples.

Concrete next step:
1. Dump x264's FDEC for the frame-0 I-slice MB3 idx0 (mode 9, DC_LEFT)
   with pass tags (`build/dump_x264_i8fdec.py` pattern).
2. Dump the oracle's `rec8[]` for the same block (instrument
   `vk_ref_dequant8_idct_add` in `cabac_enc.c`).
3. Byte-compare. If they differ, inspect `vk_ref_quant_tables8`,
   `vk_ref_quant8` and `vk_ref_dequant8_idct_add` against x264's
   `CQM_4IY` / `dequant_8x8` / `add8x8_idct8`.
4. Once the 8x8 reconstruction matches, re-land the verbatim I8 decision
   path from `build/apply_i8_exact.py` — with all inputs correct it
   produced MB5 modes 2,5,2,4 exactly.
