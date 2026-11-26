# Cross-preset byte-exact parity gap matrix

Source: the ten per-preset audits in `docs/preset_audit/`, measurements at HEAD `d455c7f`, and `build/parity/parity_report.json`. Generated after all 10 audits completed.

## Global facts (all presets)

| Fact | Evidence |
|---|---|
| x264 refs generated | 100/100 `rc=0`, 5 fixtures × 10 presets × 2 entropies |
| Phoenix encodes | 100/100 `rc=0`, 0 crashes after the `frame_num`/MMCO and lookahead-slot fixes |
| SPS / PPS / SEI | byte-exact for all cases (`tests/compare_headers.py`: 20/20, PPS tail 20/20) |
| Annex-B start codes | match x264 (`4,4,3,3,4,4…`) after commits `becb95b` and `34936ff`; the first slice of a frame uses a short code only when it shares the header NAL array |
| Slice headers | IDR slice header byte-exact for every preset (30–34 bits depending on POC type) |
| NAL type sequence | 11/11 NALs on both sides, same types and order |
| Full-stream parity | **0/100 byte-exact** — not achieved |
| First stream diffs (post-fix) | 586 (ultrafast) … 677 (placebo); median 672 |

The remaining divergence is inside slice payloads (MB decisions, coefficients and later P/B slice headers), not in headers or framing.

## Per-preset first divergent bin and root cause

| Preset | First divergent CABAC bin | x264 vs Phoenix | Root cause (source) | Owner |
|---|---:|---|---|---|
| ultrafast | **17** | ctx 169 `last_significant_coeff_flag` (I16 luma DC coeff 3): `b=0` vs `b=1`, coder state identical | Phoenix codes only 4 I16 DC coefficients (`phx_encoder.c:865`); x264 codes 16 (`encoder/cabac.c:989`, `tables.c:1792`). ultrafast `trellis=0` uses deadzone `quant_4x4_dc` (`macroblock.c:219`) | Y1 (`phx_mbencode` / `phx_quant`) |
| superfast | **5** | ctx 69 `intra4x4_pred_mode`, `b=1` vs `b=0` (x264 HU=8, Phoenix H=1), coder state identical | `subme=1` selects **SAD** in x264 (`encoder.c:1409-1410`); Phoenix hard-codes SATD (`phx_analysis.c:769`) | Z1 (`phx_analysis`) |
| veryfast | **23** | ctx 68 `intra4x4_pred_mode`, `b=1` vs `b=0` (x264 H=1 == predicted, Phoenix DDL=3) | `subme=2 → mbrd=0`; missing x264 `mb_analyse_intra` x3/x9 shortcut and `intra_analysis_shortcut` (`analyse.c:673-681/900-947`) | Z1 |
| faster | **23** | ctx 68, same pattern, MB0 I4 block 6; coefficients `[6,-3,0,…]` vs `[6,0,-2,…]` | `subme=4 → mbrd=0`: Phoenix evaluates DDL, a mode x264 never considers; also wrong 8x8/16x16 SATD tile composition (`pixel.c:311-332` vs `phx_analysis.c:244-255`) | Z1 |
| fast | **1663** | `ue_bypass` coeff level, raw record 1623: x264 `val=7` (\|coeff\|22) vs Phoenix `val=14` (\|29), coder state identical | MB1 chroma-DC level produced by the trellis path (`phx_mbencode.c:790-800` → `phx_rdo.c:1188-1205`) vs x264 `rdo.c:1114-1140` / `macroblock.c:425-426/457/477` | Y1 |
| medium | **1663** | same `val=7` vs `val=14` chroma-DC level | same trellis chroma-DC handoff; X1 fixed the *zigzag order* (`macroblock.c:34-41`, commit `019978b`) but the value still comes from the trellis path | Y1 |
| slow | **5** | ctx 69, `b=1` vs `b=0`, coder state identical | `subme=8 → mbrd=2` + `b_deblock_rdo`: x264 `intra_rd` with deblock-aware `rd_cost_mb` (`analyse.c:982-1014`, `rdo.c:162-185`); Phoenix has no equivalent (`phx_analysis.c:1304-1320`) | Z1 |
| slower | **5** (bin #6) | ctx 69, `b=1` vs `b=0`, coder state identical | `subme=9 → mbrd=2`: `intra_rd_refine` / `rd_cost_i4x4` with `COPY_CABAC` (`analyse.c:1016-1135`, `rdo.c:315-347/71-73`); 8 refs fit, high-ref arrays are NOT the bug | Z1 (plus B-adapt TRELLIS / UMH separately) |
| veryslow | **5** | ctx 69, `b=1` vs `b=0`, coder state identical | same I4 RD gap; plus missing B-adapt TRELLIS (`slicetype.c:1288-1382`), B-pyramid MMCO (`encoder.c:2571-2607`), 16-ref lists, weights, UMH (`me.c:422-616`) and `me_range=24` hardcoded to 16 | Z1 first; then lookahead/ME workers |
| placebo | **5** | ctx 69, `b=1` vs `b=0`, coder state identical | same I4 RD gap; plus TESA (`me.c:618-798`, rejected at `phx_me.c:170`), `b_fast_pskip=0` gate (`macroblock.c:3002-3014`), 16 refs / 16 B | Z1 first; then ME/skip workers |

## Shared root causes ranked by leverage

1. **I4 mode decision (Z1)** — affects superfast, veryfast, faster, slow, slower, veryslow, placebo (7 presets). Fix: x264 `mbcmp_init` SAD/SATD selection, `mbrd==0` x3/x9 shortcuts, `intra_analysis_shortcut`, full `intra_rd_refine` with size-only `COPY_CABAC`, correct SATD tile composition. Golden: exact 16-mode arrays for all 7 presets; bins 5/23 must move.
2. **I16 luma DC coefficient count (Y1)** — affects ultrafast (and any `trellis=0` preset). Fix: code 16 DC coefficients with x264's deadzone `quant_4x4_dc`/bias and scan, not 4. Golden: bin 17 must move.
3. **Chroma DC level through trellis (Y1)** — affects fast and medium. X1's zigzag-order fix is landed; the remaining `7 → 14` value comes from `phx_quant_chroma_dc_trellis`. Golden: raw record 1623 must match `val=7`, bin 1663 must move.
4. **P/B slice header semantics** — affects all presets from slice #2 onward (POC, frame_num, ref lists/MMCO, weights, slice_qp_delta). Requires real DPB (`encoder.c:2533-2660`), weighted prediction (`slicetype.c:284`) and lookahead/TRELLIS B-adapt; Z1's work does NOT cover this.
5. **ME/RD extras for high presets** — TESA/UMH, subpel, MB-tree, AQ, b_fast_pskip=0 probe, `me_range=24`.

## Acceptance rule

A preset is byte-exact only when `tools/parity_harness.py` reports `byte_exact` for both CABAC and CAVLC cases. A bin diff improving from bin 5 to bin 1663 is progress, **not** parity. 0/100 is a failure inventory.
