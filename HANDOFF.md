# Wx264 Vulkan development handoff

Vulkan fork revision `31c9b63d185854eed697e30ac37b065ec70cb97a`.


**CABAC backend status (2026-11-19):** the Vulkan CABAC path was just fixed. The
--backend vulkan (CABAC) CLI encodes are now valid H.264 streams: they decode in
libx264 (via FFmpeg) and match the GPU reconstruction sample-for-sample on every
fixture tested (harness/check.py --vulkan). However, reconstruction equality is
**not** bit-exact vs upstream x264 -- the port remains an analysis-minimal
configuration; preset parity still needs the remaining unported features
(trellis, lookahead, AQ, mb-tree, B-frames, 8x8 chroma transform). See
docs/presets.md and results/x264-vs-wx264-1080p-comparison.md for the
per-preset status and the 1080p comparison table.

The current experimental encoder runs through the CLI with `--backend vulkan --entropy cavlc`.
Its resident GPU pipeline is in `Wx264/vulkan`. In particular, `dct_parity.cpp`
records the frame graph, `motion_search.comp` handles motion estimation and
compensation, `inter_frame.comp` prepares and gathers residual data,
`cavlc_frame.comp` assembles the slice, and `cavlc_packet.comp` builds the
Annex B packet. The `Wx264/shaders` folder is also required to build the CLI.

Start with [the Vulkan port notes](Wx264/vulkan/README.md),
[the function inventory](Wx264/vulkan/INVENTORY.md), and
[the latest pipeline map and timings](results/vulkan-cavlc-packet/performance.md).
The [tiling trial](results/vulkan-motion-tiles/performance.md) measured four
tile sizes and left tiling opt-in. The current 1080p benchmark bitstreams
match the previous build byte for byte. The latest paired runs measured
roughly 27 fps on a GTX 1660 SUPER.

## Build and check

From the folder that contains this file, use a Windows x64 C++ toolchain,
the Vulkan SDK with glslc, CMake, and Python 3. Put FFmpeg and FFprobe on
PATH for the reconstruction test.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --target wx264 dct_parity
ctest --test-dir build -C Release --output-on-failure -R vulkan_cavlc_reconstruction
```

## Repo layout (after the 2026-11-19 reorganisation)

The encoder sources now live under `Wx264/` so the GPU tree is separate from the
CPU-reference x264 tree:

- `Wx264/src`, `Wx264/shaders` — CAVLC backend (default CLI, OBS plugin)
- `Wx264/vulkan` — x264 Vulkan port (CABAC/CAVLC) + `dct_parity` verification
- `x264-vulkan` (unmoved) — CPU x264 reference used only for parity fixtures
- root `CMakeLists.txt` — top-level build wiring; subdirs in `Wx264/`
- outputs land in `build/Release/` (single flat output dir)

If you reference shader paths or `dct_parity.exe` in external tooling, they moved
from `build/x264-vulkan/vulkan/...` to `build/Wx264/vulkan/...` (`.spv`) and
`build/Release/` (`.exe`).

The sources and selected reports are included. Build outputs, portable OBS,
raw footage, and captured encodes were omitted from the archive.
The [clip manifest](clips/youtube/manifest_1080p60.json) records the footage
used for file benchmarks. Reconstruction tests generate their own inputs.

## Harness output defaults (2026-11-19)

- `harness/benchmark.py` now defaults to producing playable MP4s (`--mp4` via
  `argparse.BooleanOptionalAction`, default on). wx264 still writes its raw
  `.h264` intermediate (needed for the bit-exact `--check-recon` comparison)
  and is then muxed with `ffmpeg -c copy -fflags +genpts -r <fps>` to `.mp4`;
  x264/nvenc write MP4 directly. `--no-mp4` restores raw-only behavior.
- Raw `.h264` carries no container time base; probe/play through the `.mp4`
  twins. Bitrate and VUI timing were verified identical to libx264's
  convention (num_units_in_tick=1, time_scale=2*fps, fixed_frame_rate=1).
- `Evals/00_x264_vs_wx264_vulkan_quality_parity/` holds the first 1080p60
  x264-fast vs wx264-vulkan matrix (with back-filled `.mp4` twins);
  `Evals/01_x264_vs_wx264_vulkan_quality_parity_rerun/` is the fresh re-run
  under the new default.
