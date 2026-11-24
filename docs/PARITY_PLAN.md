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
CABAC first-diff bytes   veryfast/faster 16, fast/medium/slow/slower 11,
                         veryslow/placebo 12
compare_slicetype        16/16 frame types agree (decision parity only)
recon c004 / c064        0/4096  /  2209/4096
```

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

## 4. Experiment programme (>30 falsifiable experiments)

Each experiment: **hypothesis**, **method**, **expected signal**, **gate**, **decision rule**.
A "no metric change" result is a failed experiment, not progress; revert it.
Use `build/progress_metrics.py` (first-diff bytes) as the primary signal,
`audit_presets.py` (12/120) as the acceptance signal, and all gates as a
hard constraint.

### A. CABAC pred-mode / bin-level (byte 11) — highest priority

**E1 — neighbour-mode cache source.** Hypothesis: x264's CABAC pred-mode
is derived from the *raw* neighbour modes in `h->mb.cache.intra4x4_pred_mode`,
which may retain RD-trial values, not from the final `modes4[]`. Method: dump
`i_pred` and the four neighbour cache values at x264's `cabac_intra4x4_pred_mode`
for MB1 block15, and the oracle's `i4_pred(modes4,15)` inputs. Signal: an
i_pred difference with equal modes → root cause. Gate: superfast recon.

**E2 — unavailable-neighbour sentinel.** Hypothesis: x264 uses `-1` (→DC)
for MB-edge neighbours while the oracle uses `2`; with `b_transform_8x8`
these differ. Method: compare ctx68 bin for MB1 block0/block4 under
`--no-8x8dct` vs default. Signal: first-diff changes. Gate: superfast.

**E3 — chroma pred-mode context.** Hypothesis: MB2/MB3 I16 pred differences
(oracle 4 vs x264 1) come from chroma-mode availability flags, not luma.
Method: force x264's chroma mode for MB2 and dump ctx64 bins from both.

**E4 — I16 mode candidate ordering.** Hypothesis: x264's
`predict_16x16_mode_available` returns `{4,1}` for my=0 and the oracle's
`pi16av` returns `{4,1}` too, so ordering matches; verify by dumping both
candidate lists at MB2. Experiment is a cheap confirmation.

**E5 — `x264_mb_pred_mode4x4_fix` table.** Hypothesis: a single table entry
differs for raw mode 9/10/11. Method: compare x264's `fix[13]` against
`vk_i4x4_fix` for all 13 inputs. Gate: none.

**E6 — MB1 I4 mode decision cost.** Hypothesis: x264's i_mbrd>=1 initial
analysis uses `predict_mode += 3` (all candidates) with SATD, while the
oracle still uses the shortcut for I4 when `nc>=6` (this was gated only in a
reverted experiment). Method: gate full candidate enumeration on subme>=6
*only in `vk_i4x4_decide`* (not the earlier RD rewrite). Measure first-diff.
Gate: c004 recon, superfast.

**E7 — CABAC ctx68/69 transition state.** Hypothesis: context state diverges
before bin29 due to an earlier bin that happens to encode the same byte.
Method: compare `cb->state[68]`/`[69]` immediately before bin29 in both.

**E8 — quant/dequant coefficient ordering in writer.** Hypothesis: oracle
writes `i4q` through a second zigzag that double-permutes. Method: dump the
exact zigzag index sequence passed to `cabac_coeffs` in both encoders for
MB1 block0. (This is a real 1-in-N chance given the earlier ordering artifact.)

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

## 5. Priority and dependency order

```
P0  E1–E8   pred-mode / bin-level byte-11 mismatch (six presets)
P1  E9–E17  MB-level RD + I4/I8 analysis (fast..placebo IDR byte-5 tier)
P2  E18–E22 trellis + quant (fast..placebo)
P3  E23–E32 ME, P/B slices, NAL counts (all presets + 24 structural cases)
P4  E33–E36 structural verification and regression sweeps
```

Do **not** start P1 until P0 is either fixed or proven independent — the
byte-11 mismatch is inside a single MB and is much cheaper to close.

### Verification protocol (every experiment)
1. `git status` clean before edit.
2. Apply one gated change (preserve ultrafast/superfast).
3. `cmake --build Vk264-rewrite/build --config Release`.
4. `python build/progress_metrics.py` → first-diff bytes.
5. `python build/recon_pair.py c004` → must stay 0/4096.
6. `python build/compare_slicetype.py` → must stay 16/16.
7. `ctest` 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56.
8. If no metric gain: **revert** and record the result in `HYPOTHESIS_LIST.md`.
9. If gain: full `audit_presets.py`, update docs + SVG, open/merge PR.

---

## 6. Acceptance criteria for the overall goal
- `audit_presets.py`: **120/120 byte-exact**
- `progress_metrics.py`: NAL2 pairs 20/20; no first-diff bytes
- `compare_slicetype.py`: 16/16
- recon c004 0/4096; c064 0/4096
- NAL counts equal for all 30f clips
- All gates green

This document is the tracked plan; update it after each landed fix.
