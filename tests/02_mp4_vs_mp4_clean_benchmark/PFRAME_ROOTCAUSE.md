# P-frame quality root cause (investigation, 2026-11-20)

## Symptom
- IDR frames byte-exact and 44.55 dB (x264 identical)
- P-frames: vk264 24.68 dB vs x264 41.43 dB at QP28 gop2
- Encoder reconstruction == decoder reconstruction EXACTLY (self-consistent)
- All GOPs decode CLEAN after PR #61

## Root cause: per-MB nonzero mask is zero for P-frames

VK264_DUMP_COEF on the P-frame macroblock records (all MBs):
```
VK264META mb=0    mode=0 luma=0 dcNZ=0   mask=00020000 chroma=0
VK264META mb=35   mode=20 luma=0 dcNZ=36 mask=00020000 chroma=0
VK264META mb=8000 mode=40 luma=0 dcNZ=32 mask=00020000 chroma=0
VK264META mb=8159 mode=0 luma=0 dcNZ=-64 mask=00020000 chroma=0
```
The mask is `0x00020000` = bit 17 only (the "inter" flag set in `join`).
Bits 0-15 (the 16 luma 4x4 nonzero flags) are ALL ZERO even though
`dcNZ` shows large nonzero DC coefficients (±32/64).

The CAVLC shader `writeMacroblock` uses `luma` (cbp, also 0) to decide which
blocks to code: `ue(interPattern[luma+16*chroma])` → cbp=0 → NO luma
residual emitted. That is why P-NALs are smaller (25832 vs x264 29738 B)
and quality collapses: residual energy is computed but never bitstreamed.

## Where the mask should be set
`intra16.comp:107` writes `fixtures[f].nonzero[nonzeroIndex] = uint(nonzero)`
where `nonzeroIndex = cacheBase + raster/4*8 + raster%4`, `cacheBase = 12+40*channel`.
`join()` in `inter_frame.comp:377` reads `luma4[f].nonzero[12 + 8*by + bx]`.
For luma (channel=0) these match (12 + raster/4*8 + raster%4 == 12 + by*8 + bx).
But the dumped fixture shows `nonzero[12..27]` all zero in the P-frame —
so `intra16.comp` is either not writing them or writing zeros.

## Hypothesis
For P-frames `luma4[f].decimate = 1`; `intra16.comp:129` does
`if (macroblockScore < 6) cbp = 0` and line 136/143 zero out `nonzero[]`.
The decimation score threshold may be firing for all P MBs on this content,
discarding coefficients x264 keeps. x264's P-frame decimation uses different
thresholds (enc/macroblock.c:347 i_decimate_score<7, 262 b_decimate gate,
283 i_qp>=18). Note Vk264 sets `decimate=1` unconditionally for P-frames,
but x264 only decimates when `b_dct_decimate` AND `i_qp >= 18`.

## Next step
Align P-frame DCT decimation with x264: only decimate when QP>=18 and score
below x264's threshold, and verify nonzero[12..27] becomes nonzero for
P MBs with residual energy. Then re-measure P-frame PSNR.

Evidence scripts: `frame_psnr.py`, `investigate_frames.ps1`, `error_map.py`.
