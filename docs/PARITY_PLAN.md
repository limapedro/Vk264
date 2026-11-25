# x264 parity plan — verified state, gap inventory, and experiment programme

Working plan for **100% byte-exact x264 parity**. Every claim below is either a
verified measurement or an explicitly-labelled hypothesis. Retracted claims are
kept so they are not re-run.

Sources of truth: `build/x264-ref-src/` (x264, authoritative), `Vk264-rewrite/`
(oracle), `docs/PARITY_FINDINGS.md` (629 lines, evidence log),
`tests/preset_audit/HYPOTHESIS_LIST.md` (873 lines), `docs/PRESET_PARAM_MATRIX.md`,
`build/audit_presets.py`, `build/progress_metrics.py`.

---

## 1. Verified state — `main @ c7f5ea2` (2026-11-24)

```
CTest 29/29 + 65/65     CAVLC P-slice 1360/1360     MP4 56/56
audit                    12/120 byte-exact (ultrafast only)
NAL2 pairs               3/20 byte-exact
CABAC first-diff bytes   veryfast 79, faster 17, fast/medium/slow/slower 15,
                         veryslow/placebo 16
compare_slicetype        16/16 frame types agree (decision parity only)
recon c004 / c064        0/4096  /  2209/4096
```

Current verdict counts (120 cases, fresh audit): `BYTE-EXACT 12`, `NAL2 byte4
20`, `byte5 15`, `byte6 7`, `byte10 15`, `byte15 9`, `byte16 6`, `byte17 2`,
`byte79 2`, `NAL COUNT mismatches 24`, `NAL3 byte7/byte2 2`.

2-frame first-diff signature (current):

| preset | CAVLC | CABAC |
|---|---|---|
| ultrafast | EXACT | EXACT |
| superfast | NAL2 b4 | NAL3 b7 |
| veryfast | NAL2 b4 | **NAL2 b79** |
| faster | NAL2 b4 | **NAL2 b17** |
| fast / medium / slow / slower | NAL2 b10 | NAL2 b15 |
| veryslow / placebo | NAL2 b10 | NAL2 b16 |

The gap is **preset-gated, not content-gated**: the same 2/20 result holds for
every clip × frame-count group; ultrafast is the only complete tier (12/12).
Work = add the missing tiers, not fix arbitrary content bugs.

### Durable gains landed
- PR #261/#270/#274: I8 HU predictor, CAVLC I4 predictors, lowres MV cost
- PR #277: lowres inter-MB BITS_MVD cost; MB-level reconstructed-RD for subme>=6 (first-diff 5/6 → 11/12)
- PR #278: slicetype 13/16 → 16/16 (coarse-grid ME escape + anchor forcing)
- PR #279–#286: full parameter/function documentation, dashboard, findings, plan
- PR #287: faithful scan8 i4_pred neighbour + subme>=6 full-candidate I4 enumeration (fidelity, no metric)
- **PR #288: SATD for subme>1 in I4 analysis** (`encoder.c:1409 mbcmp_init`) — first-diff sum +58 bytes; veryfast 16→49, faster 16→17, fast+ 11→15, veryslow/placebo 12→16
- **PR #289: mod2/mod16 resolution handling** — SPS frame cropping + CLI padding; 1920x1080/640x360 now emit NALs (were 0) and decode
- PR #290/#291: trellis foundation (`trellis.c/h`) retained but **disabled by default** after E18b regression (see below)
- **E37: SATD for chroma intra mode decision (subme>1)** — veryfast first-diff **49 → 79** (2f and 8f); bin-level first diff moved from bin313 (ctx64 chroma) to bin541 (ctx400 transform-size flag); all gates green

### Evidence notes
- E18: faster's byte-17 divergence is **trellis=1** (x264 default from `base.c:455`), not subme/refs/lookahead: `--preset faster --trellis 0` matches veryfast, `--preset veryfast --trellis 1` matches faster.
- **E18b (PR #291)**: analysis-time trellis wiring regressed faster 17→11 and veryfast 49→11. x264 applies trellis at CABAC **write** time with live `h->cabac.state`; oracle applied it during analysis with stale state. Foundation is opt-in via `VK_TRELLIS=1/2`; default off.
- Resolution probe on `D:\02 PehdrewTV\Videos` (2f, qp28, cabac, after PR #289): 1280x720 and 720x1280 ultrafast **EXACT**; veryfast/medium diverge at IDR byte 6/8; 1920x1080/640x360 now encode (mod16 bug fixed).
- `build/bincompare_veryfast.py` logs `(ctx,bin)` from both encoders' C `cabac.c` handlers and compares directly (no bitstream decoder). Post-E37 first diff = bin541 ctx400 (transform_size_8x8 flag), i.e. a later MB's I8-vs-I4 decision, not chroma.

---

## 2. What we have vs what we don't (per preset)

| preset | subme | x264 trellis | oracle trellis | outer gap (CABAC 8f) | missing/partial functions |
|---|---|---|---|---|---|
| ultrafast | 0 | 0 | 0 | **none — EXACT** | — (complete tier) |
| superfast | 1 | 0 | 0 | P-slice NAL3 byte7 | fast-pskip early termination |
| veryfast | 2 | 0 | 0 | NAL2 byte79 | HEX ME, Mb-tree, I8/I4 transform-size decision (bin541 ctx400) |
| faster | 4 | 1 | **0 (disabled)** | NAL2 byte17 | write-time trellis, HEX ME, lookahead |
| fast | 6 | 1 | **0 (disabled)** | NAL2 byte15 | RD intra refine, psy-RD, write-time trellis, HEX ME |
| medium | 7 | 1 | **0 (disabled)** | NAL2 byte15 | same as fast |
| slow | 8 | 2 | **0 (disabled)** | NAL2 byte15 | trellis=2, intra_rd_refine, psy-RD |
| slower | 9 | 2 | **0 (disabled)** | NAL2 byte15 | + UMH ME, b-adapt TRELLIS, PSUB8x8 |
| veryslow | 10 | 2 | **0 (disabled)** | NAL2 byte16 | + me_range=24, B-slice emission |
| placebo | 11 | 2 | **0 (disabled)** | NAL2 byte16 | + TESA ME, fast_pskip=0 |

### Function-family status (from `docs/PRESET_PARAM_MATRIX.md`)

| family | status | evidence |
|---|---|---|
| SPS/PPS/slice header | EXACT | byte-identical in all 120 cases; mod2/mod16 crop fixed in #289 |
| lowres SATD/ME + b-adapt fast | PARTIAL → decision parity | slicetype 16/16; DIA only, no HEX/UMH/TESA |
| I4 analysis (satd4x4 for subme>1) | PARTIAL | PR #288; veryfast MB1 I4 modes match x264 exactly |
| I16 mode decision | PARTIAL | `cabac_enc.c` loop uses **SAD**; `vk_i16x16_decide` (SATD) exists but is unused |
| I8x8 analysis metric | PARTIAL | `vk_satd8` available; SA8D path not faithful for subme>1 |
| **MB-level intra RD (`intra_rd`/`intra_rd_refine`)** | **MISSING/approximate** | rough reconstructed-RD gave 5/6→11/12; exact `rd_cost_mb` still needed |
| CABAC trellis | **FOUNDATION, disabled** | `trellis.c/h` ports C fallback; E18b showed analysis-time wiring regresses; must be write-time |
| psy-RD cost term | MISSING | acknowledged absent; not the *sole* cause (disproved as such) |
| P-skip early termination | PARTIAL | probe path only; ME-based P_SKIP absent |
| B-slice emission | PARTIAL | causes 24 NAL-count mismatches |
| mb-tree | PARTIAL | `mbtree.c` exists, not wired into lookahead gating |
| weighted prediction | PARTIAL | SIMPLE implemented; SMART path differs |
| rate control / CRF | PARTIAL | oracle uses simplified CRF |
| deblock | N/A for tested QP | x264 `tc0=0` at tested QP; filter is a no-op |

---

## 3. Where the residual divergence is

The byte-11 bin-level trace (PR #285) was for the **pre-SATD** state. After
PR #288 the first CABAC mismatch moved to byte49; after E37 (chroma SATD)
veryfast moved to **byte79**. Current first diffs: veryfast 79, faster 17,
fast+ 15/16. Known facts:

- MB0 header/residual and MB1 start are byte-identical for all presets.
- veryfast: MB1 I4 mode array now matches x264 exactly
  (`2,2,4,5,2,2,2,2,2,5,2,4,2,2,5,2`). E37 fixed the next mismatch (ctx64
  chroma pred mode, bin313). Bin trace now first differs at **bin541 ctx400**
  = transform_size_8x8 flag of a later MB. Harness: `build/bincompare_veryfast.py`.
- faster: byte17 is trellis-driven (E18/E18b); without a correct write-time
  trellis this tier cannot be closed.
- fast..placebo: byte15/16 is MB1 block pred-mode/RD driven; earlier trace
  localized bin29 (ctx69, MB1 block2 mode) and bin56 (ctx68, block15).

### Retracted claims — do NOT re-run these
- "MB0 reconstructs 128 vs 16" — final recon is 16 in both
- "MB0 cbp 7 vs 0" — final write is I16/cbp0 in both
- "MB1 coefficients differ" — was zigzag vs traversal dump ordering; raw DCT identical
- "psy-RD / trellis cause the 12 nonzero blocks" — x264 `--psy-rd 0 --trellis 0` keeps cbp=7
- "post-header CABAC context" — analysis runs before the writer
- "SATD-only final I4 mode selection (E15)" — x264 final modes come from RD refinement
- "analysis-time trellis is sufficient (PR #290)" — E18b: regressed; must be write-time

---

## 4. Next 16 priority work items (evidence-ranked, 2026-11-24)

One change per experiment; revert on no metric gain; keep all gates green.

| # | priority | work item | evidence / why now | concrete change | primary signal | gate |
|---|---|---|---|---|---|---|
| 1 | P0 | **Write-time CABAC trellis** | E18 + E18b: trellis is the faster+ gap, but analysis-time wiring regresses. Foundation exists in `trellis.c/h`. | Apply trellis inside `cabac_coeffs`/write path with **live** `cb->state`; keep recon in sync. Unit-test one block vs x264 `quant_trellis_cabac` first. | faster first-diff 17→higher; audit cases for faster..placebo | c004 recon 0, slicetype 16/16, CTest 29+65 |
| 2 | P0 | **Trellis-aware RD / live state plumbing** | Trellis decisions depend on context state; oracle RD quantizes without it. | Snapshot/pass `vk_cabac_t` state through RD trials and final write; use `x264_cabac_size_decision2`/entropy costs. | same as #1 | all gates |
| 3 | P0 | **Veryfast byte-79 tail (ctx400 transform-size)** | E37 moved 49→79; new bin-level first diff = bin541 ctx400 (transform-size_8x8 flag of a later MB). | Use `build/bincompare_veryfast.py` to trace MB type / I8-vs-I4 decision after bin541; compare I8 candidate costs. | veryfast first-diff 79→higher | c004 recon 0 |
| 4 | P0 | **I16 mode decision SATD for subme>1** | `cabac_enc.c:505` uses plain SAD; x264 `mbcmp_init` uses SATD at subme>1; `vk_i16x16_decide` (SATD) exists but unused. | Wire SATD16 (x264-equivalent) into I16 loop gated `vk_cur_subme>1`; compare MB0 costs to x264. | 720p diff moves; veryfast/faster first-diff | c004 recon 0, ultrafast exact |
| 5 | P1 | **I8x8 analysis metric SA8D/SATD parity** | **E40: faithful `vk_sa8d8` and `vk_satd8` both regress veryfast (79→38/48)**; the `-3*lambda` discount is SAD-calibrated, so the full x264 I4/I8 cost construction is needed first. | Port x264 cost construction (chroma-cost addition + bit costs) before switching metric; keep shortcut table behavior. | fast+ IDR first-diff | all gates |
| 6 | P1 | **MB-level RD (`intra_rd`/`intra_rd_refine`) exact port** | Rough RD gave no gain; need exact `rd_cost_mb` (SSD+psy+f8 CABAC bits) and fenc SATD cache. | Port exact costs; gate subme>=6. | fast..placebo 15/16→higher | c004 recon 0, superfast exact |
| 7 | P1 | **Psy-RD / psy-trellis terms** | x264 psy-rd=1.0; oracle has no psy cost. Needed for exact RD/trellis. | Add fenc hadamard/satd cache + psy cost; check x264 gating for psy-trellis (i_trellis>=2?). | RD decisions match on traced MBs | all gates |
| 8 | **DONE** | ~~Mod16/mod8 resolution handling~~ | PR #289: SPS crop + CLI padding; 1920x1080/640x360 now encode. | None. Add follow-up cross-res regression suite (item 16). | — | gates green |
| 9 | P1 | **Chroma mode/CBP/DC-AC parity** | **E37 done for mode decision** (SATD at subme>1); chroma CBP/DC-AC and 720p diff@6/8 remain. | Dump x264 vs oracle chroma cbp/DC/AC cost at first differing MB. | 720p diff moves | recon/gates |
| 10 | P1 | **P-slice fast P-skip / ME early termination** | superfast P-slice byte7; x264 `analyse.c:3002` uses b_fast_pskip + ME probe. | Port ME-based P_SKIP gated by preset/subme. | superfast P-slice improves | CAVLC 1360 green |
| 11 | P2 | **B-slice emission / NAL count (24 cases)** | 30f clips: x264 34/36/40 NALs vs oracle 32. | Trace NAL type sequences; compare b-adapt + B encode path + headers. | NAL counts equal | slicetype 16/16 |
| 12 | P2 | **HEX/UMH/TESA ME per preset** | x264: veryfast+ HEX, slower UMH, placebo TESA; oracle DIA only. | Implement HEX (subme>=2), then UMH/TESA; gate by preset. | P-slice/NAL2 first-diff | recon/gates |
| 13 | P2 | **Weighted prediction SMART parity** | x264 SMART for medium+; oracle simplified. | Compare `x264_weighted_pred_init` outputs and PPS flags; implement exact weights. | PPS bits match | all gates |
| 14 | P2 | **MB-tree / lookahead AQ integration** | `mbtree.c` exists, not wired into lookahead gating. | Audit qp-delta map on 30f clips; wire propagation/gating. | 30f diffs improve | slicetype 16/16 |
| 15 | P2 | **Deblock/recon at tc!=0** | Tested QP gives tc0=0; c064 recon 2209/4096 may involve other QPs. | QP sweep 18-24; compare filtered edges. | c064 recon decreases | gates green |
| 16 | P3 | **Cross-resolution/aspect regression suite** | Audit is 64x64 only; real videos exposed mod16 bug and 720p early diffs. | Automate 1280x720, 720x1280, 1920x1088, 640x368, 2-8f, ultrafast/veryfast/medium; track in metrics. | suite reports pass/fail | CI-friendly runtime |

### 4.1 Real-video resolution probe (after PR #289)

| clip | res | ultrafast | veryfast | medium |
|---|---|---|---|---|
| ssstwitter 1732160055221 | 1920x1080 | encodes, not exact | IDR diff | IDR diff |
| ssstwitter 1748848847689 | 1280x720 | **EXACT** | IDR diff@6 | IDR diff@6 |
| ssstwitter 1724445469565 | 720x1280 | **EXACT** | IDR diff@8 | IDR diff@8 |
| ssstwitter 1732158697723 | 640x360 | encodes, not exact | IDR diff | IDR diff |

Item 8 is closed; the residual 720p divergence is tracked by items 3/4/9.

## 5. Existing experiment backlog (keep; do not rerun retracted claims)

### A. CABAC pred-mode / bin-level
E1 neighbour-mode cache source · E2 unavailable-neighbour sentinel · E3 chroma
pred-mode context · E4 I16 candidate ordering · E5 `x264_mb_pred_mode4x4_fix`
table · E6 MB1 I4 decision cost · E7 ctx68/69 transition state · E8 coeff
ordering (resolved: traversal vs zigzag dump artifact).

### B. I4/I8 analysis & RD
E9 exact `rd_cost_mb` · E10 fenc SATD cache · E11 exact CABAC entropy bits in
RD (after E10) · E12 psy-RD scaling · E13 `i_mbrd` thresholds · E14 shortcut
gating · E15 I8 SA8D vs SAD · E16 I16 `intra_mbcmp_x3_16x16` · E17 `b_fast_intra`.

### C. Trellis and quantisation (faster+)
E18 trellis confirmed dominant gap · **E18b analysis-time wiring reverted** ·
E19 deadzone tables · E20 quant/dequant round trip · E21 chroma QP offset ·
E22 CQM.

### D. Motion/P/B slices
E23 HEX ME · E24 UMH/TESA · E25 me_range=24 · E26 P_SKIP early termination ·
E27 pskip MV clipping · E28 weighted SMART · E29 B-slice NAL counts · E30
b-adapt TRELLIS · E31 mb-tree edge gating · E32 lookahead multi-pass.

### E. Structural/configuration
E33 CRF vs QP · E34 deblock · E35 `--no-asm` equivalence · E36 clip coverage.

---

## 6. Priority and dependency order

1. **Items 1–2** (write-time trellis + live state) — highest ROI; E18/E18b define the problem and the failure mode.
2. **Items 3–5, 9** (veryfast tail, I16 SATD, I8 SA8D, chroma parity) — close remaining IDR mode/coeff gaps.
3. **Items 6–7** (exact MB-RD + psy) — needed after trellis gives correct coeff costs.
4. **Items 10–14** (P-skip, B-slice/NAL count, ME levels, weighted pred, mb-tree) — structural preset gaps.
5. **Items 15–16** (deblock tc!=0, cross-res suite) — coverage and reconstruction.
6. Item 8 is **DONE**; do not re-litigate.

Rule: never trade a gate for a metric; revert no-gain experiments; document every retracted claim.

## 7. Acceptance criteria for the overall goal
- `audit_presets.py`: **120/120 byte-exact**
- `progress_metrics.py`: NAL2 pairs 20/20; no first-diff bytes
- `compare_slicetype.py`: 16/16
- recon c004 0/4096; c064 0/4096
- NAL counts equal for all 30f clips
- All gates green

This document is the tracked plan; update it after each landed fix.
