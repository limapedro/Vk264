# Medium preset — byte-exact parity audit

Branch `vk264-phoenix`. Read-only audit; no source, CMake, tool, test or branch
changes were made. Sole owned output: this file.

## Method and caveats

Commands run (all read-only except writing to a private temp dir for the bin
harness):

```
python tools/first_frame_probe.py --preset medium --entropy cabac
python tools/first_frame_probe.py --preset medium --entropy cavlc
python tools/cabac_bin_diff.py --preset medium --fixture build/parity/fixture.yuv
python tools/cabac_bin_diff.py --preset medium --fixture build/parity/fixture.yuv \
    --tmp C:\Users\multi\AppData\Local\Temp\opencode\medium_audit_cabac
python -c "<read build/parity/parity_report.json; parse the two trace logs>"
```

Two reproducibility notes, both caused by concurrent worker activity in the
shared workspace, not by this audit:

1. The default temp dir used by `cabac_bin_diff.py`
   (`C:\Users\multi\AppData\Local\Temp\opencode\l1_cabac_bin_diff`) was rewritten
   by another run between my tool call and my log analysis; those logs showed a
   bin-5 divergence. I therefore re-ran the harness with a private `--tmp`
   (`medium_audit_cabac`). The private run reproduces bin 1663 exactly as in the
   starting evidence. **All bin-level numbers below come from the private,
   stable snapshot.** Do not quote the shared-dir logs.
2. `build/parity/parity_report.json` and the probe's whole-stream `first_diff`
   are stale relative to the live `.phx.h264` files (report says `phx_bytes=7210,
   first_diff=39`; current file is 7209 bytes and first raw stream difference is
   at byte 666 — a 3-byte vs 4-byte Annex-B start code before the IDR slice).
   The **IDR NAL payload first diff is byte 158 in both snapshots**, which is the
   number that matters for this audit.

The `cabac_bin_diff.py` report line
`coder state before bin: DIVERGED (x264 low/range 14208/490, phx 1984/256)` and
its `x264 record: ... intra4x4_pred_mode` / `phx record: ... chroma DC` lines are
a **reporting artifact**: `compare()` computes `first` on the *expanded bin*
stream but then prints `xbins[first]` / `pbins[first]` as if it were a record
index (`tools/cabac_bin_diff.py:434-457`). The correct record containing expanded
bin 1663 is record 1623 in both traces; its state is identical (below).

---

## 1. NAL-level diff facts for both entropies

`first_frame_probe.py` (both entropies, `synth-medium-*` case):

```
stream bytes: ref=4069, phx=7210; NALs: ref=11, phx=11        (CABAC, snapshot 1)
     0    0  7 (SPS)                7 (SPS)                     23      23  EXACT
     1    1  8 (PPS)                8 (PPS)                      6       6  EXACT
     2    2  6 (SEI)                6 (SEI)                    624     624  EXACT
     3    3  5 (IDR slice)          5 (IDR slice)             1120    1166  DIFF@158
     4    4  1 (non-IDR slice)      1 (non-IDR slice)          379     128  DIFF@2
  ...
  parameter sets: 2/2 byte-exact (SPS, PPS)
```

```
stream bytes: ref=4839, phx=8585; NALs: ref=11, phx=11        (CAVLC)
     0    0  7 (SPS)                7 (SPS)                     23      23  EXACT
     1    1  8 (PPS)                8 (PPS)                      6       6  EXACT
     2    2  6 (SEI)                6 (SEI)                    624     624  EXACT
     3    3  5 (IDR slice)          5 (IDR slice)             1448    1533  DIFF@4
     4    4  1 (non-IDR slice)      1 (non-IDR slice)          419     130  DIFF@2
  ...
  parameter sets: 2/2 byte-exact (SPS, PPS)
```

IDR slice (CABAC), header vs entropy:

```
slice #1 — reference NAL 3 / Phoenix NAL 3
  ref  slice header (NAL 3, type 5, nal_ref_idc 3): 14 fields, last field ends at bit 30
  phx  slice header (NAL 3, type 5, nal_ref_idc 3): 14 fields, last field ends at bit 30
    slice_qp_delta = -3, slice_qp (derived) = 25   (both sides)
  slice #1: HEADER MATCH (30 bits), ENTROPY/MB MISMATCH at post-header byte 153
            (RBSP byte 157, NAL payload byte 158)
    ref  ... 0098: ea 75 b2 05 b9 19 29 97 de
    phx  ... 0098: ea 75 b2 05 b9 19 2e 14 dd
```

CAVLC IDR:

```
slice #1: HEADER MATCH (30 bits), ENTROPY/MB MISMATCH at post-header byte 1
          (RBSP byte 5, NAL payload byte 6)
    ref  0000: 88 84 00 ff 89 0f 14 00
    phx  0000: 88 84 00 fe 89 38 89 4a
```

Later slices are *not* byte-identical at the header level and are a separate,
later wave of work:

```
slice #2: HEADER MISMATCH (1 differing field(s)):
    pic_order_cnt_lsb   ref=2   phx=8
    => fix the parameter-set / slice-header writer, not the entropy stage
slice #3: HEADER MISMATCH (29 differing field(s)): slice_type P vs B,
    num_ref_idx, ref_pic_list_modification, pred_weight_table, MMCO, ...
```

Every medium case in `parity_report.json` has `first_diff: 39` (stale) and
`byte_exact: false`; the live streams show the first raw stream difference at a
start-code length difference before the IDR NAL (byte 666). Neither is parity.

---

## 2. First divergent CABAC bin, element, context, coder state

Private harness output:

```
x264 golden bins: 35705; Phoenix bins: 40857
FIRST DIVERGENT BIN: index 1663 (0-based, bin #1664)
  bin 1662  x264: ctx=-2   b=0  byp      phx: ctx=-2   b=0  byp
  bin 1663  x264: ctx=-2   b=0  byp      phx: ctx=-2   b=1  byp   <== FIRST DIVERGENCE
  bin 1664  x264: ctx=-2   b=0  byp      phx: ctx=-2   b=1  byp
  bin 1665  x264: ctx=-2   b=0  byp      phx: ctx=-2   b=1  byp
  bin 1666  x264: ctx=-1   b=1  byp      phx: ctx=-1   b=1  byp
  x264 element: ue_bypass group
  phx  element: ue_bypass group
REMAINING DIVERGENCE (after removing inserted Phoenix bin(s)):
  0 inserted/deleted bin(s) removed; tail agrees for 1663 bins;
  next mismatch at aligned index 1663: x264=('byp', -2, 0), phx=('byp', -2, 1)
COEFFICIENT HANDOFF (MB0 luma 4x4 arrays):
  blocks compared: 16; raw arrays equal: 16/16; equal after zigzag rescan: 0/16
```

Parsing the actual records that contain bin 1663 (both traces: record 1623):

```
X264  rec 1623: ue ctx=-2 exp=0 val=7 n=7 low/range 896/335 -> 152208/335
PHX   rec 1623: ue ctx=-2 exp=0 val=14 n=7 low/range 896/335 -> 154553/335
```

Records 1605–1622 (the significant-coefficient / last-coefficient / level-gt1
decision bins that precede it) are **byte-identical in ctx, bin value, state and
low/range** in both traces, e.g.:

```
 rec 1621 dec ctx=263 b=1 state=31  low/range 896/363 -> 896/349   (both)
 rec 1622 dec ctx=263 b=1 state=29  low/range 896/349 -> 896/335   (both)
```

Conclusions:

* **Element:** a `ue_bypass` group used for `coeff_abs_level_minus1 > 14` —
  x264 `encoder/cabac.c:736` and Phoenix `src/phx_cabac_syntax.c:851`, both
  `encode_ue_bypass(cb, 0, abs_coeff - 15)`. The context family is the
  chroma-DC level contexts (`ctx=263`, `coeff_abs_levelgt1_ctx_chroma_dc`,
  `reference/x264/encoder/cabac.c:655`, `:672`).
* **Value:** x264 encodes `val=7` → `abs_coeff = 22`; Phoenix encodes `val=14`
  → `abs_coeff = 29`. The first divergent bin is simply the first bit where
  those two Exp-Golomb/bypass codewords differ.
* **Coder state: MATCHES.** `low/range = 896/335` before the record on both
  sides; all prior context decisions match. This is a *coefficient-value*
  error, not an arithmetic-coder, context-init, bin-order or escape error.
* **Which MB:** MB1 (the second MB, row 0 / column 1). MB0's terminal bin is at
  trace record 692; MB1's terminal bin is at 1654 (x264) / 1669 (Phoenix).
  `phx_blocks.txt` shows MB1 U chroma DC = `[29, -9, 0, 0]`, consistent with
  `val=14`; x264's corresponding magnitude is 22. MB0 chroma DC is
  `[15, -9, 0, 0] / [15, 0, -9, 0]` on both sides.
* **Do not treat the tool's "COEFFICIENT HANDOFF 0/16 after zigzag rescan" line
  as a proven missing-zigzag bug.** It is a raw measurement, but the tool's
  interpretation conflicts with the sources: Phoenix's trellis writes
  `dct[zigzag[i]]` at `src/phx_rdo.c:944`, exactly mirroring x264
  `encoder/rdo.c:890`; the MB0 arrays handed to CABAC are equal 16/16. The
  proven coefficient defect is the chroma-DC level, not a luma zigzag handoff.

---

## 3. x264 functions already byte-correct in this case

Established by the probes/bin trace (byte-exact or bin-exact evidence):

| x264 function (authoritative citation) | Evidence of byte correctness here |
|---|---|
| `x264_sps_init` / `x264_sps_write` — `encoder/set.c:101`, `:305-475` | SPS NAL 23/23 bytes EXACT in CABAC and CAVLC |
| `x264_pps_init` / `x264_pps_write` — `encoder/set.c:479`, `:505-570` | PPS NAL 6/6 bytes EXACT in both entropies |
| version SEI write — `encoder/set.c:574`, `:593`; `encoder.c:3705-3708` | SEI NAL 624/624 bytes EXACT in both |
| NAL assembly / escape — `common/bitstream.c:29`, `:55` | 11 NALs on each side, same type sequence; payload comparison aligned cleanly (framing start-code length is a separate stream-level issue) |
| `slice_header_write` (IDR) — `encoder.c:213-364` | 14 fields, 30 bits, `slice_qp_delta=-3`, `slice_qp=25`: HEADER MATCH in both entropies |
| CABAC engine — `common/cabac.c:32-183` (`context_init`, `encode_init`, `renorm`, `decision`, `bypass`, `ue_bypass`, `terminal`, `flush`) | first 1663 expanded bins and all context states identical; same `low/range` at the divergence |
| `cabac_block_residual_internal` — `encoder/cabac.c:665-748` (sig/last/level context selection, `:736` ue_bypass) | records 1605–1622 identical in ctx/bin/state/low/range |
| MB0/MB1 luma intra mode decisions; MB0 luma coefficients | starting evidence (MB0/MB1 modes match); CABAC handoff arrays 16/16 raw equal |

These are *local* results. They do not make any case byte-exact.

---

## 4. Phoenix functions missing or incorrect (with x264 citations)

### 4.1 Proven incorrect — drives the first divergence

| Phoenix function | What is wrong | x264 authority |
|---|---|---|
| `phx_mb_encode_chroma_trellis` — `src/phx_mbencode.c:727-809`, publish at `:790-797` | Publishes chroma-DC level 29 for MB1 U where x264 emits 22; this is the value encoded by the divergent `ue_bypass` | `encoder/macroblock.c:343-489`, trellis gate `:384`/`:419`, chroma-DC call `:420`, scan `:457`/`:477`; `encoder/rdo.c:1114-1140` |
| `phx_quant_chroma_dc_trellis` — `src/phx_rdo.c:1188-1206`; `phx_rdo_quant_trellis_cabac` — `src/phx_rdo.c:836-...` | Trellis QNS selects the wrong level; must be made to consume the same raw DC, QP, λ2 and live CABAC state as x264 | `encoder/rdo.c:642-895` (`quant_trellis_cabac`), `:663`/`:674` (quant coefs), `:698-706` (DC shortcut), `:1114-1140` (wrapper), `:1142-1157` (4x4) |
| `phx_mb_chroma_raw_dc` — `src/phx_mbencode.c:663-723` | Recomputes the unquantized 2x2 DC from pre-chroma reconstructed neighbours; prime candidate for a different raw residual in MB1 (prediction mode/reconstruction/border handling) | `encoder/macroblock.c:369-379` (sub8x8_dct, dct2x2dc), `:934-935` (chroma prediction) |
| `phx_mb_dct2x2dc` — `src/phx_mbencode.c:648-659`; `phx_zigzag_scan_2x2_dc` — `src/phx_chroma.c:41-47`; `phx_mb_zigzag2_dc` — `src/phx_mbencode.c:639-646` | DC coefficient order/scaling must match exactly; the 2x2 scan swaps entries 1 and 2 in x264 | `encoder/macroblock.c:34-41`, `:82-96`, `:245-294` (`mb_optimize_chroma_dc`) |

`phx_mb_chroma_qp` (`src/phx_mbencode.c:611-618`) currently passes the mapped
chroma QP into the trellis wrapper, whereas x264 `encoder/macroblock.c:420`
passes `i_qp + 3*chroma422` (the luma MB QP). At `qp=25` the two coincide
(`phx_chroma_qp_base[37] = 25`, matching `common/macroblock.h:249-270`), so this
is **not** the cause at this frame, but it must be checked at other QPs before
claiming a general fix.

`docs/FUNCTION_INVENTORY.md` classification to be aware of:
`quant_trellis_cabac` (`rdo.c:642`) is listed MISSING at line 1289 (only a
partial analogue exists in `src/phx_rdo.c`); `mb_optimize_chroma_dc`
(`macroblock.c:245`) is listed MISSING at line 1155 (only the non-trellis
`phx_optimize_chroma_2x2_dc`, `src/phx_quant.c:375`, exists);
`x264_adaptive_quant_frame` (`ratecontrol.c:304`) is MISSING at line 1209.

### 4.2 Next-order blockages, already visible in the same streams

| Gap | Phoenix | x264 authority |
|---|---|---|
| P/B slice header fields are a subset: `pic_order_cnt_lsb`, `slice_type`, `num_ref_idx_*`, RPL modification, pred-weight table, MMCO, `cabac_init_idc` all differ from slice #2 onward | `src/phx_slice.c:14`, `:107` | `encoder/encoder.c:122-211`, `:213-364` |
| CAVLC inter MB header and I_PCM are TODO / fabricated zeros; first CAVLC byte divergence is at post-header byte 1 | `src/phx_encoder.c:1078-1084`, `:1104-1105` | `encoder/cavlc.c:487-595`, `:511-535` |
| No B-frame analysis, no sub-8x8/sub-16x8/8x16, no subpel ME, no weighted prediction, no trellis B-adapt, no MB-tree/VBV lookahead | `src/phx_analysis.h:19-20`, `src/phx_lookahead.h:27-31`, `docs/X264_INTERNALS.md:63-77` | `encoder/analyse.c:1255-3733`; `encoder/slicetype.c:1288-1743`, `:1937-1943`; `encoder/me.c:801-992`, `:1027-1185`; `encoder/ratecontrol.c:541`, `:304` |
| Non-trellis chroma DC uses `quant_2x2_dc` with `>>1`/`<<1` deadzone scaling; trellis path bypasses the DC optimizer | `src/phx_chroma.c:305-314`, `src/phx_quant.c:338-382` | `encoder/macroblock.c:419-426`, `:245-294`; `encoder/rdo.c:1114-1140` |

---

## 5. Prioritized, source-aligned work list to reach byte-exact

**P0 — remove the first CABAC divergence (MB1 chroma-DC level).**
Dump, for MB1 U DC, the raw 2x2 DC residual, QP, `i_trellis_lambda2[1][*]`, the
CABAC state handed to trellis, and the final levels on both sides. Align
`phx_mb_chroma_raw_dc` (`src/phx_mbencode.c:663-723`) + `phx_mb_encode_chroma_trellis`
(`:727-809`) + `phx_quant_chroma_dc_trellis` (`src/phx_rdo.c:1188-1206`) with
`encoder/macroblock.c:369-379`, `:419-420` and `encoder/rdo.c:642-895`,
`:1114-1140`. Acceptance: `ue_bypass(exp=0, val=7)` at bin 1663 with
`low/range = 896/335` unchanged, then the entire 1663-bin prefix re-verified.
Add a golden test that pins these exact numbers (none exists today).

**P0 — confirm CAVLC uses the same coefficient source.**
The CAVLC IDR diverges at post-header byte 1, earlier than CABAC's byte 153.
There is no CAVLC bin-diff harness; add one mirroring `tools/cabac_bin_diff.py`
before tuning `src/phx_cavlc.c:999` / `src/phx_cavlc_syntax.c` against
`encoder/cavlc.c:70`, `:121`, `:208`, `:487-595`. Do not conclude the CAVLC
failure has the same root cause until a bin-level trace proves it.

**P1 — re-probe after P0; fix P/B slice headers.**
`src/phx_slice.c:107` must emit all fields of `encoder.c:213-364` (start with
`pic_order_cnt_lsb ref=2 phx=8` on slice #2, then `slice_type`, ref counts, RPL,
MMCO, `cabac_init_idc`, pred-weight table). Verify with `first_frame_probe.py
--all-slices` before touching entropy again.

**P1 — complete the missing analysis/entropy ports named in §4.2**
(B modes/sub-partitions/subpel ME, weighted prediction, trellis B-adapt,
MB-tree/AQ, CAVLC inter MB writer). Work function-by-function against the
`file:line` citations above and lock each with a golden test.

**P2 — only then claim progress.**
Re-run `python tools/parity_harness.py`, `python tools/compare_streams.py`, and
both `first_frame_probe.py` entropies; require `byte_exact: true` for all 100
cases and re-probe the previously failing medium cases.

Do **not** change x264 flags, fixtures, or the reference corpus to make a case
pass, and do not compensate for a wrong coefficient by editing CABAC contexts or
the arithmetic coder — those are proven byte-correct here.

---

## 6. Explicit parity statement

**0/100 is not parity.** `build/parity/parity_report.json` currently reports:

```
"totals": { "cases": 100, "x264_ok": 100, "x264_failures": 0,
            "compared": 100, "byte_exact": 0, "mismatches": 100,
            "first_diff_min": 35 }
"by_preset": { "medium": { "cases": 10, "x264_ok": 10, "compared": 10,
                           "byte_exact": 0, "mismatches": 10,
                           "first_diff_min": 39 } }
```

Every medium case has `byte_exact: false`. Matching SPS/PPS/SEI (3 of 11 NALs),
a 30-bit IDR slice-header match, 1663 identical CABAC bins, 16/16 equal MB0 luma
arrays, and "MB0/MB1 intra modes now match" are **local diagnostics only**. Per
`docs/PARITY_ITERATION.md:100-108` and `docs/NIGHTLY_STATUS.md:43-50`, a case
counts only when the complete Phoenix `.h264` file equals the x264 `.h264` file
byte for byte; the sole valid parity claim is 100/100 byte-exact. Medium is
0/10, the corpus is 0/100, and there is no byte-exact parity claim for this
preset.

> **Post-audit note (orchestrator):** the stream `first_diff: 39` recorded in
> the report is stale. Commit `becb95b` fixed x264's Annex-B start-code rule;
> the live 100-case first diffs are now bytes 587-671. NAL-payload offsets and
> the CABAC bin-1663 finding remain valid.

