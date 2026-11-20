# 01 — 1080p60 CBR 6000 kbps streaming benchmark — RESULTS

Source: warzone2020_verdansk, 1920x1080, 660 frames, 60 fps.
Target 6000 kbps, ultrafast preset, GOP=1 (clean — see below).

## Clean comparison (GOP=1, both decode with 0 errors)

| Metric | vk264 | x264 |
|---|---|---|
| Encode time (660f) | 90.41 s | 1.06 s |
| Throughput | **7.30 fps** | **624.25 fps** |
| Stream size | 8,316,440 B | 8,410,978 B |
| Actual bitrate | 6,048 kbps | 6,117 kbps |
| Size ratio | 0.989 | — |
| Decode errors | 0 | 0 |

Quality (vk264 vs source): PSNR Y 28.35 dB / U 41.06 / V 40.76 (avg 29.99);
SSIM All 0.8833 (Y 0.8368). Both hit the 6000 kbps target closely.

## Playback files (correctly timed MP4 — use these)

- `vk264_cbr6000_gop1.mp4` — 660 frames, 60 fps, 11.00 s
- `x264_cbr6000_gop1.mp4` — 660 frames, 60 fps, 11.00 s

Raw `.h264` files play at wrong speed in players (no container timing;
players guess 25 fps). This is an output-format gap being addressed separately.

## Critical finding: GOP=60 P-slice corruption (again)

The realistic streaming case (GOP=60, 1s keyframe interval) produced a vk264
stream with decode errors (`top block unavailable for requested intra mode`),
while x264 was clean. Quality metrics from that run were invalid (PSNR 14.7 dB
from corrupted frames). See `note_bitrate.md` and experiment 00's
`gop60_note.txt`. All valid metrics above use GOP=1.

## Other notes

- vk264 `--bitrate` is ABR (rate_control shader), not strict VBV CBR like
  x264's vbv-maxrate/vbv-bufsize. It converged to target on this content.
- The "1.7 kbps" in the initial `summary.json` was a reporting-formula bug
  (divided by frames*fps instead of seconds); corrected in `run_gop1.ps1`.
  Correct actual bitrate for GOP60: 5,997 kbps (vk264) / 5,796 kbps (x264).

## Bottom line

At 6000 kbps CBR, vk264 produces decodable, correctly-sized streams with
reasonable quality at GOP=1, but runs ~85x slower than x264 and the P-slice
path is broken for GOP>1. Output defaults to raw H.264 without container
timing — a known gap being addressed.
