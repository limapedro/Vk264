# Preset audit — `faster` (branch `vk264-phoenix`)

Read-only audit. No source, CMake, tool, test or branch changes were made; the only output is this file.
All `reference/x264/` line numbers below are from the vendored authoritative tree; all Phoenix
paths are from `src/`. Probe logs used for the bin-level forensic sections were rebuilt into a
**private** temp dir (`C:\Users\multi\AppData\Local\Temp\opencode\faster_audit_tmp`) because the
default `l1_cabac_bin_diff` temp dir is shared with other running auditors and its logs are
overwritten concurrently.

## 0. Bottom line

`faster` is **0/10 byte-exact in `build/parity/parity_report.json`**, with `first_diff = 38` on all
ten cases; the whole corpus is **0/100**. The first byte divergence is in the **IDR slice, in
macroblock 0**, after a fully matching 30-bit slice header and 23 identical CABAC bins. The
CABAC engine is *not* the culprit: at the first divergent bin the coder state matches exactly
(`low/range 23194/334` in both). The divergence is an **I4x4 intra mode decision** (block 6 of
MB0): x264 signals `mode == predicted mode` (bin `68/1`), Phoenix signals `mode != predicted
mode` (bin `68/0`, residual mode 3 instead of x264's H mode 1). The most likely exact source
cause is that Phoenix omits x264's `mbrd == 0` analysis shortcut
(`encoder/analyse.c:673-681`, `:911-918`) and its x3 SATD candidate pre-selection
(`encoder/analyse.c:900-947`), so it evaluates a mode x264 never considers at that QP/subme.
Later slices additionally diverge because weighted prediction, lookahead/B-adapt, and P/B
analysis are not ported.

---

## 1. NAL-level diff facts (both entropies)

Commands (exit 1 = divergence by design):

```
python tools/first_frame_probe.py --preset faster --entropy cabac
python tools/first_frame_probe.py --preset faster --entropy cavlc
```

### CABAC (`synth-faster-cabac`)

```
stream bytes: ref=4308, phx=7157; NALs: ref=11, phx=11
 0 0  7 (SPS)        7 (SPS)           23    23  EXACT
 1 1  8 (PPS)        8 (PPS)            5     5  EXACT
 2 2  6 (SEI)        6 (SEI)          623   623  EXACT
 3 3  5 (IDR slice)  5 (IDR slice)   1104  1157  DIFF@7
 4 4  1 (non-IDR)    1 (non-IDR)      365   124  DIFF@2
 ...
parameter sets: 2/2 byte-exact (SPS, PPS)
slice #1: HEADER MATCH (30 bits), ENTROPY/MB MISMATCH at post-header byte 2
VERDICT: DIVERGENCE (first stream byte diff 38)
```

IDR slice, first differing NAL bytes (`65 88 84 00 ff d9 28 b7` vs `... b6`):

```
ref:  65 88 84 00 ff d9 28 b7 95 e9 b3 4a ab d3 c3 be
phx:  65 88 84 00 ff d9 28 b6 c1 dc 5c 9e 55 7a 78 77
                            ^^ first diff, NAL payload byte 7
```

The probe decodes all 14 IDR slice-header fields identically in both streams
(`slice_type=7/I`, `idr_pic_id=0`, `pic_order_cnt_lsb=0`, `slice_qp_delta=-3` → `slice_qp=25`,
deblocking offsets 0/0), 30 bits each.

Later CABAC slices: all 7 non-IDR slices differ, with large length asymmetries
(ref `365/468/384/425/258/258/353` vs phx `124/1039/1003/1027/36/1040/1036`); from slice #2 the
probe reports header-field mismatches (POC `ref=2 phx=8`), slice #3+ report `slice_type ref=P
phx=B`, `frame_num` shifts (`ref=4 phx=3`, `ref=5 phx=3/4`), `cabac_init_idc`, `slice_qp_delta`,
MMCO and weight-denominator differences. That is a frame-type/decision-level divergence, not a
CABAC state bug.

### CAVLC (`synth-faster-cavlc`)

```
stream bytes: ref=5172, phx=8516; NALs: ref=11, phx=11
 0 0  7 (SPS)        7 (SPS)            23    23  EXACT
 1 1  8 (PPS)        8 (PPS)             5     5  EXACT
 2 2  6 (SEI)        6 (SEI)           623   623  EXACT
 3 3  5 (IDR slice)  5 (IDR slice)    1426  1517  DIFF@7
 4 4  1 (non-IDR)    1 (non-IDR)       427   123  DIFF@2
 ...
parameter sets: 2/2 byte-exact (SPS, PPS)
slice #1: HEADER MATCH (30 bits), ENTROPY/MB MISMATCH at post-header byte 2
VERDICT: DIVERGENCE (first stream byte diff 38)
```

```
ref:  65 88 84 00 fe 89 38 8e 42 5c df 81 a0 2a 00 02
phx:  65 88 84 00 fe 89 38 89 4a 43 9b f0 34 05 40 00
                            ^^ first diff, NAL payload byte 7
```

**Key symmetry:** both entropies fail at the *same* place — IDR slice NAL payload byte 7, i.e.
stream byte 38 — after identical SPS/PPS/SEI and an identical IDR header. The upstream MB
decision/coefficient divergence is entropy-independent; fixing the I4x4 mode decision is
therefore the highest-leverage fix for *both* CAVLC and CABAC.

---

## 2. First divergent CABAC bin, element, context and coder state

Command:

```
python tools/cabac_bin_diff.py --preset faster --fixture build/parity/fixture.yuv
```

Excerpt (identical with the private `--tmp` rerun):

```
x264 golden bins: 38026; Phoenix bins: 40701
FIRST DIVERGENT BIN: index 23 (0-based, bin #24)
  bin 19    x264: ctx=68   b=0  dec      phx: ctx=68   b=0
  bin 20    x264: ctx=69   b=1  dec      phx: ctx=69   b=1
  bin 21    x264: ctx=69   b=0  dec      phx: ctx=69   b=0
  bin 22    x264: ctx=69   b=0  dec      phx: ctx=69   b=0
  bin 23    x264: ctx=68   b=1  dec      phx: ctx=68   b=0   <== FIRST DIVERGENCE
  bin 24    x264: ctx=68   b=1  dec      phx: ctx=69   b=0
  bin 25    x264: ctx=68   b=0  dec      phx: ctx=69   b=1
  x264 element: intra4x4_pred_mode (:83-96)
  phx  element: intra4x4_pred_mode (:83-96)
  x264 record: dec ctx=68 b=1 state=114 low/range 23194/334 -> 46800/256
  phx  record: dec ctx=68 b=0 state=114 low/range 23194/334 -> 46388/412
  coder state before bin: MATCH (x264 low/range 23194/334, phx 23194/334)
REMAINING DIVERGENCE: 0 inserted/deleted bins; tail agrees for 23 bins;
  next mismatch at aligned index 23: x264=('dec',68,1), phx=('dec',68,0)
```

Interpretation (from `reference/x264/encoder/cabac.c:83-96`): `ctx 68 / b=1` means
`i_pred == i_mode`; `ctx 68 / b=0` starts a 3-bin `ctx 69` remainder. The trace's first 23 bins
(bin 0–22) are byte-for-byte equal in kind, context, value, state **and** low/range. Counting
`ctx 68` bins (`2,3,7,11,15,19,23`) puts the first divergence at the **7th I4x4 predictor bin =
4x4 block index 6** of MB0, after two matching `mb_type`/`transform_size` bins (ctx 3, ctx 399,
see `encoder/cabac.c:381-433`).

Phoenix's following three bins are `ctx69 b=0,1,0` → remainder 2 → with predicted mode 1 this
is mode 3 (DDL); x264's golden dump shows block 6 `mode=1` (H). Supporting coefficient handoff:

```
COEFFICIENT HANDOFF (MB0 luma 4x4 arrays):
  blocks compared: 16; raw arrays equal: 9/16; equal after zigzag rescan: 0/16
  block 6: x264 [6, -3, 0, 0, 0, 0] ...
           phx  [6, 0, -2, 0, 0, 0] ... phx-zigzag [6, 0, 0, -2, 0, 0] ...
```

`golden_stderr.txt` (MB0 dump at writer entry) gives x264 block 6 `mode=1 : 6 -3 0 ...`;
`phx_blocks.txt` gives Phoenix block 6 `6 0 -2 ...`, which equals x264's block 2
(`mode=3 : 6 0 -2 ...`) — consistent with Phoenix having predicted block 6 with mode 3, not
mode 1. Blocks 0–5 have identical modes and identical coefficient arrays in both dumps, so the
reconstructed neighbour border for block 6 is identical; the mode choice alone differs.

**Diagnostic caveat (do not "fix" the wrong thing):** `tools/cabac_bin_diff.py:518-522` still says
the Phoenix trellis handoff omits the zigzag scan, but current `src/phx_mbencode.c:307-318`
*does* apply `phx_zigzag4` after `phx_quant_4x4_trellis`, matching x264
(`encoder/rdo.c:890`, `encoder/macroblock.c:1411`, `common/dct.c:843-846`). The tool's
"equal after zigzag rescan: 0/16" is an artifact of re-scanning arrays that are already in
scan order on both sides; the meaningful datum is `raw arrays equal: 9/16` and the block-6
mismatch. Double-scanning would make parity worse.

---

## 3. x264 functions already byte-correct **in this case** (evidence-limited)

These claims are scoped to what the probes actually exercise (IDR MB0, first 23 bins, parameter
sets). They are not global correctness claims.

| x264 function / source | Phoenix | Evidence here |
|---|---|---|
| `x264_param_default`, `param_apply_preset` (`common/base.c:344`, `:489`, faster deltas `:538-544`) | `src/phx_param.c:25`, `:65` | All faster values match: `i_frame_reference=2`, `i_subpel_refine=4`, `b_mixed_references=0`, `WEIGHTP_SIMPLE`, `i_lookahead=20`; exact SPS/PPS/SEI imply header-relevant params align. |
| `x264_sps_init`/`write`, `x264_pps_init`/`write` (`encoder/set.c:101`, `:305`, `:479`, `:505`) | `src/phx_headers.c` | SPS 23/23 and PPS 5/5 `EXACT` in both entropies. |
| version SEI (`encoder/set.c:574`, `:593`) | `src/phx_sei.c:322`, `:340` | SEI NAL 623/623 `EXACT` in both entropies. |
| NAL encapsulation / RBSP escape / Exp-Golomb (`common/bitstream.c:29`, `:55`, `common/bitstream.h:201-299`) | `src/phx_bitstream.c:52-140` | Exact parameter sets and correctly parsed 30-bit IDR header. |
| `slice_header_init`/`write` for the IDR slice (`encoder/encoder.c:122`, `:213-364`) | `src/phx_slice.c:14`, `:107` | All 14 IDR header fields match, including `slice_qp_delta=-3` and deblocking offsets 0/0. |
| `x264_cabac_context_init`, `x264_cabac_encode_init*`, `cabac_putbyte`, `cabac_encode_renorm`, `x264_cabac_encode_decision_c` (`common/cabac.c:32-183`) | `src/phx_cabac.c:54-209` | 23 leading bins equal in ctx/b/state/low/range; at bin 23 coder state `MATCH` (`23194/334`). |
| `cabac_mb_type_intra`, `cabac_mb_header_i` (`encoder/cabac.c:35-67`, `:381-433`) | `src/phx_cabac_syntax.c` | bins 0–1 (ctx 3 / ctx 399) identical; both select I_4x4 with 4x4 transform. |
| `cabac_intra4x4_pred_mode` writer (`encoder/cabac.c:83-96`) | `src/phx_cabac_syntax.c:184-864` | 23 bins covering blocks 0–5 identical; the writer emits the correct bins for the modes it is given. |
| `x264_pixel_satd_4x4` (`common/pixel.c:265-288`) | `src/phx_analysis.c:196` (`phx_satd4`) | Source-identical C kernel; modes 0–5 agree with x264's golden dump. |
| `x264_predict_4x4_*` (`common/predict.c:481-610`), notably `v_c` (`:508-511`) | `src/phx_predict.c:426+`, local `phx_predict4_v_fixed` at `src/phx_analysis.c:449-458`, `src/phx_mbencode.c:258-270` | Same reconstruct/nnz for blocks 0–5 (9/16 coefficient arrays equal at writer entry). |
| `x264_mb_encode_i4x4` deadzone quant + zigzag handoff (`encoder/macroblock.h:132-166`, `common/quant.c:67-73`, `common/dct.c:843-846`) | `src/phx_mbencode.c:300-327`, `src/phx_analysis.c:588-604` | 9/16 luma arrays byte-equal; no zigzag double-scan (see caveat above). |

---

## 4. Phoenix functions missing / incorrect, with x264 citations

### 4.1 First divergence (MB0, IDR) — must fix first

1. **Missing `intra_analysis_shortcut` / x3 SATD candidate pre-selection.**
   x264: `encoder/analyse.c:673-681` (table), `:900-947` (x3 path at `:903`, `favour_vertical`
   at `:905`, shortcut selection at `:911-914`, mode loop at `:923-947`), mode list
   `predict_4x4_mode_available` at `:552-558`, cost table `cost_i4x4_mode` at `:171-174` and
   `:744`. The gate is `h->mb.i_mbrd < 1 + a->b_fast_intra` (`:911`); for `faster`
   `subme=4 → mbrd = (subme>=6)+... = 0` (`encoder/analyse.c:301`), so the shortcut **is** taken.
   Phoenix: `src/phx_analysis.c:750-783` (`phx_search_i4`) evaluates the full
   `phx_i4x4_mode_available` list (`src/phx_analysis.c:108-113`, `:415-421`) and its own comments
   at `:711` and `:748` admit "the x3 fast path and the directional analysis shortcut are not
   available; all [modes are evaluated]". For block 6 x264's shortcut set is
   `{DC,H,V,DDR,HD,HU}` (inferred from the chosen modes: TOPRIGHT is available per
   `common/macroblock.c:489`, and `favour_vertical=0` is the only branch that excludes DDL(3))
   and does **not** contain DDL(3); Phoenix's full list
   `{DC,H,V,DDL,VL,HU}` does, and Phoenix picks DDL. Because the first six blocks, their
   coefficients and the SATD kernel are identical, this candidate-set difference is the
   concrete cause of bin 23 and of the first byte-38 stream difference.
   Inventory status: `mb_analyse_intra` = **MISSING** (`docs/FUNCTION_INVENTORY.md:959`).

2. **Incorrect SATD composition for 8x8/16x16.**
   x264: `PIXEL_SATD_C` at `common/pixel.c:311-332` composes `x264_pixel_satd_8x8` from
   *side-by-side* 8x4 halves (`sub(pix1) + sub(pix1+4*i_pix1)`) and 16x16 from all eight 8x4
   sub-blocks. Phoenix: `src/phx_analysis.c:244-255` uses `phx_satd8x4(p1)` + `phx_satd8x4(p1+4*s1)`
   (top/bottom halves) and four such 8x8s for 16x16, covering only 8 of the 16 8x4 tiles.
   This corrupts I8/I16 costs and therefore MB-type decisions (later blocks and later frames),
   even though it is not the bin-23 cause.

3. **Stale local workaround for V prediction.** `src/phx_analysis.c:449-450` and
   `src/phx_mbencode.c:258-261` still claim `src/phx_predict.c:426` broadcasts one top-left
   sample; the current `src/phx_predict.c:426-436` is already source-exact vs
   `common/predict.c:508-511`. No functional gap remains here; only the comments/workaround
   need consolidating (do not double-fix).

### 4.2 Later slices / frame decisions (needed after 4.1)

4. **Weighted prediction analysis missing.** `faster` explicitly sets
   `analyse.i_weighted_pred = X264_WEIGHTP_SIMPLE` (`common/base.c:542`). x264 fills
   `sh.weight[]` in `encoder.c:2202-2284` via `x264_weights_analyse`
   (`encoder/slicetype.c:284`) and writes `pred_weight_table()` at `encoder/encoder.c:291-322`.
   Phoenix has the *writer* (`src/phx_slice.c:95-98`, `:188-211`) but no analysis;
   `src/phx_lookahead.h:27-31` lists "weighted prediction" as not ported. The probe shows
   `luma_log2_weight_denom ref=0 phx=1` on later P slices. Inventory:
   `x264_weights_analyse` = **MISSING** (`docs/FUNCTION_INVENTORY.md:1341`).

5. **Frame-type / lookahead decisions incomplete.** x264: `scenecut_internal`
   (`encoder/slicetype.c:1384`), `scenecut` (`:1430`), `x264_slicetype_decide` (`:1745`),
   `lookahead_slicetype_decide` (`encoder/lookahead.c:68`). Inventory marks these **MISSING**
   (`docs/FUNCTION_INVENTORY.md:1355-1361`, `:1140`). Phoenix's simplified
   `src/phx_lookahead.c:89-504` lacks MB-tree, trellis B-adapt, VBV and weighted prediction
   (`src/phx_lookahead.h:27-31`). Probe: `pic_order_cnt_lsb ref=2 phx=8`,
   `slice_type ref=P phx=B` from slice #3, shifted `frame_num`.

6. **Slice-header fields beyond I slice.** x264 `slice_header_write`
   (`encoder/encoder.c:213-364`), especially ref-list modification / MMCO / weight table at
   `:291-364`. Phoenix is a subset (`src/phx_slice.c:14`, `:107`; see
   `docs/X264_INTERNALS.md:80`: "no MBAFF/MMCO/weight table"). Probe reports `frame_num`,
   `adaptive_ref_pic_marking_mode_flag`, `memory_management_control_operation`,
   `ref_pic_list_modification` mismatches from slice #5 onward, plus the simplified DPB of a
   last reference in `src/phx_encoder.c:491-785`.

7. **P/B analysis and subpel ME.** x264 `x264_macroblock_analyse` inter paths
   (`encoder/analyse.c:1255-2692`), subpel (`subme=4` still performs quarter-pel refinement), B
   direct/modes and all P subpartitions. Phoenix only has a P L0 16x16 trial
   (`src/phx_analysis.c:1056`; `docs/X264_INTERNALS.md:73`, `src/phx_analysis.h:19-20`), no
   subpel, no B modes; ME is 16x16-only (`src/phx_me.c:160`, `src/phx_lowres.c:437`). This
   explains the very different non-IDR NAL lengths (e.g. phx slice #8 only 36 bytes vs ref 258
   in CABAC).

8. **Entropy writer functions not name-ported.** `x264_macroblock_write_cabac`
   (`encoder/cabac.c:1088-1105`) and `x264_macroblock_write_cavlc` (`encoder/cavlc.c:487`)
   are **MISSING** in `docs/FUNCTION_INVENTORY.md:1053`, `:1071`; Phoenix equivalents live under
   different names (`phx_encoder_write_cabac_mb`, `phx_encoder_cabac_block`, etc.). After the
   mode/coefficient fix, CAVLC token/nC handling (`encoder/cavlc.c:121-209`, `:487-704`) must be
   re-checked independently — both entropies share the same upstream decision divergence but
   their writers are separate code.

---

## 5. Prioritized, source-aligned work list to byte-exact `faster`

1. **P0 — port the `mbrd == 0` I4 analysis selection** into `src/phx_analysis.c:750-783`:
   `intra_analysis_shortcut` table (`encoder/analyse.c:673-681`), x3 SATD pre-pass with
   `favour_vertical` and predicted-mode bonus (`:900-914`), `predict_4x4_mode_available`
   (`:552-558`), `cost_i4x4_mode` (`:171-174`, `:744`), and `b_fast_intra`/`b_avoid_topright`
   (`:440-467`). Acceptance gate: MB0 block-6 CABAC bin becomes ctx68 `b=1` (x264 mode H),
   bin 23 divergence disappears, and the IDR slice first diff moves past NAL payload byte 7 in
   both CABAC and CAVLC. Add a golden test with x264's block-6 coefficients `6 -3 0 ...`; do not
   touch entropy state or zigzag to "make the metric move".

2. **P0 — fix 8x8/16x16 SATD composition** in `src/phx_analysis.c:244-255` to match
   `common/pixel.c:311-332` (horizontal 8x4 halves, all eight tiles for 16x16). Acceptance:
   I16/I8 costs and MB-type decisions match x264 for MB0 and MB1; keep 9/16 (then 16/16)
   raw coefficient arrays equal.

3. **P0 — golden lock** for SPS/PPS/SEI and IDR slice-1 header bytes (already exact) plus the
   MB0 bin prefix and 4x4 coefficient arrays; run `python tools/run_tests.py`.

4. **P1 — weighted prediction for `faster`**: port `x264_weights_analyse`
   (`encoder/slicetype.c:284`), `sh.weight[]` fill (`encoder.c:2202-2284`) and exact
   `pred_weight_table()` (`encoder.c:291-322`) into `src/phx_lookahead.c` / `src/phx_slice.c`;
   remove the `weighted prediction` TODO at `src/phx_lookahead.h:27-31`. Acceptance: P-slice
   `luma_log2_weight_denom` and weight flags match from slice #2.

5. **P1 — frame-type/lookahead parity**: port `scenecut_internal` (`:1384`), `scenecut`
   (`:1430`), `x264_slicetype_decide` (`:1745`), `lookahead_slicetype_decide`
   (`encoder/lookahead.c:68`). Acceptance: slice #2 POC and slice #3+ `slice_type`/`frame_num`
   match the reference; NAL counts/types align.

6. **P1 — slice header & DPB**: complete MMCO, ref-list modification and weight-table writing
   per `encoder/encoder.c:213-364`; replace the simplified last-reference DPB
   (`src/phx_encoder.c:491-785`) with x264's ref/POC/frame_num semantics
   (`encoder.c:2295`, `:2413`, `:2533`, `:3512`). Acceptance: all 8 slice headers decode to the
   same field values before entropy comparison.

7. **P2 — P/B inter analysis + subpel**: port the inter decision paths
   (`encoder/analyse.c:1255-2692`) and subpel ME used at `subme=4`, then B-direct/modes;
   acceptance: non-IDR NAL lengths converge and entropy comparison reaches the residual stage.

8. **P2 — CAVLC writer audit** (`encoder/cavlc.c:121-209`, `:487-704`) after the upstream fix;
   then the remaining entropy/context tables and rate-control/QP sequence.

9. **Final gate only**: re-run `python tools/parity_harness.py`, require **10/10** `faster`
   cases `byte_exact` and **100/100** corpus-wide, then re-probe. Nothing short of whole-file
   byte equality counts.

Do **not** act on the `cabac_bin_diff.py:518-522` zigzag comment; current Phoenix source already
scans (`src/phx_mbencode.c:307-318`). Do not change x264 flags, fixtures or the reference corpus.

---

## 6. `0/100` is not parity

From `build/parity/parity_report.json`:

```
"totals": { "cases": 100, "x264_ok": 100, "x264_failures": 0,
            "compared": 100, "byte_exact": 0, "mismatches": 100, "first_diff_min": 35 }
"by_preset": { "faster": { "cases": 10, "x264_ok": 10, "compared": 10,
                           "byte_exact": 0, "mismatches": 10, "first_diff_min": 38 } }
```

Every one of the ten `faster` cases (`synth`, `ssstwitter.com_1732159163230`,
`ssstwitter.com_1732158697723`, `ssstwitter.com_1732159939975`,
`ssstwitter.com_1748999992229`; CABAC and CAVLC) is `"byte_exact": false` with
`"first_diff": 38` — for example `synth-faster-cabac`: ref 4308 bytes vs phx 7157 bytes.

`docs/NIGHTLY_STATUS.md` states the rule explicitly: "**The byte-exact count from the parity
gate is the only parity claim.** `byte-exact 0/100` means zero of the 100 reference streams
matched byte for byte; it is **not** success and is not evidence of parity." `docs/PARITY_ITERATION.md`
adds: "A case counts only when the whole Phoenix `.h264` file equals the reference `.h264` file
byte for byte ... matching SPS/PPS, matching slice headers, or 'visually identical' output are all
**mismatches**."

Therefore: matching SPS/PPS/SEI (3 NALs), a matching 30-bit IDR header, 23 identical CABAC
bins, and a matching `low/range` coder state are **diagnostics only**. Both probes correctly exit
1; **0/100 is not parity**, and this report makes no byte-exact parity claim for `faster`.

> **Post-audit note (orchestrator):** the stream `first_diff: 38` is stale.
> Commit `becb95b` fixed x264's Annex-B start-code rule; live first diffs are
> bytes 587-671. NAL-payload and CABAC bin findings remain valid.

