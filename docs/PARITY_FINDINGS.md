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

Rule: only a byte-exact stream counts. A moving first-diff is progress, not parity.
