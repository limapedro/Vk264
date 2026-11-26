# Preset audit: `superfast` (branch `vk264-phoenix`)

Audit scope: read-only probes + source inspection only. No source, CMake, tools or tests
were changed by this audit. The only repository file written by this audit is this report.
Other workers are active on the same branch: at audit start the working tree had
`M tests/parity_report.md` plus untracked `src/phx_decode.*` / `tests/test_phx_decode.c`;
during the audit those changed, an unrelated `Z1 temporary debug` edit appeared in
`src/phx_analysis.c`, and untracked `phx_bins.txt` / `phx_blocks.txt` appeared at the repo
root from another worker's CLI run. None of those changes were made by this audit, and none
were touched. All trace logs and instrumented builds made by this audit were kept in a
private temp directory.

Commands used (the last one needed a private `--tmp` because the default temp dir is shared
with concurrent workers and the link step raced on `dbg_phx_dct.obj`):

```
python tools/first_frame_probe.py --preset superfast --entropy cabac
python tools/first_frame_probe.py --preset superfast --entropy cavlc
python tools/cabac_bin_diff.py --preset superfast --fixture build/parity/fixture.yuv --tmp C:\Users\multi\AppData\Local\Temp\opencode\superfast_audit
python -c "... build/parity/parity_report.json ..."
```

---

## 1. NAL-level diff facts for both entropies

`build/parity/parity_report.json`, `totals`: `{"cases": 100, "byte_exact": 0, "mismatches": 100, "first_diff_min": 35}`.
`by_preset.superfast`: `{"cases": 10, "x264_ok": 10, "byte_exact": 0, "mismatches": 10, "first_diff_min": 37}`.
All ten superfast cases (5 CABAC + 5 CAVLC, fixtures `synth` and four `ssstwitter.com_*`)
have `byte_exact: false` and `first_diff: 37`.

### CABAC (`synth-superfast-cabac`)

```
stream bytes: ref=5622, phx=7462; NALs: ref=11, phx=11
  ref# phx#  ref NAL                phx NAL                len ref len phx status  first diff
     0    0  7 (SPS)                7 (SPS)                     23      23  EXACT      -
     1    1  8 (PPS)                8 (PPS)                      4       4  EXACT      -
     2    2  6 (SEI)                6 (SEI)                    621     621  EXACT      -
     3    3  5 (IDR slice)          5 (IDR slice)             1149    1190  DIFF@5     5
     4    4  1 (non-IDR slice)      1 (non-IDR slice)          535     274  DIFF@5     5
     5    5  1 (non-IDR slice)      1 (non-IDR slice)          363    1036  DIFF@5     5
     6    6  1 (non-IDR slice)      1 (non-IDR slice)          603    1002  DIFF@5     5
     7    7  1 (non-IDR slice)      1 (non-IDR slice)          592    1023  DIFF@5     5
     8    8  1 (non-IDR slice)      1 (non-IDR slice)          666     187  DIFF@3     3
     9    9  1 (non-IDR slice)      1 (non-IDR slice)          457    1036  DIFF@4     4
    10   10  1 (non-IDR slice)      1 (non-IDR slice)          567    1022  DIFF@5     5
parameter sets: 2/2 byte-exact (SPS, PPS)
VERDICT: DIVERGENCE (first stream byte diff 37)
```

First slice payload excerpt:

```
  ref: 65 88 84 00 ff ce e4 4b ...
  phx: 65 88 84 00 ff d9 28 b6 ...   <== first diff at NAL payload byte 5
```

Slice headers: slices #1–#5 and #8 are `HEADER MATCH` (28–30 bits) then
`ENTROPY/MB MISMATCH at post-header byte 0 (RBSP byte 4, NAL payload byte 5)`.
Slices #6 and #7 are `HEADER MISMATCH`, e.g.:

```
  slice #6: ref_pic_list_modification_flag_l0 ref=1 phx=0
            abs_diff_pic_num_minus1[0]_l0      ref=1 phx=0
            modification_of_pic_nums_idc[1]_l0 ref=3 phx=0
            adaptive_ref_pic_marking_mode_flag ref=0 phx=28
            slice_qp (derived)                 ref=28 phx=0
            => fix the parameter-set / slice-header writer, not the entropy stage
  slice #7: difference_of_pic_nums_minus1[0]   ref=3 phx=2
            memory_management_control_operation[1] ref=1 phx=0
            slice_qp (derived)                 ref=29 phx=0
```

(Those `phx=28/29/0` values are consistent with the bit parser losing alignment after the
first wrong field in the P-slice header, not with a separate CABAC problem.)

### CAVLC (`synth-superfast-cavlc`)

```
stream bytes: ref=6702, phx=8838; NALs: ref=11, phx=11
  SPS / PPS / SEI: EXACT, EXACT, EXACT (23 / 4 / 621 bytes)
  IDR slice:     ref=1438, phx=1517, DIFF@5
  P slices:      ref=675/444/700/695/824/562/674, phx=287/1242/1197/1216/197/1264/1226
                 DIFF@4, DIFF@4, DIFF@4, DIFF@4, DIFF@3, DIFF@4, DIFF@4
parameter sets: 2/2 byte-exact (SPS, PPS)
VERDICT: DIVERGENCE (first stream byte diff 37)
```

First CAVLC slice:

```
  ref: 65 88 84 00 fe b9 3b be ...
  phx: 65 88 84 00 fe 89 38 89 ...   <== first diff at NAL payload byte 5
```

Conclusion at NAL level: headers/parameter sets are not the blocker for the first frame;
the first blocker is the first macroblock's entropy payload in both entropies. Later P-slice
header divergence is a second, independent blocker.

---

## 2. First divergent CABAC bin / element / context, and coder state

Tool output (private temp dir to avoid the shared-dir link race):

```
x264 golden bins: 41045; Phoenix bins: 40600
FIRST DIVERGENT BIN: index 5 (0-based, bin #6)
  bin 1     x264: ctx=399 b=0  dec      phx: ctx=399 b=0  dec
  bin 2     x264: ctx=68  b=1  dec      phx: ctx=68  b=1  dec
  bin 3     x264: ctx=68  b=0  dec      phx: ctx=68  b=0  dec
  bin 4     x264: ctx=69  b=1  dec      phx: ctx=69  b=1  dec
  bin 5     x264: ctx=69  b=1  dec      phx: ctx=69  b=0  dec   <== FIRST DIVERGENCE
  x264 element: intra4x4_pred_mode (:83-96)
  phx  element: intra4x4_pred_mode (:83-96)
  x264 record: dec ctx=69 b=1 state=121 low/range 6560/436 -> 6560/258
  phx  record: dec ctx=69 b=0 state=121 low/range 6560/436 -> 13636/356
  coder state before bin: MATCH (x264 low/range 6560/436, phx 6560/436)

REMAINING DIVERGENCE (after removing inserted Phoenix bin(s)):
  0 inserted/deleted bin(s) removed; tail agrees for 5 bins; next mismatch at aligned
  index 5: x264=('dec', 69, 1), phx=('dec', 69, 0)

COEFFICIENT HANDOFF (MB0 luma 4x4 arrays):
  blocks compared: 16; raw arrays equal: 3/16; equal after zigzag rescan: 0/16
  block 1: x264 [2, -1, -2, 1, 1, 0] ... phx [5, -3, 0, 0, 0, 0] ...
```

Interpretation:

* Bins 1–5 of MB0 agree in *context order and coder arithmetic*. The arithmetic coder,
  its context state, its init/flush and the syntax writer are **not** the source of the
  first difference: before bin 5 both coders have `state=121` and `low/range=6560/436`.
* Bin 5 is the second LSB of `cabac_intra4x4_pred_mode` for MB0 block 1 (ctx 69;
  `reference/x264/encoder/cabac.c:83-96`). x264 encodes `b=1`, Phoenix `b=0`.
* Decoding that unary triple: x264's mode is `8` (`I_PRED_4x4_HU`) with predictor `2`
  (`I_PRED_4x4_DC`), Phoenix's is `1` (`I_PRED_4x4_H`). The instrumented x264 MB0 dump
  confirms `L1BLK 1 nnz=1 mode=8`, and a temporary instrumented Phoenix build prints
  `AUDIT mb0 blk1 pred=2 n4=1 best=1`.
* There is no bin insertion/deletion: the streams stay aligned and only the *decision
  value* differs. Therefore the fix is upstream of the entropy stage.

### Root cause, confirmed by the cost calculators

A temporary (temp-dir-only) instrumented Phoenix build printed the per-candidate costs:

```
COST blk1 pred=2 mode=9 raw=360 adjusted=348
COST blk1 pred=2 mode=1 raw=216 adjusted=216
COST blk1 pred=2 mode=8 raw=216 adjusted=216
```

Phoenix evaluates all three candidates with **`phx_satd_4x4`**
(`src/phx_analysis.c:769`) and, on the 216/216 tie, keeps the first one (mode 1).

An instrumented x264 golden build printed the reconstructed neighbour samples for the
same block:

```
SAMPLES blk0 : 2,7,18,23 16,21,32,37 27,32,42,48 41,46,56,62
SAMPLES fenc-blk1: 28,35,42,49 41,48,55,62 54,61,68,75 67,74,81,88
```

With those neighbours, the three candidate **SAD** costs (x264's choice metric at this
preset) are:

```
mode 9 (DC_LEFT) SAD 292, minus 3*lambda=12 -> 280
mode 1 (H)       SAD 248                    -> 248
mode 8 (HU)      SAD 94                     -> 94   <== x264 picks HU
```

Why SAD and not SATD:

* `reference/x264/common/base.c:517-521` — superfast sets
  `param->analyse.i_subpel_refine = 1`.
* `reference/x264/encoder/encoder.c:1409-1410` —
  `int satd = !h->mb.b_lossless && h->param.analyse.i_subpel_refine > 1;`
  then `mbcmp[] = satd ? pixf.satd : pixf.sad_aligned`.
  For superfast, `satd == 0`, so `mbcmp[PIXEL_4x4]` is **SAD**, not SATD.
* `reference/x264/encoder/analyse.c:933` uses `h->pixf.mbcmp[PIXEL_4x4]` for the I4
  mode cost (`:893` is the SIMD x9 shortcut, which is NULL in the C-only reference);
  Phoenix hard-codes `phx_satd_4x4` at `src/phx_analysis.c:769`.
* The same selection governs I16 (`:700`), I8 (`:774`, `:816`) and chroma (`:610`, `:655`),
  so those Phoenix searches are also wrong for `subpel_refine <= 1`.
* This also explains why the documented `medium` probe (715/715 exact MB0 bins, first
  divergence later) did not expose the bug: medium has `subpel_refine > 1`, so x264 uses
  SATD there, which is what Phoenix already does.

A secondary difference at the same block is x264's x3/x9 fast paths and their mode-cost
tables (`reference/x264/encoder/analyse.c:171-173`, `:744`, `:905-906`, `:924-947`,
`:490-528`). In the C-only reference build the x9 pointer is NULL
(`reference/x264/common/pixel.c:809`, no C `intra_*_x9` is installed), so the C loop
`analyse.c:924-947` is authoritative for parity against this harness; Phoenix omits the
x3 shortcut and its `intra_analysis_shortcut` pruning entirely
(`src/phx_analysis.c:750-783` evaluates the full mode list).

---

## 3. x264 functions already byte-correct in this case

Evidence, not assumption:

| x264 function | reference | Phoenix counterpart | evidence of correctness here |
|---|---|---|---|
| `x264_sps_init`, `x264_sps_write` | `encoder/set.c:101`, `:305-475` | `phx_sps_init/write` (`src/phx_headers.c:13`, `:76`) | SPS 23/23 bytes `EXACT` for CABAC and CAVLC |
| `x264_pps_init`, `x264_pps_write` | `encoder/set.c:479`, `:505-570` | `phx_pps_init/write` (`src/phx_headers.c:98`, `:165`) | PPS 4/4 bytes `EXACT` for both entropies |
| `x264_sei_version_write` | `encoder/set.c:574`, `:593` | `phx_sei.c:322`, `:340` | SEI 621/621 bytes `EXACT` for both entropies |
| `slice_header_write` (I slices, and P slices #1–#5/#8) | `encoder/encoder.c:213-364` | `phx_slice_header_write` (`src/phx_slice.c:107-250`) | `HEADER MATCH` (28–30 bits) on 6 of 8 slice pairs |
| CABAC arithmetic engine: `x264_cabac_encode_decision`, `_noup`, init/flush | `common/cabac.c:32-183` | `phx_cabac.c:54-209` | bins 1–4 identical; before bin 5 both have `state=121`, `low/range=6560/436` |
| `cabac_mb_type_intra`, `cabac_transform_size`, `cabac_intra4x4_pred_mode` (writer logic) | `encoder/cabac.c:35-67`, `:233-239`, `:83-96` | `src/phx_cabac_syntax.c:180-228`, `:532-572` | ctx sequence `3,399,68,68,69,69` and ctx-68/69 semantics agree; only the *value* at bin 5 differs |
| `x264_mb_encode_i4x4` / `quant_4x4` / `scan_4x4` / `dequant_4x4` / `add4x4_idct` for MB0 block 0 | `encoder/macroblock.h:132-166`, `common/quant.c:59-73`, `:112-128`, `common/dct.c:272-310` | `phx_encode_i4` (`src/phx_analysis.c:587-603`) and encoder path | MB0 block-0 coefficient arrays are identical (`[-35,-3,-5,0,...,-1,...]` in both traces); blocks 2 and 3 also identical |

The tool's `equal after zigzag rescan: 0/16` line applies a second zigzag rescan to
Phoenix's already-scanned coefficient arrays; it is not independent evidence of a scan
bug. The direct comparison (`raw arrays equal: 3/16`) shows MB0 blocks 0, 2 and 3 equal
and block 1 first divergent, which is what the bin trace reports.

These are necessary, not sufficient: `docs/PORTING_ROADMAP.md` states it plainly —
"715/715 exact MB0 bins and 20/20 exact headers are necessary, not sufficient".

---

## 4. Phoenix functions missing or incorrect, with x264 citations

### 4.1 Primary blocker — the mode-cost metric is wrong for superfast

| Phoenix function | x264 source | defect |
|---|---|---|
| `phx_search_i4` — `src/phx_analysis.c:750-783`, specifically `:769` (`phx_satd_4x4`) | `encoder/encoder.c:1409-1410` (`mbcmp_init`), `encoder/analyse.c:933` (`mbcmp[PIXEL_4x4]`), `encoder/analyse.c:924-947`, `common/base.c:517-521` (superfast `i_subpel_refine=1`) | Always uses SATD. x264 uses **SAD** when `i_subpel_refine <= 1`. Directly causes bin-5 mismatch (mode `H` instead of `HU`). |
| `phx_search_i16` — `src/phx_analysis.c:690-707`, `:702` | `encoder/analyse.c:700-738` (`mbcmp[PIXEL_16x16]`) | Same metric selection bug: always SATD/16x16-SATD, never SAD for superfast. |
| `phx_search_i8` — `src/phx_analysis.c:708-745`, `:732` (`phx_sa8d_8x8`) | `encoder/analyse.c:749` (`sa8d = (mbcmp[0]==satd[0]) ? sa8d[8x8] : mbcmp[PIXEL_8x8]`), `:774`, `:816` | Same metric selection bug: with SAD `mbcmp`, x264's `sa8d` cost is SAD-8x8, not SA8D. |
| intra-chroma search — `src/phx_analysis.c:994-1045`, `:1034-1036` | `encoder/analyse.c:610`, `:655` | Same metric selection bug. |
| `phx_pixel_sad_4x4` — `src/phx_analysis.c:787-798` | `common/pixel.c:55-73` (`PIXEL_SAD_C(4,4)`) | Exists and is byte-correct, but is used only by the RDO SATD cache at `:815`; it is never selected as the analysis `mbcmp`. Wire it into the analysis cost path. |
| I4 x3 shortcut / `intra_analysis_shortcut` | `encoder/analyse.c:490-528` (availability/pruning tables), `:905-906`, `:924-947`; `common/pixel.c:55-73` (`intra_sad_x3_4x4` at C, asm-only x9) | Not ported; Phoenix evaluates the full mode list. After fixing SAD, blocks with 6+ candidates can still pick a different mode because x264 prunes and early-exits. |
| `cost_i4x4_mode` / mode-bit cost pointer | `encoder/analyse.c:171-173`, `:744`, `:893` | Not represented in Phoenix's analysis state. Needed if parity must also hold for CPU-independent or SIMD builds where `intra_mbcmp_x9_*` is non-NULL. (The C-only reference used by the harness has x9 = NULL, so this is second priority.) |

### 4.2 Secondary blocker — P-slice header state

| Phoenix function | x264 source | defect |
|---|---|---|
| `phx_slice_header_init` / state assembly — `src/phx_slice.c:14`, `:60-101` | `encoder/encoder.c:122-211`, especially `:172-194` (ref-pic-list reordering from `h->fref`), `:196` (`cabac_init_idc`), `:198-199` (`qp_delta`), `:291-322` (pred weight table), `:328-348` (MMCO) | Probe shows `b_ref_pic_list_reordering[0]`, `abs_diff_pic_num_minus1`, MMCO and derived `slice_qp` wrong on slices #6/#7. Writer itself (`src/phx_slice.c:162-239`) mirrors `encoder.c:263-364` field order; the *inputs* are wrong. |
| reference/DPB management — `src/phx_encoder.c:490-698` | `encoder/encoder.c:2533-2660` (`reference_update`, MMCO generation at `:2595-2598`), `:3504-3513` (per-frame reset), `:2295-2411` (list construction), `:3481` | Documented as "simplified DPB of last reference" in `docs/X264_INTERNALS.md` §0.2. Produces the wrong `b_ref_reorder` / `mmco_diff` used above. |
| slice QP / rate-control state feeding `sh->i_qp_delta` | `encoder/ratecontrol.c:1432`, `:1590`, `:1829`; `encoder/encoder.c:198-199` | Derived `slice_qp` 28/29 vs Phoenix 0 on those slices. Fix after header alignment is restored (fields after the first wrong one are misparsed). |

### 4.3 Not the blocker (explicitly ruled out)

* `phx_cabac_intra4x4_pred_mode` (`src/phx_cabac_syntax.c:216-228`) is a faithful copy of
  `encoder/cabac.c:83-96`, including the `i_mode--` when `i_mode > i_pred`.
* The CABAC engine (`src/phx_cabac.c`) and context tables are correct here: the coder
  state matches exactly at the divergence point.
* SPS/PPS/SEI writers are byte-exact.
* `phx_encode_i4` quant/scan/dequant/IDCT reproduce x264's MB0 block-0 coefficient array
  exactly; no evidence of a DCT/quant bug for this block.

---

## 5. Prioritized, source-aligned work list to byte-exact

1. **P0 — port `mbcmp_init` metric selection into the analysis context.**
   In x264, `encoder/encoder.c:1409-1410`: `satd = !b_lossless && i_subpel_refine > 1`;
   `mbcmp[] = satd ? satd : sad_aligned`. For `superfast` (`common/base.c:517-521`,
   `i_subpel_refine = 1`) this is **SAD**. Add a `mbcmp` selector to `phx_analysis_ctx_t`
   and use it in `phx_search_i4` (`src/phx_analysis.c:769`), `phx_search_i16` (`:702`),
   `phx_search_i8` (`:732`) and the chroma search (`:1034-1036`). Add the corresponding
   SAD kernels aligned to `common/pixel.c:55-73` (`PIXEL_SAD_C(4,4)`), `:311-332`
   (`PIXEL_SATD_C(4,4)`, 8x8, 16x16). Acceptance: `cabac_bin_diff --preset superfast`
   must pass bin 5 and the whole MB0 trace; `first_frame_probe --preset superfast` must
   move its first diff past NAL payload byte 5.
2. **P0 — port x264's I4 x3 shortcut and mode-cost handling.**
   `encoder/analyse.c:490-528` (`i4x4_mode_available`, `intra_analysis_shortcut`),
   `:905-906` (pred-mode discount on DC/H/V), `:924-947` (full loop + early break),
   `:171-173`/`:744` (`cost_i4x4_mode`). Keep the C-only reference semantics (x9 = NULL,
   `common/pixel.c:809`), then optionally add the x9 cost table behind
   `b_cpu_independent`. Acceptance: per-block cost arrays match x264, not just winners
   (`docs/PORTING_ROADMAP.md` WP4 gate).
3. **P1 — re-run `cabac_bin_diff` for all presets and both entropies.** The metric fix
   must not regress medium/slow (which currently rely on SATD) or the CAVLC path.
4. **P1 — fix P-slice reference-list/MMCO/weight state.**
   Align `src/phx_slice.c:60-101` with `encoder/encoder.c:172-194`, `:196`, `:198-199`,
   `:291-322`, `:328-348` by porting the DPB/list logic from `encoder/encoder.c:2295-2411`,
   `:2533-2660`, `:3504-3513`. Acceptance: `first_frame_probe` must report
   `HEADER MATCH` for slices #6 and #7 as well.
5. **P1 — check CAVLC after the analysis fix.** The CAVLC first diff is also at the first
   post-header byte, driven by the same wrong I4 mode/coefficient handoff; no separate
   CAVLC token-table change is indicated yet. Re-probe before touching `phx_cavlc*`.
6. **P2 — full corpus gate.** `python tools/parity_harness.py` then
   `python tools/compare_streams.py`: all 100 cases must have `byte_exact: true`, with no
   NAL-count mismatches and no decoder errors, before any parity claim is made
   (`docs/PARITY_ITERATION.md`, "The rule: no case counts until byte-exact").

---

## 6. Explicit status statement

**0/100 is not parity.** The harness reports `byte_exact: 0` for 100/100 cases and
`byte_exact: 0` for all 10 superfast cases. Matching SPS/PPS/SEI, matching slice headers,
"visually identical" pictures, or even 4/5 matching CABAC bins are diagnostics only.
Byte-exact parity means every byte of every Phoenix `.h264` file equals the corresponding
authoritative x264 file. Until all 100 cases pass, `superfast` is a divergence to be fixed,
not a success metric.

`docs/PORTING_ROADMAP.md` states the same invariant: "Full-stream parity is not achieved.
0/100 means zero of the 100 saved x264 streams are byte-identical; it is a divergence, not
a success metric." This audit confirms it for `superfast`.

> **Post-audit note (orchestrator):** the stream `first_diff: 37` is stale;
> commit `019978b` completed the Annex-B start-code fix. Live first diffs are
> bytes 586-825. NAL-payload and CABAC bin findings remain valid.

