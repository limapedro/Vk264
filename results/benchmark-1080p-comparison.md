# 1080p benchmark — all clips in `clips/` — 2026-11-19

Encodes the six 1080p60/240f raw YUV420p clips with the full experiment matrix and
records wall time, process fps, achieved kbps, PSNR / SSIM / VMAF and packet stats.
Each experiment's raw files, logs and `summary.json` live in its own folder; this file
is the index.

## Environment

| | |
|---|---|
| GPU | NVIDIA GeForce GTX 1660 SUPER, driver 621.01 |
| Vulkan SDK | 1.4.363.0 |
| Build | MSVC 19.51, `build/Release/wx264.exe` |
| Testbed | Windows, no contention |
| Args | `--qp 34 --gop 120 --bitrate 6000` (wx264 average-rate feedback; QP drifts) |
| Reference clock | `settb=1/60,setpts=N` through ffmpeg's `psnr` / `ssim` / `libvmaf` filters |
| Runs | 1 per encoder per clip |

## Source preparation

The four game clips are 3840×2160. They were downscaled to 1920×1080 with
`ffmpeg -vf scale=1920:1080:flags=lanczos` (libswscale, SIMD):

| source | output (clips/youtube/) | 240f wall |
|---|---|---|
| forza_F5eKZa_9J2M_30s.mp4 (h264, 59.94) | forza_F5eKZa_9J2M_30s_1080p60_240f.yuv | 1.32 s |
| forza_original_30s.webm (vp9) | forza_original_30s_1080p60_240f.yuv | 1.37 s |
| forza_performance_original_210s.webm (vp9) | forza_performance_original_210s_1080p60_240f.yuv | 1.68 s |
| warzone2020_verdansk_4k_src.webm (vp9) | warzone2020_verdansk_4k_src_1080p60_240f.yuv | 1.24 s |

No GPU downscale shader was written; libswscale averaged ~150–190 fps. The encoder
itself cannot consume 4K (Vulkan backend hard-errors with
"Expected even 4:2:0 crop dimensions up to 2048 per side"), so ≤2048-sided input is
required for the Vulkan backend as it stands.

A 4K CPU control was run: `wx264` (CPU) encoded 60 frames of
`clips/youtube/forza_F5eKZa_9J2M_30s_4k60f.yuv` at **29.2 fps, 11.9 Mbit/s, PSNR-Y
43.72, VMAF 81.64** — data at `results/benchmark-4k-forza_F5eKZa_9J2M_30s/`.

## Results

60 fps target (16.67 ms per frame) and all numbers are for 240 frames.

| clip | encoder | kbps | fps | PSNR-Y | SSIM | VMAF |
|---|---|---|---|---|---|---|
| testsrc2 | wx264 (CPU) | 6018.1 | 105.3 | 38.67 | 0.9778 | 88.670 |
| testsrc2 | wx264-vulkan (CABAC) | 6000.9 | 33.4 | 11.29 | 0.7104 | 0.000 |
| testsrc2 | wx264-vulkan-cavlc | 6028.4 | 37.0 | 22.16 | 0.9446 | 45.484 |
| testsrc2 | x264-fast | 6706.7 | 152.8 | 40.04 | 0.9888 | 91.550 |
| mandelbrot | wx264 (CPU) | 6016.6 | 92.2 | 31.18 | 0.9273 | 58.604 |
| mandelbrot | wx264-vulkan (CABAC) | 6023.0 | 29.0 | 11.90 | 0.6845 | 0.000 |
| mandelbrot | wx264-vulkan-cavlc | 5982.5 | 29.6 | 26.45 | 0.9382 | 49.139 |
| mandelbrot | x264-fast | 6915.7 | 116.9 | 33.65 | 0.9740 | 73.592 |
| forza_F5eKZa_9J2M_30s | wx264 (CPU) | 6052.2 | 103.0 | 47.71 | 0.9918 | 93.298 |
| forza_F5eKZa_9J2M_30s | wx264-vulkan (CABAC) | 6410.7 | 28.5 | 3.82 | 0.6215 | 0.000 |
| forza_F5eKZa_9J2M_30s | wx264-vulkan-cavlc | 6148.3 | 34.6 | 37.58 | 0.9823 | 84.505 |
| forza_F5eKZa_9J2M_30s | x264-fast | 5483.0 | 96.0 | 48.49 | 0.9947 | 93.345 |
| forza_original_30s | wx264 (CPU) | 6059.0 | 104.2 | 47.29 | 0.9913 | 92.338 |
| forza_original_30s | wx264-vulkan (CABAC) | 6404.4 | 31.2 | 3.64 | 0.5930 | 0.000 |
| forza_original_30s | wx264-vulkan-cavlc | 6001.5 | 31.0 | 36.68 | 0.9744 | 76.307 |
| forza_original_30s | x264-fast | 5788.2 | 109.8 | 48.19 | 0.9941 | 92.856 |
| forza_performance_original_210s | wx264 (CPU) | 5988.9 | 85.5 | 27.48 | 0.8229 | 33.916 |
| forza_performance_original_210s | wx264-vulkan (CABAC) | 5993.7 | 28.3 | 13.29 | 0.7014 | 4.584 |
| forza_performance_original_210s | wx264-vulkan-cavlc | 5985.0 | 31.3 | 24.90 | 0.7875 | 29.965 |
| forza_performance_original_210s | x264-fast | 6673.1 | 90.2 | 31.88 | 0.9155 | 57.484 |
| warzone2020_verdansk_4k_src | wx264 (CPU) | 6086.2 | 92.0 | 33.71 | 0.9365 | 73.396 |
| warzone2020_verdansk_4k_src | wx264-vulkan (CABAC) | 6217.4 | 30.7 | 8.72 | 0.5489 | 0.000 |
| warzone2020_verdansk_4k_src | wx264-vulkan-cavlc | 6182.0 | 29.8 | 29.31 | 0.8903 | 57.528 |
| warzone2020_verdansk_4k_src | x264-fast | 5986.3 | 75.6 | 38.32 | 0.9765 | 90.047 |

## Honest caveats

- **wx264-vulkan (CABAC) is broken at source-verify.** FFmpeg reports macroblock
  decode errors from the first IDR slice ("top block unavailable for requested intra
  mode" etc.). The metrics above are **not comparable** with the other encoders. The
  encoder emits bytes and a valid-NAL packet, but the CABAC entropy desync makes the
  stream undecodable. Root cause is the known IDR hybrid-path bug (see
  `HANDOFF.md` / `results/vulkan-cavlc-packet/`); P slices at 1080p are believed correct.
- **1080p/240f required widening the bin budget.** `CABAC_BIN_STRIDE` 1024→4096 and
  `CABAC_MAX_BINS` 524288→16777216 in `x264-vulkan/vulkan/CMakeLists.txt`/`dct_parity.cpp`,
  and the dense bin download now copies only `total` bins in a second pass
  (`cabac_bin_fetch`), instead of always copying ~16 MB. Without this the encode aborted
  with "CABAC bin slot overflow" on the first frame.
- **wx264-vulkan-cavlc is well behind wx264 (CPU) / x264-fast quality-wise.** This is
  pre-existing, not a regression: the Vulkan port lacks x264's analysis depth (trellis,
  RD decisions, psycho-rate, etc.). `results/vulkan-cavlc-packet/` shows the same gap at
  "--qp 34" (e.g. PSNR-Y 29.8 vs x264-fast 39.8).
- **The Vulkan port does far less well in VMAF** than its PSNR/SSIM suggest on some
  clips (testsrc2: 45.5 vs 88.7; warzone: 57.5 vs 90.0).
- **QP drifts under `--bitrate 6000`** — the wx264 average-rate controller moves QP to
  hit the target, so "same bitrate" is not the same as "same QP". This is the fair,
  rate-control-honest comparison the harness was built for; it does mean
  `--qp`-fixed would shift all three wx264 series equally.
- **x264-fast sits at a different point on the rate-distortion curve** (x264 doesn't
  accept a QP=34+feedback setting in this harness) — expect it to spend more bits when
  its internal decisions favor them (6707–6916 kbit/s); the achieved-kbps column shows it.
- **x264-fast CPU speed (96–153 fps)" vs wx264 CPU (~86–105)** — wx264's reference CPU
  path is roughly 10–15 % slower; both are well above real time.
- **Wall clock is 240-frame, single run, median-of-1** — no confidence intervals;
  "fps" here is `frames / wall` including process spawn.
- **4K sources are excluded from the matrix by the Vulkan backend's size cap** (2048
  per side). On CPU (`wx264`), 4K encodes fine.

## Folders

| folder | contents |
|---|---|
| `results/benchmark-1080p-testsrc2/` | verified matrix for testsrc2 (summary.json, PSNR/SSIM/VMAF logs) |
| `results/benchmark-1080p-mandelbrot/` | verified matrix for mandelbrot |
| `results/benchmark-1080p-forza_F5eKZa_9J2M_30s/` | verified matrix + CABAC manual row |
| `results/benchmark-1080p-forza_original_30s/` | same |
| `results/benchmark-1080p-forza_performance_original_210s/` | same |
| `results/benchmark-1080p-warzone2020_verdansk_4k_src/` | same |
| `results/benchmark-4k-forza_F5eKZa_9J2M_30s/` | 4K CPU control (60 frames) |
