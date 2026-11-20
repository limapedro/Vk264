# 01 — 1080p60 CBR 6000 kbps streaming benchmark

Source: `warzone2020_verdansk_4k_src_1080p_nvenc_p7_24mbps.mp4`,
1920x1080, 660 frames, 60 fps nominal.

## Goal

Streaming-style comparison at a fixed 6000 kbps target:
- vk264: `--bitrate 6000` (ABR-style rate control with bitDebt/budget)
- x264: `-b:v 6000k -minrate -maxrate -bufsize` + `vbv-maxrate/vbv-bufsize` (VBV CBR)

Track wall-clock encode time, actual achieved bitrate, output size, decode
validity, and PSNR/SSIM vs the decoded source. Mux to MP4 at 60 fps for
correct playback timing (raw .h264 defaults to 25 fps in players).

## Caveats

- vk264's `--bitrate` implements **average-rate (ABR)** control, not strict
  VBV CBR. x264 uses real VBV CBR. This is an active function-parity gap
  (see `docs/function-parity-matrix.md` — rate control is PARTIAL: fixed-QP
  path only in the port, ABR via host rate_control shader).
- The P-slice path has a known corruption issue at GOP>1 (issue #50). This run
  uses GOP=60 (realistic 1s keyframe interval) to measure the streaming case;
  if decode errors occur, see experiment 00 (`gop60_note.txt`) and the GOP=1
  fallback there for clean quality metrics.

## Files

- `run_benchmark.ps1` — reproducible script
- `vk264_cbr6000.mp4` / `x264_cbr6000.mp4` — correctly-timed playback files
- `summary.json` — machine-readable results
