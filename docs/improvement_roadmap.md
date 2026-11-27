# Vk264 improvement roadmap — ranked by effort vs expected VMAF gain

Status: HEAD `d24dc87`. This document is the single source of truth for what
has been measured, what has landed, what was tried and rejected, and what is
worth doing next. Every number below comes from a command in
`docs/quality_baseline.md`, this file, or the retained sub-agent reports under
`C:\Users\multi\AppData\Local\Temp\opencode\`.

## 1. Where we are now (measured)

GTX 1660 SUPER, Vulkan compute-shader H.264 encoder, CAVLC, I/P only,
5 refs, radius 24, paired 2-row P slices, whole-frame IDR, no B-frames,
no CABAC, no trellis/RDO, no MB-tree, no lookahead.

### Throughput (1080p, 300 frames, 6000 kb/s, `--gop 125 --qp 14`)
| Metric | Value |
|---|---:|
| Encode loop | 132–140 fps (after PR #515 ring) |
| End-to-end process | 93–103 fps (median ~99) |
| GPU stage sum | 6.5–7.2 ms/frame |
| IDR residual (single keyframe) | 1.9 ms (28x faster since PR #513) |
| Entropy | ~0.08 ms/frame |

### Quality on the 25 fps game clip (300f, `--gop 125 --qp 14`, VMAF v0)
| Rate | wx264 | x264-slow | NVENC P5 |
|---|---:|---:|---:|
| 6000 | VMAF 96.15, PSNR 47.42, 6178 kb/s | 97.99 | 97.46 |
| 8000 | VMAF 97.11, PSNR 48.64, 8129 kb/s | 98.31 | 97.87 |

### Quality on the user's 1080p60 desktop-capture content
`python harness/benchmark.py --input build/obs_new_src.yuv --out build/matrix_<br> --width 1920 --height 1080 --fps 60 --frames 300 --bitrate <br> --gop 120 --encoders wx264-medium x264-medium nvenc-p5 --vmaf`

| Target | wx264 ABR | x264-medium | NVENC P5 | wx264 fixed-QP ceiling |
|---:|---:|---:|---:|---:|
| 2000 | 18.64 Mb/s, VMAF 96.63 ❌ | 2.07 Mb/s, **84.31** | 1.93 Mb/s, 80.19 | 1.95 Mb/s, 68.86 |
| 4000 | 20.79 Mb/s, VMAF 96.93 ❌ | 4.15 Mb/s, **91.21** | 3.85 Mb/s, 89.11 | 4.14 Mb/s, 87.18 |
| 6000 | 20.93 Mb/s, VMAF 96.98 ❌ | 6.22 Mb/s, **93.39** | 5.78 Mb/s, 91.77 | 6.15 Mb/s, 92.13 |
| 8000 | 21.05 Mb/s, VMAF 97.00 ❌ | 8.19 Mb/s, **94.58** | 7.70 Mb/s, 93.54 | 7.65 Mb/s, 93.66 |

wx264 process fps in this matrix: 107–116 fps (target-independent).

Re-run with the merged `harness/matrix.py` and x264-slow instead of medium
(`build/matrix_baseline_full/matrix.json`), same source and flags:

| Target | wx264-slow | x264-slow | NVENC P5 |
|---:|---:|---:|---:|
| 2000 | 18.59 Mb/s, 96.64 | 2.07 Mb/s, **84.66** | 1.93 Mb/s, 80.19 |
| 4000 | 20.51 Mb/s, 96.90 | 4.11 Mb/s, **91.48** | 3.85 Mb/s, 89.11 |
| 6000 | 20.75 Mb/s, 96.95 | 6.12 Mb/s, **93.65** | 5.78 Mb/s, 91.77 |
| 8000 | 20.83 Mb/s, 96.96 | 8.16 Mb/s, **94.76** | 7.70 Mb/s, 93.54 |

wx264 loop fps 149–160 on this content; x264-slow process 75–105 fps.

**Dominant problem: the ABR controller, not the codec.** Fixed QP is within
~1 VMAF of x264 at 6000/8000, but ABR limit-cycles at QP 18/22 and never
converges to the QP 28 needed for 8000 on this content.

---

## 2. Ranked work list

Ranking key: **E** = implementation effort (S/M/L/XL), **G** = expected VMAF
gain at matched rate, **R** = risk to speed/conformance. "Landed" items are
kept for provenance.

| # | Item | E | G | R | Status / evidence |
|---:|---|---|---|---|---|
| 1 | **Adaptive QP floor to break the 18/22 limit cycle** | S | up to +4 at 6000 (fixes 21 Mb/s -> 6 Mb/s) | low | **CONFIRMED BLOCKED (13 variants, all reverted).** obs_new_src is 30->60 fps duplicated capture: ~25% of frames are 4x `aim`, the rest ~1/20 of it, so per-frame `rateRatio` is the wrong signal. Best variant v6 held +/-5% only on isolated cases and cost >10 VMAF on yt_forza/porsche. 25fps guard always passed. See results in `...\abfix2\measure\`. Fixed floor 18 (PR #516) remains the best rate-accuracy compromise |
| 2 | **Gate/rollback the fixed floor if it regresses hard content** | S | protects +4 above | none | fallback if #1 fails: floor only when `actual < aim` for N frames |
| 3 | **Content-adaptive preset selection (x264 autotuning analogue)** | S–M | +0.3–0.8 | low | published −11.6% BD-rate / +2.32 BD-VMAF on x264-veryslow (Disney 2024); our CLI knobs already expose preset; cheap host-side heuristic from preloaded SAD |
| 4 | **Per-frame QP plan from preloaded source complexity (qcomp)** | M | +0.3–2.9 on ABR-broken clips | medium | tested 12 arms: fixed exponents failed rate on 6 masters; needs adaptive calibration (#1) first |
| 5 | **Syntax overhead reduction (fewer P-slices, 4-byte start codes, AUD removal)** | M | +0.05 dB (~0.05 VMAF) | medium | 4-row P slices LANDED (PR #524, +8-9% process fps, VMAF +0.02-0.03). Remaining items deferred: frame_num 4-bit needs frame_num wrap that risks POC-type-2 ordering; 3-byte start codes / AUD / deblock-flag / slice_type savings total only ~0.45% at 25fps / ~1.1% at 60fps at fixed QP, and ABR re-spends nearly all of it (<0.01 VMAF). Not worth decode risk right now |
| 6 | **Coefficient trellis/QNS with pruned bit counts** | M–L | +0.2–0.4 VMAF (x264 trellis ablation -0.650 when removed; our exact-CAVLC prototype +0.344) | medium | **CONFIRMED BLOCKED (6 variants, all reverted byte-identically).** Best variant V3: +0.035 VMAF / +0.016 dB with +0.726 ms residual (budget +0.5 ms) and -15 process fps. Full x264-style V1: -0.439 VMAF and +1.744 ms. Fixed-QP diagnostics: variants move along the existing RD curve (+0.1-0.31 dB for +1-10% bits) rather than improving it. Pruned decisions cannot recover the earlier +0.344 VMAF exact-RD gain without the full CAVLC state modelling that costs too much |
| 7 | **Block-level λ / perceptual (saliency/VIF) bit allocation** | M | +0.3–1.0 (published) | medium | per-MB VMAF-feature-driven QP; no AQ (measured negative); needs per-MB QP signalling in our syntax |
| 8 | **Cheap learned/neural post-filter (H.264-conformant)** | L | +1–3 (if allowed to change decode path) | high | 516 MAC/px post-filter cited as only way past 95 without structure; would not be decodable by standard H.264 players unless applied outside bitstream |
| 9 | **8x8 adaptive transform** | L | +0.15 | high | tried; decode breakage and entropy regression; would need full syntax path |
| 10 | **CABAC** | XL | +0.55 VMAF (veryslow ablation -0.552) +3% bits | very high | arithmetic state `low/range/queue/outstanding` + 460 contexts threaded through every bin; carry can retroactively edit previous byte (`cabac.c:53-181`). Only slice boundaries reset state (`encoder.c:2791-2804`); measured slice-parallel prototype ~23 fps, 14 us/MB, 117 ms/whole-frame IDR. Frame-parallel CABAC needs a new DPB/lookahead path; CPU-CABAC hybrid would add PCIe round-trip and serial bin stream, while x264-slow on this 12-core CPU already does 75-105 fps full encode. Not viable under 100 fps gate. |
| 11 | **B-frames + lookahead + MB-tree** | XL | +0.5–0.69 | very high | needs dual-reference ABI, host DPB reorder, slicetype analysis; without lookahead B-frames LOSE 0.64 VMAF |
| 12 | **AQ / psy-RD** | S | **negative** (−0.14 to −0.22) | low | tested and reverted; do not retry |

### Already landed (provenance, byte-identical where marked)
- PR #513: IDR residual wavefront, 52–67 ms -> 1.9 ms, byte-identical.
- PR #514: spatial SAD over 64 lanes, 1.22 -> 1.09 ms, byte-identical.
- PR #515: command-buffer/fence ring + threaded input preload, loop 119 -> 132–140 fps, process 89 -> ~99 fps, byte-identical.
- PR #516: QP floor 18 at >=50 fps, easy content 23.75 -> 5.86 Mb/s at -0.70 VMAF.
- PR #511: OBS ABR starting-QP clamp.
- PR #510: portable OBS DLL layout; PR #509: Vk264/H.264 plugin; PR #517: effective-settings logging.

### Explicitly rejected (do not repeat without new evidence)
larger debt window, wider aim cap, asymmetric clamp, step damping/faster gain,
two-sided GOP-scaled controller, qcomp fixed exponents, QP ceiling 30/38,
proportional/windowed controllers, exact-CAVLC RD at old speed budget,
I4x4, 8x8 inter partitions, AQ, psy-RD, chroma QP offset, ipratio offset,
SATD finalists, older-ref subpel, MV penalty, deblock variants, low/high AC
divisor splits, more refs, veryslow preset as default.

---

## 3. Measurement protocol for every step

```
python harness/benchmark.py --input <master.yuv> --out <results> \
  --width 1920 --height 1080 --fps {25|60} --frames 300 \
  --bitrate {2000|4000|6000|8000} --gop {125|120|300} --qp 14 \
  --encoders wx264-slow x264-slow nvenc-p5 --vmaf
```
Acceptance gates: VMAF >= baseline +0.3 OR PSNR +0.3 dB at matched rate;
achieved bitrate within +/-5%; loop >=100 fps; process >=85 fps; ctest
45/46 (only pre-existing `x264_cavlc_packet_parity`); `check.py` 13/13;
`ffmpeg -err_detect explode` clean.

Masters: `build/native1080_300.yuv` (25fps game), `build/obs_new_src.yuv`
(user's hard 1080p60 capture), `build/obs_capture_src.yuv` (easy capture),
and `C:\Users\multi\AppData\Local\Temp\opencode\corpus\masters\*.yuv`.

## 3a. Cumulative improvement (evidence trail)

| Change | Before | After | PR |
|---|---:|---:|---:|
| Parallel CAVLC slice assembly | entropy 9.84 ms/f, loop 49 fps | 0.09 ms/f, loop 88-91 fps | #494 |
| YUV preload (host) | loop 88 -> 100-108 | (superseded by ring) | #495 |
| IDR QP clamp / ABR start | 640p 48.13 dB | 51.35 dB | #489 |
| Inter AC rounding 6->3 @25fps | VMAF 95.390 | 96.149 | #501, #505 |
| 5 s default GOP | VMAF 95.390 | 95.719 (+8 fps) | #503 |
| ABR debt clamp @25fps | VMAF 95.719 | 96.149 | #505 |
| IDR residual wavefront | 52-67 ms/keyframe | 1.9 ms | #513 |
| Spatial SAD over 64 lanes | 1.22 ms | 1.09 ms | #514 |
| Host ring + threaded preload | loop 119, process 89 | loop 132-140, process 93-103 | #515 |
| 60 fps QP floor 18 (easy content) | 23.75 Mb/s, VMAF 97.40 | 5.86 Mb/s, VMAF 96.69 | #516 |
| 4-row P slices | 34 slices, 91.6/101.0 fps | 17 slices, 98.9/110.2 fps | #524 |

Net on 25 fps game clip: VMAF 95.390 -> 96.179 @6000; loop 49 -> 137 fps.
OBS plugin built and verified at each ABI change (PRs #509-#512, #517).

## 3b. Ceiling on the user's 1080p60 capture content

Fixed QP28 (`wx264-medium`, no ABR) on `obs_new_src.yuv`: **7646.8 kb/s,
VMAF 92.372** (slow preset: 7485.8 kb/s / 92.376). The baseline ABR at
21 Mb/s reaches VMAF 96.996. Therefore **VMAF >=95 at 8 Mb/s is impossible on
this clip with the current codec** — the ~4.6-point drop is inherent to the
toolset, not the controller. The controller can at best trade rate accuracy
for a smaller quality loss; 13 adaptive-floor variants confirmed this.

## 3b-extra: x264/NVENC rate-control study (2026-11-27)

Two literature studies are in limapedro/Vk264-CABAC-Lab:

otes/RC_X264_DEEP.md (exact 1-pass ABR formulas from
encoder/ratecontrol.c) and 
otes/RC_NVENC_DEEP.md (VBV/token-bucket,
feedforward complexity, 1-2 s windows). The key lesson: x264's integral is
QP-normalised (cplxr_sum += bits*qscale/blurred^0.4) and driven by
QP-independent complexity; our controller integrates raw bits and is
therefore unstable on bimodal capture.

Several host-side ports were prototyped from those formulas (faithful x264
integral, calibrated rate factor, duplicate pinning, token bucket, cumulative
mean-rate) and **all failed** when integrated: rates 13-60 Mb/s at 6000/8000
targets on the two capture clips. All prototypes were reverted; the parent
tree is clean and the opt-in QP-32 cap from PR #535 remains the shipped
behaviour. This is an honest open blocker: rate control needs a calibrated
QP-independent complexity signal at runtime, which our thumbnail SAD does
not provide on this content.

## 3c. Parity scope decision (lab `notes/PARITY_ROADMAP.md`)

The definitive x264-slow parity study is in the sibling lab repo
`limapedro/Vk264-CABAC-Lab`, `notes/PARITY_ROADMAP.md`. Its measured
conclusion:

- The 1080p60 gap is **structural**, not a parameter: on the user's brief
  capture, wx264 fixed QP40 = 8.55 Mb/s / VMAF **76.38** vs x264-slow
  **86.3** (~9.9 VMAF). On `obs_new_src.yuv`, wx264 fixed-QP ceiling
  92.37 @7.65 Mb/s vs x264-slow 94.76 @8 Mb/s.
- Realistic ceiling without a structural rewrite: **~93–94 game / ~76–80
  capture**. Stacking the best conformant ≤2 ms ideas gives only +0.4…+0.9
  VMAF, and x264 tool gains are non-additive.
- CABAC is user-excluded; B-frames need a dual-reference ABI + DPB reorder;
  frame-parallel N=8 gives 60–68 fps, N=16 at best 105–120 fps with ~3.8 GiB
  scratch and N-frame RC lag that breaks bitstream identity.
- Phases 0–2 (measurement hygiene, cheap conformant decisions, offline QP
  plan) fit the current 100 fps gate. Phase 3 (structural) requires an
  explicit decision to relax the speed gate or change hardware.

This is a **scope gate**, not a technical blocker we can tune away.

## 4. Open items at time of writing
Two sub-agents are still running and must be evaluated before items #1/#3
are finalised:
- `ses_de2d0752affe5g5VSsbM249e0J`: adaptive QP floor prototype.
- `ses_de2c9edcfffexLMoKYnhDpeaoA`: x264-veryslow exact tool ablation on the
  user's content.

Completed and already folded into the ranking:
- `ses_de2c9edcfffexLMoKYnhDpeaoA`: x264-veryslow exact tool ablation on the
  user's content (`obs_new_src.yuv`, 300f/1080p60, 6000k, VMAF v0.6.1):
  medium 93.426 @6155 kb/s -> veryslow 93.984 @5829 kb/s (+0.558). Removing
  one tool from veryslow costs: mode-RD/subme -0.863, trellis -0.650,
  CABAC -0.552, B-frames -0.411, 8x8dct -0.210, MB-tree -0.166, refs -0.147;
  disabling AQ GAINS +0.241. Individual gains are non-additive (+0.279 vs
  +0.558 actual). Only parallel-shader-transferable piece: coefficient-level
  trellis/QNS, bounded at +0.2-0.4 VMAF.
- `ses_de2ca16fdffeETOw8RAliZVe0T`: state-of-the-art codec math/tricks survey
  (notes at `...\stateoftheart\RESEARCH.md`): even stacking feasible ideas
  gives ~+1.5 VMAF (93.5–94), not 95; warns that Netflix VMAF v1 (June 2026)
  can move scores 1–3 points and that some published VMAF gains are
  sharpening/VIF exploits.
