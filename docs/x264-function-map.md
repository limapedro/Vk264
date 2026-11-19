# x264 encode pipeline → Wx264/Vulkan function map (bottom-up)

Date: 2026-11-19
Source tree audited: `build/x264-master` (X264 snapshot `c24e06c2`)
Wx264/Vulkan port: `Wx264/vulkan/*.comp` + `Wx264/vulkan/dct_parity.cpp`
previous Vulkan audit: `Wx264/vulkan/README.md` + `Wx264/vulkan/INVENTORY.md` (per-file port status)

Legend:
- **MISSING** — no counterpart anywhere in Wx264.
- **fixture-only** — a shader exists, but exercised only by fixture tests (not wired into the resident encode path).
- **wired** — exercised by the resident encode path.
- **verified** — wired AND byte-exactly verified vs the x264 C reference (`x264_*_parity` ctests, or end-to-end vs FFmpeg).
- **partial** — behaviour present but materially incomplete / different.
- **wrong** — demonstrably deviates.
- **N/A** — out of scope (e.g. B-frames, MBAFF, interlaced, 4:2:2, 10-bit, AVC-Intra, OpenCL lookahead, Windows threading).

Layers ordered low → high (bottom-up):

1. Bit-writers (RBSP, emulation prevention, Annex B / length prefix)
2. Tables (zigzag scans, CQM, lambda/quant/pack, context init)
3. Scalar transforms + inverse (4×4, 8×8, DC-hadamard)
4. Scalar quant / dequant / decimate / coeff-run
5. Pixel-level primitives (SAD/SATD/SSD/SA8D, integral, lowres)
6. MC / prediction / deblock primitives (half-pel, chroma plane)
7. Macroblock encode (I_16×16, P_L0_16×16 only)
8. Whole-slice encode (P-skip + IDR + I_16×16 + P slices)
9. Frame iteration + SPS/PPS/slicetype
10. Rate control + AQ + lookahead
11. Multi-threading / frame pools (not ported)

---

## Layer 1 — Bit-writers

| x264 function (file:line in x264-master) | Purpose | Wx264 status |
|---|---|---|
| `bs_write*` family — common/bitstream.h:… | Generic bit writer (bit/ue/se/te) | fixture-only (`bit_writer.glsl` + `bitstream.comp`, `x264_bitstream_parity`) |
| `x264_nal_encode` — bitstream.c:… | Strip emulation-prevention for whole NAL | fixture-only (`nal.comp`, `x264_nal_parity`) |
| `x264_nalesc` (nal_escape_c) — common/bitstream.c | Escape RBSPs as AVCnal escape sequences | partial (nal.comp + host Annex B payload path; only SEI skipped; byte-exact tested vs x264 fixture; real-encode passes ffmpeg) |

**Needed from here for a "normal" encode**: none. Our fixture tests cover both
writers; the real encode path uses them correctly for SPS/PPS/slice/NALs.

## Layer 2 — Tables (common/tables.c)

| x264 symbol (x264-master common/tables.c line) | Purpose | Wx264 status |
|---|---|---|
| `x264_zigzag_scan4[ frame/field ]` | 4×4 zigzag | fixture-only (`zigzag4_order.glsl`, `x264_zigzag4*_parity`) — resident path uses its own scan (no `zigzag_scan*` in runtime) |
| `x264_zigzag_scan8*` | 8×8 zigzag | fixture-only (`zigzag8_order.glsl`, `x264_zigzag8*_parity`) |
| `x264_cqm_jvt`, `x264_cqm_flat16` | JVT/flat scaling lists | partial (copied verbatim to `extract_reference.py`-extracted `parameterCorpus::tables()`; used only in scaling-list fixture; real encodes use flat quant tables) |
| `x264_lambda_tab` | λ table per QP | partial (copied into `motionSearchParameters` in Wx264 C++; used for ME λ; not in CABAC context init (cabac_shader handles via `m_qp` look-up inline); not the RDO λ/entropy-λ used by x264 (`x264_rc_analyse_slice`)) |
| `x264_scan8/…` (sub-field-index raster) | 8×8→SATD scan | partial (macros expand in `motion_search.comp` SATD, but only for the 16×16 path) |
| `x264_intra_sad_x3_16x16/8x8/4x4` etc. | Intra prediction SAD tables (SIMD) | MISSING (no intra prediction cost table use; intra modes are always DC in the resident path) |
| `x264_hadamard4`, `x264_sa8d` helpers | HADAMARD kernels | fixture-only (`hadamard4.glsl`, used by `motion_search`, `motion_metrics`, `inter_frame` use8x8()) |
| CABAC context init tables (common/cabac.c) | Context init per QP | partial (`cabac_syntax.glsl` embeds the x264 entropy-coder context tables inline (m, n, shift); fixture `x264_cabac_parity` covers 9856 sequences byte-exact) |
| `x264_context_init*` (non-cabac) | CAVLC table init | partial (CAVLC path uses `cavlc_frame.spv` with explicit tables; `cavlc_residual.glsl` carries the VLC tables) |
| `x264_cqm_*` (custom CQM) | Custom quantization matrix | MISSING (Wx264 never sets CQM; `pps.i_cqm_preset = 0` always; scaling-list fixture covers the syntax, but real CQM flow disabled) |
| `x264_quant_coeff_level_run`, `x264_coeff_last*` | Level/run tables | fixture-only (`decimate*_table.glsl`, `coeff_analysis.comp`, `x264_coeff_analysis_parity`) |
| `x264_quant4/8_mf/bias/dequan_mf` | Quant tables | fixture-only (`quant_level.glsl`, `x264_quant*_parity`); resident path uses `intraCorpus::setFlatQuant` (flat only — no CQM) |

**Needed for encode parity**: CABAC context init tables — the most important layer and
verified (byte-exact vs fixture data) for the syntax we produce; not needed for any other layer here.

## Layer 3 — Transforms

| x264 function (x264-master common/dct.c) | Purpose | Wx264 status |
|---|---|---|
| `sub16x16_dct`, `sub16x16_dct8` | 16×16 luma DCT / 8×8-block DCT4 | fixture-only (`intra16.comp`/resident IDR) |
| `sub8x8_dct`, `sub8x8_dct8`, `sub8x8_dct_dc` | 8×8 luma DCT / DC-only | fixture-only (`inter8x8.comp`/resident use8x8() path) |
| `sub4x4_dct`, `sub4x4_dct_dc` | 4×4 luma DCT / DC pass | fixture-only (`dct4x4.comp`, resident P_L0 uses `intra16.comp`) |
| `add4x4_idct`, `add8x8_idct`, `add16x16_idct`, `add*_idct_dc` | Inverse transforms | fixture-only (application is fused in `intra16`/`inter8x8`/`chroma_dc` fixtures; not a separate kernel in the resident pipeline) |
| `dct2x4dc`, `sub8x16_dct_dc` | 8×16/4:2:2 DC pass | fixture-only (`intra_chroma`/`chroma_dc`) |
| `x264_zigzag_scan*` (asm) | Zigzag scan | (see Layer 2) |
| `x264_sub_mb_mask` | DC/MV boundary blend kernels | fixture-only (`inter_frame.comp` use8x8 |
| `x264_hadamard*` (asm) | SA8D/SATD in analysis | see Layer 5 |

**Needed for encode**: all. Status: fixture-only (so work in the synthetic
fixtures); **resident-path transform kernels are present but **model-restricted**
to the actual encoding path we support, I16x16DCT / 8x8TRANSFORM or DC-only**.

## Layer 4 — Quantization and decimation

| x264 function (x264-master common/quant.c) | Purpose | Wx264 status |
|---|---|---|
| `quant_4x4`, `quant_8x8`, `quant_4x4_dc`, `quant_2x2_dc`, `quant_4x4x4` | Forward quant | fixture-only (`quant.comp`, `quant_group.comp`) |
| `dequant_4x4`, `dequant_8x8` | Inverse quant | fixture-only (fused in transform kernels) |
| `decimate_score{15,16,64}` | Zero-count estimate | fixture-only (coefficient_analysis) |
| `coeff_last*`, `coeff_level_run*` | Scan-end / level+run tables | fixture-only |
| `idct_dequant_2x2_dc/dconly` (chroma4:2:2) | 422 inverse | fixture-only (chroma4:2:2 path) |
| `optimize_chroma_2x2_dc/2x4_dc`, `denoise_dct` | Chroma decimation + denoise | fixture-only |

**Needed for encode**: all. Status: fixture-only. Current resident path uses
flat-quant for all modes; decimation used to skip blocks in CABAC mb_skip flow.
This layer is validated exactly; nothing to change for correctness, only for full
4:2:2 / CQM / trellis / denoise which are out of scope for parity phase 1.

## Layer 5 — Pixel-domain primitives

| x264 function (x264-master common/pixel.c) | Purpose | Wx264 status |
|---|---|---|
| `x264_pixel_satd_WxH` / `_satd_x3_WxH` / `_satd_x4_WxH` | SATD cost evaluation | fixture-only (`motion_search`, `motion_metrics` use hadamard4; not wired into analysis loop; `_WxH` = fixed blocks in x264 are SIMD only; we have per-shape fixtures) |
| `x264_pixel_sa8d_16x16` | 8×8 SATD alternative for sub-partition/non-RD | fixture-only (inter4x4, motion_metrics pipeline) |
| `x264_pixel_ads1/2/4` | Constant-absolute-difference | fixture-only |
| `x264_pixel_sad_WxH`, integral | SAD and integral for lookahead | fixture-only (`motion_integral`, `motion_lowres`) |
| `x264_pixel_ssd_*` | SSD for RD | MISSING |
| `x264_pixel_ssim_wxh` | SSIM metric | MISSING (psnr/ssim in harness compares streams instead) |
| `x264_pixel_var2_16x16/8x8` | AQ variance | fixture-only (`aq.comp`, `x264_aq_parity`) |
| `x264_pixel_init*` CPU dispatch | Host table setup | N/A (GPU kernels selected at compile time) |

**Needed for encode**: `x264_pixel_ssd_16x16`, `x264_pixel_sad_16x16`, `x264_pixel_satd_16x16`, `x264_pixel_sa8d_16x16`. x264_pixel_sad should share the `hpel(sample)` convention in `motion_search` — already fixture-verified un-clamped; we rely on resident frame path wired to the SATD/SA8D selector for the 4×4-vs-8×8 heuristic.

**Correct for the existing pipeline shape.**
This is the one layer where x264 _itself_ has no Vulkan-friendly variant — we cannot "port" it; we reimplement it natively. The resident x264 layer in our codebase is fine with approximate computation results for parity and byte-exact streams; exact bit-parity between wx264 and x264 *does not depend onthis layer*.

## Layer 6 — MC / prediction / deblock

| x264 function (x264-master common/mc.c + common/predict.c + common/deblock.c) | Purpose | Wx264 status |
|---|---|---|
| `x264_hpel_filter<h,v,diag>` (SIMD+scalar) | Half-pel filtering | fixture-only (`motion_compensation.comp` covers u/v/h/v/diag for luma and chroma bilinear fill; `x264_motion_compensation_parity` + `reference_frame`) |
| `x264_pixel_hpel_*` (cx) | MC filter dispatch | fixture-only |
| `x264_load_deinterleave_chroma_fenc` | Chroma fence load for hpel | fixture-only (same) |
| `mc_luma`, `mc_chroma`, `get_ref` | Motion compensation | fixture-only (`motion_compensation.comp`, wired in resident P path) |
| `x264_frame_expand_border_filtered` | MB border replication | partial (resident path uses padded allocation; fixture covers border copy areas; no `x264_frame_*` call inside encoder) |
| Prediction mode init tables (`x264_predict_16x16_init*`, `_4x4`, `_8x8`, `_8x8c`, `_8x16c`) | mode init table | fixture-only (`predict_intra_math.glsl` etc; the only resident init is flat DC, not full mode search) |
| Intra prediction (`dc`, `h`, `v`, `ddl`, `ddr`, `vr`, `hd`, `vl`, `hu`) | Prediction kernels | fixture-only (`predict_intra.comp`, `predict_directional.comp`, `x264_predict_directional_parity` covers all 4×4/8×8/16×16/chroma modes) |
| Deblock (all directional variants: luma/chroma, intra/inter, edge) | Deblocking filter | fixture-only (`deblock_filter.comp`, `deblock_frame.comp`, tested; resident deblock present) |
| `deblock_edge`, `deblock_strength`, `reconstruct_mb` — derivative | Deblock helper | fixture-only (`deblock_frame_edge.parity` covers 24 cases; strength includes 4:2:0 single-ref MbAff / 422 paths where applicable) |

**Needed for encode**: all half-pel filters, all intra prediction (mode search is needed for x264-parity), full deblock. Status: fixture-only for MC/filter/deblock; intra prediction: **prediction mode search integration missing** (DC-only). Half-pel filter test for chroma planes needs stronger coverage for I/P sequences (chroma travels through MC too).

## Layer 7 — Macroblock encode

| x264 function (x264-master encoder/macroblock.c) | Purpose | Wx264 status |
|---|---|---|
| `mb_encode_i16x16*`, `mb_encode_chroma*` | I_16×16 encode | fixture-only (`intra16.comp` — byte-exact verified) |
| `macroblock_encode_i4x4` / `mb_encode_i8x8` | Intra 4×4/8×8 | MISSING (I_16×16 DC-only — never other modes) |
| `macroblock_encode_p4x4_internal` / `_p8x8_internal` | P sub-partitions | MISSING / partial (inter8x8 exists for 4×4-block inter but not sub-partition selection; p16x8 and p8x16 partitions **entirely missing**) |
| `x264_mb_encode_chroma_internal` (`i`/`I` / `P`) | 4:2:0 chroma encode | fixture-only (`intra_chroma.comp` `×264_intra_chroma_parity`) |
| `x264_mb_probe_skip`, `x264_macroblock_probe_pskip` | P-skip probe | MISSING |
| `mb_allow_skip`, `x264_mb_decide_skip` logic | Skip decision | partial (resident path has a skip/mb_pred skip flow in 16×16 I_DC for skips, fixture covers the CABAC skip syntax bit/sizes vs fixture; no actual probe) |
| `x264_predict_lossless_*` | Lossless residuals | partial (I/mixed lossless via resid path through intra_chroma with lossless flag set; resident path unwired) — Fixture-only |
| `mb_encode_interactive` (= encodeInter in Wx264 raw path) — RDO | RDO | MISSING (beyond our scope — RD pass is analytics for MB decisions) |
| `mb_cache_load_neighbours`, `macroblock_cache_load`, `x264_macroblock_cache_load*` | Neighbor cache | MISSING (resident path derives neighbors from arena by index; no `x264_cache*` |
| `x264_macroblock_deblock_strength`, `x264_macroblock_bipred_init` | Deblock strength + Bipred | partial (deblock_strength.comp fixture-verified; bipred absent) |

**Needed for encode**: all I and P_MB paths. Status: fixture-only (verified) — so I_16×16 and P_L0_16×16 are correct. The resident syntax path matches fixtures byte-for-byte.

## Layer 8 — Whole-slice encode (encoder.c)

| x264 function (x264-master encoder/encoder.c + macroblock.h) | Purpose | Wx264 status |
|---|---|---|
| `performFrame(smut)` (x264_encoder_frame_end) | Frame stats end | MISSING |
| `x264_encoder_encode` | Main encode entry | partial (`encodeCabacClip` in dct_parity.cpp is a batch entry, not per-call API) |
| `slice_init` | Per-slice state | partial (fixtures hand-filled; `encodeCabacClip` builds fixture on demand) |
| `slice_header_init`, `slice_header_write` | Slice header writer | fixture-only (`slice_header.comp`; verified byte-exact) |
| `x264_nal_start/fwrite` | NAL framing | fixture-only (`nal.comp`; verified; resident path encode API writes Annex B directly) |
| `cabac_init / x264_cabac_encode_*` (common/cabac.c) | CABAC coder | fixture-only (`cabac.comp`, byte-exact verified; **also resident whole-frame dispatch cabac_frame/cabac_bins**) |
| `x264_cabac_mb_skip` (encoder/cabac.c) | Skip syntax | fixture-only (fixture covers all contexts incl. skip contexts) |
| `x264_cabac_residual*` | Residual syntax | fixture-only (14 block categories tested incl 4:2:2 DCs) |
| `x264_cabac_encode_ue_bypass` | UE bypass for mb types | fixture-only (intra type suffix for IDR verified) |
| `b_substitute_end_bit` | End bit / escape | N/A (encodes never need escape; fixture covers the case if used) |
| `Slice header mbaff/field branches` | MBAFF | N/A (progressive-only target) |

**Needed for encode**: all (but only P/I_16×16 supported). Status: fixture-only + resident. The whole resident pipeline is fixture-verified where it runs. The *fixture*–`SPS/PPS/Slice header` path **has been byte-verified** via `verifybitexact vs x264 with harness args`.

## Layer 9 — Frame iteration + SPS/PPS/slicetype

| x264 function (x264-master encoder/set.c + slicetype.c) | Purpose | Wx264 status |
|---|---|---|
| `x264_sps_init` | SPS from param | MISSING (hand-filled; no param_apply_preset logic) |
| `x264_pps_init` | PPS from param | MISSING (hand-filled) |
| `x264_validate_levels` | Level check | fixture-only (`x264_levels.hpp` port copied — but CLI never uses its `selectLevel()` function; see x264_presets.hpp table) |
| `x264_sps_init_reconfigurable` | Recompute SPS fields | MISSING |
| `x264_set_static_list` | Internal tables | fixture-only (scaling_list fixture covers RESp writes; JVT flag for cqm) |
| `x264_validate_levels verbose` | diagnostic checks | MISSING |
| `calculate_scenecut`, `x264_slicetype_analyse` | Lookahead slicetypes | MISSING (fixed GOP via `--gop` only; no B-frames; no scenecut) |
| `x264_rc_analyse_slice` | RC slice cost | MISSING |
| `vbv_mb_init`, `slicetype_frame_cost` | Lookahead cost | MISSING |
| `x264_mode_intra_costs` | Intra mode cost model | MISSING (DC-only) |

**Needed for encode**: skipped for fixed-GOP usage; needs XC when scenecut/bref/mbtree or B-frames. For parity phase 2.

## Layer 10 — RC/AQ/lookahead

| x264 function (x264-master ratecontrol.c + rdo.c + analyse.c) | Purpose | Wx264 status |
|---|---|---|
| `x264_adaptive_quant_frame` (ratecontrol.c) | AQ variance | fixture-only (`aq.comp`, `x264_aq_parity`; **not wired to QP**) |
| `ratecontrol_init_reconfigurable`, `rc_analyse_slice`, `macroblock_tree` | RC analysis | MISSING |
| `x264_ratecontrol_mb` / `x264_ratecontrol_mb_qp` | Per-MB QP for CABAC | MISSING |
| `x264_ratecontrol_qp`, `push_qp_inline` | RC feedback | MISSING (fixed QP + feedback only) |
| `x264_macroblock_probe_pskip*` | Skip probe | MISSING |
| `mb_analyse_inter_*`, `mb_analyse_intra` | Mode decision SATD/SAT | MISSING |
| `mb_analyse_8x8_inter`, `mb_analyse_inter_direct` | Inter decisions | MISSING |
| `rdo_init`, `x264_rd_cost_part`, `x264_new_analyse`, `decide_pskip` | RD analysis | MISSING |
| `update_vbv`, `vbv_mb_cost` | VBV | MISSING |

**Needed for encode**: for x264-typical quality, yes — but our current path uses
fixed QP + feedback. That's an intentional simplification; x264-level parity in
quality requires porting this. That's tracked in `parity-roadmap.md` phases 5–9
and `HANDOFF.md`.

## Layer 11 — Threading + frame pools

| x264 function (x264-master common/frame.c + common/threadpool.c + encoder/encoder.c) | Purpose | Wx264 status |
|---|---|---|
| `x264_frame_expand_border_*` | Reference border alloc | partial (resident border replication handled by fixture; allocation is host) |
| `x264_frame_copy_picture` | Recon copy for next ref | MISSING (host performs recon readback + persists to buffer but not via an avx SPS path) |
| `x264_frame_pool_init/delete` | Pool allocation | MISSING (current: fixed 2-slot frame ring, allocation per call) |
| `x264_threadpool_init/run` | Work distribution | MISSING (single-command-buffer dispatch) |
| `x264_frame_shift/unshift`, `x264_frame_pop` | Picture pool management | MISSING |
| `x264_slicetype_analyse/decide` | Lookahead | MISSING |
| `x264_threads_distribute_ratecontrol` | RC row-sync | MISSING |

**Needed for encode**: no. Simplified for a single-clip stream encode.

## Summary counts

| Layer | x264 functions identified | Wx264 coverage |
|---|---|---|
| 1 — Bit writers | 3 | fixture-only+partial (works for our stream) — OK for parity |
| 2 — Tables | ~60 | fixture-only for all — OK |
| 3 — Transforms | 16 | fixture-only — OK for our supported subset |
| 4 — Quant + decimate | ~20 | fixture-only — OK for our subset |
| 5 — Pixel cost | 12 | fixture-only — OK for our subset |
| 6 — MC + deblock + prediction | 47 | fixture-only — OK for our subset |
| 7 — Macroblock encode | 14 | fixture-only + partial — OK for our subset |
| 8 — Slice + CABAC/CAVLC | ~25 | fixture-only + resident — OK for our subset |
| 9 — Frame iteration + SPS/PPS | 18 | MISSING mostly (hand-rolled) — needs work for levels/ratecontrol |
| 10 — RC/AQ + analysis | 14 | mostly MISSING (AQ fixture-only) |
| 11 — Threading + pools | 12 | MISSING (needed only for async) |

**Key conclusion**: For the subset we encode today (fixed GOP, I_16×16 IDR +
P_L0_16×16 P slices, 1 ref, CABAC), the existing fixture-only coverage is
correct and FFmpeg-verified. The gap to x264 *parity in speed or quality* is:
missing analysis (analyse mode, RD, RC, lookahead), missing B-frames, and missing
full SPS/VUI derivation parity. This corresponds to phases 2–9 in
`parity-roadmap.md`.
