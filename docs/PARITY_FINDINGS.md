# Parity findings (Phoenix CPU oracle)

Evidence log for byte-exact x264 parity. Every entry states the measurement, the x264 source location followed, and the gate output. "Byte-exact streams" is the only parity claim; bin/byte offsets are diagnostics.

## F1 — Annex-B start codes were wrong for every NAL

- Symptom: first stream diff byte 35–41 across all 100 cases although SPS/PPS/SEI payloads matched.
- Cause: Phoenix wrote a 4-byte start code for every NAL. x264 uses `b_long_startcode = !i || type==SPS || type==PPS` (`encoder/encoder.c:2082-2083`), so SEI and first-frame slices use 3 bytes.
- Second-order cause: lookahead can emit the first IDR after `i_frame > 0`, so an `i_frame == 0` heuristic was wrong; fixed with an explicit `b_first_slice_nal` flag.
- Result: start-code sequences now match x264 (`4,4,3,3,4,4…`) in all presets; first stream diff moved **35–41 → 586–825** (median 672).

## F2 — `frame_num` wrap caused 22/100 encode crashes

- Symptom: high-reference presets crashed `0xC0000005` after frame 27; `phx rc0 78/100`.
- ASAN: `phx_bs_write_ue()` indexed `phx_ue_size_tab[-13]` via MMCO diff `0 - 13`. Phoenix wrapped `i_frame_num` modulo `2^log2_max_frame_num`; x264 keeps it unbounded and masks only in the writer (`encoder.c:3882/136`).
- Result: `phx rc0 100/100`, 0 crashes; MB0 bin parity unchanged.

## F3 — stale lookahead slot caused 22/100 encode crashes (earlier regression)

- `phx_lh_slicetype_decide()` read a stale slot beyond the live queue (`phx_lookahead.c:449`); x264's list is NULL-terminated (`slicetype.c:1865-1872`). The reorder then memmoved a negative count.
- Result: ASAN clean, all presets rc=0.

## F4 — first-MB CABAC parity (medium)

- MB0 is byte-identical: **715/715 ordered CABAC bins** match an instrumented x264 `encoder/cabac.c` golden.
- Fixes: coeff layout `dct[scan8[k]]` (`rdo.c:890`, `macroblock.c:391/410`), transform-size gate from `x264_transform_allowed[]` (`macroblock.h:444-461`), post-CBP transform bin suppressed for I4, chroma U/V plane index `idx/16 - 1`, 2x2 chroma DC zigzag `ZIG macro` (`macroblock.c:34-41`).
- First divergence moved **11 → 63 → 68 → 76 → 158**.

## F5 — MB1 intra4x4 modes

- Cause: Phoenix overwrote caller-owned reconstructed FDEC borders during analysis; x264 reads reconstructed neighbours (`macroblock.c:501-548/883/960-977`).
- Result: MB1's 16 modes match x264; first divergent bin **715 → 1663**.

## F6 — I16 luma DC coefficient mapping (ultrafast)

- The 16 luma DC coefficient values were already identical to x264. Bin 17 came from `phx_cabac_syntax.c` mapping `DCT_LUMA_DC` to `coeff_last4`; x264 uses `coeff_last16` (`quant.c:878-879`, `cabac.c:989`, `tables.c:1792`).
- Also fixed the separate CAVLC `ncoeff = 4 → 16` handoff. Isolated verification: bin **17 → 932**, IDR byte **6 → 87**.

## F7 — chroma DC zigzag

- Phoenix copied 2x2 DC samples in natural order; x264's `ZIG(i,y,x) = dct[x*2+y]` swaps the two middle values before CABAC (`macroblock.c:34-41/457/477`).
- Result: golden arrays `{15,-9,0,0}` / `{15,0,-9,0}` match x264.

## F8 — ten-preset audits

All ten reports are in `docs/preset_audit/`; synthesis in `docs/PARITY_GAP_MATRIX.md`. Converging source-level gaps:

1. I4 mode decision: SAD-vs-SATD for `subme<2` (`encoder.c:1409-1410`), mbrd shortcuts, `intra_rd_refine`/`rd_cost_i4x4` with `COPY_CABAC` (`analyse.c:1016-1135`, `rdo.c:315-347/71-73`) — 7 presets.
2. Chroma-DC level through trellis (`phx_rdo.c:1188-1205` vs `rdo.c:1114-1140`) — fast/medium, bin 1663 (`ue val=7` vs `14`).
3. P/B slice headers and lookahead semantics: DPB/MMCO/ref lists/weights/B-adapt TRELLIS — all presets from slice #2.
4. High-preset ME/RD: UMH/TESA, `me_range=24`, MB-tree, `b_fast_pskip=0`.

## Measured state at `9871fac`

```
ctest (before in-flight analysis edits): 24/24
header-exact: 20/20 SPS+PPS, SEI byte-exact, PPS tail 20/20
phx rc0: 100/100 · x264 rc0: 100/100
byte-exact streams: 0/100  <-- parity NOT achieved
first stream diffs: 586-825
```

## F9 — real-video oracle sweep (continuous encode/decode gate)

`tools/oracle_sweep.py` drives `tools/oracle_video.py` for Phoenix and x264 with identical parameters on a real `D:/Videos` clip (64x64 crop, 30 frames, QP28, `--fps 25`), verifies every stream decodes with `ffmpeg -f null -`, and records sizes/NAL counts/first differing byte.

```
10 presets x 2 entropies: 20/20 pairs generated, 0 byte-exact
first stream diffs: 627 (ultrafast/cabac) ... 696 (placebo/cavlc)
Phoenix sizes: 6694-10812 B   x264 sizes: 2299-2621 B
ffmpeg decode errors: Phoenix 10-66 per stream (20/20 streams affected);
                      x264 0 per stream (0/20 affected)
SPS/PPS/SEI: byte-exact (21 / 4 / 542 B)
all 30 slice headers: byte-exact (IDR 24 bits, P 19 bits)
IDR entropy: first diff payload byte 46
P entropy: first diff payload byte 4 (first MB); first P frame 292 B vs x264 6 B
```

**Correction to an earlier claim:** `ffmpeg -f null -` exits 0 even when the
H.264 decoder logs errors (it conceals them), so `oracle_video.py`'s
`verify: OK` is not a decodability guarantee. `tools/oracle_sweep.py` now runs
ffmpeg with `-v error` and counts stderr lines. Typical Phoenix errors:
`mb_skip_run N is invalid`, `mb_type 45 in P slice too large`, `left/top block
unavailable for requested intra4x4 mode -1`, `Reference 8 >= 4`. These are real
syntax/state defects in the emitted streams, not just file-format issues. The
`.h264` files are raw Annex-B elementary streams; MPC-BE needs an MP4/MKV remux
(`ffmpeg -framerate 25 -i x.h264 -c copy x.mp4`), which does not fix the errors.

Because all slice headers match, the remaining stream-level failure is entirely in MB decisions and coefficients. The dominant gap is row 17 of `docs/BYTE_EXACT_FUNCTIONS.md`: `phx_encoder.c:1438` hard-codes `d.skip = 0`, so Phoenix never emits P_SKIP; x264's first P frame is all-skip (6 bytes). Rows 18-19 (residual inter and I_8x8) are required as soon as x264 codes a non-skip inter MB.

## F10 — full-resolution encode exposed SPS level/MV-length hardcoding

`tools/oracle_video.py` on the **full** `D:/Videos/ssstwitter.com_1748999992229.mp4` (native 480×480, all 131 frames, `--fps 25 --preset ultrafast --qp 28 --cabac`):

| | before fix | after fix | x264 |
|---|---|---|---|
| SPS `level_idc` | 10 (1.0) | 30 (3.0) | 30 |
| SPS `log2_max_mv_length_h/v` | 8 | 10 | 10 |
| first stream diff byte | 7 | **597** | — |
| Phoenix stream size | 1,101,676 B | 464,697 B | 323,174 B |
| SPS bytes | differ | byte-identical (`67 4d 40 1e … f1 62 ea`) | — |

Cause: `phx_sps_init()` (`src/phx_headers.c:34/:72`) hard-coded level 10 and MV length 8. x264 auto-selects the level with `encoder.c:1245-1252` walking `common/tables.c:29-49`, then derives MV length with `encoder/set.c:240-241`. The dimension-aware helper existed in `phx_encoder.c` but was applied only to padded pictures, not mod16 pictures like 480×480. Fixed by moving the level table into `phx_headers.c` and making both paths use it. The MV length helper initially had an off-by-one (`floor(log2)+2`); corrected to `floor(log2)+1`.

Still open at full resolution: x264 repeats SPS/PPS before each IDR (5 SPS + 5 PPS NALs) while Phoenix emits them only once (1 + 1); NAL counts 142 vs 134. Phoenix streams still carry ffmpeg decode errors (86) vs x264 0.

## F11 — P_SKIP wiring and non-mod16 crop

Z1 published x264's P_SKIP decision (`phx_analysis_probe_pskip`, `analyse.c:2953-3024/1294-1307/1320-1330`), and the encoder now honours it (`phx_encoder.c`):

- P_SKIP is sent through the P_L0/D_16x16 MC path (`macroblock.c:654-681`), then restored to `PHX_P_SKIP` for the neighbour cache and entropy writers.
- CABAC writes `mb_skip_flag` for skip (`cabac.c:2873`) and for coded P/B MBs (`:2877`).
- CAVLC uses the byte-exact skip-run writer (`encoder.c:2883-2890`, `:3087-3088`).

Measured on a real 64×64/30f clip, ultrafast/QP28:

| | before | after | x264 |
|---|---:|---:|---:|
| CABAC stream | 9,469 B | **2,035 B** | 2,475 B |
| CAVLC stream | 10,812 B | **2,192 B** | 2,621 B |
| CAVLC decode errors | 66 | **0** | 0 |

Higher presets still code most P MBs as intra (medium 7,048 B vs x264 2,317 B); investigating the `subme>=2` skip gates.

Non-mod16 crop was double-shifted: `phx_encoder.c` stored `(width - display_width) >> 1` and `phx_sps_write()` shifted again, so 806 px produced crop 2 instead of x264's 5 (`set.c:363-371`). Now stores raw padding; the 806×720 SPS is byte-identical and first diff moved 12 → 598.

Current first divergent CABAC bin on the real clip (instrumented x264, ultrafast) is **396**, ctx 152 `significant_coeff_flag`, with matching coder state — i.e. an upstream coefficient difference; downstream CABAC desync then produces decoder errors such as `intra4x4 mode -1`.

## F12 — real-clip bin 396 was a one-pointer chroma AC offset

M10 instrumented both encoders on the real 64×64 clip and matched residual blocks by coder state. At expanded bin 396 the state was identical (`low/range 85896/256`), but x264 fed `DCT_CHROMA_AC` array `[-1,0,0,…]` while Phoenix fed `[0,-1,0,0,…]`.

Cause: x264 `cabac.c:1078` / `cavlc.c:588` pass `h->dct.luma4x4[j]+1` — a 4×4 chroma AC block's zigzag index 0 is its own DC, coded separately in the 2×2 DC block. `phx_chroma.c:296` stores the full 16-sample scan and `phx_encoder_block()` returned it without `+1`. Fixed in `phx_encoder.c`; instrumented CABAC writer was already byte-exact for both arrays (259 ordered records). Real-clip `cabac_bin_diff`: first divergent bin **396 → 589**, Phoenix bins 11,784 → 11,773.

## F13 — P_SKIP gates are correct; B frames are the dominant size gap

Z1 instrumented x264 at `encoder.c:2865` on a real clip: `analyse.c:2959-3014` (fast gate) and `:1294-1307` (`b_try_skip`) already match x264 at ultrafast/superfast/veryfast/medium (all 16 MB skip flags). Remaining gaps:

- a single inter MB where x264's `x264_me_refine_qpel` (`me.c:801-992`) changes MV (0,0) → (0,2) — subpel API existed but was not called from analysis;
- B slices: x264 8–16 B (all 16 MBs B_SKIP via `analyse.c:3334-3359` direct MV + `probe_bskip`), Phoenix 225–250 B coded as intra — roughly 8× the remaining stream-size gap;
- placebo `b_fast_pskip=0` relies solely on `analyse.c:1320-1330` (`rd_cost_mb` → P_SKIP), needing the size-only `macroblock_size_cabac` writer.

Delegated to M4 (public subpel API), M8 (size-only CABAC cost), and a new B1 worker (`x264_mb_predict_mv_direct16x16` + `probe_bskip`).

## F14 — slice-type costs made byte-exact; strict lookahead gate enabled

The lowres per-frame cost path had six source-level defects: packed SATD halves were decomposed as `d[i]±d[i+4]` instead of low `d[0..3]` / high `d[4..7]` (`common/pixel.c:297-308`); `lr_try_bidir` and the fast-skip probe used SAD where x264 selects SATD (`slicetype.c:600/686`, `encoder.c:1411`); `cost_mv[0]` and `+5λ` were applied before subpel refinement and with a `-mvp` offset instead of after (`slicetype.c:695-697`, `me.c:925`); the HEX search was six full candidates instead of x264's half-hexagon with 3-bit direction tie-breaks (`me.c:344-419`); and subpel used `{0,2}` iterations instead of `subpel_iterations[4]={0,2,1,1}` (`me.c:38-50/794-796`).

Instrumented-x264 goldens (synth fixture, medium/QP28):

```
cost(0,1,1) 3387 -> 1993   cost(0,2,1) 510 -> 1324
cost(1,2,2) 3643 -> 2102   cost(0,2,2) 5008 -> 3008
```

All four now byte-identical; FAST slice types `I P P P BREF B B P`, frame_num and POC match the oracle, so `PHX_LOOKAHEAD_STRICT_GOLDEN` is now 1. The previously reported `cost(1,3,3)=3246` was not reproducible and the real third gap was the ME/HEX+subpel defects above. The encoder test's frame_num assertion was corrected for x264's legal non-reference-B reuse (`encoder.c:3882`).

Rule: only a byte-exact stream counts. A moving first-diff is progress, not parity.
