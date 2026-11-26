# `veryfast` preset — byte-exact parity audit

**Repo:** `E:\__PROJECTS__\Vk264`, branch `vk264-phoenix` (HEAD `55c3bad`).
**Scope:** read-only audit. No source/CMake/tools/tests were edited; no commit or
branch switch. The only output written is this file.
**Date:** 2026-11-26.

Commands executed (read-only, from repo root):

```
python tools/first_frame_probe.py --preset veryfast --entropy cabac
python tools/first_frame_probe.py --preset veryfast --entropy cavlc
python tools/cabac_bin_diff.py --preset veryfast --fixture build/parity/fixture.yuv
python tools/cabac_bin_diff.py --preset veryfast --fixture build/parity/fixture.yuv --compare-only
python -c "... json.load(open('build/parity/parity_report.json')) ..."
```

Note on the third command: on the first invocation it aborted during the
*temporary* instrumented-build step with
`LINK : fatal error LNK1104: cannot open file '...dbg_phx_lowres.obj'` — a
transient race with a concurrent worker rebuilding the shared temp dir
`C:\Users\multi\AppData\Local\Temp\opencode\l1_cabac_bin_diff`. Once the
instrumented `x264_trace.exe` / `phx_cli_trace.exe` were present, the identical
trace-and-compare path was reproduced with `--compare-only` (which still re-runs
both encoders at `--preset veryfast`, then compares freshly written logs). Log
timestamps were checked and the trace was also re-run atomically in-process
(`run_traces` + `parse_trace` + `expand_bins`) to confirm the numbers below are
a consistent veryfast snapshot, not a stale log from another worker.

---

## Headline verdict

`veryfast` is **not byte-exact**: `build/parity/parity_report.json` reports
`by_preset.veryfast = {cases: 10, compared: 10, byte_exact: 0, mismatches: 10,
first_diff_min: 37}`, and every one of the 10 veryfast cases has `first_diff: 37`
(5 fixtures × 2 entropies). The corpus total is
`{cases: 100, byte_exact: 0, mismatches: 100}`.

The first divergence is **not** in SPS/PPS/SEI, not in the IDR slice header, and
not in the CABAC arithmetic coder: it is a **macroblock decision** — the
`intra4x4_pred_mode` chosen for I4 block #6 of MB0. After that, the non-IDR
slice *headers* also diverge massively. Two independent fix sets are required.

---

## 1. NAL-level diff facts (both entropies)

### CABAC (`synth-veryfast-cabac`)

Excerpt of `first_frame_probe.py --preset veryfast --entropy cabac`:

```
stream bytes: ref=4162, phx=7158; NALs: ref=11, phx=11
 0  0  7 (SPS)       7 (SPS)          23    23  EXACT
 1  1  8 (PPS)       8 (PPS)           4     4  EXACT
 2  2  6 (SEI)       6 (SEI)         623   623  EXACT
 3  3  5 (IDR slice) 5 (IDR slice)  1113  1190  DIFF@7
    ref: 65 88 84 00 ff d9 28 b7 |e.....(.|
    phx: 65 88 84 00 ff d9 28 b6 |e.....(.|
 4  4  1 (non-IDR)   1 (non-IDR)      371   122  DIFF@2
 5  5  1 (non-IDR)   1 (non-IDR)      432  1036  DIFF@1
 6  6  1 (non-IDR)   1 (non-IDR)      360  1002  DIFF@0
 7  7  1 (non-IDR)   1 (non-IDR)      361  1023  DIFF@0
 8  8  1 (non-IDR)   1 (non-IDR)      283    33  DIFF@0
 9  9  1 (non-IDR)   1 (non-IDR)      245  1036  DIFF@0
10 10  1 (non-IDR)   1 (non-IDR)      305  1022  DIFF@0
parameter sets: 2/2 byte-exact (SPS, PPS)
VERDICT: DIVERGENCE (first stream byte diff 37)
```

Slice-header diagnosis from the same probe:

```
slice #1: HEADER MATCH (30 bits), ENTROPY/MB MISMATCH at post-header byte 2
          (RBSP byte 6, NAL payload byte 7)
  ref: 88 84 00 ff d9 28 b7 95 |.....(..|
  phx: 88 84 00 ff d9 28 b6 c1 |.....(..|
slice #2: HEADER MISMATCH: pic_order_cnt_lsb ref=2 phx=8
slice #3: HEADER MISMATCH (18 fields): slice_type ref=5(P) phx=6(B),
          num_ref_idx_active_override_flag 0/1, ref_pic_list_modification_flag_l0 0/1,
          adaptive_ref_pic_marking_mode_flag 0/29, slice_qp(derived) 28/0, ...
slice #4: HEADER MISMATCH (19 fields): slice_type P/B, pic_order_cnt_lsb 12/2, ...
slice #5: HEADER MISMATCH (14 fields): frame_num 4/3, pic_order_cnt_lsb 8/6, MMCO 1/0 ...
slice #6: HEADER MISMATCH (20 fields): slice_type B/P, frame_num 5/3,
          direct_spatial_mv_pred_flag 1/0, slice_qp 30/0, ...
slice #7: HEADER MISMATCH (11 fields): frame_num 5/4, cabac_init_idc 0/1,
          slice_qp 30/2, beta_offset_div2 0/1, ...
slice #8: HEADER MISMATCH (22 fields): slice_type P/B, pic_order_cnt_lsb 14/12,
          modification_of_pic_nums_idc[1]_l0 3/0, chroma log2_weight_denom 0/30, ...
  => fix the parameter-set / slice-header writer, not the entropy stage
```

### CAVLC (`synth-veryfast-cavlc`)

Excerpt of `first_frame_probe.py --preset veryfast --entropy cavlc`:

```
stream bytes: ref=4902, phx=8512; NALs: ref=11, phx=11
 0  0  7 (SPS)       7 (SPS)          23    23  EXACT
 1  1  8 (PPS)       8 (PPS)           4     4  EXACT
 2  2  6 (SEI)       6 (SEI)         623   623  EXACT
 3  3  5 (IDR slice) 5 (IDR slice)  1432  1517  DIFF@7
    ref: 65 88 84 00 fe 89 38 8e |e.....8.|
    phx: 65 88 84 00 fe 89 38 89 |e.....8.|
 4  4  1 (non-IDR)   1 (non-IDR)      424   123  DIFF@2
 5  5  1 (non-IDR)   1 (non-IDR)      507  1242  DIFF@1
 6 10 ... (same DIFF@0 pattern as CABAC for slices 6..10)
parameter sets: 2/2 byte-exact (SPS, PPS)
slice #1: HEADER MATCH (30 bits), ENTROPY/MB MISMATCH at post-header byte 2
slice #2..#8: HEADER MISMATCH (1, 17, 18, 13, 19, 10, 21 fields respectively)
VERDICT: DIVERGENCE (first stream byte diff 37)
```

**Facts that follow:**

* SPS (23 B), PPS (4 B) and SEI (623 B) are byte-identical for *both* entropies.
* The first divergent byte in the IDR slice is NAL payload byte 7 for both
  entropies, i.e. the stream-level byte 37 is the shared first divergence.
* The IDR slice header itself is byte-exact (all 14 decoded fields, 30 bits).
* Everything from non-IDR slice #2 onward has a *header* mismatch, so no later
  slice can be byte-exact until header generation is fixed.
* `parity_report.json` shows the same `first_diff: 37` for all 10 veryfast
  cases — one systematic cause, not per-fixture noise.

---

## 2. First divergent CABAC bin, element, context, and coder state

Excerpt of `cabac_bin_diff.py --preset veryfast --fixture build/parity/fixture.yuv --compare-only`:

```
x264 golden bins: 35339; Phoenix bins: 40589
FIRST DIVERGENT BIN: index 23 (0-based, bin #24)
  bin 20    x264: ctx=69   b=1  dec      phx: ctx=69   b=1  dec
  bin 21    x264: ctx=69   b=0  dec      phx: ctx=69   b=0  dec
  bin 22    x264: ctx=69   b=0  dec      phx: ctx=69   b=0  dec
  bin 23    x264: ctx=68   b=1  dec      phx: ctx=68   b=0  dec   <== FIRST DIVERGENCE
  bin 24    x264: ctx=68   b=1  dec      phx: ctx=69   b=0  dec
  bin 25    x264: ctx=68   b=0  dec      phx: ctx=69   b=1  dec
  x264 element: intra4x4_pred_mode (:83-96)
  phx  element: intra4x4_pred_mode (:83-96)
  x264 record: dec ctx=68  b=1 state=114 low/range 23194/334 -> 46800/256
  phx  record: dec ctx=68  b=0 state=114 low/range 23194/334 -> 46388/412
  coder state before bin: MATCH (x264 low/range 23194/334, phx 23194/334)
REMAINING DIVERGENCE: 0 inserted/deleted bin(s) removed; tail agrees for 23 bins
COEFFICIENT HANDOFF (MB0 luma 4x4 arrays):
  blocks compared: 16; raw arrays equal: 10/16; equal after zigzag rescan: 0/16
  block 6: x264 [7, -3, 0, 0, 0, 0] ... phx [5, 0, -2, 0, 0, 0]
```

Interpretation with source citations:

* The divergent syntax element is `cabac_intra4x4_pred_mode`
  (`reference/x264/encoder/cabac.c:83-96`): ctx 68 is
  `prev_intra4x4_pred_mode_flag`, ctx 69 carries `rem_intra4x4_pred_mode`.
  x264 emits the one-bin `ctx68=1` form → **the chosen mode equals the predicted
  (most probable) mode**; Phoenix emits `ctx68=0` plus three ctx-69 remainder
  bins → **the chosen mode differs**.
* **Coder state matches exactly**: both sides have context state `114` and
  `low/range = 23194/334` before bin 23, and all 23 preceding bins match in
  value *and* low/range transition. The CABAC arithmetic engine and context
  initialisation are therefore byte-correct here; the divergence is a *syntax
  decision* fed into a correct coder, not a coder bug.
* Grouping the ctx-68/69 records into per-block groups shows MB0 I4 blocks 0–5
  emit identical bin groups; **block #6 is the first differing group**. The
  golden MB0 dump (`L1MB0 type=0 part=0 pred16=0 chroma=6 cbp_l=15 cbp_c=2
  neigh=0 b8=0`) gives x264 block 6 `mode=1` (H), which equals its predicted
  mode (min of fixed modes of blocks 2 and 5 = min(DDL=3, H=1) = 1). Phoenix's
  remainder bins `0,1,0` decode to `i_mode=2` after the `i_mode--` of
  `cabac.c:90-91`, i.e. Phoenix chose mode 3 (DDL). Later I4 groups (9, 11, 18,
  …) also diverge, so this is a systematic I4 mode-selection difference.
* MB0 block 6 coefficients differ too (`x264 [7,-3,0,…]` vs `phx [5,0,-2,0,…]`),
  consistent with a different chosen prediction mode. 10/16 luma I4 arrays are
  already equal.
* **Trellis is not involved in veryfast**: `reference/x264/common/base.c:529-537`
  sets `i_trellis = 0` and `i_subpel_refine = 2` (→ `mbrd = 0`). Phoenix's
  `phx_analysis.c:576` trellis gate is therefore off and `phx_analysis.c:584-585`
  takes the deadzone path. The `compare_coefficients()` docstring in
  `tools/cabac_bin_diff.py` (written for the medium preset) and the "0/16 after
  zigzag rescan" result do **not** indicate a missing zigzag scan here —
  `src/phx_mbencode.c:308-324` already applies `phx_zigzag_scan4` per
  `encoder/macroblock.c:391/410`. The work item is the *mode decision*, not the
  quantiser handoff.

---

## 3. x264 functions already byte-correct in this preset

Evidence-based list (each row is byte-exact at the point of first divergence):

| x264 function / source | Phoenix port | Byte-exact evidence |
|---|---|---|
| `x264_sps_init` / `x264_sps_write`, `encoder/set.c:101`, `:305-475` | `src/phx_headers.c:13`, `:76` | SPS NAL 23/23 B EXACT, CABAC and CAVLC |
| `x264_pps_init` / `x264_pps_write`, `encoder/set.c:479`, `:505-570` | `src/phx_headers.c:98`, `:165` | PPS NAL 4/4 B EXACT, both entropies |
| SEI writers, `encoder/set.c:574`, `:593` | `src/phx_sei.c:322`, `:340` | SEI NAL 623/623 B EXACT, both entropies |
| Exp-Golomb / NAL escape, `common/bitstream.h:201-299`, `common/bitstream.c:29`, `:55` | `src/phx_bitstream.c:52-140` | NAL framing + parameter-set RBSP all exact |
| `x264_slice_header_init` / `_write`, `encoder/encoder.c:122-211`, `:213-364` | `src/phx_slice.c:14`, `:107` | IDR slice #1: HEADER MATCH, 14 fields / 30 bits, both entropies |
| CABAC engine: `x264_cabac_init` / `_context_init` / encode decision / renorm / bypass / terminal / flush, `common/cabac.c:32-183` | `src/phx_cabac.c:54-209` | 23 leading bins match in value **and** low/range; state 114 and low/range 23194/334 match at bin 23 |
| CABAC context tables, `common/tables.c:1717-1792` | `src/phx_cabac_syntax.c:732`, `:767`; `src/phx_rdo.c:215` | ctx 3, 68, 69, 399 are identical on both sides |
| `cabac_mb_header_i` / `transform_size` / `cabac_intra4x4_pred_mode`, `encoder/cabac.c:83-96`, `:233-239`, `:385-392` | `src/phx_cabac_syntax.c:216`, `:570-572` | bins 0–22 identical (MB header, transform flag, I4 blocks 0–5) |
| `x264_mb_predict_intra4x4_mode`, `common/macroblock.h:420-431`, fix table `common/predict.h:84-92` | `src/phx_analysis.c:425-433`, `:65-68`; `src/phx_cabac_syntax.c:174-181`, `:35-38` | predicted mode for MB0 block 6 is 1 on both sides (deduced from x264 `ctx68=1` and Phoenix remainder bins) |
| Deadzone `QUANT_ONE` / 4x4 quant, `common/quant.c:50-103` | `src/phx_quant.c:137-223` | 10/16 MB0 luma I4 coefficient arrays already identical |

---

## 4. Phoenix functions missing / incorrect (with x264 file:line citations)

### A. Blocks the IDR slice from being byte-exact (primary)

1. **I4 intra mode search** — x264 `mb_analyse_intra`, 4x4 section,
   `reference/x264/encoder/analyse.c:864-979` (pred mode `:882`; x3 fast path and
   `intra_analysis_shortcut` `:903-920`; full candidate loop with the `3*lambda`
   predicted-mode bonus and early-out `:924-947`; per-block encode-as-you-go
   `:950-960`). Listed as **MISSING** in `docs/FUNCTION_PORTS.md:862`
   (`mb_analyse_intra | encoder/analyse.c:668`).
   Phoenix replacement `src/phx_analysis.c:750-783` (`phx_search_i4`) loops all
   available modes and *does not implement* x264's `intra_analysis_shortcut`
   (`analyse.c:916-917`) or the x3/x9 initial-best path (`analyse.c:890-920`).
   This is where MB0 block 6 chose DDL(3) instead of H(1), producing the first
   divergent bin.
2. **4x4 predictors** — x264 `predict_4x4_*_c`,
   `reference/x264/common/predict.c:481-886`. Phoenix dispatch
   `src/phx_analysis.c:463-471` (`phx_predict4`), with a local source-exact V
   workaround `src/phx_analysis.c:454-459` and a now-stale comment at
   `src/phx_analysis.c:449-453` claiming `src/phx_predict.c:426` is wrong (that
   file now copies the four top samples correctly). Any SATD-level mismatch
   between these predictors and x264's SIMD `intra_mbcmp_x3/x9_4x4` feeds
   directly into item 1.
3. **SATD kernel** — x264 `x264_pixel_satd_4x4`,
   `reference/x264/common/pixel.c:265-310` (HADAMARD4 macro `:242`), used at
   `analyse.c:934`; Phoenix `phx_satd_4x4` at `src/phx_analysis.c:240` used at
   `:769`. Candidate source of the differing candidate costs.
4. **I4 encode/reconstruct for the next block's neighbours** — x264
   `x264_mb_encode_i4x4`, `reference/x264/common/macroblock.h:132-166`, called
   at `analyse.c:960`; Phoenix `phx_encode_i4` at `src/phx_analysis.c:590-607`
   with quantiser gate at `:572-586`. Determines the reconstructed samples that
   block 6 sees.
5. **I4 RD refine (not active at veryfast but required for other presets)** —
   x264 `intra_rd_refine` / `rd_cost_i4x4`, `reference/x264/encoder/analyse.c:1016-1135`,
   `encoder/rdo.c:315-347`; Phoenix `src/phx_analysis.c:885-933`, `:848-878`
   uses a size-only bit counter instead of a copied CABAC state. Not the cause
   at `subpel_refine=2`, but must stay aligned for the full corpus.

### B. Blocks every non-IDR slice from being byte-exact

6. **Slice header writer subset** — x264 `x264_slice_header_init` /
   `x264_slice_header_write`, `reference/x264/encoder/encoder.c:122-211`,
   `:213-364`. Phoenix `src/phx_slice.c:14`, `:107` is documented as a
   "subset (no MBAFF/MMCO/weight table)" in `docs/X264_INTERNALS.md:77`.
   Probe facts (§1) show wrong `pic_order_cnt_lsb`, `frame_num`, `slice_type`
   (P↔B), `num_ref_idx_active_override_flag`,
   `ref_pic_list_modification_flag_l0`, `modification_of_pic_nums_idc`,
   `adaptive_ref_pic_marking_mode_flag`, `cabac_init_idc`, `slice_qp_delta` and
   deblocking offsets from slice #2 onward.
7. **Slice-type decision / lookahead** — x264 `x264_slicetype_decide`,
   `reference/x264/encoder/slicetype.c:1745`; with
   `i_bframe_adaptive` = FAST and `rc.i_lookahead = 10`
   (`common/base.c:529-537`). Phoenix `src/phx_lookahead.c:89-504`, whose
   documented gaps (`src/phx_lookahead.h:27-31`) include MB-tree, trellis
   B-adapt, VBV lookahead and weighted prediction. Explains `slice_type P/B`
   inversions in slices #3, #4, #6, #8.
8. **POC / frame_num / reference lists / MMCO** — x264
   `reference/x264/encoder/encoder.c:2295`, `:2413`, `:2533`, `:3512`.
   Phoenix `src/phx_encoder.c:491-698`, `:698-785` uses a "simplified DPB of
   last reference" (`docs/X264_INTERNALS.md:76`). Explains `frame_num 4/3`,
   `5/3`, `5/4` and MMCO operation differences.
9. **Weighted prediction** — x264 `x264_analyse_weight_frame`,
   `reference/x264/encoder/analyse.c:219`, and weight-table syntax in
   `encoder/encoder.c:213-364`; **MISSING** (`docs/FUNCTION_PORTS.md:855`).
   Relevant to the P/B slices' `luma/chroma_log2_weight_denom` mismatches.
10. **CAVLC writer paths** — x264 `encoder/cavlc.c:70-704` (MB headers `:305-486`,
    residual `:121-196`) → `src/phx_cavlc.c:951-1114`,
    `src/phx_cavlc_syntax.c:69-411`. The CAVLC case has the same `first_diff=37`,
    so the shared MB decision/header cause dominates; these must be re-probed
    after the CABAC side is fixed, not changed speculatively now.
11. **I8x8 encoder** — x264 `encoder/macroblock.c:...` with
    `b_transform_8x8` on by default; Phoenix has no I8x8 encoder (note at
    `src/phx_encoder.c:1422-1424`, `docs/X264_INTERNALS.md:68`). MB0 uses I4 so
    it is not the first divergence, but it must be covered for whole-stream
    parity.

---

## 5. Prioritised, source-aligned work list to reach byte-exact

1. **P0 — I4 mode decision at MB0 block 6.** Port the exact x264 4x4 analysis
   path (`encoder/analyse.c:864-979`) into `src/phx_analysis.c:750-783`:
   reproduce `intra_mbcmp_x3/x9_4x4` initial costs (`:890-912`), the
   `intra_analysis_shortcut` pruning (`:916-917`), and the `3*lambda` bonus +
   `<= 0` early-out (`:935-943`) in the same mode order. Acceptance: the first
   divergent CABAC bin moves beyond index 23, MB0 I4 pred-mode bins 0–15 match,
   and block 6 coefficients become `[7,-3,0,…]`. Lock with a golden test before
   proceeding (per `docs/PARITY_ITERATION.md:50-56`).
2. **P1 — predictors, SATD and encode-as-you-go.** Align `phx_predict4`
   (`src/phx_analysis.c:463-471`) with `common/predict.c:481-886`,
   `phx_satd_4x4` (`:240`) with `common/pixel.c:265-310`, and `phx_encode_i4`
   (`:590-607`) with `common/macroblock.h:132-166`. Acceptance: all 16 MB0 luma
   I4 arrays equal before the CABAC stage.
3. **P2 — full slice-header writer.** Extend `src/phx_slice.c:14,107` to emit
   every field of `encoder/encoder.c:213-364` including ref-list modification,
   MMCO and weight tables. Acceptance: probe reports `HEADER MATCH` for slices
   #1–#8 in both entropies.
4. **P3 — frame/DPB state.** Fix `src/phx_lookahead.c:89-504` and
   `src/phx_encoder.c:491-698,698-785` against `encoder/slicetype.c:1745` and
   `encoder/encoder.c:2295,2413,2533,3512`. Acceptance: `slice_type`,
   `frame_num`, `pic_order_cnt_lsb` and ref counts match for all 8 slices.
5. **P4 — re-probe CAVLC and remaining paths.** After CABAC IDR and headers
   match, re-run `first_frame_probe.py --preset veryfast --entropy cavlc` and
   fix any CAVLC-specific residual/MB-header differences
   (`encoder/cavlc.c:70-704`); then cover I8x8 and the later frames.
6. **P5 — authority gate.** Re-run `tools/parity_harness.py` +
   `tools/compare_streams.py`; require `byte_exact` for all 100 cases. A probe
   `BYTE-EXACT` verdict alone is never sufficient
   (`docs/PARITY_ITERATION.md:100-108`).

---

## 6. `0/100` is not parity

`parity_report.json` currently reports `byte_exact: 0` for **100 of 100**
compared cases, and `byte_exact: 0` for **10 of 10** veryfast cases. That is
**0/100 and 0/10 — not parity**. Byte-exact SPS/PPS/SEI, byte-exact slice
headers on the IDR slice, 23 identical CABAC bins, a matching arithmetic-coder
state, or a visually identical decoded picture are all *diagnostics of where
the mismatch begins*, not parity. Only a whole-file byte-for-byte equality of
every Phoenix `.h264` against its x264 reference — 100/100 `byte_exact` in the
harness — constitutes parity (`docs/PARITY_ITERATION.md:3-6`, `:100-108`).
Until then the correct status for `veryfast` is **DIVERGENCE**, first stream
byte 37.

> **Post-audit note (orchestrator):** this report was captured before commit
> `becb95b` fixed x264's Annex-B start-code rule. Stream-level first diffs in
> it (e.g. byte 37) are stale; after that fix all 100 cases first diverge at
> bytes 587-671. NAL-payload offsets and CABAC bin findings remain valid.

