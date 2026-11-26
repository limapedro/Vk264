# Preset audit — `ultrafast` byte-exact parity blocker analysis

**Auditor scope:** read-only audit, subagent. HEAD `55c3bad5e23d3433391e0d4d331cc80c46c2e6be`, branch `vk264-phoenix`.
No source, CMake, tool or test was modified. Owned output: this file only.
Corpus: `build/parity/parity_report.json`; probe streams `build/parity/synth-ultrafast-{cabac,cavlc}.{ref,phx}.h264`.

---

## 0. Bottom line

`ultrafast` is **not byte-exact** and the corpus records **0/100 byte-exact for the whole 100-case set** (`build/parity/parity_report.json` → `totals: byte_exact 0, mismatches 100`). **0/100 is not parity** — it means zero of the 100 saved Phoenix streams equal their x264 reference byte for byte. `docs/PORTING_ROADMAP.md:123-127` states this explicitly: *"0/100 means zero of the 100 saved x264 streams are byte-identical; it is a divergence, not a success metric."*

The ultrafast-specific blockers, in strict byte order, are:

1. **Stream byte 35:** Phoenix writes a **4-byte Annex-B start code for the SEI NAL** where x264 writes a **3-byte** one (`reference/x264/encoder/encoder.c:2082-2083`). This is the *first* stream-level divergence of every ultrafast case and it is invisible to `first_frame_probe.py`, which strips start codes.
2. **IDR slice payload byte 6 / later slice payload bytes:** CABAC bin 17 diverges — a `last_significant_coeff_flag` for coefficient index 3 of the **I16 luma DC** block, i.e. a coefficient-value mismatch in `h->dct.luma16x16_dc[0]`, not a CABAC-engine or context bug (coder state and context state match exactly).
3. The same MB-level divergence appears in CAVLC (IDR payload byte 6; P slices byte 3) and in every subsequent slice NAL.
4. Phoenix also writes a 4-byte start code on every slice NAL (`src/phx_encoder.c:1553`), an independent byte-exact blocker once (1) is fixed.

---

## 1. NAL-level diff facts (from `first_frame_probe.py`)

### 1.1 `python tools/first_frame_probe.py --preset ultrafast --entropy cabac`

Excerpt (verbatim):

```text
CASE synth-ultrafast-cabac
  stream bytes: ref=6097, phx=8190; NALs: ref=11, phx=11
NAL comparison (payload bytes, start codes and trailing zeros excluded; NALs paired by type sequence):
  ref# phx#  ref NAL                phx NAL                len ref len phx status             first diff
     0    0  7 (SPS)                7 (SPS)                     21      21  EXACT              -
     1    1  8 (PPS)                8 (PPS)                      4       4  EXACT              -
     2    2  6 (SEI)                6 (SEI)                    542     542  EXACT              -
     3    3  5 (IDR slice)          5 (IDR slice)             1308    1174  DIFF@6             6
    ref: 0000: 65 88 84 3a f9 22 95 53  |e..:.".S|  <== first diff
    phx: 0000: 65 88 84 3a f9 22 d9 f0  |e..:."..|  <== first diff
     4    4  1 (non-IDR slice)      1 (non-IDR slice)          551     910  DIFF@4             4
     5    5  1 (non-IDR slice)      1 (non-IDR slice)          647     924  DIFF@4             4
     6    6  1 (non-IDR slice)      1 (non-IDR slice)          548     930  DIFF@4             4
     7    7  1 (non-IDR slice)      1 (non-IDR slice)          628     898  DIFF@4             4
     8    8  1 (non-IDR slice)      1 (non-IDR slice)          622     925  DIFF@4             4
     9    9  1 (non-IDR slice)      1 (non-IDR slice)          583     929  DIFF@4             4
    10   10  1 (non-IDR slice)      1 (non-IDR slice)          601     889  DIFF@4             4
  parameter sets: 2/2 byte-exact (SPS, PPS)
  slice #1: HEADER MATCH (24 bits), ENTROPY/MB MISMATCH at post-header byte 2 (RBSP byte 5, NAL payload byte 6)
  slice #2..#8: HEADER MATCH (19 bits), ENTROPY/MB MISMATCH at post-header byte 0 (RBSP byte 3, NAL payload byte 4)
VERDICT: DIVERGENCE (first stream byte diff 35)
```

### 1.2 `python tools/first_frame_probe.py --preset ultrafast --entropy cavlc`

Excerpt (verbatim):

```text
CASE synth-ultrafast-cavlc
  stream bytes: ref=7053, phx=11244; NALs: ref=11, phx=11
     0    0  7 (SPS)                7 (SPS)                     21      21  EXACT              -
     1    1  8 (PPS)                8 (PPS)                      4       4  EXACT              -
     2    2  6 (SEI)                6 (SEI)                    542     542  EXACT              -
     3    3  5 (IDR slice)          5 (IDR slice)             1644    1611  DIFF@6             6
    ref: 0000: 65 88 84 3a 0c 60 00 80  |e..:.`..|  <== first diff
    phx: 0000: 65 88 84 3a 0c 60 38 00  |e..:.`8.|  <== first diff
     4    4  1 (non-IDR slice)      1 (non-IDR slice)          619    1269  DIFF@3             3
     5    5  1 (non-IDR slice)      1 (non-IDR slice)          738    1277  DIFF@3             3
     6    6  1 (non-IDR slice)      1 (non-IDR slice)          646    1296  DIFF@3             3
     7    7  1 (non-IDR slice)      1 (non-IDR slice)          725    1300  DIFF@3             3
     8    8  1 (non-IDR slice)      1 (non-IDR slice)          706    1304  DIFF@3             3
     9    9  1 (non-IDR slice)      1 (non-IDR slice)          645    1308  DIFF@3             3
    10   10  1 (non-IDR slice)      1 (non-IDR slice)          721    1268  DIFF@3             3
  parameter sets: 2/2 byte-exact (SPS, PPS)
  slice #1: HEADER MATCH (24 bits), ENTROPY/MB MISMATCH at post-header byte 2 (RBSP byte 5, NAL payload byte 6)
  slice #2..#8: HEADER MATCH (18 bits), ENTROPY/MB MISMATCH at post-header byte 0 (RBSP byte 3, NAL payload byte 4)
VERDICT: DIVERGENCE (first stream byte diff 35)
```

### 1.3 Reconciling "SEI EXACT" with stream byte 35 (critical)

`tools/first_frame_probe.py:691` compares **payload bytes with start codes stripped**, so it cannot see the first real stream divergence. Direct byte comparison of the two files:

```text
cabac ref 6097 phx 8190 first_diff 35 refbyte 0x1 phxbyte 0x0
cabac ref[25:50]: 00 00 00 01 68 ee 09 c8 00 00 01 06 05 ff ff 1a dc 45 e9 bd e6 d9 48 b7 96
cabac phx[25:50]: 00 00 00 01 68 ee 09 c8 00 00 00 01 06 05 ff ff 1a dc 45 e9 bd e6 d9 48 b7
cavlc ref[25:50]: 00 00 00 01 68 ce 09 c8 00 00 01 06 05 ff ff 1a dc 45 e9 bd e6 d9 48 b7 96
cavlc phx[25:50]: 00 00 00 01 68 ce 09 c8 00 00 00 01 06 05 ff ff 1a dc 45 e9 bd e6 d9 48 b7
```

The SEI **payload** is identical (542 bytes) but the **start code is one byte longer** in Phoenix (4 bytes `00 00 00 01` vs x264's 3 bytes `00 00 01`). Same defect on the IDR slice:

```text
cabac ref IDR hdr at 581 preceding bytes 80 00 00 01 65 88   (3-byte start code)
cabac phx IDR hdr at 583 preceding bytes 00 00 00 01 65 88   (4-byte start code)
```

x264's rule (`reference/x264/encoder/encoder.c:2082-2083`, cited in `docs/X264_INTERNALS.md:255-259`): `b_long_startcode = !i || type == NAL_SPS || type == NAL_PPS || avcintra`. Only NAL 0 (SPS), PPS and the first NAL get 4 bytes; SEI and every slice get 3 bytes. Phoenix violates this at `src/phx_encoder.c:291` (SEI in header set), `src/phx_sei.c:379` (SEI writer) and `src/phx_encoder.c:1553` (every slice). The low-level writer `src/phx_bitstream.c:125-140` already implements x264's `x264_nal_encode` (`common/bitstream.c:55-104`) correctly — only the `b_long_startcode` flags are wrong.

### 1.4 `build/parity/parity_report.json` — all 10 ultrafast cases

| case | ref bytes | phx bytes | first_diff | byte_exact |
|---|---:|---:|---:|---|
| synth-ultrafast-cabac | 6097 | 8190 | 35 | false |
| synth-ultrafast-cavlc | 7053 | 11244 | 35 | false |
| ssstwitter.com_1748999992229-ultrafast-cabac | 5718 | 13553 | 35 | false |
| ssstwitter.com_1748999992229-ultrafast-cavlc | 6361 | 17286 | 35 | false |
| ssstwitter.com_1732159163230-ultrafast-cabac | 1610 | 5658 | 35 | false |
| ssstwitter.com_1732159163230-ultrafast-cavlc | 1596 | 6890 | 35 | false |
| ssstwitter.com_1732158697723-ultrafast-cabac | 9417 | 21357 | 35 | false |
| ssstwitter.com_1732158697723-ultrafast-cavlc | 10143 | 25355 | 35 | false |
| ssstwitter.com_1732159939975-ultrafast-cabac | 7088 | 17905 | 35 | false |
| ssstwitter.com_1732159939975-ultrafast-cavlc | 7482 | 21053 | 35 | false |

`by_preset.ultrafast`: `cases 10, x264_ok 10, compared 10, byte_exact 0, mismatches 10, first_diff_min 35`.
`totals`: `cases 100, byte_exact 0, mismatches 100, first_diff_min 35`; first-diff distribution over all cases: `{35:10, 37:20, 38:10, 39:40, 41:20}` — the `35` bucket is exactly the 10 ultrafast cases, all attributable to the SEI start-code byte before any entropy byte is compared.

---

## 2. First divergent CABAC bin — element, context, coder state

Command: `python tools/cabac_bin_diff.py --preset ultrafast --fixture build/parity/fixture.yuv`

```text
x264 golden bins: 41605; Phoenix bins: 41669
FIRST DIVERGENT BIN: index 17 (0-based, bin #18)
  bin 13    x264: ctx=167  b=0  dec      phx: ctx=167  b=0  dec
  bin 14    x264: ctx=107  b=1  dec      phx: ctx=107  b=1  dec
  bin 15    x264: ctx=168  b=0  dec      phx: ctx=168  b=0  dec
  bin 16    x264: ctx=108  b=1  dec      phx: ctx=108  b=1  dec
  bin 17    x264: ctx=169  b=0  dec      phx: ctx=169  b=1  dec       <== FIRST DIVERGENCE
  bin 18    x264: ctx=109  b=1  dec      phx: ctx=228  b=1  dec
  bin 19    x264: ctx=170  b=0  dec      phx: ctx=232  b=1  dec
  x264 element: significant_coeff_flag chroma/8x8 (cabac_block_residual, :664-748)
  phx  element: significant_coeff_flag chroma/8x8 (cabac_block_residual, :664-748)
  x264 record: dec ctx=169  b=0  state=76   low/range 8666/256 -> 17332/434
  phx  record: dec ctx=169  b=1  state=76   low/range 8666/256 -> 1432/312
  coder state before bin: MATCH (x264 low/range 8666/256, phx 8666/256)
REMAINING DIVERGENCE ... 0 inserted/deleted bin(s) removed; tail agrees for 17 bins;
  next mismatch at aligned index 17: x264=('dec', 169, 0), phx=('dec', 169, 1)
```

**Coder state matches exactly** (`low/range 8666/256`, context `state=76`) and bins 0–16 match in `ctx`, value, state and `low/range`. Therefore:

* the CABAC arithmetic engine, context initialisation, flush and the MB0 header syntax are **not** the cause; and
* the divergence is in the **coefficient values** fed to `cabac_block_residual_internal` (`reference/x264/encoder/cabac.c:664-748`, `docs/X264_INTERNALS.md:1029-1041`), specifically in the *last significant coefficient position*.

### 2.1 Correction to the starting brief: the divergent block is I16 **luma DC**, not chroma

`tools/cabac_bin_diff.py:75` labels every context in `166..199` as *"significant_coeff_flag chroma/8x8"* — a coarse label, not the authoritative x264 mapping. The authoritative mapping is:

* `reference/x264/common/macroblock.h:275` — `DCT_LUMA_DC = 0` (`:278` is `DCT_CHROMA_DC = 3`).
* `reference/x264/common/tables.c:1780` — `x264_significant_coeff_flag_offset[0][DCT_LUMA_DC] = 105+0 = 105`.
* `reference/x264/common/tables.c:1785` — `x264_last_coeff_flag_offset[0][DCT_LUMA_DC] = 166+0 = 166`.
* `reference/x264/encoder/cabac.c:675-717` — `WRITE_SIGMAP(i, i)` for a 15-coefficient block (`x264_count_cat_m1[0] = 15`, `tables.c:1792`): significance flag context `105+i`, last-significant flag context `166+i`.
* `reference/x264/encoder/cabac.c:989` — the first residual block of an I16×16 MB is `cabac_block_residual_dc_cbf(..., DCT_LUMA_DC, LUMA_DC+0, h->dct.luma16x16_dc[0], 1)`.

Trace bins 10–19 decode as: sig ctx105/106/107/108/109 all `b=1`, last ctx166/167/168 all `b=0`, then **ctx169 (`= 166+3`) is the last-significant flag for coefficient index 3**: x264 `b=0` (more significant coefficients follow), Phoenix `b=1` (Phoenix thinks this is the last one). The differing data is `h->dct.luma16x16_dc[0]`, written at `encoder/macroblock.c:224` after `dct4x4dc` (`:215`) and `quant_4x4_dc` (`:219`).

This is consistent with `docs/PORTING_ROADMAP.md:109`: medium reaches 715/715 exact bins and diverges at bin 715 (MB1), i.e. x264's **trellis** DC path is correct in Phoenix; ultrafast is the one preset with `i_trellis = 0` (`reference/x264/common/base.c:510`), so it exercises the **deadzone** `quant_4x4_dc` path (`encoder/macroblock.c:219`, `common/quant.c:88-94`) that medium never uses. **If worker X1's in-flight patch only touches `src/phx_chroma.c`, it will not fix bin 17**: bin 17 lives in `src/phx_mbencode.c:178-256` / the deadzone quant tables in `src/phx_quant.c`. Chroma deadzone DC (`src/phx_chroma.c:301-307`, x264 `encoder/macroblock.c:425-426`, `common/quant.c:96-104`) is a separate, still-unproven path and must be checked immediately after bin 17 is fixed.

---

## 3. What is already byte-correct for ultrafast

Evidence-based ("byte-correct" = observed identical bytes or identical ordered bins/fields; not a claim of full parity):

| Stage / module | Evidence | x264 authority (`docs/X264_INTERNALS.md` + `reference/x264/`) |
|---|---|---|
| SPS NAL payload | `7 (SPS) len 21/21 EXACT` in both probes; `parameter sets: 2/2 byte-exact` | `phx_sps_init/write` ← `common/set.c:101`, `:305-477` (§6.6 `:1084-1088`) |
| PPS NAL payload | `8 (PPS) len 4/4 EXACT` in both probes | `phx_pps_init/write` ← `common/set.c:479-503`, `:505-572` (§6.6 `:1090-1093`) |
| Version SEI **payload** | `6 (SEI) len 542/542 EXACT` in both probes | `phx_sei_version_write` ← `common/set.c:593-623` (§6.6 `:1095-1101`) |
| Slice headers (all 8 slices, both entropies) | `HEADER MATCH (24 bits)` for IDR, `HEADER MATCH (19 bits)` CABAC / `(18 bits)` CAVLC for P slices, every field listed equal (`first_mb_in_slice`, `slice_type`, `pps_id`, `frame_num`, `idr_pic_id`, flags, `slice_qp_delta=-3`, `disable_deblocking_filter_idc=1`) | `phx_slice_header_init/write` ← `encoder/encoder.c:122-211`, `:213-364` (§6.7 `:1106-1124`); `b_deblocking_filter=0` from `common/base.c:500` correctly reflected |
| CABAC arithmetic engine, context init, flush | bins 0–16 identical in `ctx`, `b`, `state`, `low/range`; coder state MATCH at bin 17 | `phx_cabac.c` ← `common/cabac.c:32-183` (§6.2 `:972-1005`), flush frame-bit `:174` |
| CABAC MB0 header syntax (probe order) | bins 0–9 (mb_type I16×16, chroma pred mode, qp_delta, luma-DC CBF) match | `cabac_mb_type_intra` `encoder/cabac.c:35-66`, I-slice dispatch `:381-392`, `cabac_intra_chroma_pred_mode` `:98-116`, `cabac_qp_delta` `:150-186`, `cabac_block_residual_cbf_internal` `:892-909` |
| Trellis QNS path (medium evidence, shared code) | `docs/PORTING_ROADMAP.md:109` / `README.md:88`: MB0 715/715 CABAC bins exact; first divergence only at bin 715 (MB1 mode) | `phx_rdo.c:836` ← `encoder/rdo.c:642-895` (§5.4 `:883-913`); `quant_trellis_cabac` uses live CABAC states (invariant §9.5 `:1419-1421`) |
| Encoder robustness / NAL layout | `parity_report.json` all 100 `x264_rc=0`, `phx_rc=0`; all ultrafast cases 11 vs 11 NALs with the same type sequence `7,8,6,5,1×7` | `encoder_encoder_headers` `:2096-2134`, `x264_encoder_encode` stage 27-28 (§1.4 `:189-190`) |

Note the distinction: **payload**-level SEI correctness is real; **stream**-level SEI correctness is not (§1.3).

---

## 4. Phoenix functions missing or incorrect, with x264 citations

### 4.1 Incorrect — NAL start-code flags (first stream-level blocker)

| Phoenix site | Wrong behaviour | Required x264 behaviour |
|---|---|---|
| `src/phx_encoder.c:291` (`header_nal[2]`, SEI) | `b_long_startcode = 1` | 3-byte start code: `encoder/encoder.c:2082-2083` (long only for `!i`, `NAL_SPS`, `NAL_PPS`, AVC-Intra). Cited in `docs/X264_INTERNALS.md:255-259`. |
| `src/phx_sei.c:379` | `b_long_startcode = 1` ("Annex-B first NAL" — false; SPS is NAL 0) | same rule |
| `src/phx_encoder.c:1553` (every slice NAL) | `b_long_startcode = 1` | 3-byte start code for slice NALs unless they are the first NAL of the stream; `encoder/encoder.c:2082-2083`; `x264_nal_encode` at `common/bitstream.c:55-104` |

The writer itself (`src/phx_bitstream.c:125-140`, `nal_escape` `:112-122`) already mirrors `common/bitstream.c:29-40` and `:55-104` correctly (`docs/X264_INTERNALS.md:253-276`, `:1073-1080`). Only the flags must be fixed — **no parameter changes**.

### 4.2 Incorrect — I16 luma DC coefficient values (first CABAC blocker, bin 17)

| Phoenix site | Required x264 source | Citation |
|---|---|---|
| `src/phx_mbencode.c:178-256` `phx_mb_encode_i16x16`, DC extraction `:206-209`, Hadamard `:228`, deadzone quant `:229-231`, zigzag/store `:234-237` | `mb_encode_i16x16`: `encoder/macroblock.c:126-239`; DC extraction `:168-172`; `dct4x4dc` `:215`; **`quant_4x4_dc(..., mf>>1, bias<<1)` `:219`**; nnz `:221`; `zigzag_scan_4x4` + `h->dct.luma16x16_dc[p]` `:224` | `docs/X264_INTERNALS.md:838-855` (transforms), `:857-877` (deadzone `QUANT_ONE` `common/quant.c:50-57`; `quant_4x4_dc` `:88-94`; CQM bias `common/set.c:73-206`), `:1240-1257` (reconstruction) |
| `src/phx_quant.c` `phx_quant_4x4_dc` / `phx_quant_tables_init` | `common/quant.c:50-57`, `:88-94`; `common/set.c:73-205` (`quant4_mf`, `quant4_bias`, `deadzone[4] = {32-i_luma_deadzone[1], ...}` at `:81-83`) | `docs/X264_INTERNALS.md:857-877` |
| `src/phx_dct.c` `phx_dct4x4dc` / `phx_idct4x4dc` | `common/dct.c:47-76`, `:78-107` | `docs/X264_INTERNALS.md:839-842` |
| `src/phx_cabac_syntax.c:765-864` `phx_cabac_block_residual` | `encoder/cabac.c:664-748`, especially `WRITE_SIGMAP` `:675-717`; context offsets `common/tables.c:1778-1791`; I16 DC call site `:989` | `docs/X264_INTERNALS.md:1007-1048` |

Ultrafast forces the **deadzone** branch: `reference/x264/common/base.c:510` sets `i_trellis = 0` (also `:500` deblock off, `:503` `analyse.intra = 0`, `:507` `i_subpel_refine = 0`, `:508` AQ off, `:515` lookahead 0). Therefore the correct reference path is `encoder/macroblock.c:219`, **not** `encoder/rdo.c:642-895`. Do not "fix" this by enabling trellis or changing QP.

### 4.3 To be checked next — chroma deadzone DC/AC (worker X1's area)

| Phoenix site | Required x264 source | Citation |
|---|---|---|
| `src/phx_chroma.c:207-344` `phx_macroblock_encode_chroma`: chroma DC quant `:301-307`, AC quant `:281-296`, decimation `:309-331` | `mb_encode_chroma_internal` `encoder/macroblock.c:259-498`: 4:2:0 branch `:343-485`; AC `quant_4x4x4` `:402-403`; `dct2x2dc` `:379`; chroma DC `quant_2x2_dc(mf>>1, bias<<1)` `:425-426`; nnz `:429`; `mb_optimize_chroma_dc` `:245-257`, `:444`; zigzag/chroma_dc `:457/:477`; `cbp_chroma` `:487-489` | `docs/X264_INTERNALS.md:1253-1257` and `:944-955` (nnz/PCM), `:1422-1424` (chroma QP) |
| `src/phx_chroma.c:224-231` chroma QP derivation | `encoder.c:1585` `chroma_qp_table = i_chroma_qp_table + 12 + i_chroma_qp_index_offset`; table `common/macroblock.h:249-270`; `analyse.c:257`, `:291` | `docs/X264_INTERNALS.md:1222-1232` |
| `src/phx_quant.c` `phx_quant_2x2_dc` | `common/quant.c:96-104` | `docs/X264_INTERNALS.md:857-877` |

### 4.4 To be checked after CABAC — CAVLC path

| Phoenix site | Required x264 source | Citation |
|---|---|---|
| `src/phx_cavlc.c:951-1114`, `src/phx_cavlc_syntax.c:69-411` | `x264_macroblock_write_cavlc` `encoder/cavlc.c:487-595` (CBF `:550-553`, `transform_size_8x8_flag` `:554-556`, `mb_qp_delta` `:211-241`, I16 DC/AC `:558-572`, chroma DC/AC `:578-590`); `cavlc_block_residual_internal` `:121-196`; escape `:70-118`; `ct_index` / `x264_mb_predict_non_zero_code` `:198-208`, `common/macroblock.h:432` | `docs/X264_INTERNALS.md:1050-1071`, `:915-940` |
| skip-run state | `encoder.c:2810`, `:2883-2891`, `:3087-3088` | `docs/X264_INTERNALS.md:1342-1344` |

CAVLC's first payload divergence (byte 6 IDR / byte 3 P slices) is downstream of the same MB coefficient values; after 4.2 is fixed, re-probe CAVLC before touching token tables.

### 4.5 Documented-but-unported stages relevant to later slices

The per-MB divergence will migrate to MB1 and to P slices until these are aligned (fastest/fast/medium evidence: divergence at bin 715 = MB1 mode). `docs/X264_INTERNALS.md:1461-1476` (§10.3) and `docs/FUNCTION_PORTS.md` list them explicitly: all B-slice analysis, direct/spatial MV prediction, weighted prediction, 8×8 transform-size decisions (`:1471-1472`), `macroblock_store_pic` / reconstruction pixel write-back (WP2 note `docs/PORTING_ROADMAP.md:36-37`), subpel refinement and multi-reference ME (`docs/X264_INTERNALS.md:809-811`), row VBV RC, MB-tree, 2-pass. For **ultrafast** many of these are disabled (`base.c:496-516`) — ref=1, no B frames, no 8x8, subme=0, AQ off, MB-tree off, lookahead 0 — so the immediate scope is narrow: start-code flags + I16/chroma deadzone coefficients + P L0 16×16 reconstruction/MV.

---

## 5. Prioritized, source-aligned work list (no approximations, no parameter changes)

1. **P0 — Annex-B start codes.** In `src/phx_encoder.c:291`, `src/phx_sei.c:379`, `src/phx_encoder.c:1553`, set `b_long_startcode` exactly as `encoder/encoder.c:2082-2083`: true only for NAL 0 / SPS / PPS / AVC-Intra. Golden test: assert `ref[33:37] == phx[33:37] == 00 00 01 06` and `first_diff` moves past byte 35. This is a pure syntax fix — no encoder tuning.
2. **P0 — I16 luma DC deadzone coefficients (bin 17).** Bring `src/phx_mbencode.c:178-256` byte-for-byte in line with `encoder/macroblock.c:126-239`, especially `dct4x4dc` (`:215` ← `common/dct.c:47-76`), `quant_4x4_dc(mf>>1, bias<<1)` (`:219` ← `common/quant.c:88-94`, bias from `common/set.c:73-205`) and `zigzag_scan_4x4` + `h->dct.luma16x16_dc[0]` (`:224` ← `common/dct.c:843-853`). Gate: `python tools/cabac_bin_diff.py --preset ultrafast --fixture build/parity/fixture.yuv` reports **no divergence before bin 715**, with the same `ctx/b/state/low/range` trace format.
3. **P1 — Chroma deadzone DC/AC (X1's in-flight fix).** Align `src/phx_chroma.c:207-344` with `encoder/macroblock.c:259-498` (4:2:0 branch) and chroma QP with `encoder.c:1585` / `common/macroblock.h:249-270`. Verify by extending the bin trace through the chroma DC blocks at `encoder/cabac.c:1069-1070` and AC at `:1073-1079`; require identical coeff arrays, not merely identical bin count.
4. **P1 — Re-probe CAVLC after 2–3.** `python tools/first_frame_probe.py --preset ultrafast --entropy cavlc`; require the IDR payload byte-6 diff and all P-slice byte-3 diffs to disappear via the shared coefficient fix, then align `src/phx_cavlc.c` / `src/phx_cavlc_syntax.c` with `encoder/cavlc.c:121-196`, `:487-595` only where a real diff remains.
5. **P2 — MB1+ analysis/reconstruction and P-slice syntax.** Work the next divergence reported by `cabac_bin_diff.py` (medium currently stops at bin 715, MB1 mode) against `encoder/analyse.c:2934-2952` (I slice), `encoder/macroblock.c:126-239`, and `docs/X264_INTERNALS.md:809-811` (ported ME scope). No divergence may be "fixed" by adjusting flags, QP, bitrate or fixtures.
6. **P2 — Full 100-case gate.** After each fix: `python tools/parity_harness.py` → `python tools/compare_streams.py` → `python tools/first_frame_probe.py --preset ultrafast --entropy {cabac,cavlc}` → `python tools/cabac_bin_diff.py --preset ultrafast ...` (loop from `docs/PARITY_ITERATION.md:8-60`). Only `byte_exact == true` for all 100 cases ends the loop (`docs/PARITY_ITERATION.md:100-108`).
7. **Never accepted as a fix:** changing x264 flags, fixtures or the reference corpus; matching bitrate, NAL counts, decoded picture, SPS/PPS, slice headers, or "715/715 bins"; tuning parameters to move a first-diff offset. `docs/PARITY_ITERATION.md:37-40`, `docs/VIDEO_PARITY.md:9-11`.

---

## 6. Explicit parity statement

**0/100 is not parity.** `parity_report.json` `totals` = 100 cases, 100 compared, **0 byte-exact**, 100 mismatches; `by_preset.ultrafast` = 10 cases, **0 byte-exact**. `docs/PORTING_ROADMAP.md:123-127` and `docs/PARITY_ITERATION.md:100-108` define parity strictly as byte identity of the entire `.h264` file for every case. The facts in §1–§2 (byte-exact parameter sets, matching slice headers, 17 identical CABAC bins with identical coder state, 100/100 successful encodes) are necessary diagnostics, **not** parity claims. Until the start-code flags and the I16 luma DC coefficient mismatch (and then the downstream MB/CAVLC differences) are fixed and the 100-case harness reports `byte_exact: true` everywhere, ultrafast remains a divergence.
