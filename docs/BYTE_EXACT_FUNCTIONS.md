# Byte-exact function contract — the 16 functions that must match x264

This is the work breakdown for reaching byte-exact stream parity. A case only counts when `tools/parity_harness.py` reports byte-exact for both CABAC and CAVLC. A function is "matched" only when a golden test compares its **inputs and outputs** to an instrumented x264 build — not just its chosen mode or a downstream byte offset.

Source of truth: `reference/x264/`, `docs/X264_INTERNALS.md`, `docs/PARITY_GAP_MATRIX.md`, `docs/preset_audit/*.md`. Phoenix file shown is the sole owner for each row; agents must not edit another row's file.

| # | x264 function (source) | Phoenix owner file | Byte-exact contract (golden) | Current state |
|---:|---|---|---|---|
| 1 | `mbcmp_init` (`encoder/encoder.c:1409-1410`) + `x264_pixel_sad_16x16`/`satd_16x16` (`common/pixel.c:55-73,234-330`) | `src/phx_analysis.c` (Z1, active) | For each subme/preset, the cost function selected (SAD if subme<2, SATD otherwise) and per-mode 4x4/8x8/16x16 costs equal x264's | superfast bin 5 proves SAD-vs-SATD mismatch |
| 2 | `mb_analyse_intra` I4 path with x3/x9 shortcuts (`encoder/analyse.c:864-979`) | `src/phx_analysis.c` (Z1, active) | Exact 16 I4 mode array + per-block SATD/SAD costs | veryfast/faster bin 23, slow/placebo bin 5 |
| 3 | `intra_rd`, `intra_rd_refine`, `rd_cost_i4x4` (`analyse.c:982-1135`, `rdo.c:162-197,315-347`, `COPY_CABAC rdo.c:71-73`) | `src/phx_analysis.c` + `src/phx_rdo.c` (Z1 leads) | Exact mode array and size-only bit counts for mbrd>=2 | slow/slower/veryslow/placebo |
| 4 | `x264_me_search_ref` fullpel DIA/HEX/UMH (`encoder/me.c:276-419`) + mv limits (`me.c:178-212`) | `src/phx_me.c` | For a fixed frame pair and MVP, exact MV and SAD+λ·bits cost | DIA/HEX pass; UMH rejected (`phx_me.c:170`) |
| 5 | subpel/hpel/qpel refinement (`encoder/me.c:801-992`, `common/mc.c:173-283`) | `src/phx_me.c` | Exact half/quarter-pel MV and interpolated reference samples | not ported |
| 6 | `mb_analyse_intra_chroma` (`encoder/analyse.c:586-665`) | `src/phx_chroma.c` | Exact chroma mode, 2x2 DC and 4 AC coefficient arrays | zigzag fixed (X1); mode/coeff trace pending |
| 7 | `quant_4x4`, `quant_4x4_dc`, `quant_4x4x4` and bias/bias0 tables (`common/quant.c:50-109`, `common/set.c:130-205`) | `src/phx_quant.c` (Y1, active) | 8-bit quant/dequant tables + per-coefficient outputs equal x264 objects | tables verified identical; I16 DC count fixed |
| 8 | `x264_quant_4x4_trellis` / `quant_trellis_cabac` (`encoder/rdo.c:641-895,1142-1157`) | `src/phx_rdo.c` | Per-coefficient QNS output and state transitions vs x264 golden | 10 vectors verified isolated |
| 9 | `x264_quant_chroma_dc_trellis` (`encoder/rdo.c:1114-1140`) | `src/phx_rdo.c` | `ue val=7` not `14` (|coeff|22 vs 29) at raw record 1623 | fast/medium bin 1663 open |
| 10 | `cabac_block_residual_internal` and coeff_last/ctx offsets (`encoder/cabac.c:664-748`, `common/quant.c:878-882`, `common/tables.c:1758-1792`) | `src/phx_cabac_syntax.c` | Exact ordered bin stream for one MB's residuals, including last/sigmap/level bins | coeff_last16 fixed; bin 932 next |
| 11 | `x264_macroblock_cache_load` / `_save` (`common/macroblock.c:855,1680`) | `src/phx_mbcache.c` | Exact neighbour nnz/mv/ref/pred arrays for left/top/top-left/top-right at every MB | progressive+MBAFF ported; needs golden parity per MB |
| 12 | `x264_slice_header_write` P/B fields (`encoder/encoder.c:213-364`) | `src/phx_slice.c` | Exact bit-level header (slice_type, frame_num, POC, num_ref_idx, RPL, MMCO, weights, qp_delta) | IDR exact; P/B headers differ from slice #2 |
| 13 | `x264_slicetype_decide`, scenecut, B-adapt FAST/TRELLIS (`encoder/slicetype.c:1384-1985`) | `src/phx_lookahead.c` | Exact frame-type sequence and coded-order frame_num/POC for each preset | all-P/BREF gap, TRELLIS B-adapt TODO |
| 14 | `x264_weights_analyse` (`encoder/slicetype.c:284`, `encoder/encoder.c:2163-2284`) | `src/phx_lookahead.c` | Exact pred_weight_table fields and offsets | not ported |
| 15 | `x264_ratecontrol_start`/`end`, AQ and chroma QP (`encoder/ratecontrol.c:1431-1538,1829-1928,304-415`) | `src/phx_ratecontrol.c` | Exact per-frame/per-MB QP sequence for CQP/CRF/ABR | CQP aligned; AQ/MB-tree incomplete |
| 16 | `x264_frame_deblock_row` (`common/deblock.c:378-606`, `common/macroblock.c:1438-1622`) | `src/phx_deblock.c` | Exact filtered luma/chroma sample arrays; recon equality with x264 | ported, byte-exact vs x264 objects; not wired into encoder |
| 17 | `x264_macroblock_probe_pskip` + fast P_SKIP detection (`encoder/analyse.c:2953-3024`, `:1294-1307`; `x264_mb_predict_mv_pskip` `common/mvpred.c:166-181`; `macroblock.c:639-670`) | `src/phx_analysis.c` (decision) + `src/phx_cabac_syntax.c`/`phx_cavlc_syntax.c` (writer) | For fixed fenc/fref, exact P_SKIP flag per MB and exact `mb_skip_flag`/`mb_skip_run` bins | **phx_encoder.c:1438 hard-codes `d.skip=0`; Phoenix never emits P_SKIP** — dominant cause of real-video stream blowup (P slices 292 B vs x264 6 B) |
| 18 | inter MB encode with residual: `x264_macroblock_encode` P paths (`encoder/macroblock.c:654-775`), motion compensation (`common/mc.c:251-283`) | `src/phx_mbencode.c` | For fixed fref/MV/ref/cbp, exact reconstructed samples + coefficient arrays vs x264 objects | phx_encoder.c only accepts `P_L0/D_16x16` with `cbp==0`; all residual-carrying inter modes fall back to intra |
| 19 | `I_8x8` transform/quant path (`encoder/macroblock.c:379-417`, `encoder/rdo.c:890`, `common/quant.c:4x8`) | `src/phx_mbencode.c` | Exact 64-coefficient array and reconstruction for a fixed 8x8 block | rejected in phx_mbencode.c (no 64-coefficient slot) |

## Real-video encode findings (baseline for rows 17-19)

`tools/oracle_video.py` on `D:/Videos/ssstwitter.com_1748999992229.mp4`, 64x64 crop, 30 frames, `--fps 25 --preset ultrafast --qp 28 --entropy cabac` (identical encode parameters, both streams decode OK):

```
SPS / PPS / SEI                 byte-exact (21 / 4 / 542 bytes)
IDR slice header                byte-exact (24 bits)
IDR slice entropy               first diff payload byte 46
P slice headers (#2..#30)       byte-exact (19 bits)
P slice entropy                 first diff payload byte 4 (first MB)
P slice sizes                   Phoenix 270-303 B vs x264 25-109 B
  first P frame                 Phoenix 292 B vs x264 6 B  (x264: all P_SKIP)
stream sizes                    Phoenix 9469 B vs x264 2475 B
```

Because all slice headers match, the remaining stream-level failure is **entirely in MB decisions/coefficients**, not in header writing. Row 17 (P_SKIP) is the highest-leverage gap for real content; rows 18-19 are required as soon as x264 codes a residual inter MB.

## Rules for each owner

- Golden first: write a test that calls x264's own object (or an instrumented build) with fixed inputs, then assert Phoenix's output **bit-for-bit**. No "chosen mode matches" shortcut.
- No overlap: each agent edits only its owned file(s) from the table and one test file. Integration into `phx_encoder.c` is done by the orchestrator after the owner's PR passes.
- No approximations, no parameter changes, no copied x264 bytes. Cite exact source lines and add a `SHADER:` tag.
- PR per function group with evidence: golden command output, ctest result, affected preset first-diff before/after.

## Current verified baseline

```
headers byte-exact 20/20 · SEI exact · start codes exact
100/100 phx rc0 · 100 refs · byte-exact 0/100
first stream diffs 586-825
```
