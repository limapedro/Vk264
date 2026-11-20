# 02 — Post-fix results: P-slice corruption resolved

Before PR #61, gop>1 streams from vk264 were corrupt (decode errors), which
caused the apparent quality gap vs x264. After PR #61 (conditional
transform_size_8x8_flag in cAVLC P-slices), all GOPs decode cleanly.

## Quality at GOP=60, 1080p60, 6000kbps CBR, 120 frames

| | vk264 | x264 |
|---|---|---|
| Decode | CLEAN | CLEAN |
| PSNR Y | 15.69 dB | 15.90 dB |
| PSNR avg | 17.42 dB | 17.63 dB |
| Size | 1,497,463 B | ~1,388,670 B |

The ~0.2 dB gap is normal rate-control/mode-decision difference; the
previous ~14 dB "quality gap" was entirely from the P-slice corruption.

## Byte parity

Streams are decodable but not byte-exact with x264 at GOP>1: first mismatch
in the P-slice CABAC/CAVLC payload (motion/decision differences). The
single-frame gop=1 QP28 case remains byte-exact (proven). Byte-exact
parity across presets is the ongoing goal.

## Key fix (PR #61)

cavlc_frame.comp wrote a `transform_size_8x8_flag` bit unconditionally for
P-MBs with luma!=0. Baseline/CAVLC has `transform_8x8_mode_flag=0`, so
x264 doesn't emit it. The extra bit shifted all subsequent CAVLC syntax and
corrupted P-slices. Now conditional on the profile's transform8x8 flag.

## Current state

- Native MP4 output works correctly (PR #57, ftyp fixed in #59)
- P-slices decodable at all GOPs (PR #61)
- ctest 65/65, parity gates 6/6
- Byte-exact only for proven synthetic gates + gop1 real footage at QP28
- Performance still far behind x264 (6-50 fps vs 400-650 fps)
