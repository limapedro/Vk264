# Fixed: I8x8 reconstruction was overwritten by I16

## Root cause
In `Vk264-rewrite/src/ref/cabac_enc.c`, `vk_cabac_encode_idr_recon_frame`:

```c
if (use_i4) {
    ... reconstruct 16 I4 blocks into rmb[] ...
} else
    compute_i16_full(..., rmb, 0);
```

The I8x8 analysis loop above had already reconstructed all four 8x8
blocks into `rmb[]`, and the bitstream path for `use_i8` writes the I8
quantized coefficients (`i8q`). But when `use_i8=1` and `use_i4=0`, the
`else` branch ran `compute_i16_full()` and **overwrote `rmb[]` with an
I16 reconstruction**. The coded residual (I8) and the reconstruction
(I16) therefore disagreed in every I8 MB.

## Fix
- Added `else if (!use_i8)` so the I16 path is skipped for I8 MBs.
- For I8 MBs set `cbp_luma = i8cbp` and `nz = i8cbp ? 1 : 0`, matching
  x264's FDEC CBP / non-zero-count cache state for later MBs.

## Verification
Measurement corrected to use `x264 --no-deblock` (`--deblock 0:0` does
NOT disable the in-loop filter) and the audit QP convention
(`qp_i = qp_p - 3`, x264's default I-frame QP offset).

```
ultrafast: recon diff 0/4096   (unchanged, still byte-exact)
superfast: recon diff 993 -> 355 / 4096
per-MB after fix: {5:1, 6:54, 10:153, 14:72, 15:75}
```
MB0-4 are now byte-exact. The remaining differences are exactly the I8
MBs whose **mode decisions** still diverge from x264 in
`wholeframe_diff2.py` (first at MB6: x264 I8 modes [2,2,5,2] vs oracle
[0,2,0,0]).

Gates: CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56.
Audit remains 12/120; NAL2 2/20 (bitstream decisions unchanged; this
fix corrects reconstruction, not the earlier NAL2 divergence).
