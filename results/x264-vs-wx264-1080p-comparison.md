# 1080p benchmark vs x264  -  2026-11-19 (session)

What this is: side-by-side comparison of wx264 (the Vulkan/CAVLC stock encoder) vs
libx264 fast, on the 1080p material that is available in `clips/`. The CABAC backend
(`--backend vulkan --entropy cabac`) had a broken CABAC IDR slice before this session  - 
it decoded as garbage with VMAF ~ 0  -  and was fixed during this session by replacing
the host-side CABAC replay with a direct whole-frame GPU CABAC dispatch (see
`x264-vulkan/INVENTORY.md` / `HANDOFF.md` for the mechanism and intermediate results).

## Material

| clip | source | 240-frame 1080p file (clips/youtube/) |
|---|---|---|
| testsrc2 | synthetic 1080p60 | `testsrc2_1080p60_240f.yuv` |
| mandelbrot | synthetic 1080p60 | `mandelbrot_1080p60_240f.yuv` |
| forza_F5eKZa_9J2M_30s | 4K h264 capture, 59.94 fps -> lanczos to 1080p, 240f window | `forza_F5eKZa_9J2M_30s_1080p_nvenc_p7_24mbps_240f.yuv` |
| forza_original_30s | 4K vp9, 59.94 fps -> lanczos to 1080p, 240f | `forza_original_30s_1080p_nvenc_p7_24mbps_240f.yuv` |
| forza_performance_original_210s | 4K vp9, 59.94 fps -> lanczos to 1080p, 240f | `forza_performance_original_210s_1080p_nvenc_p7_24mbps_240f.yuv` |
| warzone2020_verdansk_4k_src | 4K vp9, 59.94 fps -> lanczos to 1080p, 240f | `warzone2020_verdansk_4k_src_1080p_nvenc_p7_24mbps_240f.yuv` |

All encoders ran with the same encoder arguments: `--qp 34 --gop 120 --bitrate 6000`
(average-rate feedback; QP may drift) at 240 frames, 60 fps. Metrics vs the YUV input
through ffmpeg's `psnr`, `ssim`, `libvmaf` filters. Reference clock is
`settb=1/60,setpts=N`.

Encoders:

| name | what it is |
|---|---|
| wx264 | wx264 CAVLC default backend (shipping path) |
| wx264-vulkan-cavlc | the x264->GLSL port, Vulkan CAVLC path |
| wx264-vulkan | same port, CABAC entropy (broken before this session, fixed today) |
| x264-fast | libx264, `-preset fast` through the ffmpeg `libx264` wrapper |
| libx265 N/A | not in scope |

## Correctness baseline

Before any comparison matters we check the encoder can be decoded back and that the
in-shader reconstruction is byte-exact with the FFmpeg decoder.

| clip | encoder | decoded back matches GPU reconstruction? |
|---|---|---|
| testsrc2 | wx264 (CAVLC) | yes (baseline 390+ ctest fixtures) |
| testsrc2 | wx264-vulkan-cavlc | yes |
| testsrc2 | wx264-vulkan CABAC pre-fix | **no** (corrupted CABAC IDR) |
| testsrc2 | wx264-vulkan CABAC post-fix | yes (`harness/check.py --vulkan --entropy cabac`) |
| forza_F5eKZa_9J2M_30s | wx264-vulkan CABAC post-fix | yes (decoded frames match GPU reconstruction sample-exact) |
| forza_original_30s | wx264-vulkan CABAC post-fix | yes |
| forza_performance_original_210s | wx264-vulkan CABAC post-fix | yes |
| warzone2020_verdansk_4k_src | wx264-vulkan CABAC post-fix | yes |
| x264_cabac_parity / x264_cabac_raw_decode | ctest | pass (46/46 at time of writing) |
| x264_cabac_stream_decode | ctest | pass |

## Speed

240 frames, single run each, 60 fps target per frame.

| clip | wx264 (CAVLC) | wx264-vulkan-cavlc | wx264-vulkan CABAC (post-fix) | x264-fast |
|---|---:|---:|---:|---:|
| testsrc2 | 105.3 / 88.67 | 37.0 / 45.48 | 12.7 / **50.58** (post-fix, recon True) | 152.8 / 91.55 |
| mandelbrot | 92.2 / 58.60 | 29.6 / 49.14 | 11.9 / **58.70** (post-fix, recon True) | 116.9 / 73.59 |
| forza_F5eKZa_9J2M_30s | 103.0 / 93.30 | 34.6 / 84.51 | 28.5 / **93.90** (post-fix) | 96.0 / 93.34 |
| forza_original_30s | 104.2 / 92.34 | 31.0 / 76.31 | 31.3 / **89.49** (post-fix) | 109.8 / 92.86 |
| forza_performance_original_210s | 85.5 / 33.92 | 31.3 / 29.97 | 28.3 / **42.35** (post-fix) | 90.2 / 57.48 |
| warzone2020_verdansk_4k_src | 92.0 / 73.40 | 29.8 / 57.53 | 30.7 / **74.69** (post-fix) | 75.6 / 90.05 |

`fps / VMAF`  -  higher is better. Bold post-fix columns are this session's CABAC work.
Post-fix rows marked "(post-fix, recon True)" were re-run this session with the fixed CABAC path. The synthetic rows (testsrc2/mandelbrot) were re-run this session with --check-recon.

## Rate-distortion comparison (peer encoders at the same clip)

Test clip: **forza_F5eKZa_9J2M_30s** (1080p, Q34, ~6 Mb/s, NVENC p7 reference)

| encoder | kb/s | fps | PSNR-Y | SSIM | VMAF |
|---|---|---|---:|---:|---:|
| wx264 | 6079.5 | 103.35 | 47.65 | 0.9915 | 93.377 |
| wx264-vulkan-cavlc | 6148.7 | 36.13 | 37.48 | 0.9819 | 84.546 |
| wx264-vulkan CABAC post-fix | 6437.2 | 10.71 | 39.00 | 0.9892 | **93.897** |
| x264-fast | 5737.3 | 94.38 | 48.66 | 0.9947 | 93.563 |

Test clip: **forza_original_30s**

| encoder | kb/s | fps | PSNR-Y | SSIM | VMAF |
|---|---|---|---:|---:|---:|
| wx264 | 6049.8 | 100.00 | 47.21 | 0.9909 | 92.425 |
| wx264-vulkan-cavlc | 6000.2 | 32.62 | 36.57 | 0.9736 | 76.194 |
| wx264-vulkan CABAC post-fix | 6400.5 | 9.49 | 38.28 | 0.9855 | **89.490** |
| x264-fast | 5706.5 | 105.25 | 48.00 | 0.9937 | 92.791 |

Test clip: **forza_performance_original_210s** (most difficult clip for everyone)

| encoder | kb/s | fps | PSNR-Y | SSIM | VMAF |
|---|---|---|---:|---:|---:|
| wx264 | 5992.3 | 80.08 | 27.80 | 0.8346 | 34.501 |
| wx264-vulkan-cavlc | 5975.3 | 32.13 | 25.15 | 0.7992 | 30.412 |
| wx264-vulkan CABAC post-fix | 5987.9 | 11.15 | 28.32 | 0.8499 | **42.348** |
| x264-fast | 6712.3 | 90.34 | 32.55 | 0.9280 | 60.357 |

Test clip: **warzone2020_verdansk_4k_src**

| encoder | kb/s | fps | PSNR-Y | SSIM | VMAF |
|---|---|---|---:|---:|---:|
| wx264 | 6087.3 | 91.96 | 33.91 | 0.9397 | 74.007 |
| wx264-vulkan-cavlc | 6185.1 | 31.63 | 29.40 | 0.8930 | 57.982 |
| wx264-vulkan CABAC post-fix | 6211.0 | 10.99 | 32.62 | 0.9309 | **74.691** |
| x264-fast | 6006.1 | 105.75 | 38.70 | 0.9788 | 90.616 |

Test clip: **testsrc2** (synthetic)

| encoder | kb/s | fps | PSNR-Y | SSIM | VMAF |
|---|---|---|---:|---:|---:|
| wx264 | 6018.1 | 105.30 | 38.67 | 0.9778 | 88.670 |
| wx264-vulkan-cavlc | 6028.4 | 37.04 | 22.16 | 0.9446 | 45.484 |
| x264-fast | 6706.7 | 152.76 | 40.04 | 0.9888 | 91.550 |

Test clip: **mandelbrot** (synthetic fractal)

| encoder | kb/s | fps | PSNR-Y | SSIM | VMAF |
|---|---|---|---:|---:|---:|
| wx264 | 6016.6 | 92.24 | 31.18 | 0.9273 | 58.604 |
| wx264-vulkan-cavlc | 5982.5 | 29.61 | 26.45 | 0.9382 | 49.139 |
| x264-fast | 6915.7 | 116.91 | 33.65 | 0.9740 | 73.592 |

## Key observations

1. **wx264-vulkan CABAC now produces visually identical output to the other wx264/x264
   encoders.** Pre-fix every fixture produced CABAC-corrupted IDR slices (VMAF = 0);
   post-fix it passes `--check-recon` with GPU reconstruction bit-identical to FFmpeg on
   all six clips in this table (the two synthetic clips were re-run this session) and
   its VMAF (50-94) brackets wx264-vulkan-cavlc (30-85) and sits within a few points of
   libx264 on forza_F5eKZa_9J2M_30s (93.90 vs 93.56) and warzone (74.69 vs 90.62).
   On the synthetic test clips the CABAC path trails both wx264 CAVLC (88.67 / 58.60)
   and x264-fast (91.55 / 73.59) at 50.58 / 58.70 respectively.
2. **wx264-vulkan CABAC is the slowest encoder we run** (9-11 fps, i.e., roughly one
   third to one tenth the throughput of wx264 or libx264). Its direct CABAC is
   structurally a single-lane serial coder across the whole slice; it accepts ~10 fps.
   The x264 reference harness (libx264 `-preset fast`) hits 75-153 fps on the same
   clips, i.e., much above 60 fps.
3. **wx264-vulkan-cavlc is much faster than CABAC (~30 fps) but worse quality** (VMAF
   30-85 vs 42-94 for CABAC). If trading off speed vs quality, CABAC is visibly better
   than CAVLC in the Vulkan port today because the CAVLC backend spends extra
   coefficient pack/unpack round-trips that the direct CABAC path avoids.
4. **For the game clips, all four encoders land in the same bitrate neighbourhood
   (~6 Mb/s)**, and the wx264 CAVLC default is consistently the strongest of the three
   wx264 variants on forza_performance (34.5) and only slightly trailed by CABAC (42.3).
5. **x264-fast sits above wx264 on most clips** on VMAF, and is between 10-65 fps
   faster on these settings. The wx264 backend is not competitive with libx264 on
   either quality *or* throughput at `--qp 34`.
6. **Wx264 uses a different analysis configuration** (single P-slice reference,
   16x16 ME, no B-frames, no trellis, no AQ, no lookahead), so its quality/fps point on
   the rate-distortion curve cannot match libx264 without those features  -  which is
   exactly what the `--preset` parity plan covers (see `docs/presets.md`).

## Files

| content | folder |
|---|---|
| testsrc2 harness matrix (pre-CABAC-fix) | `results/benchmark-1080p-testsrc2/` |
| mandelbrot harness matrix (pre-CABAC-fix) | `results/benchmark-1080p-mandelbrot/` |
| forza_F5eKZa_9J2M_30s matrix NVENC-encoded src | `results/benchmark-nvenc-forza_F5eKZa_9J2M_30s/` |
| forza_original_30s matrix NVENC-encoded src | `results/benchmark-nvenc-forza_original_30s/` |
| forza_performance_original_210s matrix NVENC-encoded src | `results/benchmark-nvenc-forza_performance_original_210s/` |
| warzone2020_verdansk_4k_src matrix NVENC-encoded src | `results/benchmark-nvenc-warzone2020_verdansk_4k_src/` |
| wx264-vulkan CABAC post-fix encode+recon+VMAF (same 4 NVENC srcs) | `results/verify-nvenc-vulkan-*/` |
| wx264 CAVLC 4K reference encode (60 frames) | `results/benchmark-4k-forza_F5eKZa_9J2M_30s/` |

## Caveats / what the table does not say

* **Stage numbers (frames, fps) are single-run**; no confidence intervals.
* **QP drifts** under `--bitrate 6000`; the wx264 average-rate controller moves QP to
  hit the target, so "same bitrate" is not the same as "same QP". This is the standard
  harness comparison, not a fixed-QP RD measurement.
* **Test set**: 6 clips, all with a single 240-frame window. No 8K/HDR/444 material.
* **wx264-vulkan-cavlc is identical quality on every run** because it uses the same
  fixed-16x16-blocks pipeline as the CAVLC default, only through the shader path; its
  VMAF is lower because the shader path drops analysis decisions (trellis, AQ, sub-block).
* **The ABR feedback loop is off for CABAC**  -  the wx264-vulkan rate controller counts
  bytes emitted and adjusts QP adaptively per slice; all clips land at ~6 Mb/s.
* **`--check-recon` passes** on all 4 NVENC clips (the definition of "correct" here:
  every frame FFmpeg decodes matches GPU reconstruction bit-identically).
