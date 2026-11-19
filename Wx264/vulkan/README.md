# x264 Vulkan port

UPSTREAM-REVISION records the commit supplied in the local x264 archive. The
upstream import is tagged upstream-c24e06c2. The code under vulkan is the
first GPU port on the vulkan-port branch. See COPYING for license terms.

The kernels translate the 4x4 and 8x8 transforms and their 8x8 and 16x16
grouped variants from common/dct.c. Each invocation handles one transform
block and keeps x264's horizontal-major coefficient order within the block
and quadrant order within the group. The inverse path takes dequantized
coefficients and applies x264's narrowing, rounding and 8-bit clipping.
The test also compares the in-place coefficient writes of the 8x8 inverse.
The DC-only reconstruction shader checks the 4x4, 8x8 and 16x16 functions,
including unaffected pixels outside each destination.

The test compiles the original scalar C functions from the imported source.
It compares every GPU result against those functions over 4,096 blocks per
size, including pixel-range limits, impulses, and signed odd coefficients.
See [the upstream file inventory](INVENTORY.md) for the rest of the port.

The quantization kernel ports quant_4x4, quant_8x8, dequant_4x4, and dequant_8x8
from common/quant.c. The tests compare quantized levels, nonzero flags and
dequantized coefficients on 3,328 fixtures per size across QP 0–51. They
exercise the supplied matrix and bias inputs. The production scaling-list
setup and trellis decisions are separate stages.

The DC kernel checks nine routines from common/dct.c, common/quant.c,
and encoder/macroblock.c on 3,328 fixtures across QP 0-51. For 4:2:0 and
4:2:2 chroma inverse DC, the test checks both the DC-only result and the
full destination blocks, including AC coefficients outside the write
positions. The four-block quantizer test compares in-place coefficient
writes and all 16 possible nonzero masks.

The coefficient analysis tests exercise chroma DC optimization, residual
denoising, decimation, last-coefficient search and the nonempty-block CAVLC
run-level extraction. They compare the actual decisions, every coefficient,
unsigned noise sums and CAVLC output slots against the upstream scalar code.

The intra prediction tests cover all 45 luma and chroma scalar modes and the
8x8 luma edge filter. They check the entire padded block and edge array,
using all neighbor and filter mask combinations for the edge filter.

The deblocking tests check 14 inter and intra filters, both QP wrappers and
boundary-strength calculation. They cover luma, interleaved chroma, MBAFF,
4:2:2 and multiple strides. The shader thresholds come from the imported
x264 tables at build time.

The CABAC test compares slice arithmetic state, context bytes, and output
bytes with x264 for 9,856 sequences. It includes context initialization at
all 52 QPs, carry propagation, 960 residual-coding sequences covering every
block category and the separate 4:2:2 DC path, and 1,024 intra syntax cases.
Another 5,408 sequences cover luma and chroma coded-block patterns,
transform-size flags, P/B skip contexts, motion-vector difference components,
and every current and previous QP pair. They also check x264's effective QP
when an empty I_16x16 block suppresses a QP rise.

The bit writer and NAL tests compare 2,048 operation sequences and 1,024
payloads against the original x264 routines. The SPS/PPS test compares
2,048 payloads including 8- and 10-bit syntax, custom scaling lists, crop,
VUI timing and color, and HRD fields. A separate test compares 2,048 I/P/B
slice headers, their pending bits, and x264's weighted-prediction flag.

The I_16x16 parity test compares 4,368 lossy macroblock cases with x264.
It checks reconstruction, coded-block flags, scan-ordered DC and AC
coefficients, and denoising accumulators for all seven prediction modes
and all 52 QPs. Lossless and trellis branches pending.
The inter 4x4 luma test compares another 4,368 macroblocks with the
scalar branch of `macroblock_encode_internal`. It checks both decimation
thresholds, reconstructed samples, coefficients, coded-block flags and
noise sums across all 52 QPs and frame and field scans.
The inter 8x8 luma test compares 2,496 macroblocks with x264's corresponding
scalar branch across all 52 QPs, with denoising, decimation and field scans.

The chroma parity test compares 11,648 intra and 4,160 inter plane cases
in 4:2:0 and 4:2:2 with the scalar-quantization and lossless branches of
`mb_encode_chroma_internal`. It covers frame and field scans, all seven
prediction modes, all 52 quantizer values, and denoising in the lossy path.
The test compares reconstruction, coefficient buffers, nonzero flags and
accumulated noise against x264. The lossless test includes x264's dedicated
horizontal and vertical chroma prediction from source samples and
reconstructed boundaries.
Inter cases take precomputed prediction samples and check x264's
joint-plane variance gate, DC-only reconstruction and AC decimation.

With FFmpeg on PATH, CTest encodes five synthetic 30x30 CABAC pictures
from 4:2:0 samples. The three IDRs use fixed I_16x16 and chroma DC
prediction. The first P picture skips all macroblocks and its CABAC bytes
match x264. The second P picture uses one reference and P_L0 16x16.
Vulkan builds the half-pel reference planes, predicts each vector from
its neighbors, searches luma with UMH, interpolates the selected
fractional motion, and encodes 4x4 luma and 4:2:0 chroma residuals.
FFmpeg checks every reconstructed
sample including reference reuse across both P pictures. The stream
signals 60 fps and limited-range BT.709. The harness uses fixed QP and
stages complete macroblocks between shader passes on a shared Vulkan
device. General mode selection, motion search across multiple references
and partition sizes, and multi-frame scheduling are pending.

The raw input test reads a displaced crop from planar 8-bit YUV420, starts
at a nonzero source frame, and verifies a sequence with one IDR and 17 P
pictures. The test checks the decoded crop dimensions, timing through the
frame-number wrap, and byte-exact agreement with the shader reconstruction.
The raw encoder chooses between 4x4 and 8x8 inter luma transforms using
x264's SA8D versus SATD comparison. The rate-distortion transform decision
and trellis quantization used by the higher-quality presets are pending.
The current path takes even crop dimensions up to 2048 per side and one
slice of up to 8192 macroblocks, sufficient for 1920x1080. It signals
60 fps playback and uses QP 26, one reference, and one CABAC slice per picture.
The host submits batches of macroblocks after their spatial predictors
are ready and keeps two reconstructed pictures for the raw input path.
The progressive 4:2:0 path filters luma and both chroma planes in loop
before a picture becomes the next reference. The frame test compares 24
pictures with x264's scalar filter and boundary-strength functions in
row order.
Each raw P picture filters its reference luma once, including the padded
edges used by the motion search. The Vulkan harness reuses its dispatch
resources and reads back the results consumed by subsequent stages.

From the workspace, the raw harness accepts the source width and height,
crop left and top, crop width and height, first source frame, and frame count
after the input path. The following PowerShell example encodes six full
1920x1080 frames from the Fortnite clip.

    $build = 'build-x264-vulkan'
    $shaders = @('parameters','slice_header','intra16','intra_chroma','motion_compensation','mv_predict','motion_search','cabac_frame','nal') |
        ForEach-Object { "$build/$_.spv" }
    & "$build/Release/dct_parity.exe" cabac_raw @shaders fortnite-full.h264 fortnite-reconstructed.yuv clips/youtube/fortnite_1080p60_600f.yuv 1920 1080 0 0 1920 1080 60 6

Set `WX264_PROFILE=1` when running the Wx264 Vulkan backend directly
to report calls, bytes transferred and timings per shader.
`gpu_ms` uses queue timestamps where available. `dispatch_ms` also
includes command recording, submission and waiting. `setup_ms` includes
first-use pipeline compilation. The profiler also measures raw input and
the host packing of the motion frame. For the resident CAVLC path, the
`resident_idr_*` and `resident_p_*` rows show intervals for the transfer,
motion, transform, entropy, deblocking, and packet stages. Transform
rows accumulate across dependency waves or batches.

The raw path dispatches motion prediction, search and compensation on
shared packed picture planes in device-local memory. The search walks
the same dependency waves with a compute barrier between waves. On
devices with compute subgroup arithmetic, lanes collaborate on each
macroblock's integer SAD and SATD costs. On 32-lane compute subgroups
with shuffle support, the resident CAVLC path assigns four groups of
eight lanes to independent motion candidates and resolves ties in
search order. Other devices use the subgroup or scalar frame shader.

Set `WX264_MOTION_TILE` to `2x1`, `4x1`, `2x2`, or `4x2` to try tiled
motion search in the resident CAVLC path on devices that support batched
candidates. Single-row tiles keep the preceding motion vector in the
subgroup. Two-row tiles use workgroup shared memory for predictors
inside the tile. Unset the variable for the default wave schedule.

The frame CABAC shader stages its adaptive contexts and arithmetic lookup
tables in workgroup memory. A single lane writes each slice in syntax order.

Frame deblocking packs four pixels per storage word and uses device-local
memory. It filters 4×4 macroblock tiles in dependency order and stages each
macroblock and its left and top borders in workgroup memory. Devices with
32-lane compute subgroup voting skip empty macroblocks with a subgroup vote.
The tiled path also has a workgroup-reduction variant for other devices.

The Wx264 CLI's CAVLC backend records each picture's reference filtering,
prediction, transform selection, residual coding, reconstruction,
deblocking, entropy coding, and Annex B slice packing in one Vulkan
command buffer. Reconstructed references stay in device-local memory.
The Annex B packet shader counts emulation-prevention bytes in 64-byte
chunks, assigns output offsets, and emits the chunks in parallel.
The raw-file caller uploads source pixels and reads the finished encoded
packet. Decoded picture readback is enabled by the diagnostic `--recon`
option. The CABAC path uses the staged dispatch harness.

The motion-compensation test feeds GPU-filtered half-pel planes directly
into the luma predictor. It covers all 16 luma phases and 64 chroma phases,
signed motion vectors, explicit weighting, and the reference view returned
by get_ref. The motion predictor test checks 7,500 neighbor combinations
against x264's 16x16 and P-skip predictors and the CABAC motion-delta
contexts. The integrated P slice uses the same predictor and context
functions. Additional tests check scalar and batched SAD and SATD, SSD,
SA8D, successive elimination, wrapped integral sums, and low-resolution
reference preparation.

The motion search test covers DIA, HEX, UMH, ESA, and TESA plus the two
standalone quarter-pel refinement entry points. It compares the winning
vector, total and MV costs, and the reference threshold against the original
x264 routines across subme 0 to 11. It includes 26-pixel displacements,
weighted 4:2:0, 4:2:2, and 4:4:4 chroma, and field offsets. The TESA test
also compares the shortlist scratch records. The bidirectional SATD test
compares both vectors and the temporary prediction pixels. Bidirectional
and quarter-pel rate-distortion refinement still need porting.

The zigzag kernel matches 11 frame and field scan, lossless residual and
CAVLC interleave routines on 4,096 fixtures. The forward chroma DC kernel
matches five 4x4 residual sum, 4:2:0 and 4:2:2 routines on another 4,096
fixtures, including the source coefficient blocks with their DC entries cleared.

The test compares the bytes of native 8-bit pixels and signed 16-bit
coefficients. It first checks that the GPU's 32-bit storage values fit the
native types exactly, so a widening or overflow discrepancy fails the test.

From the Wx264 workspace with the Vulkan SDK, Python, CMake, and MSVC:

    cmake -S . -B build-x264-vulkan -A x64  # from the repository root
    cmake --build build-x264-vulkan --config Release
    ctest --test-dir build-x264-vulkan -C Release -V

These tests establish parity for 247 upstream functions or dispatch paths.
Most pixel and coefficient tests use 8-bit inputs. End-to-end encoding,
preset parity and encode throughput remain separate milestones.

