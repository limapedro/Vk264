# x264 parity plan — verified state, gap inventory, and experiment programme

This document is the working plan for reaching **100% byte-exact x264 parity**.
It is derived from the accumulated evidence in
`docs/PARITY_FINDINGS.md` (534 lines), `docs/PRESET_PARAM_MATRIX.md`,
`tests/preset_audit/HYPOTHESIS_LIST.md` (777 lines) and `HYPOTHESIS.md`
(2412 lines). Every claim below is either a verified measurement or an
explicitly-labelled hypothesis; retracted claims from earlier turns are
marked as such so they are not re-run.

---

## 1. Verified state (run `build/audit_presets.py` + `build/progress_metrics.py`)

```
CTest 29/29 + 65/65     CAVLC P-slice 1360/1360     MP4 56/56
audit                    12/120 byte-exact (ultrafast only)
NAL2 pairs               3/20 byte-exact
CABAC first-diff bytes   veryfast 49, faster 17, fast/medium/slow/slower 15,
                         veryslow/placebo 16  (was 16/16/11/12 before PR #288)
compare_slicetype        16/16 frame types agree (decision parity only)
recon c004 / c064        0/4096  /  2209/4096
```

### Delta this session (evidence)
- PR #287: faithful scan8 i4_pred neighbour + subme>=6 full-candidate enum — no metric gain, fidelity.
- PR #288: **SATD for subme>1 in I4 x3+candidate loops** (`encoder.c:1409 mbcmp_init`). First-diff sum +58 bytes across 8 presets; veryfast 16→49 and its MB1 I4 modes now match x264 exactly.
- E18 confirmed: faster vs veryfast byte-17 divergence is **trellis=1** (not subme/refs/lookahead). `--preset faster --trellis 0` == veryfast; `--preset veryfast --trellis 1` == faster.
- Resolution probe on `D:\02 PehdrewTV\Videos` (4 clips, 2f, cabac): 1280x720 and 720x1280 ultrafast EXACT; 1920x1080 and 640x360 produce **0 oracle NALs** while x264 produces 4 — confirmed mod16-height failure (1088/368 ok, 1080/360 fail). This is a real parity/robustness gap.

The gap is **preset-gated, not content-gated**: every clip × frame-count
group has exactly the same 2/20 result, and every preset has 0/12 except
ultrafast (12/12). This is the strongest single clue: the oracle already
implements one complete preset tier (ultrafast, subme=0, trellis=0, no
I4 analysis, no B-frames) byte-exactly. The work is to add the *missing
tiers*, not to fix arbitrary content bugs.

### Durable gains landed
- PR #261/#270/#274: I8 HU predictor, CAVLC I4 predictors, lowres MV cost
- PR #277: MB-level reconstructed-RD type selection for subme>=6 — first-diff 5/6 → 11/12 (six presets)
- PR #279–#285: full parameter/function documentation, dashboard, findings
- Stage 03: slicetype 13/16 → 16/16 (coarse-grid ME escape + anchor forcing; PR #278)
- PR #287: faithful scan8 i4_pred neighbour + subme>=6 full-candidate I4 enum (fidelity, no metric)
- PR #288: **SATD for subme>1 in I4 analysis** (x264 `mbcmp_init`) — veryfast 16→49, faster 16→17, fast+ 11→15, veryslow/placebo 12→16; c004 recon 0/4096, all gates green

---

## 2. What we have vs what we don't (per preset)

| preset | subme | trellis | B | outer gap (first diff) | missing/partial functions |
|---|---|---|---|---|---|
| ultrafast | 0 | 0 | 0 | **none — EXACT** | — (complete tier) |
| superfast | 1 | 0 | 3 | P-slice byte 7 | fast-pskip early termination; I4 analysis present but P-skip differs |
| veryfast | 2 | 0 | 3 | NAL2 byte 16 | I4 path, HEX ME, lookahead=10, mb-tree |
| faster | 4 | 0 | 3 | NAL2 byte 16 | I4 path, HEX ME, lookahead=20 |
| fast | 6 | 1 | 3 | NAL2 byte 11 | trellis, psy-RD, RD intra, HEX ME, mb-tree |
| medium | 7 | 1 | 3 | NAL2 byte 11 | trellis, psy-RD, RD intra, HEX ME |
| slow | 8 | 2 | 3 | NAL2 byte 11 | trellis=2, RD intra refine, psy-RD |
| slower | 9 | 2 | 3 | NAL2 byte 11 | + UMH ME, b-adapt TRELLIS, PSUB8x8 |
| veryslow | 10 | 2 | 8 | NAL2 byte 12 | + me_range=24, B-slice emission |
| placebo | 11 | 2 | 16 | NAL2 byte 12 | + TESA ME, fast_pskip=0 |

### Function-family status (from `PRESET_PARAM_MATRIX.md`)

| family | status | evidence |
|---|---|---|
| SPS/PPS/slice header | EXACT | byte-identical in all 120 cases; IDR header decodes identically (32 bits) |
| lowres SATD/ME + b-adapt fast | PARTIAL → decision parity | slicetype 16/16; DIA only, no HEX/UMH/TESA |
| I4/I8 analysis (initial) | PARTIAL | SAD/shortcut path matches superfast; subme>=6 RD path absent |
| **MB-level intra RD (`intra_rd` / `intra_rd_refine`)** | **MISSING** | root cause of byte-5 → byte-11 gain; needs exact port |
| trellis quant | MISSING | not implemented anywhere (one comment in nal.c) |
| psy-RD cost term | MISSING | acknowledged absent; not the *sole* cause (disproved as such) |
| P-skip early termination | PARTIAL | probe path only; ME-based P_SKIP absent |
| B-slice emission | PARTIAL | causes 24 NAL-count mismatches |
| mb-tree | MISSING | affects lowres gating |
| weighted prediction | PARTIAL | simple implemented; SMART path differs |
| rate control / CRF | PARTIAL | oracle uses simplified CRF |
| deblock | N/A for tested clips | x264 `tc0=0` at tested QP; filter is a no-op |

---

## 3. Where the byte-11 divergence actually is (most recent evidence)

CABAC bin-level trace of the **final-written** bins for slow:

- bins 0–28: **identical** (MB0 header + residual + MB1 start)
- **bin 29 (ctx 69)**: first mismatch = MB1 **block2 pred-mode bin** (x264 mode 5, oracle mode 4)
- forcing blocks 2/8/11 to x264's modes moves the mismatch to bins 40 → 47 → 56
- with all four differing modes forced to x264's exact array
  `9,9,5,5,9,9,2,2,2,5,2,5,2,2,5,2`, the modes match but **bin 56** still differs:
  x264 ctx68=1, oracle ctx68=0, i.e. block15's pred-mode bin.

Bin 56 comparison is between equal modes (both 2), so the difference is in
**pred-mode derivation**: x264's `x264_mb_predict_intra4x4_mode`
(`common/macroblock.h:420`) applies `x264_mb_pred_mode4x4_fix()` to each
neighbour *before* the min; the oracle's `i4_pred` (`cabac_enc.c:128`)
used fixed modes passed in, which is equivalent for within-MB neighbours —
so a first experiment normalizing it had **no effect**. The next step is
E1/E2 below, not another guess.

### Retracted claims — do NOT re-run these
These were intermediate RD-trial or dump-ordering artifacts:
- "MB0 reconstructs 128 vs 16" — final recon is 16 in **both**
- "MB0 cbp 7 vs 0" — final write is I16/cbp0 in **both**
- "MB1 coefficients differ" — was zigzag vs traversal dump ordering; raw DCT input is identical (`10,40,52,51,-2,21,50,49,0,1,35,50,1,-4,25,53`)
- "psy-RD / trellis cause the 12 nonzero blocks" — x264 `--psy-rd 0 --trellis 0` keeps cbp=7
- "post-header CABAC context" — analysis runs *before* the writer (encoder.c:2861 vs 2878)
- "SATD mode metric" — gating I16 selection to SATD had no effect

---

## 4. Next 16 priority work items (evidence-ranked, 2026-11-24)

Each item has: **why now**, **concrete change**, **signal/gate**, **owner files**. 
Rule: one change per experiment, revert on no metric gain, keep all gates green.

| # | priority | work item | evidence / why now | concrete change | primary signal | gate |
|---|---|---|---|---|---|---|
| 1 | P0 | **Faithful CABAC trellis (4x4/8x8/DC/chroma)** | E18 confirmed: faster byte-17 is trellis=1; 7 presets inherit trellis. **E18b: naive analysis-time wiring regressed faster 17->11 and veryfast 49->11; disabled by default, `VK_TRELLIS` opt-in.** Biggest single gap. | Port x264 `quant_trellis_cabac` C fallback (`encoder/rdo.c:730-895`) + `trellis_coef0/1/n`, `x264_cabac_entropy[128]`, node/level structs. Wire at 4x4/8x8/DC/chroma AC quant when `i_trellis>=1 && b_cabac`, using **live** CABAC state. Unit-test one block against x264 before integration. | faster first-diff 17→?; audit cases for faster..placebo | c004 recon 0, slicetype 16/16, CTest 29+65, CAVLC 1360, MP4 56 |
| 2 | P0 | **Trellis-aware RD: pass live cabac state through RD/encode** | Trellis depends on CABAC context state; oracle RD often quantizes without live state. | Plumb `vk_cabac_t*`/state snapshot through `compute_i16_full`, I4/I8 encode, RD trial paths; use `x264_cabac_size_decision2`/entropy cost where x264 does. | same as #1; prevents wrong trellis decisions | all gates |
| 3 | P0 | **Veryfast byte-49 tail: locate next IDR mismatch after SATD gain** | veryfast jumped 16→49, now first diverges at 49. | Bin/byte trace x264 vs oracle for veryfast cabac 8f c004; identify whether it's pred-mode, coeff, cbp, or chroma. | veryfast first-diff 49→higher | c004 recon 0 |
| 4 | P0 | **I16 mode decision: SATD vs SAD for subme>1** | x264 mbcmp_init makes I16 x3 SATD at subme>1; oracle I16 path still uses SAD (E16 partial). Resolution probe showed veryfast/medium diff@6/8 on 720p — I16 likely involved. | Apply satd16x16/x264-equivalent to I16 mode selection gated `vk_cur_subme>1`; verify against x264 costs on c004 MB0. | 720p probe diff moves; veryfast/faster first-diff | c004 recon 0, ultrafast exact |
| 5 | P1 | **I8x8 analysis metric: SA8D/SATD parity** | x264 uses `intra_sa8d_x3_8x8` when mbcmp==satd (subme>1). Oracle I8 still SAD/approx. | Add 8x8 SA8D for mode decision gated subme>1; keep x264 shortcut table behavior. | fast+ IDR first-diff; I8 MB mode matches on traces | all gates |
| 6 | P1 | **MB-level RD (`intra_rd`/`intra_rd_refine`) exact port** | E9/E10: rough RD gave no gain; need x264's exact `rd_cost_mb` with SSD+psy+f8 CABAC bits and fenc SATD cache. | Port `rd_cost_mb`, `rd_cost_i4x4`, `intra_rd`, `intra_rd_refine` semantics; gate subme>=6; use real CABAC size. | fast/medium/slow/slower/veryslow/placebo first-diff 15/16→higher | c004 recon 0, superfast exact |
| 7 | P1 | **Psy-RD / psy-trellis cost terms** | x264 defaults psy-rd 1.0, psy-trellis 0; oracle has no psy term. Needed for exact RD/trellis decisions. | Add `i_psy_rd`, `fenc_satd`/`fenc_hadamard` cache, psy cost in RD; wire psy-trellis only when i_trellis>=2? (check x264). | RD decisions match on traced MBs | all gates |
| 8 | P1 | **Mod16/mod8 resolution handling bug** | Probe: oracle returns 0 NALs for 1920x1080 and 640x360 (height%16!=0), while 1088/368 work; x264 works. | Find dimension validation/allocation in oracle CLI/encoder; support mod2/mod8 like x264 or at minimum match x264's supported dims; add tests. | 1920x1080/640x360 produce NALs and decode | no regression on 64x64/720p |
| 9 | P1 | **Chroma mode decision parity (cbp/chroma pred)** | I16 chroma/CBP differences seen in earlier traces; resolution probe shows early diff@6/8 on 720p. | Dump x264 vs oracle chroma pred mode, DC/AC cbp, and chroma SATD/SAD costs for first differing MB. | 720p diff moves; cbp/chroma bins match | recon/gates |
| 10 | P1 | **P-slice fast P-skip / early termination** | superfast P-slice byte 7; x264 analyse.c:3002-3014 uses b_fast_pskip + subme>=3 ME-based skip. | Port ME-based P_SKIP probe: cost threshold 300*lambda, mv near pskip_mv, probe; gate by preset/subme. | superfast P-slice first-diff improves | P-slice CAVLC 1360 stays green |
| 11 | P2 | **B-slice emission / NAL count mismatches (24 cases)** | Audit has NAL COUNT mismatches on multi-frame clips. | Trace NAL type sequences 30f slow; compare b-adapt decisions, B-slice encode path, and slice header. | NAL counts equal on affected cases | slicetype 16/16 |
| 12 | P2 | **HEX/UMH/TESA ME per preset** | x264: veryfast+ HEX, slower UMH, placebo TESA; oracle DIA. Affects P/B and lookahead cost. | Implement HEX first (subme>=2), then UMH/TESA; gate by preset. | P-slice first-diff/NAL2, slicetype stable | recon/gates |
| 13 | P2 | **Weighted prediction (SIMPLE/SMART) parity** | x264 uses SMART for medium+ in effect; oracle simplified. PPS/slice header bits can differ. | Compare x264 `x264_weighted_pred_init` outputs and PPS flags for medium/slow; implement exact weight calc. | PPS/header byte match; P-slice diff moves | all gates |
| 14 | P2 | **MB-tree / lookahead AQ integration** | Oracle mb-tree exists but not fully wired into lookahead; x264 uses it for qp offsets/cost. | Audit mb-tree qp delta map vs x264 for 30f clips; wire propagation/gating. | 30f NAL count/byte diffs | slicetype 16/16 |
| 15 | P2 | **Deblock & reconstruction at tc!=0** | Current tested QP gives tc0=0 so deblock no-op; c064 recon 2209/4096 may involve deblock/QP/cbp differences. | Run targeted QP sweep (e.g. 18-24) where tc>0; compare filtered edges; port missing deblock strength/BS logic if any. | c064 recon decreases; no regression at QP28 | recon/gates |
| 16 | P3 | **Cross-resolution/aspect regression suite** | Current audit is 64x64 only; real videos exposed mod16 failure and early diffs at 720p. | Add small automated probe set: 1280x720, 720x1280, 1920x1088, 640x368, odd-crop mod16, 2-8f, ultrafast/veryfast/medium. Track in progress metrics. | new suite reports pass/fail; prevents regressions | CI-friendly runtime |

### 4.1 Notes from resolution probe (2026-11-24)

Command probe (2 frames, `--qp 28 --cabac --keyint 30`, oracle CLI vs x264):

| clip | res | ultrafast | veryfast | medium | note |
|---|---|---|---|---|---|
| ssstwitter 1732160055221 | 1920x1080 | oracle 0 NALs | oracle 0 NALs | oracle 0 NALs | **mod16 height failure** |
| ssstwitter 1748848847689 | 1280x720 | EXACT | IDR diff@6 | IDR diff@6 | works |
| ssstwitter 1724445469565 | 720x1280 | EXACT | IDR diff@8 | IDR diff@8 | vertical OK |
| ssstwitter 1732158697723 | 640x360 | oracle 0 NALs | oracle 0 NALs | oracle 0 NALs | **mod16 height failure** |

Follow-up: 1920x1088 and 640x368 produce oracle output; 1080/360 fail. Treat as parity-blocking robustness bug (item 8).

## 5. Existing experiment backlog (keep, do not rerun retracted claims)

### A. CABAC pred-mode / bin-level (byte 11)

### B. I4/I8 analysis & RD (byte 5 tier) — now history, but needed for presets above subme=6

**E9 — MB-type RD with exact `rd_cost_mb`.** Prior partial version used an
estimated bit count; port x264's `rd_cost_mb` exactly (SSD + `ssd_plane`
psy term + `partition_size_cabac` bits) for I16/I4/I8 and compare MB-type
choice on c004. Gate: superfast must stay exact.

**E10 — fenc SATD cache.** x264 `cached_hadamard`/`cached_satd` primitives
feed the psy term; the oracle computes SATD on the fly. Port the cache and
compare RD costs on MB0.

**E11 — exact `x264_cabac_entropy` bit counts in RD.** Already prototyped and
reverted because RD costs were bit-dominated; re-test **only after E10**,
with the cache in place.

**E12 — psy-RD scaling.** x264 `i_psy_rd = FIX8(1.0) = 256`,
`i_psy_rd_lambda = lambda`. Verify the oracle's identical formula
(`rdo.c:144`) with a unit test on a known block before integrating.

**E13 — `i_mbrd` thresholds.** x264 `a->i_mbrd = (subme>=6)+(subme>=8)+(subme>=10)`;
verify the oracle's gating (`vk_cur_subme>=6`) matches for all presets.

**E14 — analysis shortcut gating.** `analyse.c:916`: shortcut when
`i_mbrd < 1 + b_fast_intra`. Test each preset's candidate set against x264's
`intra_analysis_shortcut` table.

**E15 — I8x8 `sa8d` vs SAD.** x264 uses `intra_sa8d_x3_8x8` when
`mbcmp==satd`; oracle's `vk_i8x8_decide_ex` uses SAD. Test gated on subme>1.

**E16 — I16 `intra_mbcmp_x3_16x16`.** x264 uses a 3-way SIMD compare for
V/H/DC; oracle loops. Verify identical outputs on c004 MB0.

**E17 — `b_fast_intra` early termination.** x264 enables it for P-slices and
I-slices with certain neighbour conditions; oracle sets 0 for IDR. Verify
against x264 for 30f clips.

### C. Trellis and quantisation (presets faster+) — HIGHEST PRIORITY

**E18 — trellis quant primitive.** **CONFIRMED as the dominant gap for 7 presets.**
Controlled experiments proved faster's byte-17 divergence is entirely
trellis=1 (inherited from x264 defaults at `base.c:455`), NOT subme/refs/
lookahead: `--preset faster --trellis 0` is byte-identical to veryfast
and `--preset veryfast --trellis 1` is byte-identical to faster. The
oracle has no trellis at all. Port x264's complete C reference
`quant_trellis_cabac` (`encoder/rdo.c:730-895`) with its helper macros
(`trellis_coef0/1/n`, rdo.c:494-660), the `x264_cabac_entropy[128]` table
(`common/tables.c:1717`), and the node/level structures (rdo.c:417-428).
Integrate at the 4x4 quant call sites gated on `i_trellis>0 && b_cabac`.
Unit-test against x264's output on one 4x4 block before integration.
This is worth 7 presets × 12 audit cases = 84 cases.

**E19 — deadzone tables.** Verify `quant4_bias` for CQM_4IY/4PC at all QPs
against `vk_ref_quant_tables`; earlier index-[25] check matched.

**E20 — quant/dequant round trip.** Compare oracle `vk_ref_quant4x4` +
`vk_ref_dequant4` + IDCT against x264's on a fixed coefficient array.

**E21 — chroma QP offset.** Oracle sets -2 for subme>=6; verify against x264
for fast..placebo including the `b_psy` clip at encoder.c:1226.

**E22 — CQM / scaling lists.** x264 default FLAT; oracle CQM unimplemented.
Confirm no effect for tested presets before deprioritising.

### D. Motion estimation and P/B slices

**E23 — HEX ME for veryfast+.** x264 `lowres_context_init` caps to HEX;
oracle uses DIA. Implement HEX in `vk_me16_dia` gated on subme>=2; check
P-slice first-diff.

**E24 — UMH for slower+, TESA for placebo.** Extend E23; gate on preset.

**E25 — me_range=24 for veryslow/placebo.** Oracle uses 16; test range change.

**E26 — P_SKIP early termination.** x264 `analyse.c:1294–1306`: cost threshold
`300*lambda`, `|mv - pskip_mv| <= 1`, then probe. Oracle uses probe alone.
Implement the ME-based path; check superfast P-slice byte 7.

**E27 — pskip MV sentinel/clipping.** x264 clips pskip_mv with
`mv_min[0]/mv_max[0]`; oracle's `pskip_mv` may differ on MB0. Dump both.

**E28 — weighted prediction SMART.** oracle uses SIMPLE for superfast..fast
and SMART-for-PPS only; medium+ use actual SMART weights. Compare PPS bits.

**E29 — B-slice emission (NAL count).** 24 cases fail on NAL count; dump
NAL type sequences for 30f slow and oracle, identify missing slice NALs.

**E30 — b_adapt TRELLIS for slower+.** Oracle implements FAST only; test
switching mode on 30f clips.

**E31 — mb-tree.** Oracle `mbtree.c` exists but is not wired into lookahead
gating; x264 `do_edges = b_mb_tree || vbv || width<=2`. Test edge-MB gating.

**E32 — lookahead multi-pass.** x264 `x264_lookahead_slicetype_decide` is
encoder-driven; oracle single-pass. Already known from stage-03 work;
needed only if E29/E30 leave residual differences.

### E. Structural / configuration

**E33 — CRF vs QP.** Audit uses `--qp 28`; confirm oracle uses the same
effective qp (both report qp=25 per MB for c004).

**E34 — deblock.** Already ruled out for tested clips (`tc0=0`); keep as a
regression test for clips with QP where tc>0.

**E35 — `--no-asm` equivalence.** x264 reference build uses `--no-asm` in
some tools; verify asm C equivalence so no false divergences are chased.

**E36 — clip/frame coverage.** Confirm all 120 cases still produce the same
verdict categories after any fix; never declare progress on a single clip.

---

## 6. Priority and dependency order

1. **Items 1-2 (trellis + live CABAC state plumbing)** - highest ROI; E18 confirmed as 7-preset gap. Do together.
2. **Items 3-5,9 (locate veryfast@49 tail, I16 SATD, I8 SA8D, chroma parity)** - close remaining IDR mode/coeff gaps.
3. **Item 8 (mod16/mod8 resolution bug)** - fix early; blocks real-video validation.
4. **Items 6-7 (exact MB-RD/intra_rd_refine + psy)** - needed after trellis gives correct coeff costs.
5. **Items 10-14 (P-skip, B-slice/NAL count, ME levels, weighted pred, mb-tree)** - structural preset gaps.
6. **Items 15-16 (deblock tc!=0, cross-res regression suite)** - coverage and reconstruction.

Rule: never trade a gate for a metric; revert no-gain experiments; document every retracted claim.

## 7. Acceptance criteria for the overall goal
- `audit_presets.py`: **120/120 byte-exact**
- `progress_metrics.py`: NAL2 pairs 20/20; no first-diff bytes
- `compare_slicetype.py`: 16/16
- recon c004 0/4096; c064 0/4096
- NAL counts equal for all 30f clips
- All gates green

This document is the tracked plan; update it after each landed fix.
