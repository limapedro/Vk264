# Encoded .h264 index

**69** encoded streams across **28** experiment folders in `results/`.

Reading the table:
- `path` is relative to `results/`. Use `ffplay`/`mpv`/VLC to play.
- `recon` `True` = byte-exact reconstruction match against FFmpeg decode.
- `PSNR-Y / SSIM / VMAF` are against the raw YUV source.
- `fps` is encode throughput on this GPU.

## Experiments

### `results/ab-forza_F5eKZa_9J2M_30s/` — A/B comparison, wx264 vs wx264-vulkan-cavlc (nearly identical outputs, small fps diff)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `ab-forza_F5eKZa_9J2M_30s/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 6148.3 | 37.58 | 0.9823 | 84.505 | 36.14 | 2.9 MB | broken |
| `ab-forza_F5eKZa_9J2M_30s/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 6052.2 | 47.71 | 0.9918 | 93.298 | 102.19 | 2.9 MB | broken |

### `results/ab-testsrc2/` — A/B comparison, wx264 vs wx264-vulkan-cavlc (nearly identical outputs, small fps diff)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `ab-testsrc2/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 6028.4 | 22.16 | 0.9446 | 45.484 | 34.48 | 2.9 MB | broken |
| `ab-testsrc2/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 6018.1 | 38.67 | 0.9778 | 88.670 | 103.90 | 2.9 MB | broken |

### `results/benchmark-1080p-forza_F5eKZa_9J2M_30s/` — 1080p60 240-frame comparison, raw YUV sources

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-1080p-forza_F5eKZa_9J2M_30s/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 6148.3 | 37.58 | 0.9823 | 84.505 | 34.57 | 2.9 MB | broken |
| `benchmark-1080p-forza_F5eKZa_9J2M_30s/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 6410.7 | 3.82 | 0.6215 | 0.000 | 28.54 | 3.1 MB | broken |
| `benchmark-1080p-forza_F5eKZa_9J2M_30s/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 6052.2 | 47.71 | 0.9918 | 93.298 | 103.01 | 2.9 MB | broken |
| `benchmark-1080p-forza_F5eKZa_9J2M_30s/x264-fast.h264` | x264-fast | — | 5483.0 | 48.49 | 0.9947 | 93.345 | 95.97 | 2.6 MB | broken |

### `results/benchmark-1080p-forza_original_30s/` — 1080p60 240-frame comparison, raw YUV sources

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-1080p-forza_original_30s/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 6001.5 | 36.68 | 0.9744 | 76.307 | 30.98 | 2.9 MB | broken |
| `benchmark-1080p-forza_original_30s/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 6404.4 | 3.64 | 0.5930 | 0.000 | 31.25 | 3.1 MB | broken |
| `benchmark-1080p-forza_original_30s/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 6059.0 | 47.29 | 0.9913 | 92.338 | 104.20 | 2.9 MB | broken |
| `benchmark-1080p-forza_original_30s/x264-fast.h264` | x264-fast | — | 5788.2 | 48.19 | 0.9941 | 92.856 | 109.76 | 2.8 MB | broken |

### `results/benchmark-1080p-forza_performance_original_210s/` — 1080p60 240-frame comparison, raw YUV sources

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-1080p-forza_performance_original_210s/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 5985.0 | 24.90 | 0.7875 | 29.965 | 31.32 | 2.9 MB | broken |
| `benchmark-1080p-forza_performance_original_210s/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 5993.7 | 13.29 | 0.7014 | 4.584 | 28.30 | 2.9 MB | broken |
| `benchmark-1080p-forza_performance_original_210s/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 5988.9 | 27.48 | 0.8229 | 33.916 | 85.53 | 2.9 MB | broken |
| `benchmark-1080p-forza_performance_original_210s/x264-fast.h264` | x264-fast | — | 6673.1 | 31.88 | 0.9155 | 57.484 | 90.24 | 3.2 MB | broken |

### `results/benchmark-1080p-mandelbrot/` — 1080p60 240-frame comparison, raw YUV sources

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-1080p-mandelbrot/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 5982.5 | 26.45 | 0.9382 | 49.139 | 29.61 | 2.9 MB | broken |
| `benchmark-1080p-mandelbrot/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 6023.0 | 11.90 | 0.6845 | 0.000 | 29.02 | 2.9 MB | broken |
| `benchmark-1080p-mandelbrot/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 6016.6 | 31.18 | 0.9273 | 58.604 | 92.24 | 2.9 MB | broken |
| `benchmark-1080p-mandelbrot/x264-fast.h264` | x264-fast | — | 6915.7 | 33.65 | 0.9740 | 73.592 | 116.91 | 3.3 MB | broken |

### `results/benchmark-1080p-testsrc2/` — 1080p60 240-frame comparison, raw YUV sources

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-1080p-testsrc2/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 6028.4 | 22.16 | 0.9446 | 45.484 | 37.04 | 2.9 MB | broken |
| `benchmark-1080p-testsrc2/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 6000.9 | 11.29 | 0.7104 | 0.000 | 33.39 | 2.9 MB | broken |
| `benchmark-1080p-testsrc2/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 6018.1 | 38.67 | 0.9778 | 88.670 | 105.30 | 2.9 MB | broken |
| `benchmark-1080p-testsrc2/x264-fast.h264` | x264-fast | — | 6706.7 | 40.04 | 0.9888 | 91.550 | 152.76 | 3.2 MB | broken |

### `results/benchmark-1080p-warzone2020_verdansk_4k_src/` — 1080p60 240-frame comparison, raw YUV sources

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-1080p-warzone2020_verdansk_4k_src/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 6182.0 | 29.31 | 0.8903 | 57.528 | 29.79 | 2.9 MB | broken |
| `benchmark-1080p-warzone2020_verdansk_4k_src/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 6217.4 | 8.72 | 0.5489 | 0.000 | 30.73 | 3.0 MB | broken |
| `benchmark-1080p-warzone2020_verdansk_4k_src/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 6086.2 | 33.71 | 0.9365 | 73.396 | 92.01 | 2.9 MB | broken |
| `benchmark-1080p-warzone2020_verdansk_4k_src/x264-fast.h264` | x264-fast | — | 5986.3 | 38.32 | 0.9765 | 90.047 | 75.61 | 2.9 MB | broken |

### `results/benchmark-nvenc-forza_F5eKZa_9J2M_30s/` — 1080p60 240-frame comparison, lanczos-downscaled 4K NVENC p7 24 Mbps sources

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-nvenc-forza_F5eKZa_9J2M_30s/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 6148.7 | 37.48 | 0.9819 | 84.546 | 36.13 | 2.9 MB | broken |
| `benchmark-nvenc-forza_F5eKZa_9J2M_30s/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 6079.5 | 47.65 | 0.9915 | 93.377 | 103.35 | 2.9 MB | broken |
| `benchmark-nvenc-forza_F5eKZa_9J2M_30s/x264-fast.h264` | x264-fast | — | 5737.3 | 48.66 | 0.9947 | 93.563 | 94.38 | 2.7 MB | broken |

### `results/benchmark-nvenc-forza_original_30s/` — 1080p60 240-frame comparison, lanczos-downscaled 4K NVENC p7 24 Mbps sources

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-nvenc-forza_original_30s/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 6000.2 | 36.57 | 0.9736 | 76.194 | 32.62 | 2.9 MB | broken |
| `benchmark-nvenc-forza_original_30s/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 6049.8 | 47.21 | 0.9909 | 92.425 | 100.00 | 2.9 MB | broken |
| `benchmark-nvenc-forza_original_30s/x264-fast.h264` | x264-fast | — | 5706.5 | 48.00 | 0.9937 | 92.791 | 105.25 | 2.7 MB | broken |

### `results/benchmark-nvenc-forza_performance_original_210s/` — 1080p60 240-frame comparison, lanczos-downscaled 4K NVENC p7 24 Mbps sources

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-nvenc-forza_performance_original_210s/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 5975.3 | 25.15 | 0.7992 | 30.412 | 32.13 | 2.8 MB | broken |
| `benchmark-nvenc-forza_performance_original_210s/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 5992.3 | 27.80 | 0.8346 | 34.501 | 80.08 | 2.9 MB | broken |
| `benchmark-nvenc-forza_performance_original_210s/x264-fast.h264` | x264-fast | — | 6712.3 | 32.55 | 0.9280 | 60.357 | 90.34 | 3.2 MB | broken |

### `results/benchmark-nvenc-warzone2020_verdansk_4k_src/` — 1080p60 240-frame comparison, lanczos-downscaled 4K NVENC p7 24 Mbps sources

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-nvenc-warzone2020_verdansk_4k_src/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 6185.1 | 29.40 | 0.8930 | 57.982 | 31.63 | 2.9 MB | broken |
| `benchmark-nvenc-warzone2020_verdansk_4k_src/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 6087.3 | 33.91 | 0.9397 | 74.007 | 91.96 | 2.9 MB | broken |
| `benchmark-nvenc-warzone2020_verdansk_4k_src/x264-fast.h264` | x264-fast | — | 6006.1 | 38.70 | 0.9788 | 90.616 | 105.75 | 2.9 MB | broken |

### `results/hbitrate-1080p-forza_F5eKZa_9J2M_30s/` — same as benchmark-nvenc- but --qp 16 --bitrate 50000 (high-quality setting)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `hbitrate-1080p-forza_F5eKZa_9J2M_30s/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=16 gop=120 br=50000 | 49649.1 | 39.60 | 0.9945 | 98.823 | 33.13 | 23.7 MB | broken |
| `hbitrate-1080p-forza_F5eKZa_9J2M_30s/wx264.h264` | wx264 | qp=16 gop=120 br=50000 | 49061.1 | 54.86 | 0.9974 | 97.072 | 104.65 | 23.4 MB | broken |
| `hbitrate-1080p-forza_F5eKZa_9J2M_30s/x264-fast.h264` | x264-fast | — | 45922.6 | 58.42 | 0.9992 | 97.127 | 77.74 | 21.9 MB | broken |

### `results/hbitrate-1080p-forza_original_30s/` — same as benchmark-nvenc- but --qp 16 --bitrate 50000 (high-quality setting)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `hbitrate-1080p-forza_original_30s/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=16 gop=120 br=50000 | 50462.1 | 39.06 | 0.9940 | 98.235 | 31.34 | 24.1 MB | broken |
| `hbitrate-1080p-forza_original_30s/wx264.h264` | wx264 | qp=16 gop=120 br=50000 | 49618.6 | 54.52 | 0.9974 | 97.042 | 104.41 | 23.7 MB | broken |
| `hbitrate-1080p-forza_original_30s/x264-fast.h264` | x264-fast | — | 48892.0 | 57.51 | 0.9990 | 97.130 | 72.50 | 23.3 MB | broken |

### `results/hbitrate-1080p-forza_performance_original_210s/` — same as benchmark-nvenc- but --qp 16 --bitrate 50000 (high-quality setting)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `hbitrate-1080p-forza_performance_original_210s/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=16 gop=120 br=50000 | 49952.5 | 37.14 | 0.9674 | 94.839 | 25.42 | 23.8 MB | broken |
| `hbitrate-1080p-forza_performance_original_210s/wx264.h264` | wx264 | qp=16 gop=120 br=50000 | 49951.1 | 41.48 | 0.9801 | 99.868 | 73.45 | 23.8 MB | broken |
| `hbitrate-1080p-forza_performance_original_210s/x264-fast.h264` | x264-fast | — | 53334.4 | 45.02 | 0.9919 | 99.945 | 51.99 | 25.4 MB | broken |

### `results/hbitrate-1080p-testsrc2/` — same as benchmark-nvenc- but --qp 16 --bitrate 50000 (high-quality setting)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `hbitrate-1080p-testsrc2/wx264.h264` | wx264 | qp=16 gop=120 br=50000 | 50229.4 | 59.08 | 0.9992 | 98.518 | 100.13 | 24.0 MB | broken |

### `results/hbitrate-1080p-warzone2020_verdansk_4k_src/` — same as benchmark-nvenc- but --qp 16 --bitrate 50000 (high-quality setting)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `hbitrate-1080p-warzone2020_verdansk_4k_src/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=16 gop=120 br=50000 | 51322.2 | 40.73 | 0.9862 | 99.227 | 28.17 | 24.5 MB | broken |
| `hbitrate-1080p-warzone2020_verdansk_4k_src/wx264.h264` | wx264 | qp=16 gop=120 br=50000 | 50305.5 | 47.95 | 0.9928 | 99.926 | 83.55 | 24.0 MB | broken |
| `hbitrate-1080p-warzone2020_verdansk_4k_src/x264-fast.h264` | x264-fast | — | 50527.8 | 50.78 | 0.9972 | 99.949 | 69.00 | 24.1 MB | broken |

### `results/benchmark-4k-forza_F5eKZa_9J2M_30s/` — 4K comparison (60 frames only, wx264 CAVLC only)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `benchmark-4k-forza_F5eKZa_9J2M_30s/wx264.h264` | wx264 | qp=34 gop=120 br=6000 | 11870.2 | 43.72 | 0.9862 | 81.640 | 29.16 | 1.4 MB | broken |

### `results/verify-cabac-postfix-mandelbrot/` — CABAC backend post-fix verification run (synthetic clips)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `verify-cabac-postfix-mandelbrot/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 6028.5 | 26.65 | 0.9509 | 58.697 | 11.85 | 2.9 MB | verified |

### `results/verify-cabac-postfix-testsrc2/` — CABAC backend post-fix verification run (synthetic clips)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `verify-cabac-postfix-testsrc2/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 5984.1 | 22.32 | 0.9559 | 50.583 | 12.70 | 2.9 MB | verified |

### `results/verify-nvenc-forza_F5eKZa_9J2M_30s-vulkan/` — CABAC / CAVLC verification on the 4 NVENC 1080p game clips

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `verify-nvenc-forza_F5eKZa_9J2M_30s-vulkan/wx264-vulkan.h264` | wx264-vulkan | — | — | — | — | — | — | 3.1 MB | unverified |

### `results/verify-nvenc-forza_F5eKZa_9J2M_30s-vulkan-cavlc/` — CABAC / CAVLC verification on the 4 NVENC 1080p game clips

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `verify-nvenc-forza_F5eKZa_9J2M_30s-vulkan-cavlc/wx264-vulkan-cavlc.h264` | wx264-vulkan-cavlc | qp=34 gop=120 br=6000 | 6148.7 | 37.48 | 0.9819 | 84.546 | 35.99 | 2.9 MB | verified |

### `results/verify-nvenc-forza_original_30s-vulkan/` — CABAC / CAVLC verification on the 4 NVENC 1080p game clips

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `verify-nvenc-forza_original_30s-vulkan/wx264-vulkan.h264` | wx264-vulkan | — | — | — | — | — | — | 3.1 MB | unverified |

### `results/verify-nvenc-vulkan-forza_F5eKZa_9J2M_30s/` — CABAC / CAVLC verification on the 4 NVENC 1080p game clips

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `verify-nvenc-vulkan-forza_F5eKZa_9J2M_30s/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 6437.2 | 39.00 | 0.9892 | 93.897 | 10.71 | 3.1 MB | verified |

### `results/verify-nvenc-vulkan-forza_original_30s/` — CABAC / CAVLC verification on the 4 NVENC 1080p game clips

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `verify-nvenc-vulkan-forza_original_30s/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 6400.5 | 38.28 | 0.9855 | 89.490 | 9.49 | 3.1 MB | verified |

### `results/verify-nvenc-vulkan-forza_performance_original_210s/` — CABAC / CAVLC verification on the 4 NVENC 1080p game clips

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `verify-nvenc-vulkan-forza_performance_original_210s/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 5987.9 | 28.32 | 0.8499 | 42.348 | 11.15 | 2.9 MB | verified |

### `results/verify-nvenc-vulkan-warzone2020_verdansk_4k_src/` — CABAC / CAVLC verification on the 4 NVENC 1080p game clips

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `verify-nvenc-vulkan-warzone2020_verdansk_4k_src/wx264-vulkan.h264` | wx264-vulkan | qp=34 gop=120 br=6000 | 6211.0 | 32.62 | 0.9309 | 74.691 | 10.99 | 3.0 MB | verified |

### `results/preset-bench-forza_F5eKZa_9J2M_30s/` — Wx264-Vulkan preset sweep experiments (intermediate)

| path | encoder | qp / gop / bitrate | kbps | PSNR-Y | SSIM | VMAF | fps | size | recon |
|---|---|---|---|---:|---:|---:|---:|---:|---:|
| `preset-bench-forza_F5eKZa_9J2M_30s/wx264-fast.h264` | wx264-fast | — | — | — | — | — | — | 2.8 MB | unverified |
| `preset-bench-forza_F5eKZa_9J2M_30s/wx264-faster.h264` | wx264-faster | — | — | — | — | — | — | 2.8 MB | unverified |
| `preset-bench-forza_F5eKZa_9J2M_30s/wx264-medium.h264` | wx264-medium | — | — | — | — | — | — | 2.7 MB | unverified |
| `preset-bench-forza_F5eKZa_9J2M_30s/wx264-superfast.h264` | wx264-superfast | — | — | — | — | — | — | 3.1 MB | unverified |
| `preset-bench-forza_F5eKZa_9J2M_30s/wx264-ultrafast.h264` | wx264-ultrafast | — | — | — | — | — | — | 3.1 MB | unverified |
| `preset-bench-forza_F5eKZa_9J2M_30s/wx264-veryfast.h264` | wx264-veryfast | — | — | — | — | — | — | 2.9 MB | unverified |
