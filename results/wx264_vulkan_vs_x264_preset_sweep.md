# wx264-vulkan vs x264 preset sweep at 6000 kbit/s, 1080p60

- Date: 2026-11-19
- Clips (240 frames @ 1080p60): `forza_F5eKZa_9J2M_30s`, `forza_original_30s`, `forza_performance_original_210s`, `mandelbrot`, `testsrc2`, `warzone2020_verdansk_4k_src`
- wx264-vulkan presets supported: ultrafast, superfast, veryfast, faster, fast (slower presets are rejected by the CLI; missing trellis/lookahead/mb-tree/8x8/AQ/sub8x8 — see docs/presets.md)
- All encodes: `--qp 34 --gop 120 --bitrate 6000` (x264 via ffmpeg `-c:v libx264 -sc_threshold 0 -maxrate 6000k -bufsize 12000k`; wx264-vulkan `--bitrate 6000` average-rate RC)
- wx264-vulkan `--bitrate` is constrained average-rate; x264 VBR under `-maxrate 6000k` may undershoot/overshoot target
- Settings logged per clip in clip `summary.json`; outputs are MP4 (raw intermediates removed by the harness)
- VMAF intentionally not collected in this sweep; PSNR-Y and SSIM are full-frame means
- GPU: NVIDIA GTX 1660 SUPER; x264 ran single-node 16-thread CPU

## Cross-clip averages

| encoder | avg kbit/s | avg process fps | avg PSNR-Y | avg SSIM |
|---|---|---|---|---|
| wx264-vulkan-ultrafast | 6213.6 | 11.5 | 30.3235 | 0.906795 |
| wx264-vulkan-superfast | 6213.6 | 11.6 | 30.3235 | 0.906795 |
| wx264-vulkan-veryfast | 6241.2 | 11.4 | 30.5492 | 0.915767 |
| wx264-vulkan-faster | 6285.7 | 11.6 | 30.8508 | 0.933735 |
| wx264-vulkan-fast | 6102.7 | 11.9 | 31.0084 | 0.939531 |
| x264-ultrafast | 6185.4 | 518.6 | 36.8119 | 0.946337 |
| x264-superfast | 5855.2 | 336.9 | 38.7847 | 0.970715 |
| x264-veryfast | 6328.7 | 232.5 | 40.1113 | 0.975393 |
| x264-faster | 6325.8 | 130.9 | 40.3790 | 0.976363 |
| x264-fast | 6312.9 | 118.0 | 40.2775 | 0.976384 |
| x264-medium | 6141.1 | 105.2 | 40.4879 | 0.977113 |
| x264-slow | 6042.6 | 77.0 | 40.5216 | 0.977357 |
| x264-slower | 5945.7 | 33.6 | 40.6353 | 0.977999 |
| x264-veryslow | 5817.6 | 21.1 | 40.6736 | 0.978414 |
| x264-placebo | 5846.4 | 6.3 | 40.6429 | 0.977717 |

## Realtime factor (60 fps target)

| encoder | avg process fps | realtime factor | >=60 fps? |
|---|---|---|---|
| wx264-vulkan-ultrafast | 11.5 | 0.19x | no |
| wx264-vulkan-superfast | 11.6 | 0.19x | no |
| wx264-vulkan-veryfast | 11.4 | 0.19x | no |
| wx264-vulkan-faster | 11.6 | 0.19x | no |
| wx264-vulkan-fast | 11.9 | 0.20x | no |
| x264-ultrafast | 518.6 | 8.64x | yes |
| x264-superfast | 336.9 | 5.61x | yes |
| x264-veryfast | 232.5 | 3.87x | yes |
| x264-faster | 130.9 | 2.18x | yes |
| x264-fast | 118.0 | 1.97x | yes |
| x264-medium | 105.2 | 1.75x | yes |
| x264-slow | 77.0 | 1.28x | yes |
| x264-slower | 33.6 | 0.56x | no |
| x264-veryslow | 21.1 | 0.35x | no |
| x264-placebo | 6.3 | 0.11x | no |

## Per-clip results

### forza_F5eKZa_9J2M_30s

| encoder | kbit/s | process fps | PSNR-Y | SSIM |
|---|---|---|---|---|
| wx264-vulkan-ultrafast | 6842.5 | 11.1 | 38.7555 | 0.985032 |
| wx264-vulkan-superfast | 6842.5 | 11.1 | 38.7555 | 0.985032 |
| wx264-vulkan-veryfast | 6553.0 | 11.2 | 38.7979 | 0.985749 |
| wx264-vulkan-faster | 6541.5 | 11.3 | 38.7776 | 0.987125 |
| wx264-vulkan-fast | 5939.9 | 12.1 | 38.8811 | 0.988140 |
| x264-ultrafast | 4832.5 | 566.6 | 43.9952 | 0.985397 |
| x264-superfast | 5279.4 | 388.7 | 45.6735 | 0.991344 |
| x264-veryfast | 5589.4 | 252.2 | 47.9504 | 0.993999 |
| x264-faster | 5679.8 | 146.2 | 48.6491 | 0.994515 |
| x264-fast | 5734.6 | 115.1 | 48.6493 | 0.994710 |
| x264-medium | 5385.1 | 106.9 | 48.7942 | 0.994781 |
| x264-slow | 5255.4 | 80.1 | 48.8375 | 0.994853 |
| x264-slower | 5135.9 | 37.3 | 48.8803 | 0.994905 |
| x264-veryslow | 5047.5 | 20.0 | 48.9779 | 0.994962 |
| x264-placebo | 5176.7 | 6.0 | 49.0175 | 0.995003 |

### forza_original_30s

| encoder | kbit/s | process fps | PSNR-Y | SSIM |
|---|---|---|---|---|
| wx264-vulkan-ultrafast | 6360.1 | 11.2 | 37.8809 | 0.977948 |
| wx264-vulkan-superfast | 6360.1 | 11.2 | 37.8809 | 0.977948 |
| wx264-vulkan-veryfast | 6733.1 | 10.9 | 37.9935 | 0.979566 |
| wx264-vulkan-faster | 6958.1 | 10.5 | 38.0144 | 0.982448 |
| wx264-vulkan-fast | 6467.1 | 11.4 | 38.2154 | 0.984813 |
| x264-ultrafast | 5658.4 | 562.0 | 44.0430 | 0.984751 |
| x264-superfast | 5161.7 | 387.8 | 46.2532 | 0.991542 |
| x264-veryfast | 5899.6 | 230.7 | 47.9727 | 0.993624 |
| x264-faster | 5812.7 | 135.5 | 48.0612 | 0.993510 |
| x264-fast | 5702.2 | 102.9 | 48.0014 | 0.993678 |
| x264-medium | 5457.4 | 99.1 | 48.2038 | 0.993840 |
| x264-slow | 5277.3 | 71.7 | 48.2071 | 0.993890 |
| x264-slower | 5226.5 | 34.7 | 48.2519 | 0.993972 |
| x264-veryslow | 5156.7 | 18.2 | 48.3524 | 0.994075 |
| x264-placebo | 5269.8 | 5.1 | 48.3410 | 0.994029 |

### forza_performance_original_210s

| encoder | kbit/s | process fps | PSNR-Y | SSIM |
|---|---|---|---|---|
| wx264-vulkan-ultrafast | 6003.3 | 10.3 | 27.1073 | 0.786946 |
| wx264-vulkan-superfast | 6003.3 | 10.3 | 27.1073 | 0.786946 |
| wx264-vulkan-veryfast | 6000.4 | 10.2 | 27.3504 | 0.797244 |
| wx264-vulkan-faster | 6001.5 | 10.7 | 27.9134 | 0.828497 |
| wx264-vulkan-fast | 5998.2 | 11.2 | 28.1423 | 0.840284 |
| x264-ultrafast | 6404.5 | 534.2 | 29.5773 | 0.855704 |
| x264-superfast | 6005.8 | 273.4 | 31.5620 | 0.917423 |
| x264-veryfast | 6684.8 | 165.8 | 32.2125 | 0.926239 |
| x264-faster | 6836.6 | 102.2 | 32.6203 | 0.929104 |
| x264-fast | 6712.2 | 90.6 | 32.5489 | 0.927911 |
| x264-medium | 6358.5 | 68.4 | 32.7084 | 0.928490 |
| x264-slow | 6147.0 | 56.3 | 32.6851 | 0.928483 |
| x264-slower | 6103.4 | 19.0 | 32.7193 | 0.929851 |
| x264-veryslow | 6063.0 | 8.9 | 32.8000 | 0.931207 |
| x264-placebo | 6086.4 | 3.0 | 32.5978 | 0.927480 |

### mandelbrot

| encoder | kbit/s | process fps | PSNR-Y | SSIM |
|---|---|---|---|---|
| wx264-vulkan-ultrafast | 5997.5 | 12.4 | 25.6937 | 0.915931 |
| wx264-vulkan-superfast | 5997.5 | 12.2 | 25.6937 | 0.915931 |
| wx264-vulkan-veryfast | 6043.2 | 11.9 | 26.0908 | 0.922891 |
| wx264-vulkan-faster | 6089.3 | 12.6 | 26.7290 | 0.944368 |
| wx264-vulkan-fast | 6052.5 | 12.6 | 26.7380 | 0.948495 |
| x264-ultrafast | 7248.9 | 553.7 | 30.4100 | 0.928764 |
| x264-superfast | 6668.3 | 379.4 | 33.3923 | 0.967919 |
| x264-veryfast | 6706.3 | 250.3 | 33.6752 | 0.971235 |
| x264-faster | 6971.8 | 150.0 | 33.9573 | 0.973653 |
| x264-fast | 6987.4 | 123.8 | 33.7133 | 0.974348 |
| x264-medium | 6937.3 | 111.3 | 33.8587 | 0.975760 |
| x264-slow | 6952.8 | 82.8 | 33.8693 | 0.976030 |
| x264-slower | 6810.1 | 33.2 | 33.8705 | 0.976494 |
| x264-veryslow | 6232.9 | 23.8 | 33.5444 | 0.976361 |
| x264-placebo | 6201.8 | 7.6 | 33.3652 | 0.975718 |

### testsrc2

| encoder | kbit/s | process fps | PSNR-Y | SSIM |
|---|---|---|---|---|
| wx264-vulkan-ultrafast | 5991.2 | 13.3 | 22.1809 | 0.899401 |
| wx264-vulkan-superfast | 5991.2 | 13.4 | 22.1809 | 0.899401 |
| wx264-vulkan-veryfast | 6001.5 | 13.1 | 22.2581 | 0.921901 |
| wx264-vulkan-faster | 6008.7 | 13.1 | 22.2601 | 0.947148 |
| wx264-vulkan-fast | 6010.3 | 12.8 | 22.3022 | 0.953968 |
| x264-ultrafast | 6842.3 | 474.0 | 38.5962 | 0.983749 |
| x264-superfast | 6417.5 | 344.0 | 39.5074 | 0.987003 |
| x264-veryfast | 6865.5 | 311.4 | 40.4548 | 0.989791 |
| x264-faster | 6621.8 | 148.3 | 40.3405 | 0.989090 |
| x264-fast | 6736.9 | 175.0 | 40.0527 | 0.988873 |
| x264-medium | 6719.7 | 172.8 | 40.2204 | 0.989507 |
| x264-slow | 6701.3 | 110.3 | 40.2133 | 0.989846 |
| x264-slower | 6615.9 | 49.0 | 40.7122 | 0.991161 |
| x264-veryslow | 6604.6 | 40.4 | 40.8075 | 0.991655 |
| x264-placebo | 6601.2 | 12.5 | 41.0658 | 0.992107 |

### warzone2020_verdansk_4k_src

| encoder | kbit/s | process fps | PSNR-Y | SSIM |
|---|---|---|---|---|
| wx264-vulkan-ultrafast | 6087.3 | 11.0 | 30.3229 | 0.875514 |
| wx264-vulkan-superfast | 6087.3 | 11.2 | 30.3229 | 0.875514 |
| wx264-vulkan-veryfast | 6115.8 | 10.7 | 30.8042 | 0.887253 |
| wx264-vulkan-faster | 6115.3 | 11.2 | 31.4103 | 0.912825 |
| wx264-vulkan-fast | 6148.2 | 11.2 | 31.7712 | 0.921489 |
| x264-ultrafast | 6125.7 | 421.3 | 34.2495 | 0.939656 |
| x264-superfast | 5598.2 | 247.9 | 36.3196 | 0.969062 |
| x264-veryfast | 6226.4 | 184.4 | 38.4020 | 0.977471 |
| x264-faster | 6032.0 | 103.4 | 38.6454 | 0.978305 |
| x264-fast | 6003.8 | 100.7 | 38.6996 | 0.978784 |
| x264-medium | 5988.6 | 72.6 | 39.1420 | 0.980299 |
| x264-slow | 5921.5 | 60.9 | 39.3173 | 0.981039 |
| x264-slower | 5782.6 | 28.3 | 39.3778 | 0.981610 |
| x264-veryslow | 5800.7 | 15.0 | 39.5592 | 0.982221 |
| x264-placebo | 5742.1 | 3.8 | 39.4699 | 0.981965 |

## Head-to-head at similar bitrates (per clip)

| clip | wx264-vulkan-fast (kbit/s) | PSNR-Y / SSIM | x264-fast (kbit/s) | PSNR-Y / SSIM | ΔPSNR-Y | ΔSSIM |
|---|---|---|---|---|---|---|
| forza_F5eKZa_9J2M_30s | 5940 | 38.88 / 0.9881 | 5735 | 48.65 / 0.9947 | -9.77 dB | -0.0066 |
| forza_original_30s | 6467 | 38.22 / 0.9848 | 5702 | 48.00 / 0.9937 | -9.79 dB | -0.0089 |
| forza_performance_original_210s | 5998 | 28.14 / 0.8403 | 6712 | 32.55 / 0.9279 | -4.41 dB | -0.0876 |
| mandelbrot | 6052 | 26.74 / 0.9485 | 6987 | 33.71 / 0.9743 | -6.98 dB | -0.0259 |
| testsrc2 | 6010 | 22.30 / 0.9540 | 6737 | 40.05 / 0.9889 | -17.75 dB | -0.0349 |
| warzone2020_verdansk_4k_src | 6148 | 31.77 / 0.9215 | 6004 | 38.70 / 0.9788 | -6.93 dB | -0.0573 |

## Notes

- wx264-vulkan `ultrafast` and `superfast` produced bit-identical settings (identical kbps/PSNR/SSIM in every clip).
- x264 presets slow <-> placebo keep quality flat (~+0.3 dB PSNR-Y total) but cost 4–40x speed; x264-fast is the knee of its curve (≈identical quality at 10–20x the speed of placebo).
- None of the wx264-vulkan presets reach real time on GTX 1660 SUPER (avg 10.5–13.3 process fps ≈ 0.18–0.22x realtime). The deficit is expected from missing trellis/8x8/AQ/lookahead/QP tuning; see docs/presets.md.
- x264 at `-ultrafast` already outruns realtime ~7-9x; wx264-vulkan is ~50x slower than that.
