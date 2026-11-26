# Preset audit — `placebo` byte-exact parity blocker analysis

**Auditor scope:** read-only subagent audit, branch `vk264-phoenix`. No source, CMake, tool, test or other file was modified. Owned output: this file only.

Evidence used (all read-only):

- `python tools/first_frame_probe.py --preset placebo --entropy cabac`
- `python tools/first_frame_probe.py --preset placebo --entropy cavlc`
- `python tools/cabac_bin_diff.py --preset placebo --fixture build/parity/fixture.yuv`
- `build/parity/parity_report.json` (entries for all 10 `placebo` cases), plus direct byte comparison of `build/parity/synth-placebo-{cabac,cavlc}.{ref,phx}.h264`
- `docs/X264_INTERNALS.md`, `docs/FUNCTION_INVENTORY.md`, `docs/FUNCTION_PORTS.md`, `reference/x264/`, `src/`, and the sibling audit `docs/preset_audit/ultrafast.md`

---

## 0. Bottom line

`placebo` is **not byte-exact**. `build/parity/parity_report.json` → `by_preset.placebo`: `cases 10, x264_ok 10, compared 10, byte_exact 0, mismatches 10, first_diff_min 41`; global `totals`: `cases 100, byte_exact 0, mismatches 100, first_diff_min 35`.

**0/100 is not parity** (see §6). All 100 encodes succeed and the parameter sets are byte-identical, but not one saved Phoenix `.h264` equals its x264 reference.

The blockers, in strict byte/decision order, are:

1. **Stream byte 41:** Phoenix emits a **4-byte Annex-B start code for the SEI NAL** where x264 emits a **3-byte** one. Same defect as ultrafast (`docs/preset_audit/ultrafast.md:14-18`), shifted from byte 35 to byte 41 because placebo's SPS is 25 bytes (vs 21) and PPS 6 bytes (vs 4). The SEI **payload** is byte-exact.
2. **IDR slice payload byte 6 (CABAC) / byte 5 (CAVLC):** the first MB's CABAC bin 5 diverges at context 69 (`intra4x4_pred_mode`) with **identical coder state** — so the arithmetic coder is correct and the **intra4x4 prediction-mode decision / coefficient values** are wrong. This is a subme=11 / `i_mbrd=3` analysis and trellis-QNS defect, not an entropy defect.
3. **Every later slice header diverges** (POC, `frame_num`, `num_ref_idx` override, ref-pic-list modification, `pred_weight_table`, direct flag, `slice_qp_delta`, `cabac_init_idc`) — a consequence of the unported 16-ref/16-B DPB, MB-tree, weighted-P and trellis B-adapt paths, plus the million-dollar detail that fast-pskip is *disabled* at placebo and Phoenix ignores that flag.

---

## 1. NAL-level diff facts for both entropies

### 1.1 `python tools/first_frame_probe.py --preset placebo --entropy cabac` (excerpt)

```text
CASE synth-placebo-cabac
  stream bytes: ref=3192, phx=9103; NALs: ref=11, phx=11
NAL comparison (payload bytes, start codes and trailing zeros excluded; NALs paired by type sequence):
  ref# phx#  ref NAL                phx NAL                len ref len phx status             first diff
     0    0  7 (SPS)                7 (SPS)                     25      25  EXACT              -
     1    1  8 (PPS)                8 (PPS)                      6       6  EXACT              -
     2    2  6 (SEI)                6 (SEI)                    627     627  EXACT              -
     3    3  5 (IDR slice)          5 (IDR slice)             1066    1174  DIFF@6             6
    ref: 0000: 65 88 81 00 0f ff ce f4  |e.......|  <== first diff
    phx: 0000: 65 88 81 00 0f ff d9 3c  |e......<|  <== first diff
     4    4  1 (non-IDR slice)      1 (non-IDR slice)          300    1070  DIFF@3             3
     5    5  1 (non-IDR slice)      1 (non-IDR slice)          225    1083  DIFF@3             3
     6    6  1 (non-IDR slice)      1 (non-IDR slice)          168    1003  DIFF@6             6
     7    7  1 (non-IDR slice)      1 (non-IDR slice)          129    1016  DIFF@3             3
     8    8  1 (non-IDR slice)      1 (non-IDR slice)          185    1003  DIFF@0             0
     9    9  1 (non-IDR slice)      1 (non-IDR slice)          239    1018  DIFF@0             0
    10   10  1 (non-IDR slice)      1 (non-IDR slice)          180    1034  DIFF@2             2
  parameter sets: 2/2 byte-exact (SPS, PPS)
  slice #1: HEADER MATCH (34 bits), ENTROPY/MB MISMATCH at post-header byte 0 (RBSP byte 5, NAL payload byte 6)
    ref: 0000: 88 81 00 0f ff ce f4 35  |.......5|  <== first diff
    phx: 0000: 88 81 00 0f ff d9 3c a8  |......<.|  <== first diff
  slice #2: HEADER MISMATCH (14 differing field(s)):
    pic_order_cnt_lsb ref=8 phx=14; luma_log2_weight_denom ref=5 phx=0;
    luma_weight_l0_flag[0] ref=1 phx=0; luma_weight_l0[0] ref=31 phx=0;
    cabac_init_idc ref=0 phx=28; slice_qp ref=28 phx=0; ...
  slice #3: HEADER MISMATCH (1 differing field(s)): pic_order_cnt_lsb ref=4 phx=6
  slice #4: HEADER MATCH (38 bits), ENTROPY/MB MISMATCH at post-header byte 0
  slice #5: HEADER MISMATCH (6 differing field(s)): pic_order_cnt_lsb ref=6 phx=4;
    direct_spatial_mv_pred_flag ref=1 phx=0; num_ref_idx_l0_active ref=2 phx=1; ...
  slice #6: HEADER MISMATCH (43 differing field(s)): slice_type ref=P phx=B;
    pic_order_cnt_lsb ref=14 phx=8; num_ref_idx_l0_active ref=5 phx=1;
    ref_pic_list_modification_flag_l0 ref=1 phx=2; weight table absent in phx; ...
  slice #7: HEADER MISMATCH (11 differing field(s)): frame_num ref=4 phx=3;
    direct_spatial_mv_pred_flag ref=0 phx=1; num_ref_idx_l0_active ref=3 phx=2; ...
  slice #8: HEADER MISMATCH (3 differing field(s)): frame_num ref=5 phx=3;
    num_ref_idx_l0_active ref=4 phx=2
VERDICT: DIVERGENCE (first stream byte diff 41)
```

### 1.2 `python tools/first_frame_probe.py --preset placebo --entropy cavlc` (excerpt)

```text
CASE synth-placebo-cavlc
  stream bytes: ref=4047, phx=10961; NALs: ref=11, phx=11
     0    0  7 (SPS)  25/25  EXACT   |   1    1  8 (PPS)  6/6  EXACT
     2    2  6 (SEI)  627/627 EXACT
     3    3  5 (IDR slice)  1434/1532 DIFF@5
    ref: 0000: 65 88 81 00 0f eb 90 88  |........|
    phx: 0000: 65 88 81 00 0f e8 93 88  |........|
     4    4  1 (non-IDR slice)  389/1325 DIFF@3
     5    5  1 (non-IDR slice)  297/1284 DIFF@3
     6    6  1 (non-IDR slice)  232/1201 DIFF@5
     7    7  1 (non-IDR slice)  192/1218 DIFF@3
     8    8  1 (non-IDR slice)  336/1232 DIFF@0
     9    9  1 (non-IDR slice)  281/1229 DIFF@0
    10   10  1 (non-IDR slice)  186/1238 DIFF@2
  parameter sets: 2/2 byte-exact (SPS, PPS)
  Slice NALs: ref=8, phx=8
  slice #1: HEADER MATCH (34 bits), ENTROPY/MB MISMATCH at post-header byte 0 (RBSP byte 5, NAL payload byte 6)
    ref: 0000: 88 81 00 0f eb 90 88 94  |........|
    phx: 0000: 88 81 00 0f e8 93 88 94  |........|
  slice #2: HEADER MISMATCH (13 differing field(s)) — POC, weight table, cabac_init_idc, slice_qp, ...
  slice #4: HEADER MATCH (37 bits), ENTROPY/MB MISMATCH at post-header byte 0
  slice #6: HEADER MISMATCH (42 differing field(s)) — slice_type ref=P phx=B, ref counts,
            ref_pic_list_modification, whole pred_weight_table absent, ...
VERDICT: DIVERGENCE (first stream byte diff 41)
```

### 1.3 The probe hides the first stream-level byte: direct byte comparison

`tools/first_frame_probe.py` strips start codes, so its "SEI EXACT" does not mean the stream is exact. Direct comparison of the saved files:

```text
synth-placebo-cabac len 3192/9103 first_diff 41
 ref[32:60]: 01 68 e8 43 89 2c 8b 00 00 01 06 05 ff ff 6f dc 45 e9 bd e6 d9 48 b7 96 2c d8 20 d9
 phx[32:60]: 01 68 e8 43 89 2c 8b 00 00 00 01 06 05 ff ff 6f dc 45 e9 bd e6 d9 48 b7 96 2c d8 20
synth-placebo-cavlc len 4047/10961 first_diff 41
 ref[32:60]: 01 68 c8 43 89 2c 8b 00 00 01 06 05 ff ff 6f dc 45 e9 bd e6 d9 48 b7 96 2c d8 20 d9
 phx[32:60]: 01 68 c8 43 89 2c 8b 00 00 00 01 06 05 ff ff 6f dc 45 e9 bd e6 d9 48 b7 96 2c d8 20
```

Both show `00 00 01 06` (x264) vs `00 00 00 01 06` (Phoenix): the **SEI start code is one byte too long**. x264's rule is `reference/x264/encoder/encoder.c:2082-2083` — long start code only for `!i` (the first NAL), SPS, PPS and AVC-Intra; SEI and all slices get 3 bytes. Phoenix violates it at `src/phx_encoder.c:291`, `src/phx_sei.c:379` and `src/phx_encoder.c:1553`. This is the true first divergence of every placebo case (byte 41 = 35 + 6 bytes of longer SPS/PPS relative to ultrafast).

### 1.4 `build/parity/parity_report.json` — all 10 placebo cases

| case | ref bytes | phx bytes | first_diff | byte_exact |
|---|---:|---:|---:|---|
| synth-placebo-cabac | 3192 | 9103 | 41 | false |
| synth-placebo-cavlc | 4047 | 10961 | 41 | false |
| ssstwitter.com_1748999992229-placebo-cabac | 5108 | 15200 | 41 | false |
| ssstwitter.com_1748999992229-placebo-cavlc | 5518 | 15831 | 41 | false |
| ssstwitter.com_1732159163230-placebo-cabac | 2041 | 6671 | 41 | false |
| ssstwitter.com_1732159163230-placebo-cavlc | 2045 | 7015 | 41 | false |
| ssstwitter.com_1732158697723-placebo-cabac | 7033 | 21698 | 41 | false |
| ssstwitter.com_1732158697723-placebo-cavlc | 7546 | 22697 | 41 | false |
| ssstwitter.com_1732159939975-placebo-cabac | 5585 | 17173 | 41 | false |
| ssstwitter.com_1732159939975-placebo-cavlc | 5956 | 17652 | 41 | false |

Every case has `x264_rc=0`, `phx_rc=0`, `byte_exact=false`. Phoenix streams are roughly 2–3× the reference size, consistent with the later slices choosing different (non-skip, larger) decisions — i.e. fast-pskip being effectively disabled in x264 at placebo while Phoenix never probes skip at all.

---

## 2. First divergent CABAC bin: element, context and coder state

Command: `python tools/cabac_bin_diff.py --preset placebo --fixture build/parity/fixture.yuv` (re-verified in isolation with `--compare-only`).

```text
x264 golden bins: 28368; Phoenix bins: 40919
FIRST DIVERGENT BIN: index 5 (0-based, bin #6)
  bin 1     x264: ctx=399  b=0  dec      phx: ctx=399  b=0  dec
  bin 2     x264: ctx=68   b=1  dec      phx: ctx=68   b=1  dec
  bin 3     x264: ctx=68   b=0  dec      phx: ctx=68   b=0  dec
  bin 4     x264: ctx=69   b=1  dec      phx: ctx=69   b=1  dec
  bin 5     x264: ctx=69   b=1  dec      phx: ctx=69   b=0  dec       <== FIRST DIVERGENCE
  bin 6     x264: ctx=69   b=1  dec      phx: ctx=69   b=0  dec
  bin 7     x264: ctx=68   b=0  dec      phx: ctx=68   b=0  dec
  bin 8     x264: ctx=69   b=0  dec      phx: ctx=69   b=0  dec
  bin 9     x264: ctx=69   b=1  dec      phx: ctx=69   b=1  dec
  x264 element: intra4x4_pred_mode (:83-96)
  phx  element: intra4x4_pred_mode (:83-96)
  x264 record: dec ctx=69   b=1  state=121  low/range 6560/436 -> 6560/258   intra4x4_pred_mode (:83-96)
  phx  record: dec ctx=69   b=0  state=121  low/range 6560/436 -> 13636/356   intra4x4_pred_mode (:83-96)
  coder state before bin: MATCH (x264 low/range 6560/436, phx 6560/436)

REMAINING DIVERGENCE (after removing inserted Phoenix bin(s)):
  0 inserted/deleted bin(s) removed; tail agrees for 5 bins; next mismatch at aligned index 5:
  x264=('dec', 69, 1), phx=('dec', 69, 0)

COEFFICIENT HANDOFF (MB0 luma 4x4 arrays):
  blocks compared: 16; raw arrays equal: 6/16; equal after zigzag rescan: 0/16
  block 1: x264 [3, -1, -2, 1, 1, 0] ... phx [7, -3, 0, 0, 0, 0] ... phx-zigzag [7, 0, -3, 0, 0, 0] ...
```

Facts and implications:

- The first five bins agree in **context index, context state (`state=121`), and arithmetic-coder `low/range` (6560/436)**. The divergence is a different **bin value** on the same context, so it is a different *syntax-element value*, not corrupted coder state and not a context-indexing error.
- The element mapping is from `tools/cabac_bin_diff.py:68` (`ctx 68–69 → intra4x4_pred_mode`) and x264's authoritative writer `reference/x264/encoder/cabac.c:83-96` (`cabac_intra4x4_pred_mode`: one bin at ctx 68 for "same as predicted", else three bins at ctx 69 for the residual mode). Phoenix's writer `src/phx_cabac_syntax.c:216` already implements this exactly; the wrong value comes from the **analysis decision** fed into it (`d->intra4x4_pred_mode`, `src/phx_analysis.c:1220`).
- The coefficient handoff shows **6/16** luma blocks have equal raw arrays and **0/16** after the zigzag-relabelling the probe tries. That rules out a pure scan-order bug: the quantized coefficient *values* themselves differ. Since `src/phx_mbencode.c:313-318` already applies `scan[k] = dct[phx_zigzag4[k]]` (matching `encoder/macroblock.c:391/410`), the defect is in QNS/RD, not in the handoff permutation.
- The golden harness uses the actual placebo preset and the reference fixture; x264's first I-frame QP is 25 (`L1QP 25 25 0`), matching the probe's `slice_qp_delta=-3`, `slice_qp=25`.

---

## 3. What is already byte-correct for placebo

"Byte-correct" here means observed identical bytes/bins/fields, not a claim of overall parity.

| Stage / function | Evidence | x264 authority |
|---|---|---|
| SPS NAL payload | `7 (SPS) 25/25 EXACT`, both entropies; `parameter sets: 2/2 byte-exact` | `phx_sps_init/write` ← `common/set.c:101`, `:305-477` (`docs/X264_INTERNALS.md:1084-1088`) |
| PPS NAL payload | `8 (PPS) 6/6 EXACT`, both entropies | `phx_pps_init/write` ← `common/set.c:479-503`, `:505-572` (§6.6 `:1090-1093`) |
| Version SEI **payload** | `6 (SEI) 627/627 EXACT`, both entropies | `phx_sei_version_write` ← `common/set.c:593-623` (§6.6 `:1095-1101`) |
| Preset parameter deltas | `phx_param_apply_preset` `placebo` branch `src/phx_param.c:113-120` is source-for-source identical to `common/base.c:589-602`: `ME_TESA`, `subme=11`, `me_range=24`, `16` refs, TRELLIS B-adapt, `PSUB8x8`, `b_fast_pskip=0`, `trellis=2`, `bframe=16`, `lookahead=60`, `DIRECT_AUTO` | `docs/X264_INTERNALS.md:57-78` |
| CABAC arithmetic engine, context init, flush | bins 0–4 identical in ctx/b/state/low/range; state MATCH at bin 5 | `phx_cabac.c:54-209` ← `common/cabac.c:32-183`, flush frame bit `:174` (§6.2 `:972-1005`) |
| CABAC MB0 header syntax up to the divergent element | bin 0 `ctx=3 b=0` (I_4x4 `cabac_mb_type_intra`), bin 1 `ctx=399 b=0` (`cabac_transform_size`), bins 2–4 `ctx=68/69` (`cabac_intra4x4_pred_mode`) all identical | `encoder/cabac.c:35-66`, `:381-392`, `:83-96`, `:233-239` |
| IDR slice header writer | `HEADER MATCH (34 bits)` in both entropy probes, all 14 fields equal (`first_mb`, `slice_type=I`, `pps_id`, `frame_num`, `idr_pic_id`, `pic_order_cnt_lsb=0`, `slice_qp_delta=-3`, deblock flags/offsets) | `phx_slice_header_init/write` ← `encoder/encoder.c:122-211`, `:213-364` (§6.7 `:1106-1124`) |
| One later slice header (slice #4) | `HEADER MATCH (38 bits)` CABAC / `(37 bits)` CAVLC | same as above |
| Encoder robustness / NAL type sequence | `parity_report.json`: 10/10 `x264_rc=0` and `phx_rc=0`; probes: 11 vs 11 NALs with sequence `7,8,6,5,1×7` | `encoder.c:2096-2134`, `:3323-3902` (§1.4) |
| Trellis-QNS infrastructure (shared with lower presets) | `phx_rdo.c:836-948` mirrors `rdo.c:642-895` node/level-tree structure including the `if (0)` second-pass reachable only by `goto` (`rdo.c:881-885`) | `docs/X264_INTERNALS.md:883-913` |

**Payload**-level SEI correctness is real; **stream**-level SEI correctness is not (§1.3).

---

## 4. Phoenix functions missing or incorrect, with x264 citations

### 4.1 Incorrect — Annex-B start-code flags (first stream-level blocker, byte 41)

| Phoenix site | Wrong behaviour | Required x264 behaviour |
|---|---|---|
| `src/phx_encoder.c:291` (header SEI NAL) | `b_long_startcode = 1` | 3-byte start code: `reference/x264/encoder/encoder.c:2082-2083` |
| `src/phx_sei.c:379` | `b_long_startcode = 1` ("Annex-B first NAL" — false; SPS is NAL 0) | same rule |
| `src/phx_encoder.c:1553` (every slice NAL) | `b_long_startcode = 1` | 3-byte start code for slice NALs; `encoder.c:2082-2083`; writer `common/bitstream.c:55-104` already correct in `src/phx_bitstream.c:125-140` |

Only the flags are wrong; changing `phx_nal_encode` itself would be a regression.

### 4.2 Incorrect — I4 RD decision and coefficient values (first CABAC blocker, bin 5)

This is the placebo-specific core. At subme=11, x264 sets `b_early_terminate = subme < 11` → **false** (`reference/x264/encoder/analyse.c:303`) and `i_mbrd = 3` (`:296-301`), then for an I slice:

1. `mb_init_fenc_cache(h, a->i_mbrd >= 2)` — `analyse.c:2937-2938`, implementation `:570-584`; psy-RD SATD cache used by `rdo.c:94-148`.
2. `mb_analyse_intra` — `analyse.c:2939` (Phoenix has `phx_search_i4/i8/i16`).
3. `intra_rd` — `analyse.c:2940-2941`, implementation `:982-1014`, calls `rd_cost_mb` (`rdo.c:162`) to re-cost **all of I16/I4/I8** with RD.
4. `intra_rd_refine` — `analyse.c:2950-2951`, implementation `:1016-1135`: I16 branch `:1022-1037`, chroma branch `:1039-1084`, **I4 branch `:1086-1135`**, I8 branch `:1136+`, using `rd_cost_i4x4` (`rdo.c:315-347`) and `partition_i4x4_size_cabac` (`encoder/cabac.c:1203-1211`).

Phoenix:

| Phoenix site | Gap vs x264 |
|---|---|
| `src/phx_analysis.c:1095-1112` (`phx_analysis_init`) | Computes `mbrd` only (`:1101`); **no `b_early_terminate`**, no `mb_init_fenc_cache`. `docs/FUNCTION_INVENTORY.md:957` marks `mb_init_fenc_cache` **MISSING**. |
| `src/phx_analysis.c:1304-1320` (`phx_macroblock_analyse_slice`) | Picks MB type by SATD only (`phx_analysis_finalise`, `:1121-1135`). **`intra_rd` / `rd_cost_mb` are absent** (`docs/FUNCTION_INVENTORY.md:962`, `:1269` → MISSING); the comment at `:1310-1314` acknowledges the gap but only runs the I4 refine. At subme=11 x264's `intra_rd` uses `COST_MAX` thresholds, so it *always* re-costs I16/I4/I8 — Phoenix never does. This changes which MB type and which I4 modes reach the CABAC writer. |
| `src/phx_analysis.c:885-933` (`phx_analysis_intra_rd_refine_i4`) | Only the luma I4 half of `analyse.c:1086-1135`; missing I16 `:1022-1037`, chroma `:1039-1084`, I8 `:1136+`. Uses `phx_analysis_rd_i4` (`:848-878`) and `phx_rdo_ssd4` (`:824-839`). Evidence: first divergent bin is an I4 mode, so this is the immediate suspect. |
| `src/phx_analysis.c:824-839` (`phx_rdo_ssd4`) | Recomputes fdec SATD uncached (`phx_rdo_satd_ac_4x4`); x264 uses the `mb_init_fenc_cache(h, 1)` cache at `analyse.c:2937-2938` / `rdo.c:94-148`. Must be aligned before RD costs can be trusted at subme=11. |
| `src/phx_rdo.c:504-520`, `:406` (`phx_rdo_size_partition_i4x4`, `phx_rdo_size_intra4x4_pred_mode`) | Size-only mirror of `encoder/cabac.c:1203-1211` + `:83-96`; checked only against `RDO_SKIP_BS` bins, not against x264's `f8_bits_encoded` at subme=11. |
| `src/phx_rdo.c:836-948` (`phx_rdo_quant_trellis_cabac`) | Mirrors `rdo.c:642-895`, but coefficients fed in differ (`6/16` equal raw arrays, `0/16` after rescan). Re-align with `rdo.c:656-693` (deadzone baseline), `:695-706` (last_nnz/DC shortcut), `:747-862` (node/tree init and `TRELLIS_LOOP`), `:887-892` (level write-back), and the quant tables `common/set.c:73-205` / `common/quant.c:50-103`. |
| `src/phx_mbencode.c:287-333` (`phx_mb_encode_i4`) | Final I4 encode with `i_trellis=2`; compare against `encoder/macroblock.c:759-766` (`x264_mb_encode_i4x4`) and the factor `b_trellis` at `analyse.c:261` / `:3724`. |

### 4.3 Incorrect — high-ref / 16-B-frame slice headers and DPB state

The probe's later slices show real header mismatch fields (not parser artefacts of a wrong entropy stream):

| Observed mismatch | x264 authority | Phoenix site |
|---|---|---|
| `pic_order_cnt_lsb` (POC) differs on slices #2,#3,#5,#6 | `encoder/encoder.c:3485-3513` (`i_poc = 2*(i_frame - max(last_idr,0))`), `:239-244` (write) | `src/phx_encoder.c:691-785` — simplified POC/DPB (`docs/X264_INTERNALS.md:174`, `:76`) |
| `frame_num` differs on slices #7,#8 | `encoder.c:3881-3882` (advance only on reference pictures), `:227` (write), `:2615-2633` (IDR toggle) | `src/phx_encoder.c:691-698` |
| `num_ref_idx_l0/l1_active` and override flag differ; ref counts up to 5 vs 1 | `encoder.c:2295` (`reference_build_list`), `:2608-2675` (`slice_init` → `:2639-2645`), `:252-261` (write) | `src/phx_encoder.c:491-698` (`phx_encoder_reference_*`), `src/phx_slice.c:14-58`, `:153-160` |
| `ref_pic_list_modification_flag_l0`, `modification_of_pic_nums_idc[]`, `abs_diff_pic_num_minus1[]` differ (e.g. ref has 5 entries, phx 3) | `encoder.c:263-289` / `:276-289` (RPL write), `:180-194` (RPL arg build), `:2186-2197` (SMART duplicate) | `src/phx_slice.c:60-76`, `:162-186`; `src/phx_encoder.c:656-670` |
| Whole `pred_weight_table` absent/different (`luma_log2_weight_denom`, `luma_weight_l0[]`, `luma_offset_l0[]`) | `encoder.c:291-322` (write), `:2202-2284` (weight init); `x264_weights_analyse` `encoder/slicetype.c:284`, called `:1942`; `encoder.c:3876-3879` | `src/phx_slice.c:188-215` (writer exists) but `src/phx_encoder.c:656-669` hard-codes `i_scale=1, i_offset=-1` and comments "`x264_weights_analyse` is a lookahead port TODO". `docs/FUNCTION_INVENTORY.md:1341` → **MISSING** |
| `direct_spatial_mv_pred_flag` differs | `encoder.c:152-170` (direct-mode auto heuristic), `:249-250` (write) | `src/phx_encoder.c:678-688` (approximation with POC comparison), `src/phx_slice.c:150-151` |
| `slice_qp_delta` / derived `slice_qp` differ (e.g. ref 28 phx 0) | `encoder.c:3855-3861`, `:2787-2789`; `x264_ratecontrol_mb_qp` `encoder/ratecontrol.c:1754-1768`; AQ `ratecontrol.c:304-415`; MB-tree `slicetype.c:1029-1049`, `:1091-1184` | `src/phx_ratecontrol.c:327/448/529/626` — no MB-tree (`src/phx_lookahead.c:209`, `:335` TODO; `docs/X264_INTERNALS.md:1196-1198`), so `f_qp_offset` differs |
| `slice_type` P vs B on slice #6 | `slicetype.c:1745-1974` (`x264_slicetype_decide`), `:1950-1958` (coded-order reorder); B-adapt TRELLIS `slicetype.c:1333-1382`, `:1553-1579` | `src/phx_lookahead.c:251-253` — explicit TODO for `slicetype_path()`/Viterbi; only FAST branch (`:190-193`) is implemented |

### 4.4 Missing — placebo-specific analysis stages

| x264 function (exact citation) | Phoenix state |
|---|---|
| `x264_me_search_ref` / ESA-TESA branch — `encoder/me.c:182-798`, TESA/ESA `:618-798`; `subpel_iterations` `:38-50` | `src/phx_me.c:169-171`: `if (method != PHX_ME_DIA && method != PHX_ME_HEX) return -1;` — `PHX_ME_TESA`/`PHX_ME_UMH` are enum-only (`src/phx_param.h:13`). `docs/FUNCTION_INVENTORY.md:1185` → **MISSING** |
| `x264_me_refine_qpel` — `encoder/me.c:801-992` (and QPRD `:1233+` for subme≥10) | **MISSING** (`docs/FUNCTION_INVENTORY.md:1191`, `:1199`); `src/phx_analysis.c:1298-1302` only runs P L0 16×16 fullpel ME |
| `x264_macroblock_probe_skip` — `encoder/macroblock.c:1129`, macro `encoder/macroblock.h:38-39`; fast-pskip gate `encoder/analyse.c:3002-3014` (`b_fast_pskip=0` at `common/base.c:598`) | **MISSING** (`docs/FUNCTION_INVENTORY.md:1173`). `b_fast_pskip` appears only in `src/phx_param.c:60,118` and `src/phx_sei.c:241`; `src/phx_analysis.c:1078-1085` uses an ad-hoc SAD/pskip-MV shortcut and cannot express "no fast skip". This directly explains Phoenix's much larger slices. |
| `b_early_terminate = subme < 11` — `encoder/analyse.c:303` | Not represented in `src/phx_analysis.c`; thresholds in the SATD searches (`analyse.c:859-861`, `:695-696`) therefore use wrong subme behaviour. |
| `macroblock_tree` / `macroblock_tree_finish` — `encoder/slicetype.c:1091-1184`, `:1029-1049`; propagation `common/mc.c:511-600` | No function in `src/`; only TODOs at `src/phx_lookahead.c:209`, `:335`. `src/phx_lowres.c:149-151` merely sets a `b_mb_tree` flag. Affects every frame's QP at placebo, hence `slice_qp_delta`. |
| `slicetype_path` — `encoder/slicetype.c:1333-1382`; TRELLIS B-adapt `:1553-1579` | **MISSING** (`docs/FUNCTION_INVENTORY.md:1354`); `src/phx_lookahead.c:251-253` leaves the TRELLIS branch empty. With `i_bframe=16` this changes the entire I/P/B pattern (probe slice #6: P vs B). |
| B-slice analysis — `encoder/analyse.c:3305-3733` (direct `:3314-3351`, 16×16 `:1890`, 8×8/direct `:2283`, 16×8/8×16 `:2360/:2454`, bidir RD `:2622/:2692`) | `src/phx_analysis.c:1298-1302` keeps B slices on the intra-only path; `docs/X264_INTERNALS.md:67` documents the gap |
| `x264_weights_analyse` — `encoder/slicetype.c:284`, `:1942`; SMART duplicate `encoder.c:2372-2405` | **MISSING**; hard-coded stub at `src/phx_encoder.c:656-669` |
| `x264_ratecontrol_mb` (row VBV RC) — `encoder/ratecontrol.c:1590-1746` | **MISSING** (`docs/FUNCTION_INVENTORY.md:1232`); `docs/X264_INTERNALS.md:1200-1212` |

### 4.5 CAVLC path

The CAVLC probe's IDR byte-5 and later byte-0/2/3 diffs are downstream of the same MB decisions and coefficients. Once §4.2 is fixed, re-check `src/phx_cavlc.c:951-1114` / `src/phx_cavlc_syntax.c:69-411` against `encoder/cavlc.c:121-196`, `:198-208`, `:305-486`, `:487-595` (MB header, CBP, I16 DC/AC, chroma DC/AC, skip runs `encoder.c:2810`, `:2883-2891`, `:3087-3088`). Do not "fix" CAVLC token tables while the coefficient arrays still differ.

---

## 5. Prioritized, source-aligned work list to reach byte-exact

No parameter changes, no fixture changes, no approximations, no "matching" of NAL counts or decoded pictures.

1. **P0 — Annex-B start codes.** In `src/phx_encoder.c:291`, `src/phx_sei.c:379`, `src/phx_encoder.c:1553`, set `b_long_startcode` per `encoder/encoder.c:2082-2083` (true only for the first NAL, SPS, PPS, AVC-Intra). Gate: direct byte compare of `synth-placebo-{cabac,cavlc}.{ref,phx}.h264` must show `ref[39:43] == phx[39:43] == 00 00 01 06` and `first_diff > 41`.
2. **P0 — I4 RD decision at bin 5.** Port x264's full `analyse.c:2934-2951` I path in order: `mb_init_fenc_cache` (`:2937-2938` ← `:570-584`), `intra_rd` (`:2940-2941` ← `:982-1014` + `rd_cost_mb` `rdo.c:162`), then the complete `intra_rd_refine` (`:2950-2951` ← `:1016-1135`, not just the luma I4 half in `src/phx_analysis.c:885-933`). Add `b_early_terminate` from `analyse.c:303` and the psy-RD cache from `rdo.c:94-148`. Gate: `cabac_bin_diff.py --preset placebo` must report **no divergence before bin 5**, and bins 5–7 (`ctx 69` rem-mode bits) must match with identical `state`/`low/range`.
3. **P0 — Coefficient handoff.** Align `phx_rdo_quant_trellis_cabac` (`src/phx_rdo.c:836-948`) and `phx_mb_encode_i4` (`src/phx_mbencode.c:287-333`) with `rdo.c:642-895` and `encoder/macroblock.c:759-766`. Gate: the probe's `COEFFICIENT HANDOFF` must report `16/16` raw arrays equal (do not accept the zigzag-rescan count as a substitute; the tool's `0/16` shows values differ, not ordering).
4. **P1 — fast-pskip disabled.** Add `x264_macroblock_probe_skip` (`encoder/macroblock.c:1129`, `encoder/macroblock.h:38-39`) and gate it on `analyse.b_fast_pskip` exactly as `encoder/analyse.c:3002-3014`. At placebo (`base.c:598` → 0), Phoenix must *not* take its current ad-hoc skip shortcut at `src/phx_analysis.c:1078-1085`. This is required before P/B slices can match.
5. **P1 — 16-ref / 16-B DPB and slice-header state.** Replace the simplified state in `src/phx_encoder.c:491-698` with the x264 paths `encoder.c:2295` (`reference_build_list`), `:2413`, `:2533` (`reference_update`), `:3481` (`reference_update`), `:3485-3513` (POC), `:3517-3555`, `:3881-3882`, `:2608-2675` (`slice_init`); wire `src/phx_slice.c:14-58`, `:150-186`, `:188-215` to the resulting fields. Gate: all 8 slice headers in `first_frame_probe.py --preset placebo --entropy {cabac,cavlc}` report `HEADER MATCH`.
6. **P1 — weighted prediction and MB-tree.** Port `x264_weights_analyse` (`encoder/slicetype.c:284`, called `:1942`) and `encoder.c:2372-2405`, replacing the stub at `src/phx_encoder.c:656-669`; port `macroblock_tree`/`macroblock_tree_finish` (`slicetype.c:1029-1049`, `:1091-1184`) and propagation (`common/mc.c:511-600`) so `f_qp_offset` and `slice_qp_delta` match.
7. **P1 — TRELLIS B-adapt + ME at subme=11.** Port `slicetype_path` (`encoder/slicetype.c:1333-1382`, `:1553-1579`) into `src/phx_lookahead.c:251-253`; port `x264_me_search_ref`'s ESA/TESA branch (`encoder/me.c:618-798`) and `x264_me_refine_qpel` (`me.c:801-992`, QPRD `:1233+`) into `src/phx_me.c` (currently rejects TESA at `:169-171`). Gate: x264's I/P/B frame-type sequence and 16-ref list sizes must match before entropy is compared.
8. **P2 — CAVLC re-probe.** After steps 2–4, re-run `first_frame_probe.py --preset placebo --entropy cavlc`; only then align `src/phx_cavlc*.c` with `encoder/cavlc.c:121-196`, `:305-486`, `:487-595` where a real diff remains.
9. **P2 — full gate.** Loop `python tools/parity_harness.py` → `python tools/first_frame_probe.py --preset placebo --entropy {cabac,cavlc}` → `python tools/cabac_bin_diff.py --preset placebo --fixture build/parity/fixture.yuv` (`docs/PARITY_ITERATION.md:8-60`). Only `byte_exact == true` for all 100 cases ends the loop (`docs/PARITY_ITERATION.md:100-108`).
10. **Never accepted as a fix:** changing x264 flags, fixtures or the reference corpus; matching bitrate, NAL counts, decoded picture, SPS/PPS, slice headers, or a bin prefix; tuning QP/parameters to move a first-diff offset (`docs/PARITY_ITERATION.md:37-40`, `docs/VIDEO_PARITY.md:9-11`).

---

## 6. Explicit parity statement

**0/100 is not parity.** `build/parity/parity_report.json` → `totals`: 100 cases, 100 compared, **0 byte-exact**, 100 mismatches; `by_preset.placebo`: 10 cases, **0 byte-exact**, 10 mismatches, `first_diff_min 41`. `docs/PORTING_ROADMAP.md:123-127` states: *"0/100 means zero of the 100 saved x264 streams are byte-identical; it is a divergence, not a success metric."* `docs/PARITY_ITERATION.md:3` and `docs/VIDEO_CORPUS.md:9` likewise require byte identity of the entire stream.

The facts in §1–§3 — byte-exact SPS/PPS/SEI payload, matching IDR slice header, five matching CABAC bins with identical coder state, 100/100 successful encodes — are necessary diagnostics, **not** parity claims. Until the SEI start-code flags, the subme=11 I4 RD/coefficient divergence, and the downstream high-ref/B-frame/weighted-P/MB-tree/fast-pskip gaps are fixed and the harness reports `byte_exact: true` for all 100 cases, `placebo` remains a divergence.

> **Post-audit note (orchestrator):** this report was captured before commit
> `becb95b` fixed x264's Annex-B start-code rule. The stream-level `first_diff=41`
> is stale (post-fix first diffs are bytes 587-671 across the corpus); the
> NAL-payload and CABAC bin findings remain valid.

