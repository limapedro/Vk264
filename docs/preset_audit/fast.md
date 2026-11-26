# Preset audit — `fast` (branch `vk264-phoenix`)

Read-only audit. No source, CMake, tool, test, branch or commit was touched. The
only file written is this report. All analysis commands are reproducible; the
CABAC bin diff was re-run into an isolated temp directory
(`C:\Users\multi\AppData\Local\Temp\opencode\fast_audit_bins`) because the default
shared temp dir is concurrently overwritten by other workers (the first run's
logs had been replaced; its counts did not match its own printed report). The
isolated re-run reproduced the original tool output exactly.

## Commands run (read-only)

```
python tools/first_frame_probe.py --preset fast --entropy cabac
python tools/first_frame_probe.py --preset fast --entropy cavlc
python tools/cabac_bin_diff.py --preset fast --fixture build/parity/fixture.yuv
python tools/cabac_bin_diff.py --preset fast --fixture build/parity/fixture.yuv --tmp <isolated>
```

`build/parity/parity_report.json`, `docs/X264_INTERNALS.md`,
`docs/FUNCTION_INVENTORY.md`, `docs/FUNCTION_PORTS.md` and the cited
`reference/x264/` sources were read.

---

## 0. Verdict

**`fast` is not byte-exact for either entropy. 0 of 10 `fast` cases pass;
0 of 100 cases in the whole parity report pass.**

Excerpt — `build/parity/parity_report.json`:

```
TOTALS: {'cases': 100, 'x264_ok': 100, 'x264_failures': 0,
         'compared': 100, 'byte_exact': 0, 'mismatches': 100,
         'first_diff_min': 35}
by_preset['fast'] = {'byte_exact': 0, 'cases': 10, 'compared': 10,
                     'first_diff_min': 39, 'mismatches': 10,
                     'x264_failures': 0, 'x264_ok': 10}
```

The ten `fast` cases are 5 fixtures × 2 entropies, all `byte_exact: False`,
all `first_diff: 39`, all `phx_rc = 0` / `x264_rc = 0` (both encoders complete;
this is a wrong-bytes failure, not a crash):

```
synth-fast-cabac            ref=4204  phx=7204
synth-fast-cavlc            ref=4975  phx=8582
ssstwitter.com_1748999992229-fast-cabac     ref=5157  phx=12854
ssstwitter.com_1748999992229-fast-cavlc     ref=5556  phx=13296
ssstwitter.com_1732159163230-fast-cabac     ref=1879  phx=5088
ssstwitter.com_1732159163230-fast-cavlc     ref=1883  phx=5211
ssstwitter.com_1732158697723-fast-cabac     ref=7034  phx=18152
ssstwitter.com_1732158697723-fast-cavlc     ref=7511  phx=18775
ssstwitter.com_1732159939975-fast-cabac     ref=5660  phx=15302
ssstwitter.com_1732159939975-fast-cavlc     ref=5906  phx=15548
```

### 0/100 is not parity

**0/100 byte-exact is the complete absence of parity, not a near miss and not a
score.** Every single compared case is a mismatch; there is no fixture, preset
or entropy point where Phoenix currently reproduces x264 byte-for-byte. A
parity claim requires at least one byte-identical stream (and then the whole
matrix); zero passes cannot be described as "partial parity", "close" or
"parity with known differences". The `0/100` row is the gate: until it is
non-zero, every downstream "port is byte-correct" statement is only a
per-function observation, not an encoder-level claim. The 10/10 `x264_ok`
count only proves the oracle runs; it must not be read as a Phoenix result.

---

## 1. NAL-level diff facts — both entropies

The probe pairs NALs by type sequence and compares **payload bytes only**
(start codes and trailing zeros excluded). Therefore "parameter sets:
2/2 byte-exact" does **not** mean the Annex-B streams are byte-identical.

Shared facts (both `--entropy cabac` and `--entropy cavlc`):

```
NALs: ref=11, phx=11
     0    0  7 (SPS)       23   23  EXACT
     1    1  8 (PPS)        6    6  EXACT
     2    2  6 (SEI)      624  624  EXACT
parameter sets: 2/2 byte-exact (SPS, PPS)
VERDICT: DIVERGENCE (first stream byte diff 39)
```

### 1.1 CABAC (`synth-fast-cabac`)

```
stream bytes: ref=4204, phx=7204; NALs: ref=11, phx=11
 3    3  5 (IDR slice)   1120  1166  DIFF@158
 4    4  1 (non-IDR)      379   128  DIFF@2
 5    5  1 (non-IDR)      384  1050  DIFF@1
 6    6  1 (non-IDR)      368  1006  DIFF@0
 7    7  1 (non-IDR)      408  1030  DIFF@0
 8    8  1 (non-IDR)      250    36  DIFF@0
 9    9  1 (non-IDR)      248  1051  DIFF@0
10   10  1 (non-IDR)      352  1040  DIFF@0
```

IDR slice header is **byte-correct**:

```
slice #1: HEADER MATCH (30 bits), ENTROPY/MB MISMATCH at post-header byte 153
          (RBSP byte 157, NAL payload byte 158)
  ref: 0098: 75 b2 05 b9 19 29 97 de   <== first diff
  phx: 0098: 75 b2 05 b9 19 2e 14 dd   <== first diff
```

Both IDR headers decode identically: `slice_type=I`, `qp_delta=-3`,
`slice_qp=25`, all loop/deblock fields equal.

### 1.2 CAVLC (`synth-fast-cavlc`)

```
stream bytes: ref=4975, phx=8582; NALs: ref=11, phx=11
 3    3  5 (IDR slice)   1448  1533  DIFF@4
 4    4  1 (non-IDR)      419   130  DIFF@2
 5    5  1 (non-IDR)      473  1254  DIFF@1
 6    6  1 (non-IDR)      502  1201  DIFF@0
 7    7  1 (non-IDR)      450  1226  DIFF@0
 8    8  1 (non-IDR)      310    35  DIFF@0
 9    9  1 (non-IDR)      290  1271  DIFF@0
10   10  1 (non-IDR)      388  1235  DIFF@0
```

```
slice #1: HEADER MATCH (30 bits), ENTROPY/MB MISMATCH at post-header byte 1
          (RBSP byte 5, NAL payload byte 6)
  ref: 0000: 88 84 00 ff 89 0f 14 00   <== first diff
  phx: 0000: 88 84 00 fe 89 38 89 4a   <== first diff
```

### 1.3 The first stream byte difference at offset 39 is a start-code bug, not header content

Raw bytes of `synth-fast-cabac.{ref,phx}.h264`:

```
ref  0:48: 00 00 00 01 67 ... 00 00 00 01 68 ... 00 00 01 06 05 ff ff
phx  0:48: 00 00 00 01 67 ... 00 00 00 01 68 ... 00 00 00 01 06 05 ff
```

x264 writes a **3-byte** start code before the SEI; Phoenix writes 4 bytes.
The same difference repeats before the IDR slice:

```
ref  IDR boundary: 3d 30 00 80 00 00 01 65 88 84 00 ff
phx  IDR boundary: 30 00 80 00 00 00 01 65 88 84 00 ff
```

P/B slice start codes are 4-byte in both streams, so only the SEI and the
first IDR slice are affected. Every subsequent absolute byte offset is shifted
by the inserted byte(s). The payloads remain byte-equal, which is why the
probe still reports SPS/PPS/SEI as EXACT.

### 1.4 P/B slice headers are wrong from slice #2 onward

Both entropies show the same class of failures (probe excerpts):

```
slice #2: HEADER MISMATCH (1 differing field(s)):
  pic_order_cnt_lsb   ref=2   phx=8
  => fix the parameter-set / slice-header writer, not the entropy stage

slice #3: HEADER MISMATCH (17-18 differing field(s)):
  slice_type          ref=5(P)  phx=6(B)
  num_ref_idx_active_override_flag  ref=0  phx=1
  luma/chroma_log2_weight_denom     ref=0  phx=1
  adaptive_ref_pic_marking_mode_flag ref=0 phx=1
  slice_qp_delta      ref=-3     phx=29
  cabac_init_idc      ref=6      phx=1  (CABAC run)

slice #5: HEADER MISMATCH:
  frame_num           ref=4  phx=3
  memory_management_control_operation[0] ref=1 phx=30

slice #6: HEADER MISMATCH (24-25 differing field(s)):
  slice_type          ref=6(B)  phx=5(P)
  direct_spatial_mv_pred_flag ref=1 phx=0
  pic_order_cnt_lsb   ref=6  phx=14
  cabac_init_idc      ref=0  phx=3
```

This is a frame-type/POC/reference-list/QP problem upstream of entropy, not a
bit-writer problem: the IDR header (same writer, same code path) is exact.

---

## 2. First divergent CABAC bin, element and coder state

Excerpt — `python tools/cabac_bin_diff.py --preset fast --fixture build/parity/fixture.yuv`:

```
x264 golden bins: 37078; Phoenix bins: 40857
FIRST DIVERGENT BIN: index 1663 (0-based, bin #1664)
  bin 1662  x264: ctx=-2   b=0  byp      phx: ctx=-2   b=0  byp
  bin 1663  x264: ctx=-2   b=0  byp      phx: ctx=-2   b=1  byp   <== FIRST DIVERGENCE
  x264 element: ue_bypass group
  phx  element: ue_bypass group
  x264 record: dec ctx=69   b=1 state=115 low/range 14208/490 -> 14208/315
  phx  record: dec ctx=271  b=0 state=78  low/range 1984/256 -> 3968/430
  coder state before bin: DIVERGED (x264 low/range 14208/490, phx 1984/256)

REMAINING DIVERGENCE (after removing inserted Phoenix bin(s)):
  0 inserted/deleted bin(s) removed; tail agrees for 1663 bins;
  next mismatch at aligned index 1663: x264=('byp', -2, 0), phx=('byp', -2, 1)
```

### 2.1 Corrected interpretation (raw logs, isolated run)

The tool's two `record:` lines and its `coder state before bin: DIVERGED` line
are **indexing artifacts**: `compare()` finds `first` in the *expanded* bin list
but then indexes the *raw record* list with it
(`tools/cabac_bin_diff.py:444-457`). A `ue` record expands to many bins, so
record 1663 is not the record containing expanded bin 1663. Reading the raw
logs directly gives the actual first divergence:

```
rec 1622 X dec ctx=263 b=1 state=29 low/range 896/349 -> 896/335
rec 1622 P dec ctx=263 b=1 state=29 low/range 896/349 -> 896/335
rec 1623 X ue ctx=-2 exp=0 val=7  n=7 low/range 896/335 -> 152208/335
rec 1623 P ue ctx=-2 exp=0 val=14 n=7 low/range 896/335 -> 154553/335
rec 1624 X byp ctx=-1 b=1 low/range 152208/335 -> 288/335
rec 1624 P byp ctx=-1 b=1 low/range 154553/335 -> 882/335
rec 1625 X dec ctx=100 b=0 state=59 low/range 288/335 -> 9488/480
rec 1625 P dec ctx=100 b=1 state=59 low/range 882/335 -> 882/305
```

Facts:

* **First divergent bin:** expanded index **1663** (0-based), a **bypass bin
  inside a `ue_bypass` group** (`ctx = -2`); x264 emits `b=0`, Phoenix `b=1`.
* **Element:** `coeff_abs_level_minus1` escape for `abs_coeff >= 15`, written by
  `x264_cabac_encode_ue_bypass(cb, 0, abs_coeff - 15)` at
  `reference/x264/encoder/cabac.c:727-736`, implemented at
  `reference/x264/common/cabac.c:142-157`. The raw record is
  `ue(exp_bits=0, val=7, nbins=7)` in x264 vs `val=14` in Phoenix — i.e. the
  coefficient magnitude reaching the escape is **22 in x264, 29 in Phoenix**.
* **Coder state matches at the first divergent bin.** Both coders have
  `low/range = 896/335` before the `ue` group and identical context states
  (ctx 263, states 41→39→37→35→33→31→29, seven identical `b=1` bins) for the
  seven preceding bins. The first `low/range` divergence is at expanded bin
  1666 / raw record 1624 (`152208/335` vs `154553/335`) and is a *consequence*
  of encoding a different `val`, not a cause. The arithmetic engine
  (renormalisation, `putbyte`, bypass queue) is not the fault.
* **No insertion/deletion:** "0 inserted/deleted bin(s) removed; tail agrees
  for 1663 bins". Streams are aligned up to the divergence; Phoenix simply
  emits a different coefficient value (and therefore 40857 bins overall vs
  x264's 37078, +10%).
* **Location:** raw record 693 is MB1's first `mb_type` bin and record 1655 is
  MB2's; record 1623 therefore sits at the **end of MB1's residual**, not MB0.
  MB0 is byte-correct.

### 2.2 Coefficient handoff

```
COEFFICIENT HANDOFF (MB0 luma 4x4 arrays):
  blocks compared: 16; raw arrays equal: 16/16; equal after zigzag rescan: 0/16
```

Raw MB0 luma arrays are identical (16/16), including blocks 14 and 15 with all
16 coefficients nonzero. The "0/16 after zigzag rescan" line is a **harness
assumption artifact**: `tools/cabac_bin_diff.py:517-523` assumes Phoenix's
arrays are in natural order and x264's are zigzag-scanned. Direct comparison
shows Phoenix already feeds the same zigzag-ordered arrays as x264's
`h->dct.luma4x4` after `reference/x264/encoder/macroblock.c:391` and `:410`
(the trellis path writes `dct[zigzag[i]]` at
`reference/x264/encoder/rdo.c:890`). No missing zigzag rescan is indicated by
this data; the divergence is a coefficient **value** in MB1.

---

## 3. x264 functions already byte-correct in the `fast` path

"Byte-correct" here means the observable bytes/bins produced by the Phoenix
port match the x264 oracle in this case; it is not a claim about other presets
or unexercised branches.

| x264 reference | Phoenix port | Evidence |
|---|---|---|
| `x264_sps_init` `encoder/set.c:101`, `x264_sps_write` `encoder/set.c:305` | `phx_sps_init`, `phx_sps_write` (`src/phx_headers.c:13`, `:98`) | SPS 23/23 EXACT in both entropies |
| `x264_pps_init` `encoder/set.c:479`, `x264_pps_write` `encoder/set.c:505` | `phx_pps_init`, `phx_pps_write` (`src/phx_headers.c:76`, `:165`) | PPS 6/6 EXACT in both entropies |
| `x264_sei_version_write` `encoder/set.c:593` (payload) | `phx_sei_version_write` (`src/phx_sei.c:340`) | SEI payload 624/624 EXACT (start code is not) |
| `param_apply_preset` `common/base.c:489`, `fast` delta `common/base.c:546-551` | `phx_param_apply_preset` `src/phx_param.c:91-93` | Field-for-field identical: `ref=2`, `subme=6`, `weightp=SIMPLE`, `lookahead=30` |
| `x264_param_default` `common/base.c:344` + chroma-QP psy rule `encoder/encoder.c:1218-1231` | `phx_param_default`, `phx_param_finalize` (`src/phx_param.c:25`, `:129`) | IDR `slice_qp_delta=-3`, derived `slice_qp=25` match |
| `slice_header_write` `encoder/encoder.c:213` — IDR branch | `phx_slice_header_write` (`src/phx_slice.c:107`) | 30-bit IDR header MATCH, all 14 fields equal |
| `x264_cabac_context_init` `common/cabac.c:32`, `x264_cabac_encode_init` `:61` | `src/phx_cabac.c:54-68`, `:…` | Bins 0..1662 match with identical context states |
| `x264_cabac_encode_decision_c` `common/cabac.c:113`, `..._bypass_c` `:128`, `cabac_encode_renorm` `:101`, `cabac_putbyte` `:69` | `phx_cabac_encode_decision/bypass/renorm/putbyte` (`src/phx_cabac.c:113-159`) | 1663 aligned bins and identical `low/range` up to the value divergence |
| `x264_cabac_encode_ue_bypass` `common/cabac.c:142-157` (algorithm) | `phx_cabac_encode_ue_bypass` (`src/phx_cabac.c:168-191`) | Given the same `val`, the 7 bypass bins are produced correctly; the *input* `val` is wrong |
| `x264_quant_4x4_trellis` `encoder/rdo.c:1142`, `dct[zigzag[i]]` `:890`, `zigzagf.scan_4x4` `encoder/macroblock.c:391/410` | `phx_rdo.c` trellis + `phx_dct` scans | MB0 16/16 luma coefficient arrays identical |
| `x264_nal_encode` `common/bitstream.c:55`, `nal_escape` `:29` | `phx_nal_encode` (`src/phx_bitstream.c:125`), `phx_nal_escape` (`:112`) | All payload bytes and emulation-prevention bytes match; only caller-selected start-code length differs |

---

## 4. Missing / incorrect Phoenix functions (with x264 citations)

### 4.1 Proven byte-level defects

| # | Defect | Phoenix | x264 reference |
|---|---|---|---|
| A | SEI NAL emitted with a 4-byte Annex-B start code | `src/phx_sei.c:379` (`nal.b_long_startcode = 1`) — contradicts the correct rule already present at `src/phx_encoder.c:291-293` | `reference/x264/encoder/encoder.c:2082-2083`: long start code only for `!i \|\| NAL_SPS \|\| NAL_PPS \|\| avcintra` |
| B | IDR slice emitted with a 4-byte start code (x264: 3-byte because the slice is not the first NAL of the keyframe batch) | `src/phx_encoder.c:1553` (`nal.b_long_startcode = 1` unconditionally for every slice) | `reference/x264/encoder/encoder.c:2082-2083` |
| C | `coeff_abs_level_minus1` escape value wrong in MB1 (`val=14` vs `val=7`, i.e. |coef| 29 vs 22) | producer feeding `src/phx_cabac_syntax.c:851` (`phx_cabac_encode_ue_bypass(cb, 0, abs_coeff - 15)`) is wrong; the encoder function itself at `src/phx_cabac.c:168` is correct | `reference/x264/encoder/cabac.c:727-736` and `reference/x264/common/cabac.c:142-157`; coefficient values originate from `reference/x264/encoder/rdo.c:890` → `reference/x264/encoder/macroblock.c:388-392` |
| D | P/B slice header fields wrong (slice_type, POC, ref counts/reordering, MMCO, `cabac_init_idc`, `slice_qp_delta`) | `src/phx_slice.c:14` (`phx_slice_header_init`), `:107` (`phx_slice_header_write`), driven by `src/phx_encoder.c:491-785` | `reference/x264/encoder/encoder.c:213-364` (`slice_header_write`), `:122-211` (`slice_header_init`), `:2608-2675` (`slice_init`), `:2295` (`reference_build_list`), `:2413`, `:2533`, `:3512` (POC) |

### 4.2 Missing x264 stages that necessarily prevent parity at `fast`

The inventory (`docs/FUNCTION_INVENTORY.md`) marks these MISSING; `fast`
enables `bframes=3`, `b-adapt=FAST`, `weightp=SIMPLE`, `subme=6`, `trellis=1`,
`lookahead=30`, so all of them are live in this preset:

| x264 function | source | Consequence for `fast` |
|---|---|---|
| `x264_slicetype_decide` | `encoder/slicetype.c:1745` (inventory line 1361: **MISSING**) | frame-type order and reordering diverge → P/B `slice_type` mismatches in probe slices #3, #6, #8 |
| B-adapt FAST / `slicetype_path_cost` | `encoder/slicetype.c:1580-1621`, `:1288` | `phx_lookahead.c:194`, `:390` is only a FAST-equivalent subset; no trellis B-adapt |
| `x264_me_refine_qpel` | `encoder/me.c:801` (inventory line 1191: **MISSING**) | `subme=6` in `fast` requires qpel refinement; predicted/inter coefficients differ → MB1 coefficient magnitude divergence |
| `x264_me_refine_qpel_rd` | `encoder/me.c:1233` (inventory line 1199: **MISSING**) | RD-based partition selection absent at `subme>=6` |
| `x264_macroblock_tree_read` / `macroblock_tree_finish` / `macroblock_tree_propagate` | `encoder/ratecontrol.c:541`, `encoder/slicetype.c:1029`, `:1051` (inventory: **MISSING**) | MB-tree is enabled at `fast`; AQ/QP offsets differ → `slice_qp_delta` mismatches |
| weighted-P analysis | `encoder/slicetype.c:1937-1943`, `encoder/encoder.c:3876-3879`, `encoder/macroblock.c` weight tables | `weightp=SIMPLE` is set by `src/phx_param.c:93` but not fully applied; probe shows `log2_weight_denom` ref=0 vs phx=1/2 |
| VBV/row rate control, 2-pass | `encoder/ratecontrol.c:1432`, `:1506-1520`, `:1829` | QP source diverges (`slice_qp` ref=28/29/30 vs phx=0/1) |

`docs/X264_INTERNALS.md:59-77` and `docs/FUNCTION_PORTS.md` document the same
coverage gaps (inter analysis is "only P L0 16x16 trial; no sub-8x8, no B
modes"; lookahead "MB-tree, trellis B-adapt, VBV lookahead, weighted prediction
not ported").

---

## 5. Prioritized, source-aligned work list to reach byte-exact

**P0 — cheap byte-level wins (do first; they mask every later diff)**

1. Fix SEI start code: in `src/phx_sei.c:379` set `b_long_startcode = 0`, or
   route SEI through `phx_encoder.c:291-293` and the x264
   `encoder_encapsulate_nals` rule `encoder.c:2082-2083`. This removes the
   `first_diff = 39` in all ten `fast` cases (both entropies).
2. Fix first-IDR slice start code: replace the unconditional
   `src/phx_encoder.c:1553` assignment with x264's `!i || SPS || PPS` rule, and
   reproduce the keyframe NAL batching (`encoder.c:3662-3703`, `:2080-2085`) so
   the IDR slice is NAL index > 0 and gets a 3-byte start code.

**P0 — first entropy divergence**

3. Isolate the MB1 coefficient whose `abs_coeff` is 29 in Phoenix and 22 in
   x264 (raw record 1623 of `L1_BINS_X264.txt`; `encoder/cabac.c:736`). Dump
   the block index and `dct[]` array at that point in both encoders (the MB0
   dump at `tools/cabac_bin_diff.py:241-263` is the template). Check the
   trellis → `zigzagf.scan_4x4` → `cabac_block_residual` handoff at
   `rdo.c:890` / `macroblock.c:388-392` and the QP/dequant tables feeding it.
   Acceptance: first divergent bin index moves past 1663, then past MB1.
4. Re-run `cabac_bin_diff.py` after each step; the gate is
   `VERDICT: first MB0 bin streams are IDENTICAL` (currently 37078 vs 40857
   bins). Then re-run `first_frame_probe.py` for both entropies.

**P0 — slice headers (independent of entropy choice)**

5. Bring the P/B slice header to x264 `encoder.c:213-364` semantics: `slice_type`
   from `x264_slicetype_decide` (`:1745`), `pic_order_cnt_lsb` from `:3512`,
   ref counts/override from `:2639-2645`, ref-pic-list modification from
   `:263-289`, MMCO from `:2533`/`:337-350`, `cabac_init_idc` from `:196`,
   `slice_qp_delta` from `:2787-2789`. Acceptance: probe reports HEADER MATCH
   for slices #2..#8 in both entropies.

**P1 — decision stages that feed the above (largest effort)**

6. Port `x264_me_refine_qpel` (`encoder/me.c:801`) and qpel-RD (`:1233`) needed
   by `fast`'s `subme=6`; without them inter residuals cannot match.
7. Complete FAST B-adapt (`encoder/slicetype.c:1580-1621`) and
   `x264_slicetype_decide` (`:1745`) including BREF/keyint rules
   (`:1799-1889`) and reorder (`:1950-1958`).
8. Add weighted prediction (`:1937-1943`, `encoder.c:3876-3879`) and MB-tree
   (`ratecontrol.c:541`, `slicetype.c:1029/1051/1675-1678`).
9. Align rate-control QP (`ratecontrol.c:1432`, `:1506-1520`) with MBRC/AQ state
   so `slice_qp_delta` matches; only then expect CABAC/CAVLC residual streams to
   converge.

**Exit criterion:** `build/parity/parity_report.json` must show
`by_preset['fast'].byte_exact == 10` (and `totals.byte_exact > 0`), with
`first_frame_probe.py` printing `VERDICT: BYTE-EXACT` for both entropies.

---

## 6. Reproducibility notes

* All cited `reference/x264/...:N` locations were checked against this
  checkout on `vk264-phoenix`.
* The CABAC harness builds instrumented copies of
  `reference/x264/encoder/cabac.c` and `common/cabac.c` and a
  `-DPHX_CABAC_BIN_TRACE` Phoenix CLI under the temp directory only; no
  repository file is modified.
* The default shared temp dir for `cabac_bin_diff.py` is used by concurrent
  workers and can be overwritten between the tool run and a subsequent log
  read. Use `--tmp <private-dir>` for any further audit; the results in §2 were
  verified with an isolated directory and match the original tool output.

> **Post-audit note (orchestrator):** the start-code defect listed as item 1
> was fixed in commit `becb95b`. All 100 cases now first diverge at stream
> bytes 587-671, not byte 39. NAL-payload and CABAC bin findings remain valid.

