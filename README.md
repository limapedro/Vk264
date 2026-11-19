# Vk264

**A Vulkan compute H.264/AVC encoder ported function-by-function from x264, with byte-exact output as the target.**

`Vk` = Vulkan · `264` = x264. The project started as `Vk264`; this repository is its new home and name.

## Goal

Port x264's encoder to Vulkan compute shaders so that, preset by preset, Vk264 produces a **byte-for-byte identical H.264 stream to libx264** at the same settings — while keeping the throughput advantage of a GPU implementation. Current baseline: GTX 1660 SUPER, 1080p60, fixed QP 28.

Progress is tracked per preset and per function in [`docs/PROGRESS.dm`](docs/PROGRESS.dm), with the bottom-up x264-function → shader map in [`docs/x264-function-map.md`](docs/x264-function-map.md).

## Working arrangement

Development is PR-based: each change (a parity fix, a new shader, a test) lands on its own branch and is reviewed via a pull request before merging into `main`. Multiple agents work in parallel on separate branches/PRs against this remote.

- Remote: `https://github.com/limapedro/Vk264`
- Default branch: `main`
- CI/local gate before every merge: `ctest --test-dir build -C Release` and the byte-parity harness described below.

## Repository layout

| Path | What it is |
|---|---|
| `Vk264/` | The encoder source and the x264→GLSL port. `Vk264/src` is the CLI runner; `Vk264/vulkan` holds the Vulkan shaders (`.comp`), the x264-derived C++ primitives, and the `dct_parity` fixture harness. |
| `x264-vulkan/` | x264 fork used **only as a CPU reference** for parity fixtures and correctness checks. Not shipped. |
| `references/x264-master/` | Pinned x264 source snapshot used to generate tables and verify functions bottom-up. |
| `harness/` | Python benchmark and check runner (`benchmark.py`, `check.py`). |
| `docs/` | `PROGRESS.dm` (live status), `x264-function-map.md` (function→shader map), `parity-roadmap.md` (phase plan). |
| `clips/` | Local test sources (not committed — see `.gitignore`). |
| `results/`, `Evals/` | Experiment outputs and reports (large artifacts ignored; markdown reports kept). |

The encoder takes planar 8-bit YUV420 and writes an Annex B elementary stream (wrapped to `.mp4` by the harness for delivery). It uses compute shaders only — no Vulkan Video / fixed-function encoder.

## Build and test

Requirements: CMake, a C++17 compiler, Python 3.9+, the Vulkan SDK with `glslc`; FFmpeg/FFprobe for decode and quality checks.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Windows with the Khronos validation layer installed, set `$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'` and `$env:VK_LAYER_VALIDATE_SYNC='1'` before CTest for synchronization checking.

## Verifying byte-exact parity

The authoritative check compares Vk264's Annex B stream to libx264's, ignoring only encoder-identity SEI NALs:

```powershell
python harness/parity_check.py --preset ultrafast --pattern flat --width 16 --height 16 --frames 1 --gop 1
python harness/parity_check.py --preset superfast --pattern flat --width 16 --height 16 --frames 1 --gop 1
```

`parity_check.py` exits `0` only on `BYTE-EXACT MATCH`, `1` on a byte mismatch (with the first offset), and `2` on invocation errors.

### Automated parity gate (CTest)

The known byte-exact cases are registered as CTest entries, so the same check that agents run by hand is part of the normal suite:

```
ctest --test-dir build -C Release -R x264_parity_ --output-on-failure
  x264_parity_ultrafast_flat_16x16
  x264_parity_ultrafast_flat_64x64
  x264_parity_ultrafast_noise_32x32
  x264_parity_fast_flat_16x16
```

They are gated on FFmpeg being available at configure time (the `find_program(FFMPEG_EXECUTABLE ffmpeg)` check in `Vk264/vulkan/CMakeLists.txt`); without FFmpeg they are not registered. Each case uses its own artifact directory under `build/parity-gate/`, so `ctest -j` is safe. A regression that breaks any exact case makes `ctest` fail. `BYTE-EXACT MATCH (ignoring encoder-identity SEI)` is the only acceptable result — a "first mismatch at offset N" line means the case is NOT exact and must not be claimed as parity.

A `BYTE-EXACT MATCH` line means the SPS, PPS, slice header, entropy-coded macroblock data and deblocking all match libx264 byte-for-byte. Quality/speed comparison across real clips:

```powershell
python harness/benchmark.py --input <clip.yuv> --out <dir> --frames 240 --encoders vk264-vulkan x264-fast --vmaf --check-recon
```

`--check-recon` re-encodes and verifies the GPU reconstruction against an independent FFmpeg decode, byte for byte.

## Current parity status (summary)

- **Byte-exact:** `ultrafast` (uniform/noise/small-motion cases) and `fast` (16×16 flat).
- **In progress:** `superfast` / `veryfast` / `faster` — x264 selects I4×4 macroblocks (subme < 6) where Vk264 emits I16×16; the I4×4 analysis model already reproduces x264's reconstruction at all 256 luma samples, and the CABAC emitter matches the first bytes of the slice.
- **Open:** varying-content mode decisions (SATD/RD), and `medium` and above, which additionally need B-frames, multi-reference, weighted prediction, trellis, AQ and lookahead.

See [`docs/PROGRESS.dm`](docs/PROGRESS.dm) for the full, evidence-backed state.

## Speed context

The original Vk264 CAVLC/Vulkan encoder runs at roughly 10–37 fps on 1080p60 (GTX 1660 SUPER), versus ~80–170 fps for x264 `fast`; the CABAC whole-frame bin coder is the main bottleneck. The port work is correctness-first; slice-level CABAC parallelism is the planned speed pass once parity is established.

## License and references

Ported from x264 (GPL-2.0-or-later); see [`references/x264-master`](references/x264-master). Legacy CAVLC/deblocking lookup tables originated from OpenH264 — see `Vk264/src/OPENH264_LICENSE.txt`. [ITU-T H.264](https://www.itu.int/rec/T-REC-H.264-202408-S/en) · [x264](https://www.videolan.org/developers/x264.html) · [OpenH264](https://github.com/cisco/openh264).
