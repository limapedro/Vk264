# P-slice header fixes (superfast warzone)

The IDR NAL2 is byte-exact, so divergence moved to the P slice (NAL3).
Three real header bugs were found by comparing x264's
`slice_header_write()` against `vk_slice_header_write()`:

1. **POC lsb was always written as 0.** Added `i_poc` to
   `vk_slice_header_t` and set it from x264's `POC = 2*frame_num`
   in `oracle_video_cli.c`.
2. **pred_weight_table was missing.** x264 writes it for every P
   slice when PPS `b_weighted_pred` is set (even when no weights are
   actually used). Added luma/chroma denominators and per-ref flags.
3. **Syntax element order was wrong.** oracle wrote
   `dec_ref_pic_marking` before `pred_weight_table`; x264 writes
   `pred_weight_table` first. Reordered.

## Verified effect
```
exp004 superfast warzone NAL3:  byte 2 -> byte 3 -> byte 7
header bytes 0..6 now identical: 41 9a 21 18 ff be ed
audit full byte-exact: 12/120 (unchanged; CABAC payload still differs)
gate: CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56
```
The remaining NAL3 difference is in the CABAC slice payload
(byte 7 onward), not the header.
