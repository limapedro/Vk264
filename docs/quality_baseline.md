# Wx264 shader encoder — measured quality baseline

Source encoder copied from `E:\__PROJECTS__\Wx264-ChatGPT` into this repository
(source, shaders, x264-vulkan fork, docs, harness, result markdown only).
Build: `cmake -S . -B build -A x64`, `cmake --build build --config Release -j`.
`ctest --test-dir build -C Release`: 45/46 pass; the one failure
`x264_cavlc_packet_parity` is pre-existing ("Unknown kernel test" — the stage
is registered but has no dispatch branch in `dct_parity.cpp`).

Smoke: `build\Release\wx264.exe build\smoke.yuv build\smoke.h264 64 64 2 30 --qp 30`
→ exit 0, 1032 bytes, GPU NVIDIA GeForce GTX 1660 SUPER.

## Real-video baseline

Clip: `D:\Videos\ssstwitter.com_1748999992229.mp4`, decoded to
`build/baseline_640x360_60f.yuv` (640×360, 60 frames, 25 fps), 6000 kbit/s
target, GOP 50. Command:

```
python harness/benchmark.py --input build/baseline_640x360_60f.yuv \
  --out build/baseline_q --width 640 --height 360 --fps 25 --frames 60 \
  --bitrate 6000 --encoders wx264-slow x264-slow nvenc-p4
```

| Encoder | Achieved kb/s | Process fps | PSNR-Y | SSIM |
|---|---:|---:|---:|---:|
| wx264-slow | 6108 | 176 | 48.13 dB | 0.99369 |
| x264-slow | 5675 | 98 | 54.57 dB | 0.99834 |
| nvenc-p4 | 6526 | 153 | 53.96 dB | 0.99797 |

Gap: ~6.4 dB PSNR-Y vs x264, ~5.8 dB vs NVENC at the same target bitrate.
Wx264 is faster than x264 here but below NVENC. Full data:
`build/baseline_q/summary.json`.

## Relevant encoder settings from the run

`search_radius=24`, `quarter_pel=1`, `satd_candidates=4`, `deblock_mode=2`,
`flat_intra=1`, `p_slice_rows=2`, `reference_frames=5`, `b_frames=0`,
`lookahead_frames=0`, `entropy=cavlc`, `aq=0`, `trellis=0`, `vbv=0`.
