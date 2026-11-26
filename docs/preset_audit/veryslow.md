# Preset audit — `veryslow` byte-exact parity blocker analysis

**Auditor scope:** read-only subagent audit on branch `vk264-phoenix`. No source,
CMake, tool or test file was modified by this audit. Owned output: this file only.

**Snapshot / provenance:** probes were run 2026-11-26 03:27–03:29 local time.
The repository is being changed concurrently by other workers during this audit
(`HEAD` moved from `e661577c90938e3ad0ad53fc97eed77b0ee186f2` at 03:27 to
`deece5edce78e1d10c8c35516db5b9c92e2702a7` at 03:31). All numbers below are from
the artifacts present at probe time; where a value changed mid-audit it is
recorded explicitly. Evidence files:
`build/parity/parity_report.json`, `build/parity/synth-veryslow-{cabac,cavlc}.{ref,phx}.h264`,
`build/parity/fixture.yuv`, `build/x264_ref/x264.exe`, `build/tests/Release/phx_cli.exe`.

---

## 0. Bottom line

`veryslow` is **not byte-exact**. `build/parity/parity_report.json` →
`by_preset.veryslow` at probe time:

```json
{"cases": 10, "x264_ok": 10, "x264_failures": 0, "compared": 10,
 "byte_exact": 0, "mismatches": 10, "first_diff_min": 669}
```

`totals`: `{"cases": 100, "x264_ok": 100, "compared": 100,
"byte_exact": 0, "mismatches": 100, "first_diff_min": 586}`.

**"0/100 is not parity."** It is the measured *absence* of parity: zero of 100
Phoenix streams equal their x264 reference byte for byte. This is stated by the
repository itself in `docs/NIGHTLY_STATUS.md` ("The byte-exact count from the
parity gate is the only parity claim … `byte-exact 0/100` means zero of the 100
reference streams matched byte for byte; it is **not** success and is not
evidence of parity") and in `docs/PORTING_ROADMAP.md:123-127`. Byte-exact
parameter sets, a matching IDR slice header, identical CABAC coder state at the
first divergent bin and 100/100 successful encodes are diagnostics, **not**
parity claims.

The veryslow-specific blockers, in byte/decision order:

1. **Raw stream byte 669:** Phoenix still writes a **4-byte Annex-B start code**
   for the IDR slice while x264 writes 3 bytes (`phx_encoder.c:1558` vs
   `reference/x264/encoder/encoder.c:2082-2083`). This was *not* the first
   divergence at the start of the audit: an earlier snapshot had
   `first_diff_min: 41` from a 4-byte SEI start code; that SEI flag was fixed by
   another worker mid-audit (`phx_encoder.c:293`, `phx_sei.c:381`), exposing the
   slice-start-code defect at 669.
2. **IDR slice payload byte 6 (CABAC) / byte 5 (CAVLC):** first divergence is a
   CABAC `intra4x4_pred_mode` decision bin (`ctx=69`, x264 `b=1`, Phoenix `b=0`)
   with **identical coder state** (`low/range 6560/436`, context `state=121`).
   The arithmetic coder and the syntax writer are correct; the *chosen I4
   prediction mode and its residual* are wrong. This is upstream analysis/RD
   (`analyse.c:864-979`, `:982-1015`, `:1086-1135`), not entropy.
3. **All seven non-IDR slices have header mismatches**, dominated by the
   veryslow-specific decision paths: B-adapt TRELLIS + B-pyramid frame ordering
   and POC, 16-ref list sizes/reordering/MMCO, and weighted-prediction tables.
4. **Every later slice also diverges in entropy**, because the frame types
   (P vs B), reference lists and MB decisions differ.

---

## 1. NAL-level diff facts (both entropies)

### 1.1 `python tools/first_frame_probe.py --preset veryslow --entropy cabac`

Verbatim excerpt:

```text
CASE synth-veryslow-cabac
  ref: build/parity/synth-veryslow-cabac.ref.h264
  phx: build/parity/synth-veryslow-cabac.phx.h264
  stream bytes: ref=3132, phx=9100; NALs: ref=11, phx=11
NAL comparison (payload bytes, start codes and trailing zeros excluded; NALs paired by type sequence):
  ref# phx#  ref NAL                phx NAL                len ref len phx status             first diff
     0    0  7 (SPS)                7 (SPS)                     25      25  EXACT              -
     1    1  8 (PPS)                8 (PPS)                      6       6  EXACT              -
     2    2  6 (SEI)                6 (SEI)                    625     625  EXACT              -
     3    3  5 (IDR slice)          5 (IDR slice)             1066    1174  DIFF@6             6
    ref: 0000: 65 88 81 00 1f ff ce f4  |e.......|  <== first diff
    phx: 0000: 65 88 81 00 1f ff d9 3c  |e......<|  <== first diff
     4    4  1 (non-IDR slice)      1 (non-IDR slice)          303    1070  DIFF@3
     5    5  1 (non-IDR slice)      1 (non-IDR slice)          193    1083  DIFF@3
     6    6  1 (non-IDR slice)      1 (non-IDR slice)          141    1003  DIFF@3
     7    7  1 (non-IDR slice)      1 (non-IDR slice)          233    1016  DIFF@0
     8    8  1 (non-IDR slice)      1 (non-IDR slice)          198    1003  DIFF@0
     9    9  1 (non-IDR slice)      1 (non-IDR slice)          130    1018  DIFF@2
    10   10  1 (non-IDR slice)      1 (non-IDR slice)          170    1034  DIFF@2
  parameter sets: 2/2 byte-exact (SPS, PPS)
  slice #1: HEADER MATCH (33 bits), ENTROPY/MB MISMATCH at post-header byte 0 (RBSP byte 5, NAL payload byte 6)
VERDICT: DIVERGENCE (first stream byte diff 669)
```

### 1.2 `python tools/first_frame_probe.py --preset veryslow --entropy cavlc`

Verbatim excerpt:

```text
CASE synth-veryslow-cavlc
  stream bytes: ref=3987, phx=10954; NALs: ref=11, phx=11
     0    0  7 (SPS)                7 (SPS)                     25      25  EXACT              -
     1    1  8 (PPS)                8 (PPS)                      6       6  EXACT              -
     2    2  6 (SEI)                6 (SEI)                    625     625  EXACT              -
     3    3  5 (IDR slice)          5 (IDR slice)             1434    1532  DIFF@5
    ref: 0000: 65 88 81 00 1f d7 21 11  |e.....!.)|
    phx: 0000: 65 88 81 00 1f d1 27 11  |e.....'.)|
     4    4  1 (non-IDR slice)      1 (non-IDR slice)          463    1324  DIFF@3
     5    5  1 (non-IDR slice)      1 (non-IDR slice)          222    1284  DIFF@3
     6    6  1 (non-IDR slice)      1 (non-IDR slice)          178    1201  DIFF@3
     7    7  1 (non-IDR slice)      1 (non-IDR slice)          342    1217  DIFF@0
     8    8  1 (non-IDR slice)      1 (non-IDR slice)          275    1232  DIFF@0
     9    9  1 (non-IDR slice)      1 (non-IDR slice)          162    1228  DIFF@2
    10   10  1 (non-IDR slice)      1 (non-IDR slice)          213    1237  DIFF@2
  parameter sets: 2/2 byte-exact (SPS, PPS)
  slice #1: HEADER MATCH (33 bits), ENTROPY/MB MISMATCH at post-header byte 0
  slice #2: HEADER MISMATCH (13 differing field(s))
  slice #3: HEADER MISMATCH (1 differing field(s))
  slice #4: HEADER MISMATCH (6 differing field(s))
  slice #5: HEADER MISMATCH (46 differing field(s))
  slice #6: HEADER MISMATCH (11 differing field(s))
  slice #7: HEADER MISMATCH (7 differing field(s))
  slice #8: HEADER MISMATCH (3 differing field(s))
VERDICT: DIVERGENCE (first stream byte diff 669)
```

Both entropies: **SPS/PPS/SEI payloads byte-exact**, 11 vs 11 NALs with the same
type sequence `7,8,6,5,1×7`, IDR slice header byte-exact, entropy/MB divergence
immediately after the header. Phoenix streams are ~2.9× (CABAC) / ~2.7×
(CAVLC) larger than the x264 references — it is writing many more bits per MB
(and many more B slices), not merely different bits.

### 1.3 Reconciling "SEI EXACT" with raw `first_diff=669`

`first_frame_probe.py` compares NAL payloads with start codes stripped. Direct
raw-byte comparison shows the first raw difference is the IDR slice start code:

```text
cabac firstdiff 669
  ref[663:683]: 3d 30 00 80 00 00 01 65 88 81 00 1f ff ce f4 35 4d c1 e4 cd
  phx[663:683]: 3d 30 00 80 00 00 00 01 65 88 81 00 1f ff d9 3c a8 68 3c 19
cavlc firstdiff 669
  ref[663:683]: 3d 30 00 80 00 00 01 65 88 81 00 1f d7 21 11 29 4b 9b f0 34
  phx[663:683]: 3d 30 00 80 00 00 00 01 65 88 81 00 1f d1 27 11 29 48 73 7e
```

x264's rule is `reference/x264/encoder/encoder.c:2082-2083`:
`b_long_startcode = !i || type == NAL_SPS || type == NAL_PPS || avcintra`.
Phoenix's slice site `src/phx_encoder.c:1558` uses
`!(fc.b_idr && h->i_frame == 0)`; `h->i_frame` was already incremented at
`src/phx_encoder.c:1321`, so the first slice is never recognised and all slices
get 4 bytes. This is an independent, pure-syntax blocker behind the entropy
blockers.

### 1.4 Slice-header facts specific to veryslow (CABAC probe)

IDR (slice #1): **HEADER MATCH (33 bits)** — every field equal, including
`slice_qp_delta=-3`, `slice_qp=25`, `cabac_init_idc` absent as required for I.

Non-IDR slices (#2–#8), verbatim highlights:

```text
slice #2: pic_order_cnt_lsb ref=6 phx=14
          luma_log2_weight_denom ref=5 phx=0; luma_weight_l0_flag[0] ref=1 phx=0
          luma_weight_l0[0] ref=31 phx=0; luma_offset_l0[0] ref=5 phx=[0,0]
          (luma/chroma log2_weight_denom) ref=[5,0] phx=0
slice #3: pic_order_cnt_lsb ref=2 phx=6
slice #4: pic_order_cnt_lsb ref=4 phx=2; direct_spatial_mv_pred_flag ref=1 phx=0
          num_ref_idx_l0_active_minus1 ref=1 phx=0; num_ref_idx_l1_active_minus1 ref=0 phx=1
slice #5: slice_type ref=5 (P) phx=6 (B); pic_order_cnt_lsb ref=14 phx=4
          num_ref_idx_active_override_flag ref=1 phx=0
          num_ref_idx_l0_active_minus1 ref=4 phx=1 (effective 5 vs 0)
          ref_pic_list_modification_flag_l0 ref=1 phx=1
          modification_of_pic_nums_idc[0..5]_l0 ref=0,0,0,1,0,3 phx=2,0,2,...
          luma_log2_weight_denom ref=5 phx=<none>; chroma_log2_weight_denom ref=4 phx=<none>
          luma_weight_l0[0]=31, offset=3; chroma_weight_l0[0][1]=15, offset=-10
          luma_weight_l0[1]=31, offset=2; chroma_weight_l0_flag[1]=0   [all phx=<none>]
slice #6: frame_num ref=4 phx=3; pic_order_cnt_lsb ref=10 phx=8
          direct_spatial_mv_pred_flag ref=0 phx=1; num_ref_idx_l0_active_minus1 ref=2 phx=1
slice #7: frame_num ref=5 phx=3; pic_order_cnt_lsb ref=8 phx=10
          num_ref_idx_l1_active_minus1 ref=1 phx=0
slice #8: frame_num ref=5 phx=3; num_ref_idx_l0_active_minus1 ref=3 phx=1
```

(The `cabac_init_idc=28`, `slice_qp=0` and `<none>` readings in slices #2/#5/#6
are parser desynchronisation after the diverging fields, not evidence of a
deliberate value; the fix belongs in the producer of those fields.)

### 1.5 All 10 veryslow cases in `parity_report.json`

| case | ref bytes | phx bytes | first_diff | byte_exact |
|---|---:|---:|---:|---|
| synth-veryslow-cabac | 3132 | 9100 | 669 | false |
| synth-veryslow-cavlc | 3987 | 10954 | 669 | false |
| ssstwitter.com_1748999992229-veryslow-cabac | 5124 | 15371 | 669 | false |
| ssstwitter.com_1748999992229-veryslow-cavlc | 5488 | 15967 | 669 | false |
| ssstwitter.com_1732159163230-veryslow-cabac | 1985 | 6776 | 669 | false |
| ssstwitter.com_1732159163230-veryslow-cavlc | 2008 | 7099 | 669 | false |
| ssstwitter.com_1732158697723-veryslow-cabac | 6982 | 21997 | 669 | false |
| ssstwitter.com_1732158697723-veryslow-cavlc | 7575 | 23008 | 669 | false |
| ssstwitter.com_1732159939975-veryslow-cabac | 5627 | 17385 | 669 | false |
| ssstwitter.com_1732159939975-veryslow-cavlc | 5985 | 17942 | 669 | false |

Every veryslow case has `x264_rc=0`, `phx_rc=0` and `byte_exact=false`; the
`first_diff` bucket is identical across all fixtures because it lands on the
IDR slice start code, upstream of any fixture-dependent pixels.

---

## 2. First divergent CABAC bin — element, context, coder state

Command (the tool's own temp dir was locked, so a fresh `--tmp` was used — no
repository file was touched):

```text
python tools/cabac_bin_diff.py --all --preset veryslow \
    --fixture build/parity/fixture.yuv --tmp <fresh temp dir>
```

Verbatim output:

```text
x264 golden bins: 28249; Phoenix bins: 40919
FIRST DIVERGENT BIN: index 5 (0-based, bin #6)
  bin 1     x264: ctx=399  b=0  dec      phx: ctx=399  b=0  dec
  bin 2     x264: ctx=68   b=1  dec      phx: ctx=68   b=1  dec
  bin 3     x264: ctx=68   b=0  dec      phx: ctx=68   b=0  dec
  bin 4     x264: ctx=69   b=1  dec      phx: ctx=69   b=1  dec
  bin 5     x264: ctx=69   b=1  dec      phx: ctx=69   b=0  dec       <== FIRST DIVERGENCE
  bin 6     x264: ctx=69   b=1  dec      phx: ctx=69   b=0  dec
  bin 7     x264: ctx=68   b=0  dec      phx: ctx=68   b=0  dec
  x264 element: intra4x4_pred_mode (:83-96)
  phx  element: intra4x4_pred_mode (:83-96)
  x264 record: dec ctx=69   b=1  state=121  low/range 6560/436 -> 6560/258   intra4x4_pred_mode (:83-96)
  phx  record: dec ctx=69   b=0  state=121  low/range 6560/436 -> 13636/356   intra4x4_pred_mode (:83-96)
  coder state before bin: MATCH (x264 low/range 6560/436, phx 6560/436)
REMAINING DIVERGENCE (after removing inserted Phoenix bin(s)):
  0 inserted/deleted bin(s) removed; tail agrees for 5 bins;
  next mismatch at aligned index 5: x264=('dec', 69, 1), phx=('dec', 69, 0)
COEFFICIENT HANDOFF (MB0 luma 4x4 arrays):
  blocks compared: 16; raw arrays equal: 6/16; equal after zigzag rescan: 0/16
  block 1: x264 [3, -1, -2, 1, 1, 0] ... phx [7, -3, 0, 0, 0, 0] ...
```

**Exact answer to "does coder state match?" — YES.** Bin 5 has the same
context index (69), the same context state (`121`) and the same arithmetic
coder interval (`low/range 6560/436`) in both traces. Bins 1–4 match in `ctx`,
value, state and interval. Therefore:

* the CABAC arithmetic engine, context initialisation and the syntax writer are
  **not** the cause of the first divergence;
* the differing bin is a **decision** — the `rem_intra4x4_pred_mode` bits of
  `cabac_intra4x4_pred_mode` (`reference/x264/encoder/cabac.c:83-96`, called at
  `:425-427`). x264's golden MB0 dump for this fixture is `type=0` (I_4x4),
  block 1 raw mode `8`, coefficients `[3,-1,-2,1,1,0,…]`; Phoenix emits mode
  bits `1,0,0` instead of x264's `1,1,1` and block-1 coefficients
  `[7,-3,0,0,0,0]`. The I4 mode search/RD picked a different mode, so the
  residual is different too. Note also that 0/16 blocks agree even after a
  zigzag rescan, so the coefficient difference is not explained by a scan
  order alone.

The tool's coarse label for context 69 (`CTX_LABELS` at
`tools/cabac_bin_diff.py:68`) is only a category name; the authoritative
context derivation is `reference/x264/encoder/cabac.c:83-96` and the context
offset tables in `reference/x264/common/tables.c:1758-1791`.

---

## 3. What is already byte-correct for veryslow

"Byte-correct" here means observed identical bytes/fields/bins — not a claim of
full parity.

| Stage / module | Evidence from this audit | x264 authority |
|---|---|---|
| SPS NAL payload | `7 (SPS) 25/25 EXACT`, both entropies; `parameter sets: 2/2 byte-exact` | `phx_sps_init/write` ← `encoder/set.c:101`, `:305-477` (`docs/X264_INTERNALS.md:1084-1088`). Notably the 16-ref SPS (`num_ref_frames`) is already correct. |
| PPS NAL payload | `8 (PPS) 6/6 EXACT`, both entropies | `phx_pps_init/write` ← `encoder/set.c:479-503`, `:505-572` (`docs/X264_INTERNALS.md:1090-1093`) |
| Version SEI payload | `6 (SEI) 625/625 EXACT`, both entropies | `phx_sei_version_write` ← `encoder/set.c:593-623` (`docs/X264_INTERNALS.md:1095-1101`) |
| IDR slice header | `HEADER MATCH (33 bits)` in both entropies; all 14 fields equal | `phx_slice_header_init/write` ← `encoder/encoder.c:122-211`, `:213-364` (`src/phx_slice.c:14`, `:107`) |
| CABAC arithmetic coder + I-slice MB0 header syntax | bins 1–4 identical in `ctx/b/state/low/range`; coder state MATCH at bin 5 | `common/cabac.c:32-183`; `cabac_mb_type_intra` `encoder/cabac.c:35-66`; I-slice dispatch `:381-392`; `cabac_intra4x4_pred_mode` `:83-96` ← `src/phx_cabac_syntax.c:216-229` (source-identical) |
| I4 neighbour mode prediction helper | writer and predictor present and source-shaped | `x264_mb_predict_intra4x4_mode` `common/macroblock.h:420-431` ← `src/phx_cabac_syntax.c:174-181` |
| CAVLC header + first entropy bytes | IDR `DIFF@5`: bytes 0–4 (`65 88 81 00 1f`) match; header parsed as 33-bit match | `encoder/cavlc.c:487-595`, `:121-196` ← `src/phx_cavlc.c`, `src/phx_cavlc_syntax.c` |
| Encoder robustness / NAL inventory | 100/100 `x264_rc=0` and `phx_rc=0`; every veryslow case 11 vs 11 NALs with type sequence `7,8,6,5,1×7` | `encoder.c:2096-2134`, `:3323-3902` |
| Header gate (independent evidence) | `build/nightly_status.json` header gate: `header_exact 20/20`, `pps_tail_verified 20/20` | `docs/NIGHTLY_STATUS.md` gate table |

Also correct at the *parameter* level (knobs are set, algorithms are not):
`src/phx_param.c:106-112` mirrors `reference/x264/common/base.c:576-588` —
UMH, subme=10, me_range=24, 16 refs, B-adapt TRELLIS, PSUB8x8, trellis=2,
8 B-frames, lookahead=60, direct=AUTO. This must not be mistaken for functional
parity: see §4.

---

## 4. Phoenix functions missing or incorrect, with exact x264 citations

### 4.0 Incorrect — slice NAL start-code flag (raw first blocker at byte 669)

| Phoenix site | Wrong behaviour | Required x264 behaviour |
|---|---|---|
| `src/phx_encoder.c:1558` | `b_long_startcode = !(fc.b_idr && h->i_frame == 0)` with `h->i_frame` already incremented at `:1321` → all slices get 4-byte start codes | `reference/x264/encoder/encoder.c:2082-2083`: long only for NAL index 0, SPS, PPS or AVC-Intra; `x264_nal_encode` at `common/bitstream.c:55-104`. Low-level writer `src/phx_bitstream.c:125-140` is already correct. |

The header-set SEI flag was fixed during this audit (`src/phx_encoder.c:293`,
`src/phx_sei.c:381` now 0); the slice flag is the remaining instance.

### 4.1 Incorrect — I4 mode decision / RD (first CABAC blocker, bin 5)

| Phoenix site | Required x264 source |
|---|---|
| `src/phx_analysis.c:750` `phx_search_i4` (SATD-only I4 mode search) | `encoder/analyse.c:864-979` (I4 SATD search with 3λ predicted-mode bonus `:909`, `:937`, top-right emulation `:886-888`, encode-as-you-go `:959-960`) |
| `src/phx_analysis.c:1091-1112` `phx_analysis_init` — TODO at `:1092-1093`: "RD refinement (i_mbrd>=1) and fast-intra heuristics … are not evaluated" | `encoder/analyse.c:294-305` (`i_mbrd`, `b_deblock_rdo`, `b_early_terminate`), `mb_analyse_init_qp` `:255-292` |
| `src/phx_analysis.c:1310-1320` — **whole-MB `intra_rd()` is never called**; only `phx_analysis_intra_rd_refine_i4` (`:880`) runs when `mbrd>1`. Comment at `:1312-1313` says the code was tuned for "subme=7 => mbrd=1" | `encoder/analyse.c:982-1015` `intra_rd()` called at `:2941` for I slices, `:3218` (P), `:3613` (B); per-block `intra_rd_refine` `:1016-1135` called at `:2950`; `rd_cost_i4x4` `encoder/rdo.c:315-347`, `rd_cost_i8x8` `:279`, `rd_cost_chroma` `:349` |
| CABAC writer is **not** at fault: `src/phx_cabac_syntax.c:216-229` | `encoder/cabac.c:83-96`; write site `:425-427`; neighbour prediction `common/macroblock.h:420-431`; cache save `common/macroblock.c:673-846` / `analyse.c:3734-3753` |

veryslow gives `subme=10` → `i_mbrd = (subme>=6)+(subme>=8)+(subpel_refine>=10) = 3`
(`base.c:579`, `analyse.c:296-303`). So x264 runs both whole-MB RD and
per-block RD refinement, plus deblock-RDO; Phoenix runs neither whole-MB RD nor
deblock-RDO and uses a reduced per-block refine. That is consistent with the
observed block-1 mode/coefficient mismatch.

### 4.2 Missing — B-adapt TRELLIS and B-pyramid (frame types, POC, frame_num)

| Phoenix site | Required x264 source |
|---|---|
| `src/phx_lookahead.c:251-253`: `if (i_bframe_adaptive == PHX_B_ADAPT_TRELLIS) { /* TODO: slicetype_path()/Viterbi … Deliberately not approximated. */ }` | `encoder/slicetype.c:1288-1327` `slicetype_path_cost`, `:1333-1382` `slicetype_path`, TRELLIS branch `:1553-1579` |
| `src/phx_lookahead.c:254-...` implements only FAST; BREF inserted at `:468-477` | FAST `:1580-1621`; BREF insertion `:1891-1896` |
| `src/phx_lookahead.c:390` `phx_lh_slicetype_decide` (reorder at `:491`) | `encoder/slicetype.c:1745-1974`: BREF/B-pyramid walk `:1799-1885`, coded-order reorder `:1950-1958` |
| `src/phx_encoder.c:698-781` `phx_encoder_frame_context`; POC at `:720` from `display_frame` | `encoder.c:3485-3513` (`i_poc = 2*(i_frame - max(last_idr,0))` at `:3512-3513`), NAL type/slice type `:3517-3555`, `i_frame_num++` only on refs `:3881-3882` |
| `src/phx_encoder.c:625-656` ad-hoc MMCO for BREF | `encoder.c:2571-2607` `reference_hierarchy_reset` (MMCO commands `:2594-2600`, `i_mmco_remove_from_end` `:2603-2605`); `reference_update` `:2533-2562` |

veryslow selects `X264_B_ADAPT_TRELLIS` at `common/base.c:582` and inherits
`X264_B_PYRAMID_NORMAL` (`base.c:391`). Observed consequences: ref coded POC
sequence `0,6,2,4,14,10,8,…` vs Phoenix `0,14,6,2,4,8,10,…`; ref slice #5 is
P while Phoenix emits B; ref frame_num `4/5` vs Phoenix `3`. The **writer slots
already exist** (`src/phx_slice.c:151-186` ref-list reordering with the `:274`/`:288`
terminator, `:217-243` dec-ref marking, `:188-215` weight table); the state
producing them is wrong.

### 4.3 Missing — 16-ref list construction and analysis

| Phoenix site | Required x264 source |
|---|---|
| `src/phx_encoder.c:558-690` `phx_encoder_build_ref_lists` (simplified DPB split/sort/caps at `:571-605`, reorder heuristic `:609-619`, MMCO approximation `:625-656`) | `encoder.c:2295-2411` `reference_build_list`: POC split `:2305-2313`, MMCO reorder `:2315-2338`, distance sort `:2340-2360`, caps `:2364-2370`, weightp duplicates `:2372-2406` |
| Active list sizes/override: `src/phx_slice.c:51-58`, `src/phx_encoder.c:760` | `encoder.c:2639-2645` (`slice_init`) and `encoder.c:252-261` (writer) |
| `src/phx_analysis.c:1047-1055` — "reduced to the full-pel, **single-reference** path"; ref iteration at x264 `analyse.c:1268` is not ported | `encoder/analyse.c:1255-1333` (`for i_ref` at `:1268`); P 8x8 mixed refs `:3049-3052`; B bidir `analyse.c:1890-2692` |
| `src/phx_encoder.c:1355-1359` keeps one `i_last_ref_frame` input picture as `fref[0][0]` | `encoder.c:3583` + `common/macroblock.c:568-672` `macroblock_load_pic_pointers` (reconstructed hpel/ref planes) |

The arrays themselves are large enough (`PHX_ENC_DPB_MAX 16`,
`src/phx_encoder.h:138`; `PHX_SLICE_REF_MAX 16`, `src/phx_slice.h:27`), so the
16-ref failure is algorithmic, not an array-overflow problem. This is visible
as `num_ref_idx_l0_active_minus1 ref=4 phx=1` (slice #5),
`ref=3 phx=1` (slice #8) and mismatching `ref_pic_list_modification` entries.

### 4.4 Missing — weighted prediction (all P slices)

| Phoenix site | Required x264 source |
|---|---|
| No content-derived weights; `src/phx_encoder.c:661-672` hardcodes the offset −1 duplicate | `encoder/slicetype.c:284-...` `x264_weights_analyse`; helpers `weight_get_h264` `:64`, `weight_cost_init_luma` `:77`, `weight_cost_init_chroma` `:111`, `weight_slice_header_cost` `:170`, `weight_cost_luma` `:191`, `weight_cost_chroma` `:224`; called from `slicetype_decide` `:1937-1943` |
| `src/phx_encoder.c:558-690` never calls weight init | `encoder.c:2163-2201` `weighted_reference_duplicate`, `:2202-2284` `weighted_pred_init` |
| Writer exists but receives zeroed state: `src/phx_slice.c:98` (`memcpy(sh->weight, frame->weight, …)`), `:188-215` | `encoder.c:291-322` `pred_weight_table` (denominators `:296/:298`, per-ref luma/chroma flags and scales `:299-321`) |
| MC with weights absent | `common/mc.c:79` `pixel_avg_weight_wxh`, `:113` `weight_cache`, `:119` `mc_weight`; `common/frame.c:825` `x264_weight_scale_plane` |

`docs/FUNCTION_INVENTORY.md:1332-1341` marks every `weight_*` slicetype function
**MISSING**, and `:948`, `:1104-1105`, `:237`, `:330-332` mark
`x264_analyse_weight_frame`, `x264_weights_analyse`,
`weighted_reference_duplicate`, `weighted_pred_init`, `weight_cache`,
`mc_weight`, `x264_weight_scale_plane` **MISSING**. Observed:
`luma_log2_weight_denom ref=5 phx=0`, `chroma_log2_weight_denom ref=4 phx=<none>`,
`luma_weight_l0[0] ref=31 phx=0`.

### 4.5 Missing — UMH motion search and `me_range=24`

| Phoenix site | Required x264 source |
|---|---|
| `src/phx_me.c:160-171` `phx_me16`: `if (method != PHX_ME_DIA && method != PHX_ME_HEX) return -1;` — **UMH (method 2) is rejected outright.** `src/phx_analysis.c:1067-1070` turns that `-1` into "no inter candidate"; `src/phx_encoder.c:1430-1443` falls back to I16 | `encoder/me.c:182-798` `x264_me_search_ref`, UMH block `:422-616` (uneven cross, multi-hex, predictor refinement `:448-519`, cross + 4-hex `:521-616`); predictors `:216-275`; range `:199-209`, `:188` |
| No subpel refinement at all (`docs/X264_INTERNALS.md:809-811`, `src/phx_analysis.h:19-20`) | `encoder/me.c:801-810` `x264_me_refine_qpel`, `refine_subpel` `:865-992`, `subpel_iterations[10]` `:38-50` |
| `src/phx_lowres.c:687-691` hardcodes `PHX_ME_HEX, 16` despite veryslow's 24 | `encoder/slicetype.c:694` calls `x264_me_search`, which reads `h->param.analyse.i_me_range` (`encoder/me.c:188`); veryslow sets 24 (`common/base.c:580`). Lowres *method* HEX is correct (`encoder/slicetype.c:50-53`), but the range is not. `encoder.c:1159-1161` clips range but only forces 16 for DIA/HEX methods. |
| `docs/FUNCTION_INVENTORY.md:1185`: `x264_me_search_ref` **MISSING** | `encoder/me.c:182` |

### 4.6 Missing — subme=10 RD / MBRD, B analysis, 8x8 transform

| Phoenix site | Required x264 source |
|---|---|
| `src/phx_analysis.c:1092-1093`, `:1310-1320` (see §4.1) | `encoder/analyse.c:2934-2952` I-slice RD; `:2953-3304` P-slice incl. QPRD `:3246-3302`; `:3305-3733` B-slice analysis; `analyse_update_cache` `:3734-3753` |
| No B-slice analysis; `src/phx_encoder.c:1421-1443` accepts only P_L0 16x16 or I modes | `encoder/analyse.c:3305-3733`; direct mode `common/mvpred.c:183-288` (temporal), `:289-448` (spatial), `:454-516` `x264_mb_predict_mv_direct16x16`; `common/macroblock.c:1883-1925` `x264_macroblock_bipred_init`; bidirectional RD `encoder/analyse.c:2622`, `:2692`, `encoder/me.c:1027`, `:1180`, `:1185` |
| `src/phx_encoder.c:1449` forces `d.b_transform_8x8 = 0` on every MB and `src/phx_analysis.c:1104` forces `c->cache->b_transform_8x8 = 0`, although PPS `transform_8x8_mode_flag=1` is byte-exact (`src/phx_headers.c:94`, `:183-187`) | `encoder/cabac.c:233-237` `cabac_transform_size`; `encoder/rdo.c:2773` transform-size RD; `analyse.c:745-862` I8 RD. Golden veryslow dump shows `b8=1` for later frames, so x264 does use it. |
| Trellis QNS itself is shared and byte-correct on MB0 for medium (`docs/PORTING_ROADMAP.md:109`), so do not rework `src/phx_rdo.c:836`/`:979` before fixing the decisions that feed it | `encoder/rdo.c:642-895` (`quant_trellis_cabac`), `:921-1096` (`quant_trellis_cavlc`); note zigzag handoff at `rdo.c:890` and `encoder/macroblock.c:391/410` |

`docs/X264_INTERNALS.md:1471-1472` lists "All B-slice analysis, direct/spatial MV
prediction, weighted prediction and 8x8 transform size decisions" as unported;
`:1322-1326` records that Phoenix's DPB mirror keeps only the single list-0
reference. These are the exact stages veryslow activates and no other preset
activates as strongly.

---

## 5. Prioritized, source-aligned work list to reach byte-exact

No parameter, flag, fixture or reference-stream changes are acceptable fixes.
Each step must be verified with the same probe commands and a strictly smaller
first divergence.

1. **P0 — slice Annex-B start code.** Change `src/phx_encoder.c:1558` to
   implement `encoder.c:2082-2083` (long start code only for NAL index 0 / SPS /
   PPS / AVC-Intra). Gate: raw `first_diff` moves past byte 669 into the IDR
   slice payload; `first_frame_probe.py` still shows SPS/PPS/SEI EXACT.
2. **P0 — I4 mode selection at MB0 (bin 5).** Align
   `src/phx_analysis.c:750` / `:1091-1112` / `:1310-1320` with
   `encoder/analyse.c:864-979`, then add the missing whole-MB `intra_rd()`
   (`analyse.c:982-1015`, call site `:2941`) and the mbrd=3 deblock-RDO path
   (`analyse.c:302`). Gate: `cabac_bin_diff.py --preset veryslow` reports no
   divergence before bin 5, with identical `ctx/b/state/low/range`; the MB0
   block-1 coefficient array must match x264's `[3,-1,-2,1,1,0,…]`, not just
   the bin count. Do **not** touch `src/phx_cabac_syntax.c:216-229` — it already
   matches `encoder/cabac.c:83-96`.
3. **P1 — B-adapt TRELLIS + B-pyramid ordering.** Implement
   `slicetype_path`/`slicetype_path_cost` (`slicetype.c:1288-1382`) behind
   `src/phx_lookahead.c:251-253`, and align the coded-order reorder and BREF/MMCO
   bookkeeping (`slicetype.c:1799-1896`, `:1950-1958`; `encoder.c:2571-2607`).
   Gate: slice #2–#8 `pic_order_cnt_lsb`, `frame_num`, `slice_type` and
   `direct_spatial_mv_pred_flag` match in `first_frame_probe.py`.
4. **P1 — 16-ref lists / active counts / reorder / MMCO.** Replace the
   simplified `src/phx_encoder.c:558-690` with `encoder.c:2295-2411` semantics
   (split, MMCO reorder, distance sort, caps, weightp duplicates) and feed
   `src/phx_slice.c:51-76`, `:99-101`. Gate: `num_ref_idx_l0/l1_active_minus1`
   and every `modification_of_pic_nums_idc`/`abs_diff_pic_num_minus1` entry in
   slices #4–#8 match; MMCO flag/commands on reference NALs match.
5. **P1 — weighted prediction.** Port `x264_weights_analyse`
   (`slicetype.c:284-...`, helpers `:64-283`, call `:1937-1943`) and
   `weighted_pred_init`/`weighted_reference_duplicate`
   (`encoder.c:2163-2284`), then replace the hardcoded `src/phx_encoder.c:661-672`
   duplicate. Gate: `luma/chroma_log2_weight_denom` 5/4 and all
   `luma_weight_l0[]`, `luma_offset_l0[]`, `chroma_weight_l0[][]` in slices #2
   and #5 match.
6. **P2 — UMH + me_range=24 + subpel/QPRD.** Implement UMH
   (`encoder/me.c:422-616`) behind `src/phx_me.c:170`, remove the hardcoded
   `PHX_ME_HEX, 16` at `src/phx_lowres.c:687-691` (use the preset value),
   then add `x264_me_refine_qpel` (`me.c:801-810`, `:865-992`) and P QPRD
   (`analyse.c:3246-3302`). Gate: P-slice MB decisions and MVs match the golden
   trace, not merely the bit count.
7. **P2 — B-slice analysis, direct prediction, 8x8 transform.** Port
   `encoder/analyse.c:3305-3733`, `common/mvpred.c:183-516`,
   `common/macroblock.c:1883-1925` and remove the forced
   `src/phx_encoder.c:1449` / `src/phx_analysis.c:1104` `b_transform_8x8=0`
   (`encoder/cabac.c:233-237`, `encoder/rdo.c:2773`). Gate: B slices parse with
   the same `slice_type`, direct flag and transform-size bins.
8. **P3 — CAVLC re-probe and full corpus.** After steps 1–7, re-run
   `first_frame_probe.py --preset veryslow --entropy cavlc` (IDR `DIFF@5` and
   all P-slice `DIFF@3/0/2` must disappear through the shared decisions), then
   `parity_harness.py` → `compare_streams.py` → both probes → `cabac_bin_diff.py`
   (`docs/PARITY_ITERATION.md:8-60`). Only `byte_exact == true` for **all 100**
   cases ends the loop (`docs/PARITY_ITERATION.md:100-108`).
9. **Never accepted as a fix:** changing x264 options, fixtures or the
   reference corpus; matching bitrate, NAL counts, decoded picture, SPS/PPS,
   slice headers, or "N/N identical bins"; tuning QP or parameters to move a
   `first_diff` offset (`docs/PARITY_ITERATION.md:37-40`,
   `docs/VIDEO_PARITY.md:9-11`).

---

## 6. Explicit parity statement

**0/100 is not parity.** At the audit snapshot,
`build/parity/parity_report.json` reports `totals.byte_exact = 0` out of 100
compared cases and `by_preset.veryslow.byte_exact = 0` out of 10 veryslow cases
(5 fixtures × 2 entropies); all 10 veryslow cases are mismatches. The facts in §1–§3 — byte-exact SPS/PPS/SEI, a 33-bit-identical
IDR slice header, five identical ordered CABAC bins with identical context and
coder state, 100/100 successful encodes — are necessary diagnostics, **not**
parity. Until the slice start code, the MB0 I4 mode/RD decision, the B-pyramid
frame ordering, the 16-ref list, weighted prediction, UMH/subme-10 RD and the
B/8x8 paths are aligned and the harness reports `byte_exact: true` for every
case, `veryslow` remains a byte-exact divergence.

> **Post-audit note (orchestrator):** the `first_diff=669` start-code defect
> was fixed in the follow-up to `becb95b` by tracking the first slice NAL
> explicitly (lookahead delay could emit the IDR after `i_frame > 0`). Live
> first diffs are now bytes 586-825 across the corpus. NAL-payload and CABAC
> bin findings remain valid.

