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

## Speed breakthrough (PRs #494, #495)

Parallel CAVLC slice assembly (per-MB bodies + prefix sums + 64-byte chunked
emulation prevention) plus single preload of source YUV removed the entire
entropy bottleneck with **byte-identical streams**:

| Case | Entropy ms/f | Encode-loop fps | Process fps |
|---|---:|---:|---:|
| 1080p 60f before | 9.84 | 49 | 42 |
| 1080p 60f after | 0.11 | 100-108 | 65-69 |
| 1080p 300f after | 0.09 | 112 | 85 |
| 640x360 60f before | 2.79 | 215 | 133 |
| 640x360 60f after | 0.06 | 509 | 203 |

Single whole-frame IDR entropy: 289.8 ms -> 1.45 ms. SHA-256 of 1080p and
640p streams unchanged (1080p `A51E2A8070EF5A3C80D75C585418C3FCEB798F41971D93E203A2BAC42FF8F20C`).
Quality at 6000 kb/s unchanged: 1080p 300f VMAF 95.297, 640p 99.52.

Current object: 100 fps at 1080p is met in the encode loop (85 fps end-to-end
on 300 frames); remaining gap is perceptual quality on game content (~2.6 VMAF).

## Negative motion experiments (reverted)

On the 300f game clip at 6000 and 8000 kbps (exact `--fps 25 --gop 50 --qp 34`):

| Change | ΔPSNR-Y | ΔVMAF | Cost |
|---|---:|---:|---|
| SATD finalists 4→8 | +0.020 | −0.005 | spatial 1.19→1.76 ms, process 86.5→82.1 |
| SATD for subpel | +0.014 | −0.020 | motion 1.96→2.65 ms, process →68.6 |
| Subpel refinement of older references | +0.013 / −0.071 | +0.017 / −0.113 | process →72.5, loop →92 |
| MV cost penalty ×4 / ×0 | −0.226 / −0.014 | −0.093 / −0.020 | — |
| `--partitions 1` | −1.03 | −0.97 | entropy 0.09→7.04 ms |
| `--aq 1` | −0.63 | −1.28 | process →67 |

Reason: `matches[].cost` only drives 8×8 partitions (off in `slow`), and older
references cover ~0.76 % of P macroblocks. Remaining gap is structural —
no B-frames, CABAC or trellis/RDO/MB-tree. Baseline hash
`0be1a42df0209d5bbb367b5693fced48d0d75e95ff72a6feba9e6b198ee16d03`.

## Negative Intra4x4 experiment (reverted)

Implemented x264 I4 prediction modes (analyse.c:512-528/864-979,
predict.c:478-621) with SATD + λ·bs_size_ue, 16-mode/CBP/mb_type syntax and
commit-on-win after the I16 pass. Findings for a future attempt: write pred
modes in **scan order** (lumaOrder scan→raster); gate the win on
`total+bias < i16Cost`; keep all coefficients in scratch until the win; emulate
top-right at `bx==3` by replicating `t[3]` (analyse.c:886-888); availability of
above-left is `topAvail && leftAvail`; DDL is `f2(t[x+y],t[x+y+1],t[x+y+2])`.

Result: DC+H/V candidate set passed synthetic fixtures, but real 1080p and
640p streams failed `ffmpeg -err_detect explode` (`total_coeff=16`, invalid
intra mode, out-of-range chroma pred mode); adding directional modes also
caused 1-LSB recon mismatches. A serial I4 walk raised 1080p IDR residual from
~65.7 ms to ~352 ms. Reverted with zero gain; baseline hash and 45/46 ctest
restored. Structural gaps remain: no B-frames/CABAC/trellis/MB-tree in the
native path.

## Negative native-CABAC feasibility result (reverted)

Slice-level CABAC is *architecturally* parallel only at slice boundaries:
`x264-vulkan/common/cabac.c:53-181` threads `low/range/queue/outstanding/p`
plus 460 context states through every bin; carry can retroactively edit the
previous byte (`:69-99`). Slice headers reset arithmetic state and contexts
(`encoder.c:2791-2804`), so each slice can be one serial lane. Implemented with
one workgroup per paired two-row slice, x264 tables and CABAC syntax, but
reverted because:
- FFmpeg `-err_detect explode` rejected MB 0 (`intra mode -1`, `cabac_init_idc`
  overflow) — slice-header/bin integration not resolved.
- Even with correct syntax, 320x180/240 MB cost ~3.45 ms (~14 us/MB); 1080p
  whole-frame IDR would take ~117 ms/keyframe and P slices ~5-7 ms/frame.
  Measured 1080p/20f probe: entropy 27.05 ms, loop 23 fps vs baseline 113.
So native CABAC cannot meet the >=100 fps loop gate on this GPU; the exact
blocker is the per-slice serial arithmetic chain plus single-lane throughput.
Baseline and ctest 45/46 restored.

## Artifact-driven quantizer tuning (PR #501 accepted)

Frame-level + 16x16-MB error maps on the 300f game clip showed the gap is
fine detail, not blocking: boundary/mid-MB error ratios 1.075–1.097 match
x264 (1.070–1.072) and NVENC (1.093); VIF scale0 and ADM2 carry the deficit;
56% of P MBs were P-skip on the worst frames vs x264 ~16%, with flat plateaus
on source edges. Tested zero-cost variants at matched bitrate:

| Variant | 6000 VMAF | 8000 VMAF | Decision |
|---|---:|---:|---|
| baseline inter AC divisor 6 | 95.297 | 95.669 | — |
| inter RD lambda 0.55→0.40/0.25/0.15 | 95.34/95.32/95.32 | 96.68/–/– | ABR absorbs, rejected |
| coded-bits estimate −4 | 95.31 | 96.64 | rejected |
| inter AC divisor 4 | 95.414 | 96.723 | small gain |
| **inter AC divisor 3** | **95.390** | **96.777** | **accepted (PR #501)** |
| inter AC divisor 2 | 95.204 | 96.557 | overshoot, rejected |
| inter DC divisor 1 | 93.551 | 95.422 | rejected |
| low-freq AC 2 / high-freq 4 | 95.250 | 96.694 | rejected |
| P-skip factor 0.25/0.75/1.0 | 95.17/95.40/95.39 | –/96.75/96.73 | rejected |
| flat-intra gate *9→*25 | 95.389 | 96.777 | neutral, reverted |

Final at HEAD: 1080p/300f 6000k PSNR-Y 46.748 / VMAF 95.390 / 85 fps;
8000k PSNR-Y 48.241 / VMAF 96.777. x264 97.898 / 98.259 VMAF. Speed unchanged.

Also reverted: explicit x264 `ipratio=1.40` I-frame QP offset (-3) in
`shaders/layout.glsl`. At matched rate it *reduced* VMAF 95.30 -> 95.00
(6000) and 96.67 -> 96.47 (8000); the ABR controller already gives IDRs a
separate budget and the extra offset only starved P frames.

## Longer default GOP (PR #503)

A 2 s GOP (50f @25fps) over-spends on IDRs: with the parallel CAVLC path an
IDR is cheap in time (1.45 ms) but still expensive in bits, and P frames use
the budget more efficiently. Default changed to 5 s (125f @25fps,
300f @60fps); explicit --gop still overrides.

| Case | GOP 50 | GOP 125 | Delta |
|---|---:|---:|---:|
| 1080p/300f 6000 PSNR-Y | 46.748 | 47.003 | +0.255 |
| 1080p/300f 6000 VMAF | 95.390 | 95.719 | +0.329 |
| 1080p/300f 6000 process fps | 85 | 89 | +4 |
| 1080p/300f 6000 encode-loop fps | 112 | 120 | +8 |
| 1080p/300f 8000 VMAF | 96.669 | 96.957 | +0.288 |
| second 180f 1080p clip 6000 VMAF | 94.824 | 95.377 | +0.553 |

Damping the ABR P-frame QP step (scan.comp maxStep 2->1, gain 3->1.5)
was measured and reverted: VMAF 95.740 @6000 (+0.021) but 96.926 @8000
(-0.031). Per-frame QP swings 14->28->17->32 are largely content-driven;
damping did not improve matched-rate quality.

Current HEAD state: 1080p/300f --gop 125 --qp 14 6000k VMAF 95.719,
8000k VMAF 96.957; x264-slow at the same GOP ~97.9 / ~98.3. ctest 45/46.

## ABR debt-clamp breakthrough (PR #505)

The largest remaining quality loss was **not** a missing codec tool: it was
single-pass ABR debt starvation after a scene cut. On the 1080p game clip the
hard cut at frame 206 costs ~596 kB (20x a per-frame allocation). The old
scan.comp:42 fed the whole debt into correction = bitDebt / (fps/2),
driving im to its floor and ramping QP by +2/frame through the rest of the
GOP. Frames 218-227 were coded at QP 33-34 with PSNR-Y 43.98 dB / VMAF 91.41,
while x264 held QP ~19-22 and PSNR ~52 dB.

Fix: cap the per-frame correction at +/-allocation/4. Debt is repaid
gradually; no extra shader work (scan stage 0.006 ms).

| Metric @6000, 300f, --gop 125 | Before | After |
|---|---:|---:|
| PSNR-Y (all 300f) | 47.003 | 47.416 |
| VMAF (all 300f) | 95.719 | **96.149** |
| PSNR-Y frames 218-227 | 43.98 | 45.93 |
| VMAF frames 218-227 | 91.41 | **96.05** |
| Max per-frame QP in that range | 34 | 29 |
| Achieved kb/s | 6082 | 6178 (+1.6 %) |
| Encode-loop fps | 118.8 | 118.7 |
| Process fps | 89 | 89 |

@8000: VMAF 96.957 -> 97.113, PSNR-Y 48.409 -> 48.643, kb/s 8104 -> 8129.
Remaining gap to x264-slow at matched GOP: 1.84 VMAF @6000 (was 2.6),
1.20 VMAF @8000 (was 1.35). ctest 45/46, check.py 13/13, decode clean.

Rejected RC variants (measured, reverted): /3 clamp (+0.27 VMAF but only
+0.05 % rate); /6 clamp (+0.55 VMAF but +3.8 % rate); capping the debt
*increment* at one budget (+0.33 VMAF but +12 % rate); damping the QP step
to max 1/gain 1.5 on top of the clamp (+0.01 VMAF but +1.3 % more rate).
Starting QP 12 is worse; 14/16/18 are identical because --bitrate clamps to 14.

## 1080p60 target audit (NOT MET)

All earlier benchmarks used --fps 25. A true 60 fps source was located:
E:\__PROJECTS__\Optim\Warzone120Plan\assets\video\trailerA.mp4
(1920x1080, 60000/1001 fps, 840f), decoded losslessly to
uild/native60_1080.yuv. Benchmarked with the exact harness command:

`
python harness/benchmark.py --input build/native60_1080.yuv --out build/q60_6000 \
  --width 1920 --height 1080 --fps 60 --frames 840 --bitrate 6000 --gop 300 \
  --encoders wx264-slow x264-slow nvenc-p4 --vmaf
`

| @6000, true 60 fps | wx264 | x264-slow | NVENC P4 |
|---|---:|---:|---:|
| VMAF | **72.30** | 95.43 | 93.80 |
| PSNR-Y | 31.21 | 40.71 | 39.14 |
| achieved kb/s | 7472 (+25 %) | 5614 | 6138 |
| process fps | 90 | 78 | 383 |
| encode-loop fps | 122 | — | — |

Diagnosis: the ABR controller drifts to QP 51 on complex frames (frames
400-600 mean VMAF 55.7, minimum 25.7) while the static intro frames 0-130
sit at QP 12 / 511 B. Debt accumulated in the static section is discarded by
the +/-30-frame clamp, and the 1.5x aim cap prevents spending it later.
Fixed-QP encodes prove the codec itself is intact: QP 18/22/26 give VMAF
99.21/98.95/98.41; QP 40 (=5872 kb/s, matched rate) gives VMAF 80.39.
So the ABR instability costs ~8 VMAF at matched rate.

Same game frames labelled 60 fps (300f, --gop 300):
@6000 VMAF 88.70 / 6938 kb/s (+16 %) / 92 fps vs x264 95.72;
@8000 VMAF 92.40 / 8704 kb/s (+9 %) / 92 fps vs x264 96.87.

Rejected 60 fps RC variants (measured, reverted): longer debt window (30->120
and 30->150 frames), wider aim cap (1.5x->4x), asymmetric correction clamp,
QP-step damping (step 1/gain 1.5) and faster response (step 4/gain 6). All
either left VMAF unchanged or increased rate overshoot. Process fps stays
90-92 (<100); encode loop remains ~120 fps (>100).

Conclusion: the 1080p60 target at 6000/8000 kb/s is **not met**. At 60 fps the
per-frame budget is 2.4x smaller than at 25 fps, exposing the missing
B-frames/CABAC/RDO/MB-tree; the remaining gap is structural, not a parameter
that can be tuned without a new encoder path.
