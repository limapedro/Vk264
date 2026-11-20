# 00 — Ultrafast real-footage benchmark (vk264 vs x264)

Source: `E:\__PROJECTS__\Wx264-Vulkan-Opencode\clips\youtube\warzone2020_verdansk_4k_src_1080p_nvenc_p7_24mbps.mp4`
- 1920x1080, 660 frames, ~59.94 fps, H.264 (NVENC P7 source)

## Goal

Measure the current vk264 encoder on real gameplay footage and compare against
x264 ultrafast on the same source: encoding throughput (fps/s), output size,
and quality metrics (PSNR/SSIM vs the decoded source).

## Method

1. Decode source MP4 to raw YUV420p (`source_1920x1080_660f.yuv`, ~2 GB).
2. Encode all 660 frames with `vk264.exe --backend vulkan --preset ultrafast
   --qp 23 --gop 60` (CAVLC entropy, x264-faithful SAD mode selection).
3. Encode the same frames with `ffmpeg -c:v libx264 -preset ultrafast -qp 23
   -g 60` as the reference.
4. Wall-clock timing via PowerShell `Stopwatch`; PSNR/SSIM via ffmpeg filters.

## Files

- `run_benchmark.ps1` — reproducible benchmark script
- `vk264_ultrafast.h264` / `x264_ultrafast.h264` — encoded streams
- `vk264_stdout.txt` / `x264_stderr.txt` — encoder logs
- `psnr_vk.txt` / `ssim_vk.txt` — per-frame quality metrics
- `summary.json` — machine-readable results

## Notes

- Bitstream byte equality is NOT expected at QP23 on real footage: only the
  synthetic flat/noise gates and full ultrafast real-footage parity at the
  harness's specific QP/GOP settings are proven byte-exact (see issue #30).
- This benchmark measures the full encoder (host orchestration + GPU shaders),
  not individual kernel throughput.
