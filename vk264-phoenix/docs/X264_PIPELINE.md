# How x264 encodes — the function-by-function pipeline

All line numbers refer to `reference/x264/`. This is a **map**,
not a replacement for reading the source: before porting a function, open its
source lines and copy the behaviour exactly.

## 0. Entry points (`encoder/api.c`)

| function | line | role |
|---|---:|---|
| `x264_encoder_open` | 76 | allocates `x264_t`, validates params, builds cost tables, pixel function pointers |
| `x264_encoder_headers` | 159 | emits SPS / PPS / SEI (the SEI NAL our oracle now copies in E96) |
| `x264_encoder_encode` | 166 | the main per-frame entry: lookahead → slice → rate control → NAL output |
| `x264_encoder_close` | ~1800 | frees frames, tables, lookahead |

The oracle's CLI currently emulates `x264_encoder_encode` with ad-hoc C in
`tests/oracle_video_cli.c`. Phoenix must replace that with an explicit
`phx_encoder_encode` that calls the same sub-stages in the same order.

## 1. Parameter defaults and presets (`common/base.c`)

- `x264_param_default` (base.c:344) — medium defaults; the baseline for all presets.
- `param_apply_preset` (base.c:489) — ten preset deltas (ultrafast … placebo).
- `x264_param_apply_fastfirstpass`, `x264_param_apply_profile` (717, 749).

**Rule:** the oracle must mirror every field x264's presets touch, even when a
function using it is not yet ported. See `docs/PRESET_PARAM_MATRIX.md` §2b.

## 2. Frame input and lowres construction (`common/mc.c`, `encoder/lookahead.c`)

| function | line | role |
|---|---:|---|
| `x264_picture_alloc` | common/common.c | allocates plane buffers |
| `x264_frame_init_lowres` | common/mc.c:458 | builds the half-res planes used by lookahead (B-adapt, MB-tree, scenecut) |
| `x264_lookahead_put_frame` | encoder/lookahead.c:192 | queues a frame |
| `x264_lookahead_get_frames` | encoder/lookahead.c:223 | runs slicetype analysis and returns coding order |

The lowres plane layout is 4 sub-planes per luma 2×2 block. x264's
`frame_init_lowres_core` filters with `(((a+b+1)>>1)+((c+d+1)>>1)+1)>>1`. Our
oracle has `vk_lowres_luma` with the same formula — that function is one of
the few genuinely complete ports and should be kept verbatim.

## 3. Slice-type decision (`encoder/slicetype.c`)

| function | line | role |
|---|---:|---|
| `x264_slicetype_frame_cost` | ~580 | lowres intra/P/B cost arrays |
| `macroblock_tree` | 1091 | propagates lookahead costs to MB QP offsets |
| `scenecut_internal` | 1384 | `pcost >= (1-bias)*icost` with gop-dependent bias |
| `scenecut` | 1430 | flash-detection pruning (`real_scenecut` flag) |
| `x264_slicetype_analyse` | 1473 | sequential lookahead walk; checks only `frames[1]` |
| `x264_slicetype_decide` | 1745 | final I/P/B assignment, reordering, frame_num/POC |
| `x264_rc_analyse_slice` | 1976 | CRF cost estimate for VBV/lookahead |

**This is the single largest remaining gap** (E96/E98/E99/E101). The current
oracle uses a global per-frame scenecut rule (`vk_slicetype_idr_flags`), which
cannot reproduce x264's candidate set. Phoenix must port `scenecut` +
`x264_slicetype_analyse` as a state machine over `lookahead->last_nonb`.

Key detail from E98 (measured on the reference build):

```
clip 103 superfast real scenecut checks: frames 4, 13, 17, 23
clip 043 superfast: frame 23 only
clip 083 superfast: frame 4 only
```

## 4. Per-MB analysis (`encoder/analyse.c`, `encoder/me.c`)

| function | line | role |
|---|---:|---|
| `x264_mb_analyse_init` / `mb_analyse_init_qp` | 2918 / 255 | per-MB cost tables and QP |
| `x264_mb_analyse_intra` | ~2918 | I16 / I8 / I4 / chroma mode + cost decisions |
| `mb_analyse_intra8x8` | ~740 (inside file) | I8x8 analysis with base `lambda*4` and per-block `+3λ` |
| `mb_analyse_intra_chroma` | ~600 | chroma DC/H/V/DDL modes |
| `x264_mb_analyse_inter_p16x16` | ~2900 | P16 motion estimation (DIA/HEX/UMH/TESA) |
| `x264_mb_analyse_transform_rd` | ~2737 | 8x8-vs-4x4 transform RD at subme≥6 |
| `intra_rd` | ~3100 | full RD re-evaluation at high subme |

Cost-table init (`init_costs`, analyse.c:143) and `mb_analyse_load_costs`
(248) are currently missing in the oracle — a direct cause of the E88/E89
cost-model drift. Port them before any further cost tweaks.

## 5. Encode and reconstruct (`encoder/macroblock.c`)

| function | line | role |
|---|---:|---|
| `x264_macroblock_cache_load` | 855 | loads left/top neighbour MV, nnz, intra-mode caches |
| `x264_macroblock_encode` | 974 | dispatches by MB type (I16/I8/I4/P/B) |
| `x264_mb_encode_i4x4` | macroblock.h:132 | predict → DCT → quant → dequant → IDCT → add |
| `x264_mb_encode_i8x8` | macroblock.h:168 | 8x8 transform path; `STORE_8x8_NNZ` (macroblock.h:112) |
| `mb_encode_chroma_internal` | macroblock.c:259 | chroma DC 2×2 Hadamard + `quant_2x2_dc` |
| `x264_macroblock_cache_save` | common/macroblock.c:1680 | writes the reconstructed MB into caches |
| `x264_macroblock_write_cabac` | encoder/cabac.c:1088 | CABAC syntax |
| `x264_macroblock_write_cavlc` | encoder/cavlc.c:487 | CAVLC syntax |

The E83–E94 investigation showed real bugs in this area (I4 top-neighbour
`+bx`, top-left availability, I4 pred scan8 lookup, nnz cache semantics).
Phoenix should port `x264_macroblock_cache_load` / `_save` first, then all
mode-specific encoders, rather than patching the current ad-hoc copies.

## 6. Quantization and RD (`common/quant.c`, `encoder/rdo.c`)

| function | line | role |
|---|---:|---|
| `quant_4x4` / `quant_8x8` | quant.c:67 / 59 | plain deadzone quantization (`QUANT_ONE`, 50) |
| `quant_2x2_dc` | quant.c:96 | chroma DC quantization |
| `x264_quant_4x4_trellis` | rdo.c:1142 | QNS/CABAC trellis for 4x4 |
| `x264_quant_8x8_trellis` | rdo.c:1159 | 8x8 split into 4 4x4 QNS passes |
| `quant_trellis_cavlc` | rdo.c:921 | CAVLC bit-count-driven QNS |
| `x264_quant_luma_dc_trellis` | rdo.c:1098 | I16 DC trellis |
| `x264_quant_chroma_dc_trellis` | rdo.c | chroma DC trellis |

E70 (`cost_sig`), E86 (bounded CAVLC bit counter), E89 (I8 +3λ double count)
all live here. Trellis must be ported with the same `cabac.state` pointers and
`ctx_block_cat` values as x264 — never with a "close enough" cost model.

## 7. Entropy (`common/cabac.c`, `common/bitstream.c`, `encoder/cabac.c`, `encoder/cavlc.c`)

- `x264_cabac_init`, `x264_cabac_encode_decision{,_noup,_bypass}` — arithmetic coder.
- `x264_cabac_mb_type_*`, `cabac_cbp_luma`, `cabac_cbp_chroma`,
  `cabac_transform_size`, `cabac_coeffs` — syntax elements.
- `x264_cavlc_*`, coefficient token tables in `common/tables.c`.

**State rule:** the CABAC state array is part of the encoder's serial state.
Any shader mapping must keep it in a single-threaded serial stage (see
`SHADER_MAPPING.md`).

## 8. Rate control (`encoder/ratecontrol.c`)

`x264_ratecontrol_start` (1432), `x264_adaptive_quant_frame` (304),
`x264_ratecontrol_end` (1829). The audit uses fixed QP (`--qp 28`), so rate
control is currently bypassed; a faithful port still needs it for the real
project (CRF presets).

## Porting order implied by this map

1. Parameters + cost tables (§1–2)
2. Lookahead / slicetype state machine (§3) — closes the NAL-count gap
3. Neighbour caches + MB encode (§5) — closes the I4/I8 recon gaps
4. Analysis + quantization (§4, §6) — closes first-diff bytes
5. Entropy (§7) — byte-exact stream
6. Rate control (§8) — CRF parity
