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

## F15 — suspected P-slice CABAC init table mismatch disproven

M8 reported that `phx_cabac_context_init_PB` in `src/phx_tables.c` differed from x264 `common/tables.c:768` (e.g. ctx 40: 125 vs 127). Direct comparison after stripping comments shows **all 6,144 int8 values are identical**, and `phx_cabac_context_init()` uses the same formula and table index as x264 `cabac.c:37-43` (`PB[model+1]`, `clip3((m*qp)>>4 + b, 1, 126)`, `(min(state,127-state)<<1)|(state>>6)`). For qp=26/model=0, ctx 40/43/46 all encode to the same states on both sides. No source change was made — this is a reported-and-disproven finding, not a bug to chase.

## F16 — the ME refine API is correct; its inputs are not

M4 instrumented the **real** x264 encode (10,063 refine calls) and the real Phoenix encode of `build/oracle_full/diag.yuv`, then replayed x264's exact `fenc`/`fref`/arguments through `phx_me_refine_subpel_mv`. Post-refine MV/cost/cost_mv were byte-identical (`medium 0,0/599/12`; `veryfast 1,20/485/114`). The API is therefore not the cause of the remaining P-frame mismatch.

Three upstream input differences remain:

1. **AQ/λ**: x264's `--qp 28` keeps AQ enabled, so per-MB QP becomes 30 and λ=8 (`ratecontrol.c:1754-1767`, `tables.c:97-104`). Phoenix's CLI disables AQ for CQP (`tools/phx_cli.c:142`) and uses λ=6.
2. **References**: x264's ME reads reconstructed/deblocked pictures (`macroblock.c:1438-1622`); Phoenix passes the raw source frame (`phx_encoder.c:1335-1339`).
3. **Frame pairing**: x264 codes B frames at display frames 1–3, so its first analysed P frame is display frame 4; Phoenix's first P frame is display frame 1.

Delegated to M15 (CQP AQ), DPB1 (reconstructed/deblocked reference DPB) and B1 (B-direct/B_SKIP). Fixing the inputs is a prerequisite for real-clip MB-level parity, independent of any further ME work.

## F17 — 320-pair scaled matrix exposed two SPS bugs

`tools/video_parity_matrix.py --frames 12 --width 320 --height 180` over the 16-clip manifest × 10 presets × 2 entropies (320 pairs, all native-clip content rescaled):

```
byte_exact 0/320  phx_clean_decode 6/320 (all ultrafast/CAVLC)
first_diff histogram: byte 13 -> 224 pairs, byte 7 -> 64, byte 12 -> 32
Phoenix/x264 size ratio: 0.93 (ultrafast) .. 5.62 (placebo)
```

All three early first-diffs were in SPS, not entropy. Field comparison found two source bugs:

1. `phx_sps_write()` emitted `crop_top`/`crop_bottom` verbatim; x264 `set.c:366-371` applies `v_shift=1` for 4:2:0 `frame_mbs_only` (320×180 coded 192: Phoenix 12 vs x264 6).
2. Auto level checked only MBPS and frame size; x264 also enforces `x264_level_t.dpb` — 16 refs at 320×180 require level 21, not 12 (and MV length 10, not 9).

After both fixes the 320×180 medium and veryslow SPS payloads are byte-identical and first diffs moved **13/7 → 674/677**. A fresh 320-pair matrix is running to measure the new distribution.

## F18 — CQP AQ hypothesis disproven; QP 30 is x264's B-frame offset

M15 instrumented `reference/x264` with the exact parity-harness flags and showed that for **every** CQP encode x264 sets `i_aq_mode=0`, `b_mb_tree=0` (`encoder.c:951-966`) with zero AQ offsets and `inv_qscale=256` (`ratecontrol.c:329-333`). M4's observed QP 30 / λ=8 is the B-frame QP offset (`qp_constant[B]=clip(28+6·log2(1.3)+.5)=30`, `ratecontrol.c:825`), not AQ. Enabling AQ in Phoenix would break CQP parity; it stays off.

Instrumented CQP goldens (ultrafast / veryfast / medium):

```
frame QP   I=25, P=28, B=30, BREF=(30+28)/2=29   (ratecontrol.c:823-825/1508-1511)
lambda     x264_lambda_tab: 4/6/7/8 for QP 25/28/29/30  (tables.c:97-109)
per-MB QP  frame1 B qp=30 lambda=8; frame2 BREF qp=29 lambda=7  (bit-identical)
```

Added `phx_ratecontrol_set_kept_as_ref()` (BREF midpoint) and `phx_ratecontrol_lambda()`; wired the former into `phx_encoder.c` before `phx_ratecontrol_start()`. The residual M4 gap is now definitively frame type (Phoenix P where x264 codes B) and raw-vs-reconstructed references, not rate control.

## F19 — headers byte-exact across all 320 pairs

Re-ran the same 16-video × 10-preset × 2-entropy scaled matrix after the F17 SPS fixes:

| metric | before | after |
|---|---:|---:|
| byte-exact streams | 0/320 | 0/320 |
| first stream diff | 7 / 13 / 13 (min/median/max) | **595 / 676 / 2580** |
| ffmpeg decode errors, Phoenix | median 17 | median 17 |
| clean Phoenix streams | 6/320 (all ultrafast/CAVLC) | 6/320 |

Every case now has byte-identical SPS, PPS, SEI, NAL framing and slice headers; all divergence is inside slice entropy (MB decisions/coefficients). The decode-error count did not change, confirming these are bitstream-syntax/state defects, not header defects. Per-preset clean counts: ultrafast 6/32, all others 0/32; ultrafast CABAC/CAVLC stream sizes are now close to x264 (ratio ~0.93), while higher presets remain 3.6–5.6× larger because Phoenix codes B frames as intra (B1's direct/B_SKIP goldens are landed but not yet integrated).

## F20 — harness false positive, empty DPB, and full-frame reconstruction

A matrix run appeared to report 75/320 byte-exact. All 75 were cases where Phoenix emitted **no stream** (I had rebuilt `phx_cli.exe` mid-run, so the CLI was briefly absent); `byte_exact` was computed as `first_diff is None`, which also held for missing files. The harness now requires both streams to exist and be non-empty. True byte-exact remains 0/320.

Instrumenting the B path then found that **every** `phx_dpb_store_frame()` returned -1: `phx_encoder.c` passed x264's per-MB `FDEC_STRIDE=32` (`common.h:571`) while the DPB expected a full-frame stride (`>= i_width`). DPB1 now accepts the per-MB layout and assembles full planes MB-by-MB; M8/M11 replaced the constant `0x1700` CBP with x264's data-derived bits (`encoder/macroblock.c:945-951`), which moved the real-clip first divergent bin 589 → 379.

Phoenix's FDEC scratch physically holds only two 16x16 MBs per 16-row band, so it cannot represent a 64-wide reconstructed frame. `phx_encoder` now assembles a full `phx_frame_t` from each MB's luma/chroma (x264's `h->fdec->plane`) and feeds that to the DPB; reference counts are now non-zero and B list0/list1 can be built. B_SKIP still does not fire on real clips (Z1 investigating), and the strict encoder test still expects x264's MVD (0,0) where Phoenix emits (8,0) (M4 investigating).

## F21 — B_SKIP gate fixed; MVD golden authoritative

Z1 instrumented the real-clip B path: direct prediction, fref1 and colocated state were valid, but `phx_mvdirect_probe_bskip()` always returned 0. Cause: `phx_chroma_qp_of()` omitted x264's `+12` table offset (`encoder.c:1585`: `chroma_qp_table = i_chroma_qp_table + 12 + offset`); at luma QP30/offset −2 Phoenix used chroma QP 16 instead of 28, making the B_SKIP decimate threshold far too small. Fixed with exact citation.

Real 64×64/30f medium/CABAC: stream **5931 → 4729 B** (x264 1380), B slices 230–260 → 117–218 B, P slices 28–35 → 13–21 B. Remaining B-slice gap is traced to uninitialised DPB reference samples (value 128), not the analysis gate.

M4 and T4 reconciled the P-MB MVD golden: T4's first instrumentation read `h->mb.mv/mvd` **before** `macroblock_cache_save()` (`encoder.c:2987`) and saw stale IDR zeros. Authoritative after-save value is `(7,0)` qpel (M4's independent ME trace: fullpel `(8,0)`/cost 308 → refined `(7,0)`/cost 298 at subme 7 on x264's deblocked REF). The strict test golden is now `(7,0)`; Phoenix emits `(8,0)` because its ME REF content diverges (raw vs deblocked reconstruction) — source-side, not a golden issue.

## F22 — bin 379 was the wrong chroma-DC nnz slot

After F20's CBP fix, the real-clip first divergent CABAC bin became 379: x264 ctx 100, Phoenix ctx 99, identical state. M8 traced it to MB1's V-DC CBF (`cat=3`, `idx=50`, array `{3,-1,0,0}`) — the coefficient array and following residual bins already matched.

x264 `encoder/macroblock.c:948-950` sets the CBP high bits from `nnz[LUMA_DC=48]`, `nnz[CHROMA_DC+0=49]`, `nnz[CHROMA_DC+1=50]`. Phoenix used `phx_scan8[16]`/`[32]` (luma 4x4 slots) for the chroma bits, so `cbp_left` was 287 instead of x264's 1823 and `cabac_cbf_ctxidxinc()` derived 99 instead of 100. Fixed in `phx_mbcache.c` to slots 49/50. Also corrected `phx_encoder.c`'s CABAC chroma-DC index `48+ch` → `49+ch` (x264 `cabac.c:1069-1070`); CAVLC keeps `48+ch` per `cavlc.c:581-582`.

No `phx_rdo` defect; `ctest -R phx_rdo` 1/1 PASS.

## F23 — chroma predictor cache, I16 mode MB4, and ±1 reconstruction

The CABAC chroma-pred context stayed at 64 because `phx_cabac_mb_t`'s
`chroma_pred_mode_left/top` were never populated. x264 stores a fixed chroma
predictor per MB (`common/macroblock.c:1835`) and reads it through the left/top
neighbours (`cabac.c:104-107`). Added `phx_mb_state_t.chroma_pred_mode` +
`phx_mb_cache_t.i_chroma_pred_mode`, saved with x264's fix table, and wired the
CABAC neighbour fields. Real-clip bin 1167 → 2250; 8/8 ctest PASS.

M8 classified bin 809 on the other real fixture as an I16 **mode decision**:
MB4 x264 raw I16 mode 0 (V) vs Phoenix raw 5 (DC_TOP → fix16 2); writer and
coder state are exact. Owner: Z1, `analyse.c:688-742` / `phx_search_i16()`.

M4 proved `phx_me_refine_subpel_mv()` is byte-exact: fed x264's exact REF it
returns x264's (7,0)/298/50. Phoenix's DPB REF has 14 pixels off by ±1 in the
16x16 block and 748 differing pixels in the refine neighbourhood (borders
e.g. 128 vs 124), so refine cannot improve (8,0) → (7,0). Owner: reconstruction
/ deblock / border expansion, not ME.

## F24 — bin 2250 is the same I16-MB4 mode defect

After the chroma-predictor fix, the scaled-clip first divergent bin moved 1167 → 2250. M8's instrumented trace shows it is the **same** root cause as the earlier bin 809 on the other fixture: MB4 I_16x16, x264 raw prediction mode 0 (V, fix16 0) vs Phoenix raw mode 5 (DC_TOP, fix16 2), at identical coder state (`low/range 816/360`, `ctx 9 b 0` vs `b 1`). The two bits are `i_pred>>1` and `i_pred&1` (`cabac.c:64-65`); the writer and context are exact. It also explains the byte divergence in CAVLC (`cavlc.c:310` uses the same fix table). Owner remains Z1's `phx_search_i16()` (`analyse.c:688-742`). No new work item.

## F25 — I16 MB4 mode is a consequence of DC reconstruction

Z1 instrumented the real fixture: x264's I16 MB4 search sees a reconstructed top border of 203,203 and picks mode 0 (V, cost 6752) over mode 5 (DC_TOP, 7160). Phoenix sees 110,110 and picks mode 5 (18310 vs 18382). The quantized luma DC already matches x264 (`dc=102`, `nnz=1`, `pred16=6`), and SAD mbcmp, bit costs and tie-break order are source-aligned — so forcing mode 0 would be a fixture-specific approximation. The wrong border comes from the I16 DC dequant/IDCT reconstruction helpers (`phx_dct.c`/`phx_quant.c` vs `common/dct.c`/`common/quant.c`), now delegated with a full-16x16 golden requirement.

Rule: only a byte-exact stream counts. A moving first-diff is progress, not parity.
