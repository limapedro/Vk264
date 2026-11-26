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

## Current after intra RD + ABR QP clamp (HEAD 3e19a1f)

| Clip | Encoder | kb/s | PSNR-Y | SSIM | VMAF |
|---|---|---:|---:|---:|---:|
| youtube 640x360/60f | wx264-slow | 6164 | 51.35 | 0.99537 | 99.52 |
| youtube 640x360/60f | x264-slow | 5731 | 54.62 | 0.99836 | 99.52 |
| youtube 640x360/60f | nvenc-p4 | 6526 | 53.96 | 0.99797 | 99.50 |
| real 1920x1080/60f | wx264-slow | 6529 | 45.87 | 0.99350 | 93.96 |
| real 1920x1080/60f | x264-slow | 4998 | 49.91 | 0.99743 | 96.94 |
| real 1920x1080/60f | nvenc-p4 | 6410 | 50.41 | 0.99709 | 96.66 |

VMAF is essentially tied on the small clip; ~3 VMAF behind on real 1080p.
Reports: build/q_final3/summary.json, build/native1080_final/summary.json.

## 8000 kbps target and sustained 300-frame runs

Real 1080p source `D:\Videos\ssstwitter.com_1732160055221.mp4`:

| Case | Encoder | kb/s | Process fps | PSNR-Y | SSIM | VMAF |
|---|---|---:|---:|---:|---:|---:|
| 60f @6000 | wx264-slow | 6529 | 41 | 45.87 | 0.99350 | 93.96 |
| 60f @6000 | x264-slow | 4998 | 52 | 49.91 | 0.99743 | 96.94 |
| 60f @6000 | nvenc-p4 | 6410 | 154 | 50.41 | 0.99709 | 96.66 |
| 60f @8000 | wx264-slow | 8613 | 41 | 47.76 | 0.99515 | 95.92 |
| 60f @8000 | x264-slow | 6606 | 55 | 51.32 | 0.99795 | 97.52 |
| 60f @8000 | nvenc-p4 | 8444 | 157 | 51.60 | 0.99758 | 97.25 |
| 300f @6000 | wx264-slow | 6026 | 63.5 | 46.62 | 0.99321 | 95.30 |
| 300f @6000 | x264-slow | 5603 | 65.3 | 51.50 | 0.99779 | 97.90 |
| 300f @8000 | wx264-slow | 8031 | 59.7 | 48.07 | 0.99436 | 96.67 |
| 300f @8000 | x264-slow | 7533 | 58.4 | 52.92 | 0.99827 | 98.26 |

Preset sweep at 6000 kbps/60f: wx264-fast/medium/slow/veryslow VMAF
93.84/93.93/93.96/93.95, process fps 43.9/45.0/44.6/43.1 — preset effort
has almost no effect on speed because the entropy stage dominates
(`gpu_stages_mean_ms.entropy = 10.57 ms`, motion 1.94, residual 2.68,
deblock 1.78; encode loop ~47.5 fps). Reaching 100 fps requires an
entropy-stage optimization, not a cheaper motion preset.

Single-frame profiling (`build/perf_1`, `build/perf_2`) isolates it further:

| Frames | Entropy ms | Residual ms | Motion ms | Encode-loop fps |
|---:|---:|---:|---:|---:|
| 1 (IDR) | 289.8 | 65.7 | 0.0 | 2.8 |
| 2 (IDR+P) | 136.3 | 26.1 | 1.0 | 6.0 |

The IDR is a single whole-frame slice (`src/gpu.cpp` / `shaders/layout.glsl`)
so one workgroup invocation emits all 8160 MB bodies serially. CAVLC I slices
have no skip-run dependency between MBs, so the bodies could be emitted in
parallel and concatenated — the next performance work item.

## Negative quality experiments (reverted)

All measured on the real 1080p clip unless noted; wx264-slow, 60 f, VMAF:

| Experiment | Result | Decision |
|---|---|---|
| IDR QP step clamp ±4 → ±10 (`shaders/scan.comp`) | frame-1 PSNR unchanged (40.64) | reverted |
| chroma QP offset −2 + PPS signalling | 640p PSNR 48.13→48.05, SSIM/VMAF neutral | reverted |
| ABR RC constants from later tree (fps/3, 5/4 aim cap, ±10 debt, 4.0 step) | 1080p 6000 VMAF 93.96→93.75; 8000 95.92→95.81 | reverted |
| variance AQ on (`--aq 1`) | 1080p 6000 VMAF 93.96→93.06 | not adopted |
| initial QP 8/4 at 6000/8000 | bitrate overshoot 7.4–9.6 Mb/s, VMAF lower | not adopted |
| SATD candidates 4→8 | VMAF 93.960→93.962 | negligible |
| 8×8 inter transform | decode breakage on multi-row fixtures; no clean gain | reverted |
| exact CAVLC bit-cost RD (validated 15/15 oracle fixtures) | best +0.123 dB / +0.344 VMAF, below gate; residual 1.8→2.7 ms | reverted |

The only accepted quality changes so far: x264 V/H/DC/Plane I16 prediction with
SATD+λ bits and x264's intra quantizer deadzone (PR #488), and ABR starting-QP
clamp to 14 (PR #489). Together they took the 640p clip from 48.13 dB/0.99369
to 51.35 dB/0.99537 at the same ~6.1 Mb/s.
