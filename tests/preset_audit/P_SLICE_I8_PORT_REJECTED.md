# P-slice I4/I8 intra port: attempted and reverted

## What was implemented
- Extracted a `p_analyse_i8()` helper (copy of the byte-exact I-slice
  I8 analysis) using the P-slice padded reconstruction buffer as
  neighbour source.
- Added I8 cost to the P-slice intra-vs-inter decision
  (`min(i16, i8) < inter`), matching x264's `mb_analyse_intra`.
- Added P-slice I8 CABAC syntax: ctx14/17 mb_type prefix,
  transform_size ctx399, four pred modes, chroma mode, CBP,
  qp_delta, 8x8 residual (cat 5), chroma DC, plus cache and
  reconstruction updates.

## Measured result (rejected)
```
Before: exp004 NAL3 first diff byte 7, length 53; oracle mb1 inter
After:  exp004 NAL3 first diff byte 7, length 57; oracle mb1 intra
        audit still 12/120, NAL2 3/20
        PSNR for 8f warzone: 21.71 -> 20.75 (slightly worse)
```
The decision fix was verified (`do_intra=1` for frame1 MB1, i8cost
244 < i16cost 2602), but the encoded CABAC stream still diverges
at byte 7 and NAL3 grew by 4 bytes. The transform_size bin was
written (`TSHIT` confirmed the call site fires), yet the bin dump
showed no ctx399 decision — the P-slice intra mb_type/transform
syntax is still not exactly x264's, so this partial port is worse
than leaving the I16-only path.

All changes were reverted. Tree clean at d4637ee; gates green
(29/29, 65/65, CAVLC 1360/1360, MP4 56/56), audit 12/120,
NAL2 3/20, recon 0/4096.

## Next
The P-slice I8 port needs the exact x264 `cabac_mb_header_p` order
and context offsets verified bin-by-bin before it can be kept.
