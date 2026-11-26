# Preset audit — `slower` byte-exact parity blocker analysis

**Scope:** read-only subagent audit on branch `vk264-phoenix`. No source, CMake, tool, test or fixture was edited by this audit. Owned output: this file only. An isolated temp directory was used for the CABAC harness (`C:\Users\multi\AppData\Local\Temp\opencode\audit_slower_cabac_v{1,2}`); nothing was written inside the repo.

**Concurrency note:** other workers regenerated `build/parity/*slower*` and `parity_report.json` while this audit ran. The *first* probe snapshot recorded `first_diff 39` and phx CABAC bytes `9217`; the *current* snapshot records `first_diff 666` and phx CABAC bytes `9216`. `parity_report.json` was likewise updated from `first_diff_min 39` to `666`. All facts below are from the **current** snapshot unless a historical value is explicitly labelled. The CABAC-bin result was reproduced twice (v1 and v2 isolated builds) and is unchanged.

---

## 0. Bottom line

`slower` is **not byte-exact**. All 10 `slower` cases are mismatches and the whole 100-case corpus is still **0 byte-exact**. **0/100 is not parity** (§6).

The blockers, in strict byte order, are:

1. **Raw stream byte 666:** Phoenix emits one extra `0x00` before the first IDR slice's Annex-B start code — `ref[664:668] = 00 00 01 65`, `phx[664:668] = 00 00 00 01`. This is still an Annex-B/`b_long_startcode` discrepancy (`reference/x264/encoder/encoder.c:2082-2083` vs `src/phx_encoder.c:1555-1558`) even though the earlier SEI start-code divergence (byte 39 in the first snapshot) has since been fixed by another worker.
2. **IDR NAL payload byte 5 / CABAC bin #6:** x264 and Phoenix agree on SPS, PPS, SEI, the entire 31-bit IDR slice header, the first 5 ordered CABAC bins and the coder state (`low/range 6560/436`, context `state=121`). Bin 6 diverges on `intra4x4_pred_mode` suffix bit 1: x264 `ctx=69 b=1`, Phoenix `ctx=69 b=0`. The writer is byte-correct; the **I4 prediction-mode decision fed into it is wrong**.
3. Later slices diverge in both header semantics and entropy: POC, ref counts, slice type (ref P vs Phoenix B and vice versa), ref_pic_list_modification, and a completely missing weighted-prediction table. These are consistent with the unported **trellis B-adapt**, simplified high-ref list/weight handling, missing **UMH/subpel** ME, and missing **subme=9 deblock RDO**.
4. Trellis QNS is *partly* right (6/16 MB0 luma arrays identical), but 10/16 differ and a pure zigzag rescan fixes none of them — the coefficient **values** differ, not merely their scan order.

`slower` is configured correctly at the parameter level (`src/phx_param.c:100-105` mirrors `reference/x264/common/base.c:565-575`: UMH, subme=9, 8 refs, B-adapt TRELLIS, trellis=2, lookahead 60). The failure is in *executing* those decisions, not in naming them.

---

## 1. NAL-level diff facts (both entropies)

### 1.1 `python tools/first_frame_probe.py --preset slower --entropy cabac` (current snapshot)

```text
CASE synth-slower-cabac
  stream bytes: ref=3109, phx=9216; NALs: ref=11, phx=11
NAL comparison (payload bytes, start codes and trailing zeros excluded; NALs paired by type sequence):
  ref# phx#  ref NAL                phx NAL                len ref len phx status             first diff
     0    0  7 (SPS)                7 (SPS)                     23      23  EXACT              -
     1    1  8 (PPS)                8 (PPS)                      6       6  EXACT              -
     2    2  6 (SEI)                6 (SEI)                    624     624  EXACT              -
     3    3  5 (IDR slice)          5 (IDR slice)             1065    1173  DIFF@5             5
    ref: 0000: 65 88 82 00 7f ce f4 35 4d  |e......5M|  <== first diff
    phx: 0000: 65 88 82 00 7f d9 3c a8 68  |e.....<.h|  <== first diff
     4    4  1 (non-IDR slice)      1 (non-IDR slice)          292    1098  DIFF@2             2
     5    5  1 (non-IDR slice)      1 (non-IDR slice)          187    1057  DIFF@2             2
     6    6  1 (non-IDR slice)      1 (non-IDR slice)          144    1003  DIFF@2             2
     7    7  1 (non-IDR slice)      1 (non-IDR slice)          250    1024  DIFF@0             0
     8    8  1 (non-IDR slice)      1 (non-IDR slice)          196    1075  DIFF@1             1
     9    9  1 (non-IDR slice)      1 (non-IDR slice)          125    1056  DIFF@0             0
    10   10  1 (non-IDR slice)      1 (non-IDR slice)          155    1034  DIFF@6             6
  parameter sets: 2/2 byte-exact (SPS, PPS)
  slice #1: HEADER MATCH (31 bits), ENTROPY/MB MISMATCH at post-header byte 0 (RBSP byte 4, NAL payload byte 5)
    ref: 0000: 88 82 00 7f ce f4 35 4d ...
    phx: 0000: 88 82 00 7f d9 3c a8 68 ...
  slice #5: HEADER MISMATCH (47 differing field(s)):
    slice_type                     ref=5 (P)  phx=6 (B)
    num_ref_idx_l0_active_minus1   ref=4      phx=1
    luma_log2_weight_denom         ref=5      phx=<none>
  slice #6: HEADER MISMATCH (38 differing field(s)):
    slice_type                     ref=6 (B)  phx=5 (P)
    frame_num                      ref=4      phx=3
  slice #8: HEADER MATCH (37 bits), ENTROPY/MB MISMATCH at post-header byte 0
VERDICT: DIVERGENCE (first stream byte diff 666)
SUMMARY: 1 case(s), at least one divergence
```

The first snapshot of the same command (before the concurrent rebuild) reported `ref=3109, phx=9217` and `VERDICT: DIVERGENCE (first stream byte diff 39)`. Byte 39 is the SEI start-code byte (SPS 23 + PPS 6 puts the SEI start code at offset 37); that specific defect has since been fixed.

### 1.2 `python tools/first_frame_probe.py --preset slower --entropy cavlc` (current snapshot)

```text
CASE synth-slower-cavlc
  stream bytes: ref=3875, phx=11097; NALs: ref=11, phx=11
     0    0  7 (SPS)  23/23  EXACT
     1    1  8 (PPS)   6/6   EXACT
     2    2  6 (SEI)  624/624 EXACT
     3    3  5 (IDR slice) 1433/1532 DIFF@5
    ref: 0000: 65 88 82 00 7f 5c 84 44 a5 ...
    phx: 0000: 65 88 82 00 7f 44 9c 44 a5 ...
     4..10  1 (non-IDR slice) DIFF@2/@2/@2/@0/@1/@0/@5
  parameter sets: 2/2 byte-exact (SPS, PPS)
  slice #1: HEADER MATCH (31 bits), ENTROPY/MB MISMATCH at post-header byte 0 (RBSP byte 4, NAL payload byte 5)
  slice #5: HEADER MISMATCH (46 fields), ref slice_type P vs phx B, ref weight table present
  slice #6: HEADER MISMATCH (37 fields), ref slice_type B vs phx P
VERDICT: DIVERGENCE (first stream byte diff 666)
SUMMARY: 1 case(s), at least one divergence
```

Both entropies: same NAL count (11), same type sequence `7,8,6,5,1×7`, same three exact parameter-set/SEI payloads, same 31-bit IDR header, same first payload divergence at IDR byte 5, same raw first stream divergence at 666. CAVLC is therefore downstream of the *same* MB-level decision, not a separate CAVLC-token bug.

### 1.3 Raw stream byte 666 — the remaining Annex-B defect

```text
ref[658:674] 61 71 3d 30 00 80 00 00 01 65 88 82 00 7f ce f4 35
phx[658:674] 61 71 3d 30 00 80 00 00 00 01 65 88 82 00 7f d9 3c
                                       ^ index 666: ref 0x01, phx 0x00
```

`tools/first_frame_probe.py` strips start codes before its NAL comparison, so its `DIFF@5` is the first *payload* difference; the raw file's first difference is the extra `0x00` at 666. x264's rule is `b_long_startcode = !i || type == NAL_SPS || type == NAL_PPS || avcintra` (`reference/x264/encoder/encoder.c:2082-2083`, `x264_nal_encode` `common/bitstream.c:55-104`). Phoenix's slice decision is at `src/phx_encoder.c:1555-1558` (`nal.b_long_startcode = !(fc.b_idr && h->i_frame == 0)`), which does not reproduce the first-IDR case in the generated stream. Later slice NALs also differ in start-code length in the current snapshot.

### 1.4 `parity_report.json` — all 10 `slower` cases (current)

```text
synth-slower-cabac                               cabac ref=  3109 phx=  9216 ratio=2.96 exact=False firstdiff=666
synth-slower-cavlc                               cavlc ref=  3875 phx= 11097 ratio=2.86 exact=False firstdiff=666
ssstwitter.com_1748999992229-slower-cabac        cabac ref=  5086 phx= 15900 ratio=3.13 exact=False firstdiff=666
ssstwitter.com_1748999992229-slower-cavlc        cavlc ref=  5542 phx= 16557 ratio=2.99 exact=False firstdiff=666
ssstwitter.com_1732159163230-slower-cabac        cabac ref=  1957 phx=  7032 ratio=3.59 exact=False firstdiff=666
ssstwitter.com_1732159163230-slower-cavlc        cavlc ref=  1983 phx=  7349 ratio=3.71 exact=False firstdiff=666
ssstwitter.com_1732158697723-slower-cabac        cabac ref=  7014 phx= 22686 ratio=3.23 exact=False firstdiff=666
ssstwitter.com_1732158697723-slower-cavlc        cavlc ref=  7435 phx= 23756 ratio=3.20 exact=False firstdiff=666
ssstwitter.com_1732159939975-slower-cabac        cabac ref=  5636 phx= 17963 ratio=3.19 exact=False firstdiff=666
ssstwitter.com_1732159939975-slower-cavlc        cavlc ref=  5959 phx= 18571 ratio=3.12 exact=False firstdiff=666

by_preset.slower: cases 10, x264_ok 10, compared 10, byte_exact 0, mismatches 10, first_diff_min 666
totals:           cases 100, x264_ok 100, compared 100, byte_exact 0, mismatches 100, first_diff_min 586
```

`x264_ok=10/10` only says the golden encoder ran; it is not a parity metric.

---

## 2. First divergent CABAC bin: element, context, coder state

The requested command initially failed because the shared temp directory held a stale/locked object:

```text
ERROR: compile failed: ...\l1_cabac_bin_diff\encoder\cabac.c
fatal error C1083: Cannot open compiler generated file:
'...\trace_encoder_cabac.obj': Permission denied
```

Re-running with an isolated `--tmp` succeeded (and was reproduced in a second fresh directory):

```text
$ python tools/cabac_bin_diff.py --preset slower --fixture build/parity/fixture.yuv --tmp ...\audit_slower_cabac_v2
building instrumented x264 golden harness ...
building Phoenix -DPHX_CABAC_BIN_TRACE debug CLI ...
x264 golden bins: 27819; Phoenix bins: 40841
FIRST DIVERGENT BIN: index 5 (0-based, bin #6)
  bin 1     x264: ctx=399  b=0  dec      phx: ctx=399  b=0  dec
  bin 2     x264: ctx=68   b=1  dec      phx: ctx=68   b=1  dec
  bin 3     x264: ctx=68   b=0  dec      phx: ctx=68   b=0  dec
  bin 4     x264: ctx=69   b=1  dec      phx: ctx=69   b=1  dec
  bin 5     x264: ctx=69   b=1  dec      phx: ctx=69   b=0  dec   <== FIRST DIVERGENCE
  x264 element: intra4x4_pred_mode (:83-96)
  phx  element: intra4x4_pred_mode (:83-96)
  x264 record: dec ctx=69   b=1  state=121  low/range 6560/436 -> 6560/258
  phx  record: dec ctx=69   b=0  state=121  low/range 6560/436 -> 13636/356
  coder state before bin: MATCH (x264 low/range 6560/436, phx 6560/436)
  0 inserted/deleted bin(s) removed; tail agrees for 5 bins;
  next mismatch at aligned index 5
COEFFICIENT HANDOFF (MB0 luma 4x4 arrays):
  blocks compared: 16; raw arrays equal: 6/16; equal after zigzag rescan: 0/16
  block 1: x264 [3, -1, -2, 1, 1, 0] ... phx [7, -3, 0, 0, 0, 0] ...
```

Reading the ordered bins against `reference/x264/encoder/cabac.c`:

* bin 0 = `mb_type` I-slice ctx3 (`cabac_mb_header_i`, `encoder/cabac.c:381-392`) — both `b=0`;
* bin 1 = `transform_size_8x8_flag` ctx399 (`cabac_transform_size`, `:235-238`) — both `b=0`, so MB is I4x4;
* bins 2–6 = `cabac_intra4x4_pred_mode` (`:83-96`) for MB0's first two 4x4 blocks, driven by the loop at `:414-428` / `partition_i4x4_size_cabac` `:1203-1210`.

Bin 5 is the **second suffix bin** (`i_mode >> 1 & 1`, `cabac.c:93`) of block 1. x264 encodes `1,1,1` (signalled FIX4 mode 7 after the `>` decrement at `:90-91`, i.e. raw mode 8 in this MB — the golden dump reports `L1BLK 1 ... mode=8`); Phoenix encodes `1,0,0` (adjusted mode 1, i.e. raw mode 1 or 2 depending on the predictor; its block-1 coefficient array is `[7,-3,0,...]`). The coder state at the divergent bin is **identical**, so this is a **decision/input-value divergence**, not an arithmetic-coder or context-init bug. The later `low/range` difference (`6560/258` vs `13636/356`) is a consequence of the differing bin.

Both traces agree on the writer itself: `src/phx_cabac_syntax.c:215-229` is a line-for-line port of `encoder/cabac.c:83-96`, and `phx_predict_intra4x4_mode` (`:173-181`) mirrors `common/macroblock.h:420-431`.

**Coefficient-handoff caveat:** `cabac_bin_diff.py:517-565` tests a zigzag-rescan hypothesis, but the measurement here is `6/16` raw matches and `0/16` after rescan. A pure scan-order fix would therefore make agreement *worse*; the 10 mismatching coefficient arrays have different *values*. Also note the harness hard-codes `--qp 28` (`tools/cabac_bin_diff.py:370-373, 381`), which matches the parity harness (`tools/parity_harness.py:228, 239`), so the bin result and the NAL result are at the same QP.

---

## 3. What is already byte-correct for `slower`

"Byte-correct" here means observed identical bytes/fields/bins in this preset — not a claim of overall parity.

| Stage | Evidence (current run) | x264 authority |
|---|---|---|
| SPS NAL payload | `7 (SPS) 23/23 EXACT`, both entropies | `x264_sps_init/write` `common/set.c:101`, `:305`; `phx_headers.c:13,76` |
| PPS NAL payload (incl. 8-ref count) | `8 (PPS) 6/6 EXACT`, both entropies | `x264_pps_init/write` `common/set.c:479`, `:505`; `phx_headers.c:83-84` sets `i_num_ref_idx_l0=8` |
| Version SEI payload | `6 (SEI) 624/624 EXACT`, both entropies | `x264_sei_version_write` `common/set.c:593`; `phx_sei.c:322,340` |
| IDR slice header | `HEADER MATCH (31 bits)`, all 14 fields equal (`slice_qp_delta=-3`, `slice_qp=25`) in CABAC **and** CAVLC | `slice_header_write` `encoder/encoder.c:213-364`; `phx_slice.c:14,107` |
| CABAC engine, context init, renorm, first bins | bins 0–5 identical in `ctx`, `b`, `state`, `low/range`; coder state MATCH at the first divergence | `common/cabac.c:32-183`; `phx_cabac.c:54-209` |
| CABAC MB0 header syntax writers | `mb_type`, `transform_size_8x8_flag`, intra4x4 prefix and first suffix bin match | `encoder/cabac.c:83-96`, `:235-238`, `:381-392`, `:414-428`; `phx_cabac_syntax.c:215-229` |
| Trellis QNS quant core (partially) | MB0 luma block 0 is identical: `x264 -36 -3 -5 0 ...` = `phx -36 -3 -5 0 ...`; 6/16 blocks identical | `x264_quant_4x4_trellis` `encoder/rdo.c:642-895`; `phx_rdo.c:836,1118-1144` |
| NAL structure / QP / encoder robustness | 11 vs 11 NALs, same type sequence in all 10 cases; `x264_rc=0`, `phx_rc=0` everywhere | `x264_encoder_headers` `encoder/encoder.c:2096-2134`; stage 27-28 `:3884-3901` |
| Preset field mapping | slower deltas match source order exactly | `common/base.c:565-575` ↔ `src/phx_param.c:100-105` |

The header "mismatches" for slices #2–#7 in the probe are partly **parser desync** after the first entropy divergence (values such as `cabac_init_idc=28`, `slice_qp=0`, `<none>` are not credible encoder outputs). The probe parses each NAL independently from its NAL header (`tools/first_frame_probe.py:762-803`). Treat the POC/ref-count/slice-type/weight patterns as diagnostic of upstream lookahead divergence, not as 47 separate writer bugs.

---

## 4. Phoenix functions missing or incorrect, with x264 citations

### 4.1 Incorrect — Annex-B start code on the IDR slice (raw first blocker)

| Phoenix site | Wrong behaviour | Required x264 behaviour |
|---|---|---|
| `src/phx_encoder.c:1555-1558` | `nal.b_long_startcode = !(fc.b_idr && h->i_frame == 0)` produces `00 00 00 01` before the first IDR in the generated stream | `b_long_startcode = !i \|\| type == NAL_SPS \|\| type == NAL_PPS \|\| avcintra`, `encoder/encoder.c:2082-2083`; writer `common/bitstream.c:55-104` (`phx_bitstream.c:125-140` is already correct) |

The SEI instance of this bug (`src/phx_sei.c:379`, `src/phx_encoder.c:291` in the first snapshot) has been fixed by another worker; the IDR byte remains.

### 4.2 Missing — `intra_rd()` for all subme≥9 I-slices (primary cause of bin #6)

| Required x264 function | Citation | Phoenix state |
|---|---|---|
| `mb_analyse_init` sets `i_mbrd=2` and `b_deblock_rdo=1` at subme=9 | `encoder/analyse.c:294-303`, specifically `:301-302` | `src/phx_analysis.c:1295` calls `phx_analysis_init`, but no `b_deblock_rdo` exists anywhere in `src/` |
| `intra_rd()` replaces SATD with whole-MB RD costs when `i_mbrd` | `encoder/analyse.c:982-1014`, called at `:2940-2941` | **Not ported.** `src/phx_analysis.c:1304-1320` goes straight from SATD search to `phx_analysis_finalise`; its own comment at `:1310-1314` describes the gap |
| `mb_init_fenc_cache(h, i_mbrd>=2)` before analysis | `encoder/analyse.c:2938` | Not ported; `phx_setup_fdec` (`src/phx_analysis.c`) uses a simplified FENC/FDEC cache |
| `rd_cost_mb()` applies `x264_macroblock_deblock()` before SSD | `encoder/rdo.c:162-185` (`:171-172`), `common/deblock.c:615`; `COPY_CABAC` at `rdo.c:71-73` | `src/phx_analysis.c:848-877` `phx_analysis_rd_i4` computes SSD without deblocking and builds a fresh `phx_cabac_mb_t` (`:851-861`) rather than copying live state |

This is the direct explanation for the first divergent bin: MB0 is I4x4 with all 16 CBF bits set (`L1MB0 type=0 ... cbp_l=15`), and at subme=9 x264 chooses block 1's mode from deblock-aware whole-MB RD; Phoenix chooses it from SATD (plus its local refine). Until `intra_rd` + `b_deblock_rdo` are ported, any change to the intra4x4 writer or CABAC engine is misdirected.

### 4.3 Incomplete — `intra_rd_refine()`, trellis coefficient handoff

| Required x264 function | Citation | Phoenix state |
|---|---|---|
| `intra_rd_refine()` per-block I4 re-test over the full mode list | `encoder/analyse.c:1016-1135` (`:1086-1135` for I4), called at `:2950-2951` | `src/phx_analysis.c:885-933` `phx_analysis_intra_rd_refine_i4` is a luma-only approximation; gated at `:1315-1320`. Comment at `src/phx_analysis.h:19-20` is now stale (RD refine exists in a reduced form) |
| `rd_cost_i4x4()` with live CABAC-state copy | `encoder/rdo.c:315-347`, `COPY_CABAC` `:71-73` | `src/phx_analysis.c:848-877` uses size-only writers (`src/phx_rdo.c:406-420`) on a fresh MB context |
| `x264_quant_4x4_trellis()` output written in natural order, then zigzag-scanned into `h->dct.luma4x4` | `encoder/rdo.c:890`; I16 luma call+scan `encoder/macroblock.c:174-184` (`:177`, `:180`); chroma `:388-391` | `src/phx_rdo.c:944` mirrors `:890` and `src/phx_mbencode.c:317,324` / `src/phx_analysis.c:601,662` apply the scan. **Ordering is not the observed bug** — 0/16 arrays agree after rescan. The 10 differing arrays have different coefficient values. |
| `x264_quant_8x8_trellis()` | `encoder/rdo.c:921` onward | Explicitly excluded: `src/phx_rdo.c:1146-1149`, `src/phx_rdo.h:206-209` ("a few differing coefficients … do not use in a parity-critical path"). Slower's `b_transform_8x8` is off in MB0, but I8 decisions elsewhere need it. |

### 4.4 Missing — trellis B-adapt (explains later slice types/POC/ref counts)

| Required x264 function | Citation | Phoenix state |
|---|---|---|
| `slicetype_path_cost()` (Viterbi path accumulation, threshold pruning, pyramid costs) | `encoder/slicetype.c:1288-1327` | **Not ported** |
| `slicetype_path()` (path enumeration, best-path storage) | `encoder/slicetype.c:1329-1382` | **Not ported** |
| `i_bframe_adaptive == X264_B_ADAPT_TRELLIS` dispatch and frame-type loading | `encoder/slicetype.c:1553-1579` (`:1561-1562`, `:1565-1577`) | `src/phx_lookahead.c:251-253` is an empty `TODO` branch: `/* TODO: slicetype_path()/Viterbi … Deliberately not approximated. */` |
| lookahead declaration of what is missing | — | `src/phx_lookahead.h:27-31`: "Not ported … trellis B-adapt … weighted prediction, full lowres motion search" |

Phoenix therefore runs no trellis Viterbi decision for `slower`. This matches the probe: slice #5 ref=P / Phoenix=B, slice #6 ref=B / Phoenix=P, and POC sequences `6/2/4/14/10/8` vs `8/4/2/6/14/10`. Note that x264's lookahead ME is *downgraded* to DIA/HEX (`encoder/slicetype.c:50-59`), so UMH absence does not excuse the trellis path — its frame costs still must match.

### 4.5 Missing — UMH fullpel ME and subme=9 subpel refinement

| Required x264 function | Citation | Phoenix state |
|---|---|---|
| `case X264_ME_UMH` (predictor refinement, uneven cross, 4-hex grid, SAD early exits) | `encoder/me.c:422-616` | `src/phx_me.c:169-171` rejects every method except `PHX_ME_DIA`/`PHX_ME_HEX`; `slower` selects `PHX_ME_UMH` at `src/phx_param.c:101` |
| Lowres fullpel search used by slicetype | `encoder/me.c:320-419` (DIA/HEX) via `slicetype.c:50-59` | `src/phx_lowres.c:437-477` implements DIA/HEX only — *acceptable for lookahead by design*, but `lr_fullpel_search` receives `method` from the preset (`:576`) and has no UMH branch |
| `x264_me_refine_qpel` / `refine_subpel`, `subpel_iterations[subme]` | `encoder/me.c:38-50`, `:801-810`, `:865-992` | Not ported; `src/phx_analysis.h:19` lists sub-pel ME as TODO. `src/phx_lowres.c:479-507` only mimics the *lowres* 2-iteration qpel refinement |
| Multi-ref / mixed-ref / 8x8 sub-partitions for `slower` | `encoder/analyse.c:3049-3113`, `:3119-3179` | `src/phx_analysis.c:1298-1302` does P L0 16x16 only; `src/phx_analysis.h:19-20` documents the gap |

This does not cause bin #6 (MB0 is I), but it blocks byte-exact P/B slices, and it feeds different lookahead frame costs into the still-missing trellis B-adapt.

### 4.6 Missing — weighted prediction content (visible in slice #2/#5 headers)

| Required x264 function | Citation | Phoenix state |
|---|---|---|
| `x264_weights_analyse()` (per-plane scale/offset from `i_pixel_sum`/`i_pixel_ssd`) | `encoder/slicetype.c:284` onward; called at `:862` (lookahead) and `:1942` (encode) | Not ported (`src/phx_lookahead.h:30`). `src/phx_encoder.c:656-669` only inserts a SMART offset `-1` duplicate ref |
| `pred_weight_table()` writer, luma/chroma flags and denom | `encoder/encoder.c:291-326`, called from `:2791`/`slice_header_write` | `src/phx_slice.c:188-215` writes the syntax but has no real weights; B implicit weights are a TODO there, mirroring x264's own `:323-326` TODO |
| P/B slice QP and AQ interactions | `encoder/encoder.c:3855-3861`, `:2787-2789` | Present via `phx_ratecontrol.c`; not the current blocker |

The reference header shows `luma_log2_weight_denom=5`, `luma_weight_l0[0]=31`, `luma_offset_l0[0]=5`, `chroma_log2_weight_denom=4`, `chroma_weight_l0[0][1]=15`, `chroma_offset_l0[0][1]=-10`; Phoenix emits no weight table at all.

### 4.7 Checked and *not* a blocker — high-ref array sizing

`slower` asks for `i_frame_reference=8` (`common/base.c:569`). The arrays are large enough:

* x264 `X264_REF_MAX 16` — `reference/x264/common/base.h:137`; clipping at `encoder/encoder.c:1064-1065`; SPS `num_ref_frames` at `encoder/set.c:151`.
* Phoenix `PHX_SLICE_REF_MAX 16` — `src/phx_slice.h:27`; `PHX_ENC_DPB_MAX 16` — `src/phx_encoder.h:138`.
* PPS `num_ref_idx_l0_default_active_minus1=7` is byte-exact in the probe.

So **do not "fix" this by growing arrays**. What is wrong is list/POC/MMCO/weight *semantics*: `src/phx_encoder.c:556-689` (`phx_encoder_build_ref_lists`) is a simplified DPB sort with a duplicate-only weight entry (`:656-669`); x264's authoritative list build is `encoder/encoder.c:2295-2411` with hierarchy reset `:2571-2606`. The probe's `num_ref_idx_l0_active ref=5 phx=1` (slice #5) and `ref=2 phx=4/1` (slices #4/#6) indicate those semantics, not allocation size.

---

## 5. Prioritized, source-aligned work list to reach byte-exact

No parameter, QP, fixture, flag or golden stream may be changed to "move" a diff (`docs/PARITY_ITERATION.md:37-40`, `docs/VIDEO_PARITY.md:9-11`).

1. **P0 — Annex-B byte 666.** Align `src/phx_encoder.c:1555-1558` with `encoder/encoder.c:2082-2083`; require the stream file's first differing byte to move past the IDR start code and the raw bytes at the NAL boundary to equal x264's (`00 00 01 65`). Re-run both `first_frame_probe.py` commands: their `VERDICT` offset must move beyond 666 and the NAL payload table must remain `SPS/PPS/SEI EXACT`.
2. **P0 — subme=9 RD mode decision (bin #6).** Port `intra_rd` (`encoder/analyse.c:982-1014`, called `:2940-2941`) and `rd_cost_mb` with `b_deblock_rdo` (`encoder/rdo.c:162-185`, gate `analyse.c:302`, `x264_macroblock_deblock` `common/deblock.c:615`) into `src/phx_analysis.c`, replacing the direct `phx_analysis_finalise` path at `:1309` for `i_mbrd>=1`. Gate: `cabac_bin_diff.py --preset slower --fixture build/parity/fixture.yuv` must show **bins 0–5 still identical and bin 6 matching**, with identical `ctx/b/state/low/range`; do not "fix" it by changing `phx_cabac_intra4x4_pred_mode` (`src/phx_cabac_syntax.c:215-229`, already correct).
3. **P0 — coefficient values behind the mode/refine decision.** Resolve the 10 differing MB0 luma arrays against `x264_quant_4x4_trellis` (`encoder/rdo.c:642-895`, output `:890`) and the I16 zigzag handoff (`encoder/macroblock.c:174-184`). Requirement: `COEFFICIENT HANDOFF` reports `16/16 raw arrays equal`, not merely equal bin counts; a zigzag-only change is disproved (0/16 after rescan).
4. **P1 — trellis B-adapt.** Implement `slicetype_path_cost`/`slicetype_path` (`encoder/slicetype.c:1288-1382`) and the TRELLIS dispatch (`:1553-1579`) in place of the empty `src/phx_lookahead.c:251-253` branch. Gate: all 8 slice headers in `first_frame_probe.py --preset slower --entropy {cabac,cavlc}` must show matching `slice_type`, `frame_num`, `pic_order_cnt_lsb`, `num_ref_idx_*` and `ref_pic_list_modification_*` fields before entropy bytes are compared.
5. **P1 — weighted prediction.** Port `x264_weights_analyse` (`encoder/slicetype.c:284`, calls `:862`/`:1942`) and the `pred_weight_table` content (`encoder/encoder.c:291-326`) into `src/phx_encoder.c:656-669` and `src/phx_slice.c:188-215`. Gate: the reference's `luma_log2_weight_denom=5`, `luma_weight_l0[0]=31`, `chroma_*` fields must appear byte-identically. Keep `PHX_SLICE_REF_MAX`/`PHX_ENC_DPB_MAX` at 16.
6. **P2 — UMH/subpel and P/B analysis.** Port `x264_me_search_ref` UMH (`encoder/me.c:422-616`), `x264_me_refine_qpel`/`refine_subpel` (`:801-810`, `:865-992`) and the P/B analysis paths missing from `src/phx_analysis.c:1250-1327` (`encoder/analyse.c:2953-3733`). This is needed for byte-exact later slices and for correct trellis B-adapt path costs; it is *not* the IDR bin #6 cause.
7. **P2 — re-probe CAVLC.** After steps 2–3 the CAVLC IDR byte-5 diff should disappear through the shared MB decision/coefficient fix; only then touch `src/phx_cavlc.c:951-1114` / `src/phx_cavlc_syntax.c:69-411` against `encoder/cavlc.c:121-196, 318-330, 487-595`. CAVLC's first diff is downstream (`encoder/cavlc.c:323-329` consumes the same `i_pred`/`i_mode`).
8. **Final gate.** Rebuild the full corpus (`tools/parity_harness.py` → `tools/compare_streams.py`), then require `parity_report.json` `byte_exact: true` for **all 100** cases, plus `first_frame_probe.py --preset slower --entropy {cabac,cavlc}` `BYTE-EXACT` and `cabac_bin_diff.py --preset slower` "first MB0 bin streams are IDENTICAL". Anything less is not parity.

---

## 6. Explicit parity statement

**0/100 is not parity.** `parity_report.json` still reports `totals: cases 100, x264_ok 100, compared 100, byte_exact 0, mismatches 100`; `by_preset.slower` reports `cases 10, byte_exact 0, mismatches 10`. `docs/PORTING_ROADMAP.md:123-127` says outright: *"0/100 means zero of the 100 saved x264 streams are byte-identical; it is a divergence, not a success metric"*, and `docs/PARITY_ITERATION.md:100-108` requires whole-file byte identity for every case.

The byte-exact SPS/PPS/SEI, 31-bit IDR header, 6 matching CABAC bins with matching coder state, and 100/100 successful x264 encodes are **necessary diagnostics, not parity claims**. Until the Annex-B byte, the subme=9 RD decision, the trellis B-adapt path, weighted prediction and the downstream CAVLC/P/B paths are fixed, `slower` remains a divergence.
