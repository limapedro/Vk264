# Wx264

Wx264 is an experimental H.264/AVC encoder implemented in Vulkan compute shaders. The target is **1920×1080 at 60 fps and 6,000 kbit/s with x264 `slow`-class quality**, with 100 encode fps as a stretch throughput target.

The encoder accepts planar 8-bit YUV420 and emits an Annex B elementary stream. The shader path handles reference filtering, motion estimation, prediction, mode selection, transform, quantization, reconstruction, CAVLC, parameter sets, slice headers, emulation prevention, rate feedback and bitstream assembly. The C++ runner handles Vulkan resources, scheduling, transfers and file I/O. It uses a compute queue and requires no Vulkan Video extensions or fixed-function encoder.

## Build and run

The Windows OBS plugin uses the standalone CAVLC backend and its presets. See the [OBS plugin build and installation instructions](obs/README.md).

Use CMake, a C++17 compiler, Python 3.9+, and the Vulkan SDK with `glslc`. The build uses Python to generate the x264 reference tables. The test and benchmark scripts also need FFmpeg and FFprobe. The default benchmark suite needs an FFmpeg build with libx264 and a working NVIDIA NVENC device. Select individual encoders with `--encoders`.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Windows with the Khronos validation layer installed, set `$env:VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation'` and `$env:VK_LAYER_VALIDATE_SYNC='1'` before running CTest to check Vulkan synchronization as well.

```powershell
build/Release/wx264.exe clips/youtube/warzone2020_verdansk_1080p60_600f.yuv build/sample.h264 1920 1080 600 60 --qp 34 --bitrate 6000
ffmpeg -r 60 -i build/sample.h264 -c:v copy build/sample.mp4
```

The positional arguments after the input and output paths are width, height, frame count and frame rate. Supported input dimensions are even and at most 1920×1080, at integer rates up to 60 fps. `--qp` sets the initial QP, or the fixed QP if `--bitrate` is omitted. `--gop` sets the fixed keyframe interval and defaults to two seconds. `--search` sets the motion radius from 0 to 32 luma pixels and defaults to 24. `--subpel 0` uses integer motion only. The default P slices span two rows. `--p-slice-rows 1` selects the single-row comparison path. `--partitions 1` enables experimental 8×8 inter partitions with single-row slices. `--deblock 2` is the default and filters across slice boundaries. Mode 1 filters within slices and mode 0 disables filtering. On the two-row P path, `--flat-intra 1` is the default and allows DC-only Intra16×16 coding for dark, flat macroblocks. Use `--flat-intra 0` to disable that candidate. `--recon` saves the padded planar shader reconstruction for testing.

## Presets

The CLI also offers `--backend vulkan`, which runs the x264 Vulkan port with CABAC, 4×4 and 8×8 inter luma transforms, and in-loop deblocking. It currently uses one reference, one slice per picture, and a fixed motion-search configuration. `--qp`, `--bitrate`, `--gop`, and the frame-rate and color arguments apply to this backend. Its `--recon` output uses visible dimensions. It accepts even dimensions up to 2048 pixels per side and up to 8,192 macroblocks per picture.

Use `--backend vulkan --entropy cavlc` for the GPU-resident file encoder. Its Vulkan command buffer covers prediction, transform selection, reconstruction, deblocking, CAVLC, and Annex B slice packing. References stay on the GPU. Raw-file input uploads pixels once and reads back the encoded packet. Use `--encoders wx264-vulkan-cavlc wx264-vulkan` in the benchmark to compare it with the CABAC path.

The Wx264 preset names apply to the default `--backend cavlc` path.

`--preset fast|medium|slow|veryslow` selects the Wx264 motion-search effort and reference-picture budget. The default is `slow`. Explicit options such as `--ref`, `--search`, and `--satd-candidates` override the preset regardless of flag order. Run `wx264 --list-presets` to inspect the defaults without starting Vulkan.

| Wx264 preset | Reference pictures | Search radius in luma pixels | Full-resolution SATD candidates |
|---|---:|---:|---:|
| fast | 2 | 12 | 0, use sampled SAD |
| medium | 3 | 16 | 2 |
| slow | 5 | 24 | 4 |
| veryslow | 16 | 32 | 8 |

The reference budgets match the corresponding x264 presets. Wx264 uses an I/P GOP, CAVLC, and zero lookahead at every tier. The listed search radius sets the dense near search. For 16×16 motion on the newest reference, the hierarchical search also samples an outer grid. At the default settings, `slow` reaches ±96 pixels and `veryslow` reaches ±128 pixels per axis. With subpixel refinement enabled, the newest reference is refined at quarter-pixel resolution. Older pictures use seeded integer-pixel candidates within the listed radius. Each P slice activates only the reference indexes it needs. The encode report records the requested and configured budgets, the deepest reference actually selected, and how many macroblocks used an older picture. [x264 preset definitions](https://github.com/mirror/x264/blob/master/common/base.c#L514-L556)

Use `--ref 1` for the previous single-reference configuration. Multi-reference search uses the two-row 16×16 P path. Single-row P slices and experimental 8×8 mode use one reference, and short GOPs reduce the available pool. At 1080p the five-picture budget signals H.264 Level 5.0 and the sixteen-picture budget signals Level 5.1. [x264 level limits](https://github.com/mirror/x264/blob/master/common/tables.c#L39-L44)

## Benchmark

```powershell
python harness/benchmark.py --input clips/youtube/warzone2020_verdansk_1080p60_600f.yuv --out results/quick --frames 120 --check-recon
python harness/benchmark.py --input clips/youtube/warzone2020_verdansk_1080p60_600f.yuv --out results/preset-comparison --frames 600 --presets fast medium slow veryslow --check-recon
python harness/benchmark.py --input clips/youtube/warzone2020_verdansk_1080p60_600f.yuv --out results/vulkan-comparison --frames 600 --qp 30 --encoders wx264-vulkan wx264-slow x264-slow nvenc-p4 --vmaf --mp4
```

The default suite encodes the same source frames with Wx264, x264 `slow`, NVENC `p4` and NVENC `p6`. `--presets` pairs each named Wx264 tier with the same-named x264 preset. The report includes the effective Wx264 settings and the x264 settings carried in the bitstream when available. It records achieved bitrate, peak one-second bitrate, wall-clock process fps, initialized Wx264 loop fps, GPU stage timings, PSNR and SSIM. `--runs` reports median process time across repeated runs. `--vmaf` adds frame-aligned VMAF when FFmpeg has `libvmaf`. `--check-recon` repeats the shader encode and verifies the visible Y, Cb and Cr samples byte for byte against an independent FFmpeg decode. That verification is outside the timed run. Use `--frames 600` to test the full supplied clip.

x264 uses single-pass ABR with VBV and NVENC uses CBR. Both references use a two-second buffer. Wx264 reserves keyframe bits across the GOP and adapts the IDR and P-frame QPs separately. Its average-rate feedback can exceed the target over short windows. The harness records actual rates and exact commands in `summary.json`.

### Measured on the GTX 1660 SUPER

Each run used 600 frames at 1920×1080 and 60 fps, targeting 6 Mbps with 120-frame GOPs. PSNR-Y is measured against the supplied YUV masters, which came from compressed gameplay captures.

| Clip | Encoder | Average Mbps | Process fps | PSNR-Y (dB) | SSIM |
|---|---|---:|---:|---:|---:|
| Warzone | Wx264 slow, one reference | 6.03 | 60.3 | 32.43 | 0.9264 |
| Warzone | Wx264 slow, five references | 6.03 | 58.1 | 32.65 | 0.9274 |
| Warzone | Wx264 slow, full deblocking and flat intra | 6.03 | 56.2 | 33.25 | 0.9403 |
| Warzone | x264 slow | 5.77 | 71.4 | 40.44 | 0.9843 |
| Warzone | NVENC p4 | 5.89 | 340.9 | 41.36 | 0.9843 |
| Warzone | NVENC p6 | 5.88 | 202.6 | 41.38 | 0.9841 |
| Fortnite | Wx264 slow, one reference | 6.07 | 60.4 | 32.09 | 0.9006 |
| Fortnite | Wx264 slow, five references | 6.05 | 56.5 | 32.08 | 0.8998 |
| Fortnite | Wx264 slow, full deblocking and flat intra | 6.05 | 52.5 | 32.45 | 0.9095 |
| Fortnite | x264 slow | 6.31 | 60.1 | 39.60 | 0.9759 |
| Fortnite | NVENC p4 | 5.99 | 354.3 | 39.97 | 0.9747 |
| Fortnite | NVENC p6 | 6.01 | 204.0 | 39.93 | 0.9744 |

With the block artifact fixes, Wx264 ran at 104.6 fps on Warzone and 91.7 fps on Fortnite after Vulkan initialization, including input and output I/O. FFmpeg matched all 1,200 reconstructed frames byte for byte. The [quality parity study](docs/quality-parity.md) describes the slice seams, dark-area measurements and x264 feature comparisons.

## Current coding tools

The stream uses High profile syntax with CAVLC, an I/P GOP, luma Intra16×16 DC prediction, and P 16×16, P 8×8 and P-skip. The default motion search uses a quarter-resolution luma search, full-pixel and quarter-pixel refinement, neighboring motion candidates, and full-resolution SATD to select among the four shortlisted vectors for the newest picture. Older candidates come from a GPU ring of reconstructed pictures. The residual shader evaluates four 8×8 luma groups and a chroma group independently. IDR slices contain up to 64 macroblocks from one row. In two-row P slices, the entropy shader emits macroblock bodies in parallel, then a slice shader joins them and applies emulation prevention. In-loop deblocking walks the full picture in staggered GPU tiles. Its result becomes the next reference. The shader assembler uses word-aligned Annex B slots with `trailing_zero_8bits` between NAL units.

The CAVLC backend and the Vulkan CABAC prototype use I/P pictures and average-rate feedback. Broader intra/inter mode decisions, lookahead, VBV and x264 rate-distortion parity remain open.

The CAVLC and deblocking lookup tables came from the supplied OpenH264 archive. Their source digests are in [cavlc_codes.hpp](src/cavlc_codes.hpp) and [deblock_tables.glsl](shaders/deblock_tables.glsl). The license is in [OPENH264_LICENSE.txt](src/OPENH264_LICENSE.txt). The Vulkan CABAC backend is built from [the x264 fork](x264-vulkan/vulkan/README.md). The benchmark also runs CPU x264 through FFmpeg. [ITU-T H.264](https://www.itu.int/rec/T-REC-H.264-202408-S/en) · [OpenH264](https://github.com/cisco/openh264) · [x264](https://www.videolan.org/developers/x264.html)
