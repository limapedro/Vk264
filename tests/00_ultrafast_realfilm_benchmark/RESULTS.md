# 00 — Ultrafast real-footage benchmark — RESULTS

Source: `warzone2020_verdansk_4k_src_1080p_nvenc_p7_24mbps.mp4`, 1920x1080,
660 frames, source ~59.94 fps. All runs: QP23, 60 fps nominal.

## Playback files (correctly timed — use these)

| File | Frames | Duration | Frame rate |
|---|---|---|---|
| `vk264_gop1_full.mp4` | 660 | 11.00 s | 60/1 |
| `x264_gop1_full.mp4` | 660 | 11.00 s | 60/1 |

**Why the raw .h264 files played at half speed:** raw H.264 elementary streams
carry no container timing; ffprobe/players default to 25 fps. Muxing with
`ffmpeg -fflags +genpts -r 60 -i stream.h264 -c copy out.mp4` fixes it. The
`vk264_gop1_30f.h264` file was a 30-frame isolation test (0.5 s) — the full
660-frame version is `vk264_gop1_full.mp4`.

## Clean benchmark (GOP=1, all-IDR — both streams decode with 0 errors)

| Metric | vk264 ultrafast | x264 ultrafast |
|---|---|---|
| Encode time (660f) | 105.89 s | 1.01 s |
| Encoding throughput | **6.23 fps** | **653.88 fps** |
| Stream size | 113,966,428 B | 113,964,331 B |
| Size ratio | 1.00002 | — |
| Decode errors | 0 | 0 |

Quality (vk264 vs decoded source): **PSNR Y 43.93 dB / U 47.62 / V 50.46**
(avg 45.03); **SSIM All 0.9928** (Y 0.9921).

Size is effectively identical to x264 (0.0018% difference), but the streams
are NOT byte-exact at these settings: first SEI-stripped mismatch at offset 12
(frame 0 slice header). Byte-exact real-footage parity is proven only at the
harness's specific settings (issue #30), not at QP23/60 fps here.

## Critical finding: GOP=60 P-slice corruption

The first run at `--gop 60` produced a vk264 stream that ffmpeg cannot decode
cleanly (`cbp too large`, `mb_type too large in P slice`, etc.), while x264's
GOP60 stream decodes with zero errors. Isolation (30 frames, QP23):
- GOP=1  : 5,682,390 B, decode CLEAN
- GOP=60 : 1,599,062 B, decode ERRORS

This matches issue #50 (gop>1 P-slice payload divergence). All quality metrics
above therefore use GOP=1. See `gop60_note.txt`.

## Files

- `run_benchmark.ps1` — GOP60 benchmark (revealed corruption)
- `run_benchmark_gop1.ps1` — clean GOP1 benchmark
- `summary.json`, `summary_gop1.json` — machine-readable results
- `compare_bytes.py` — byte comparison
- `vk264_gop1_full.mp4`, `x264_gop1_full.mp4` — corrected playback files
- `psnr_vk_stderr.txt`, `ssim_vk_stderr.txt` — metric logs
- `gop60_note.txt` — corruption analysis

## Bottom line

The encoder produces **visually high-quality, correctly-sized, decodable
streams at GOP=1**, but is ~105x slower than x264 ultrafast on this GPU
(GTX 1660 SUPER) and the P-slice path is broken for GOP>1. Both are active
parity/performance gaps, not measurement artifacts.
