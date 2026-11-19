# Upstream file inventory and Vulkan port status

This inventory lists every tracked file in the imported x264 snapshot
`c24e06c2e184345ceb33eb20a15d1024d9fd3497`. The local import adds
`UPSTREAM-REVISION`.

## Port order and validation

| Upstream code | Vulkan work | Verification |
|---|---|---|
| `common/dct.c` | `sub4x4_dct`, `add4x4_idct`, `sub8x8_dct8`, `add8x8_idct8`, `dct4x4dc`, `idct4x4dc` | Byte-exact coefficients and pixels |
| `common/dct.c` | `sub8x8_dct`, `sub16x16_dct`, `add8x8_idct`, `add16x16_idct`, `sub16x16_dct8`, `add16x16_idct8` | Byte-exact grouped coefficient order, pixels and inverse coefficient writes |
| `common/dct.c` | `add4x4_idct_dc`, `add8x8_idct_dc`, `add16x16_idct_dc` | Byte-exact rounding, clipping and destination boundaries |
| `common/dct.c` | `sub4x4_dct_dc`, `sub8x8_dct_dc`, `sub8x16_dct_dc`, `dct2x4dc` | Byte-exact residual sums, DC output and source coefficient mutations |
| `common/dct.c` | `zigzag_scan_4x4_{frame,field}`, `zigzag_scan_8x8_{frame,field}`, `zigzag_sub_4x4_{frame,field}`, `zigzag_sub_4x4ac_{frame,field}`, `zigzag_sub_8x8_{frame,field}`, `zigzag_interleave_8x8_cavlc` | Byte-exact scan and residual order, lossless pixel copies, return flags, AC-only DC output and CAVLC context |
| `common/quant.c` | `quant_4x4`, `quant_8x8`, `dequant_4x4`, `dequant_8x8`, `quant_4x4_dc`, `quant_2x2_dc`, `dequant_4x4_dc` | Byte-exact coefficients and matching nonzero flags across QP 0 to 51 |
| `common/quant.c` | `quant_4x4x4` | Byte-exact coefficients and all 16 nonzero masks |
| `common/quant.c` | `idct_dequant_2x4_dc`, `idct_dequant_2x4_dconly` | Byte-exact 4:2:2 inverse chroma DC and untouched AC coefficients across QP 0 to 51 |
| `common/quant.c` | `optimize_chroma_2x2_dc`, `optimize_chroma_2x4_dc`, `denoise_dct` | Byte-exact retained or reduced coefficients, nonzero returns and unsigned noise accumulators |
| `common/quant.c`, `common/tables.c` | `decimate_score{15,16,64}`, `coeff_last{4,8,15,16,64}`, `coeff_level_run{4,8,15,16}` | Exact decisions, indices, CAVLC masks and native signed levels, including untouched output slots |
| `encoder/macroblock.c` | `dct2x2dc`, `idct_dequant_2x2_dc`, `idct_dequant_2x2_dconly` | Byte-exact chroma buffers, including the AC positions outside DC writes |
| `encoder/macroblock.c` | `mb_encode_i16x16`, lossy scalar quantization path for one plane | Byte-exact reconstruction, coefficient and nonzero buffers, coded-block pattern, and noise accumulators across QP 0 to 51, including denoising and decimation |
| `encoder/macroblock.c` | `mb_encode_chroma_internal`, intra lossy scalar path, including `zigzag_scan_2x2_dc` and `zigzag_scan_2x4_dc` | Byte-exact 4:2:0 and 4:2:2 reconstruction, scan order, coded-block flags and noise sums across QP 0 to 51 |
| `encoder/macroblock.c` | `x264_predict_lossless_chroma` and the lossless residual path of `mb_encode_chroma_internal` | Byte-exact 4:2:0 and 4:2:2 frame and field residuals, DC scan, coded-block flags and copied pixels for every chroma mode |
| `encoder/macroblock.c`, `common/pixel.c` | Inter branch of `mb_encode_chroma_internal` with scalar quantization and `pixel_var2_{8x8,8x16}` | Byte-exact reconstructed pixels, coefficients and coded-block flags with precomputed prediction, joint-plane variance gating and AC decimation |
| `encoder/macroblock.c` | `macroblock_encode_internal`, inter 4x4 scalar luma path with supplied prediction | Byte-exact pixels, coded-block flags, coefficients and noise sums, including block and macroblock decimation |
| `encoder/macroblock.c` | `macroblock_encode_internal`, inter 8x8 scalar luma path with supplied prediction | Byte-exact pixels, coded-block flags, coefficients and noise sums, including frame and field scans |
| `common/set.c` | Scaling lists and quantization table setup | Compare generated matrices and biases with x264 for the same configuration |
| `common/mc.c` | `hpel_filter`, `mc_luma`, `get_ref`, `mc_chroma` | Byte-exact horizontal, vertical and diagonal half-pel planes, all luma and chroma phases, explicit weights, and scratch-versus-reference selection |
| `common/mc.c` | Frame-wide luma half-pel filtering with replicated reference boundaries | Byte-exact x264 horizontal, vertical and diagonal planes, including every padded sample in five frame shapes |
| `common/mvpred.c`, `common/base.h` | `x264_mb_predict_mv_16x16`, `x264_mb_predict_mv_pskip`, `x264_cabac_mvd_sum` | Exact predicted vectors and contexts across 7,500 cases covering unavailable neighbors, reference matches, top-left fallback and signed vector extremes |
| `common/mc.c` | `integral_init{4,8}{h,v}`, `frame_init_lowres_core` | Byte-exact wrapped integral sums and all four low-resolution reference planes |
| `common/pixel.c` | Fixed-block `x264_pixel_{sad,ssd,satd,sa8d}_WxH` and `x264_pixel_{sad,satd}_x{3,4}_WxH` | Integer score parity for 54 scalar entry points, including complete score arrays and their untouched slots |
| `common/pixel.c` | `x264_pixel_ads{1,2,4}` | Exact thresholding and retained candidate order |
| `encoder/me.c` | `x264_me_search_ref` for DIA, HEX, UMH, ESA, and TESA, plus `x264_me_refine_qpel` and `x264_me_refine_qpel_refdupe` | Exact quarter-pel vectors, total costs, MV costs, and reference thresholds across every luma partition and subme 0 through 11. Covers weighted chroma refinement, field offsets, and 26-pixel displacement. |
| `encoder/me.c` | `x264_me_refine_bidir_satd` | Exact winning vectors for both lists and byte-exact temporary luma pixels, including weighted prediction, tie handling, and boundary early returns |
| `common/pixel.c`, `common/mc.c`, `encoder/me.c` | Remaining metrics, reference preparation, bidirectional rate-distortion and quarter-pel rate-distortion refinement | Compare samples, scores and winning vectors using the same references and tie breaks |
| `common/predict.c` | All scalar 16x16, 4x4 and 8x8 luma modes, 8x8 and 8x16 chroma modes, and `predict_8x8_filter_c` | Byte-exact pixels in all 45 modes and all 128 combinations of filter and neighbor masks |
| `common/deblock.c` | `deblock_{v,h}_luma_c`, `deblock_h_luma_mbaff_c`, `deblock_{v,h}_chroma_c`, `deblock_h_chroma_{422,mbaff}_c` and their seven intra variants | Byte-exact inter and intra edge pixels with multiple strides, including pixels outside the filtered edge |
| `common/deblock.c` | `deblock_edge`, `deblock_edge_intra`, `deblock_strength_c` | Exact QP and offset thresholds, reference and motion-vector boundary strengths, and untouched strength slots |
| `common/deblock.c` | Progressive 4:2:0 frame-row filtering for 4x4 transforms, with uniform picture QP and a single reference | 24 bit-exact comparisons using the x264 edge and strength functions in row order. FFmpeg also matches the filtered Vulkan reconstruction across successive P pictures. |
| `encoder/analyse.c`, `encoder/rdo.c`, `encoder/macroblock.c` | Candidate selection, reconstructed distortion and bit cost | Compare per-block choices, with Warzone frame 31 as a visual regression case |
| `common/cabac.c` | `x264_cabac_init`, `x264_cabac_context_init`, `x264_cabac_encode_init_core`, `x264_cabac_encode_init`, `x264_cabac_encode_decision_c`, `x264_cabac_encode_bypass_c`, `x264_cabac_encode_ue_bypass`, `x264_cabac_encode_terminal_c`, `x264_cabac_encode_flush` | Exact contexts for every QP and model, arithmetic state, carry propagation, and output bytes |
| `encoder/cabac.c` | `cabac_block_residual_internal`, `x264_cabac_block_residual_c` | Exact bytes and context state after residual coding in all 14 block categories and the 4:2:2 DC path with frame and field scans |
| `encoder/cabac.c` | `cabac_mb_type_intra`, `cabac_intra4x4_pred_mode`, `cabac_intra_chroma_pred_mode` | Exact bytes and context state for intra type suffixes, I_PCM termination, luma modes and chroma neighbor contexts |
| `encoder/cabac.c` | `cabac_cbp_luma`, `cabac_cbp_chroma`, `cabac_qp_delta`, `cabac_transform_size` | Exact bytes, context state and effective QP across all current and previous QP pairs, including unavailable neighbors and empty I_16x16 blocks |
| `encoder/cabac.c` | `x264_cabac_mb_skip`, `cabac_mvd_cpn` | Exact bytes and contexts for P/B skip and both motion-vector components, including zero, signed Exp-Golomb tails, and the returned magnitude cap |
| `encoder/cabac.c` | Integrated I-slice I_16x16 CABAC path with fixed QP and 4:2:0 chroma | FFmpeg reconstruction matches every Vulkan Y, Cb and Cr sample for pictures containing empty, DC-only and AC-coded macroblocks |
| `encoder/me.c`, `encoder/cabac.c` | Integrated P-skip and single-reference 16x16 P_L0 paths with luma UMH search and fractional motion | Byte-exact x264 P-skip slice and sample-exact FFmpeg reconstruction of both P pictures, including interpolated reference, 4x4 luma residuals, and empty, DC-only and AC-coded chroma |
| `encoder/encoder.c`, `encoder/cabac.c` | Integrated raw 4:2:0 input with batched spatial dependencies, one IDR and successive P pictures | FFmpeg sample-exact reconstruction and FFprobe crop, order and timing checks across frame-number wrap. The same path processes full 1920x1080 pictures. |
| `common/bitstream.h` | `bs_init`, `bs_pos`, `bs_flush`, `bs_realign`, `bs_write{,1,32,_ue,_ue_big,_se,_te}`, `bs_align_{0,1,10}`, `bs_rbsp_trailing`, `bs_size_{ue,ue_big,se,te}` | Byte-exact writes, bit positions and 64-bit reservoir against the native writer |
| `common/bitstream.c` | `nal_escape_c`, `x264_nal_encode` | Exact emulation-prevention bytes, Annex B and length-prefixed output, and AVC-Intra padding |
| `encoder/set.c` | `scaling_list_write`, `x264_sps_write`, `x264_pps_write` | Byte-exact 8- and 10-bit syntax, VUI timing and color fields, cropping, HRD, default and custom scaling matrices, and AVC-Intra branches |
| `encoder/encoder.c` | `slice_header_write` | Exact I/P/B headers and pending bit-writer state, including MBAFF, POC, reference reordering, P weighting and reference marking |
| `encoder/cavlc.c`, `encoder/cabac.c` | Remaining entropy syntax and slice assembly | Compare bins and bytes against the reference, then decode complete slices independently |
| `encoder/slicetype.c`, `encoder/ratecontrol.c`, `encoder/encoder.c` | Lookahead, rate control, GOP decisions and boundary scheduling | Compare frame decisions, achieved bitrate, decoded clips and GPU frame time |

The target is the full encoding pipeline. Platform I/O, CLI parsing, muxing,
and Vulkan command submission have host responsibilities. The CPU C and SIMD
implementations remain reference code for parity. The GPU rewrite needs one
implementation per operation across vendors. The scalar transform and scan
entry points in `common/dct.c` have Vulkan parity tests for the 8-bit build.
The tests exercise `pixel_sub_wxh` through its transform callers. The init
functions select CPU implementations and remain with the C reference.
The scalar `common/quant.c` and `common/predict.c` dispatch entries
also have 8-bit parity tests. Helpers are exercised through their callers.
Deblocking is verified at the edge and strength level and for the
progressive 4:2:0 frame path. Frame-level scheduling for MBAFF,
other chroma formats, and mixed macroblock types remains to be ported.

## Full tree

### Repository root (18)

- `.gitignore`
- `.gitlab-ci.yml`
- `AUTHORS`
- `COPYING`
- `Makefile`
- `UPSTREAM-REVISION`
- `autocomplete.c`
- `config.guess`
- `config.sub`
- `configure`
- `example.c`
- `version.sh`
- `x264.c`
- `x264.h`
- `x264cli.h`
- `x264dll.c`
- `x264res.manifest`
- `x264res.rc`

### common (168)

- `common/aarch64/asm-offsets.c`
- `common/aarch64/asm-offsets.h`
- `common/aarch64/asm.S`
- `common/aarch64/bitstream-a.S`
- `common/aarch64/bitstream.h`
- `common/aarch64/cabac-a.S`
- `common/aarch64/dct-a-common.S`
- `common/aarch64/dct-a-sve.S`
- `common/aarch64/dct-a-sve2.S`
- `common/aarch64/dct-a.S`
- `common/aarch64/dct.h`
- `common/aarch64/deblock-a-common.S`
- `common/aarch64/deblock-a-sve.S`
- `common/aarch64/deblock-a.S`
- `common/aarch64/deblock.h`
- `common/aarch64/mc-a-common.S`
- `common/aarch64/mc-a-sve.S`
- `common/aarch64/mc-a.S`
- `common/aarch64/mc-c.c`
- `common/aarch64/mc.h`
- `common/aarch64/pixel-a-common.S`
- `common/aarch64/pixel-a-sve.S`
- `common/aarch64/pixel-a.S`
- `common/aarch64/pixel.h`
- `common/aarch64/predict-a.S`
- `common/aarch64/predict-c.c`
- `common/aarch64/predict.h`
- `common/aarch64/quant-a.S`
- `common/aarch64/quant.h`
- `common/arm/asm.S`
- `common/arm/bitstream-a.S`
- `common/arm/bitstream.h`
- `common/arm/cpu-a.S`
- `common/arm/dct-a.S`
- `common/arm/dct.h`
- `common/arm/deblock-a.S`
- `common/arm/deblock.h`
- `common/arm/mc-a.S`
- `common/arm/mc-c.c`
- `common/arm/mc.h`
- `common/arm/pixel-a.S`
- `common/arm/pixel.h`
- `common/arm/predict-a.S`
- `common/arm/predict-c.c`
- `common/arm/predict.h`
- `common/arm/quant-a.S`
- `common/arm/quant.h`
- `common/base.c`
- `common/base.h`
- `common/bitstream.c`
- `common/bitstream.h`
- `common/cabac.c`
- `common/cabac.h`
- `common/common.c`
- `common/common.h`
- `common/cpu.c`
- `common/cpu.h`
- `common/dct.c`
- `common/dct.h`
- `common/deblock.c`
- `common/frame.c`
- `common/frame.h`
- `common/loongarch/dct-a.S`
- `common/loongarch/dct.h`
- `common/loongarch/deblock-a.S`
- `common/loongarch/deblock.h`
- `common/loongarch/loongson_asm.S`
- `common/loongarch/loongson_util.S`
- `common/loongarch/mc-a.S`
- `common/loongarch/mc-c.c`
- `common/loongarch/mc.h`
- `common/loongarch/pixel-a.S`
- `common/loongarch/pixel-c.c`
- `common/loongarch/pixel.h`
- `common/loongarch/predict-a.S`
- `common/loongarch/predict-c.c`
- `common/loongarch/predict.h`
- `common/loongarch/quant-a.S`
- `common/loongarch/quant.h`
- `common/loongarch/sad-a.S`
- `common/macroblock.c`
- `common/macroblock.h`
- `common/mc.c`
- `common/mc.h`
- `common/mips/dct-c.c`
- `common/mips/dct.h`
- `common/mips/deblock-c.c`
- `common/mips/deblock.h`
- `common/mips/macros.h`
- `common/mips/mc-c.c`
- `common/mips/mc.h`
- `common/mips/pixel-c.c`
- `common/mips/pixel.h`
- `common/mips/predict-c.c`
- `common/mips/predict.h`
- `common/mips/quant-c.c`
- `common/mips/quant.h`
- `common/mvpred.c`
- `common/opencl.c`
- `common/opencl.h`
- `common/opencl/bidir.cl`
- `common/opencl/downscale.cl`
- `common/opencl/intra.cl`
- `common/opencl/motionsearch.cl`
- `common/opencl/subpel.cl`
- `common/opencl/weightp.cl`
- `common/opencl/x264-cl.h`
- `common/osdep.c`
- `common/osdep.h`
- `common/pixel.c`
- `common/pixel.h`
- `common/ppc/dct.c`
- `common/ppc/dct.h`
- `common/ppc/deblock.c`
- `common/ppc/deblock.h`
- `common/ppc/mc.c`
- `common/ppc/mc.h`
- `common/ppc/pixel.c`
- `common/ppc/pixel.h`
- `common/ppc/ppccommon.h`
- `common/ppc/predict.c`
- `common/ppc/predict.h`
- `common/ppc/quant.c`
- `common/ppc/quant.h`
- `common/predict.c`
- `common/predict.h`
- `common/quant.c`
- `common/quant.h`
- `common/rectangle.c`
- `common/rectangle.h`
- `common/set.c`
- `common/set.h`
- `common/tables.c`
- `common/tables.h`
- `common/threadpool.c`
- `common/threadpool.h`
- `common/vlc.c`
- `common/win32thread.c`
- `common/win32thread.h`
- `common/x86/bitstream-a.asm`
- `common/x86/bitstream.h`
- `common/x86/cabac-a.asm`
- `common/x86/const-a.asm`
- `common/x86/cpu-a.asm`
- `common/x86/dct-32.asm`
- `common/x86/dct-64.asm`
- `common/x86/dct-a.asm`
- `common/x86/dct.h`
- `common/x86/deblock-a.asm`
- `common/x86/deblock.h`
- `common/x86/mc-a.asm`
- `common/x86/mc-a2.asm`
- `common/x86/mc-c.c`
- `common/x86/mc.h`
- `common/x86/pixel-32.asm`
- `common/x86/pixel-a.asm`
- `common/x86/pixel.h`
- `common/x86/predict-a.asm`
- `common/x86/predict-c.c`
- `common/x86/predict.h`
- `common/x86/quant-a.asm`
- `common/x86/quant.h`
- `common/x86/sad-a.asm`
- `common/x86/sad16-a.asm`
- `common/x86/trellis-64.asm`
- `common/x86/util.h`
- `common/x86/x86inc.asm`
- `common/x86/x86util.asm`

### doc (5)

- `doc/ratecontrol.txt`
- `doc/regression_test.txt`
- `doc/standards.txt`
- `doc/threads.txt`
- `doc/vui.txt`

### encoder (19)

- `encoder/analyse.c`
- `encoder/analyse.h`
- `encoder/api.c`
- `encoder/cabac.c`
- `encoder/cavlc.c`
- `encoder/encoder.c`
- `encoder/lookahead.c`
- `encoder/macroblock.c`
- `encoder/macroblock.h`
- `encoder/me.c`
- `encoder/me.h`
- `encoder/ratecontrol.c`
- `encoder/ratecontrol.h`
- `encoder/rdo.c`
- `encoder/set.c`
- `encoder/set.h`
- `encoder/slicetype-cl.c`
- `encoder/slicetype-cl.h`
- `encoder/slicetype.c`

### extras (8)

- `extras/avisynth_c.h`
- `extras/cl.h`
- `extras/cl_platform.h`
- `extras/getopt.c`
- `extras/getopt.h`
- `extras/intel_dispatcher.h`
- `extras/inttypes.h`
- `extras/stdint.h`

### filters (13)

- `filters/filters.c`
- `filters/filters.h`
- `filters/video/cache.c`
- `filters/video/crop.c`
- `filters/video/depth.c`
- `filters/video/fix_vfr_pts.c`
- `filters/video/internal.c`
- `filters/video/internal.h`
- `filters/video/resize.c`
- `filters/video/select_every.c`
- `filters/video/source.c`
- `filters/video/video.c`
- `filters/video/video.h`

### input (9)

- `input/avs.c`
- `input/ffms.c`
- `input/input.c`
- `input/input.h`
- `input/lavf.c`
- `input/raw.c`
- `input/thread.c`
- `input/timecode.c`
- `input/y4m.c`

### output (10)

- `output/flv.c`
- `output/flv_bytestream.c`
- `output/flv_bytestream.h`
- `output/matroska.c`
- `output/matroska_ebml.c`
- `output/matroska_ebml.h`
- `output/mp4.c`
- `output/mp4_lsmash.c`
- `output/output.h`
- `output/raw.c`

### tools (21)

- `tools/bash-autocomplete.sh`
- `tools/checkasm-a.asm`
- `tools/checkasm-aarch64.S`
- `tools/checkasm-arm.S`
- `tools/checkasm-loongarch.S`
- `tools/checkasm.c`
- `tools/cltostr.sh`
- `tools/countquant_x264.pl`
- `tools/digress/__init__.py`
- `tools/digress/cli.py`
- `tools/digress/comparers.py`
- `tools/digress/constants.py`
- `tools/digress/errors.py`
- `tools/digress/scm/__init__.py`
- `tools/digress/scm/dummy.py`
- `tools/digress/scm/git.py`
- `tools/digress/testing.py`
- `tools/gas-preprocessor.pl`
- `tools/msvsdepend.sh`
- `tools/q_matrix_jvt.cfg`
- `tools/test_x264.py`

