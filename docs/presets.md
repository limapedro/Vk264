# x264 presets → oracle: settings, implementation status, and roadmap

This document is the working reference for **what each x264 preset actually
does** and **how much of it the oracle implements**. It exists so progress is
tracked per preset/feature instead of as a single parity percentage.

- Authoritative source: `build/x264-ref-src/common/base.c` — `x264_param_default()`
  (lines 344–487) and `param_apply_preset()` (lines 489–609).
- Oracle mapping: `Vk264-rewrite/src/ref/nal.c` — `vk_param_apply_preset()`
  (lines 68–136), plus the feature files listed in §6.
- Companion tables: `docs/PRESET_PARAM_MATRIX.md` (parameter × function matrix),
  `docs/PARITY_PLAN.md` (experiment programme), `docs/PARITY_FINDINGS.md`
  (evidence log E01–E81), `tests/preset_audit/HYPOTHESIS_LIST.md`.
- The earlier Vulkan-backend preset note is preserved at
  `docs/presets_legacy_vulkan.md`.
- Refresh rule: after any source change, re-run `build/audit_presets.py` then
  `build/progress_metrics.py`; the metrics read generated
  `build/preset_audit_work/exp*.h264` files. Never trust a stale metric after
  editing `Vk264-rewrite/`.

---

## 1. How x264 presets work

x264 starts from `x264_param_default()` (the **medium** preset, i.e. no
override) and then applies a fixed set of overrides for the named preset. A
preset is therefore a *bundle of deltas*, not an independent configuration.

### 1.1 Baseline (`x264_param_default`, base.c:344–487)

| group | default (medium) |
|---|---|
| rate control | CRF 23, AQ variance 1.0, lookahead 40, MB-tree on |
| GOP | 250 keyint, keyint_min AUTO, 3 B-frames, scenecut 40, b-adapt FAST, B-pyramid NORMAL |
| entropy | CABAC on, deblocking on (α=0, β=0) |
| analysis | refs 3, intra I4x4\|I8x8, inter I4x4\|I8x8\|PSUB16x16\|BSUB16x16, direct SPATIAL |
| motion | HEX, me_range 16, subme 7, mixed refs on, chroma ME on, fast_pskip on |
| RD | psy-RD 1.0, trellis 1, weighted pred SMART, weighted bipred on, transform 8x8 on |

### 1.2 Per-preset overrides (base.c:489–609)

| preset | exact overrides relative to medium |
|---|---|
| **ultrafast** | refs=1, scenecut=0, deblock=0, CABAC=0, B=0, intra=0, inter=0, 8x8dct=0, DIA, subme=0, AQ=0, mixedref=0, trellis=0, b-adapt NONE, MB-tree=0, wpred NONE, bipred=0, lookahead=0 |
| **superfast** | inter=I8x8\|I4x4, DIA, subme=1, refs=1, mixedref=0, trellis=0, MB-tree=0, wpred SIMPLE, lookahead=0 |
| **veryfast** | subme=2, refs=1, mixedref=0, trellis=0, wpred SIMPLE, lookahead=10 |
| **faster** | mixedref=0, refs=2, subme=4, wpred SIMPLE, lookahead=20 |
| **fast** | refs=2, subme=6, wpred SIMPLE, lookahead=30 |
| **medium** | *none* — pure defaults |
| **slow** | subme=8, refs=5, direct AUTO, trellis=2, lookahead=50 |
| **slower** | UMH, subme=9, refs=8, b-adapt TRELLIS, direct AUTO, inter += PSUB8x8, trellis=2, lookahead=60 |
| **veryslow** | UMH, subme=10, me_range=24, refs=16, b-adapt TRELLIS, direct AUTO, PSUB8x8, trellis=2, B=8, lookahead=60 |
| **placebo** | TESA, subme=11, me_range=24, refs=16, b-adapt TRELLIS, direct AUTO, PSUB8x8, fast_pskip=0, trellis=2, B=16, lookahead=60 |

### 1.3 Effective values (the table used for alignment work)

| param | ultra | super | very | faster | fast | medium | slow | slower | very slow | placebo |
|---|---|---|---|---|---|---|---|---|---|---|
| refs | 1 | 1 | 1 | 2 | 2 | 3 | 5 | 8 | 16 | 16 |
| B-frames | 0 | 3 | 3 | 3 | 3 | 3 | 3 | 3 | 8 | 16 |
| scenecut | 0 | 40 | 40 | 40 | 40 | 40 | 40 | 40 | 40 | 40 |
| b-adapt | NONE | FAST | FAST | FAST | FAST | FAST | FAST | TREL | TREL | TREL |
| CABAC | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| deblock | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| 8x8dct | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| ME | DIA | DIA | HEX | HEX | HEX | HEX | HEX | UMH | UMH | TESA |
| subme | 0 | 1 | 2 | 4 | 6 | 7 | 8 | 9 | 10 | 11 |
| me_range | 16 | 16 | 16 | 16 | 16 | 16 | 16 | 16 | 24 | 24 |
| mixed refs | 0 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 |
| fast pskip | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 0 |
| weighted pred | NONE | SIMPLE | SIMPLE | SIMPLE | SIMPLE | SMART | SMART | SMART | SMART | SMART |
| trellis | 0 | 0 | 0 | 1 | 1 | 1 | 2 | 2 | 2 | 2 |
| mb-tree | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | 1 | 1 |
| lookahead | 0 | 0 | 10 | 20 | 30 | 40 | 50 | 60 | 60 | 60 |
| direct | SPA | SPA | SPA | SPA | SPA | SPA | AUTO | AUTO | AUTO | AUTO |

`i_chroma_qp_offset` is 0 for ultrafast…faster and −2 for fast+ (psy-RD
active at subme ≥ 6).

---

## 2. What the oracle actually uses

`vk_param_apply_preset()` sets only the fields the encoder needs today:

- **Header-visible**: `i_frame_reference`, `i_bframe`, `b_transform_8x8`,
  `b_cabac`, `b_deblocking_filter`, `b_weighted_pred`, `i_weighted_bipred`.
  These are aligned with x264 for all 10 presets — SPS/PPS are byte-identical
  in the 120-case audit.
- **Gates/derived globals**: `i_subme` → `vk_cur_subme`; `vk_i4_analysis_enabled`
  (subme ≥ 1); `vk_i8_analysis_enabled` (`b_transform_8x8`);
  `vk_chroma_qp_offset` (−2 at subme ≥ 6); `vk_cur_fast_pskip` (0 for placebo);
  `vk_cur_me_range` (24 for veryslow/placebo); `vk_cur_psy_rd` (256 at subme ≥ 6);
  `vk_cur_trellis` (0/1/2 by subme, `VK_TRELLIS` env override).
- **Not represented at all**: `i_me_method` (only DIA exists),
  `rc.b_mb_tree`, `rc.i_lookahead` as a multi-pass process, `b_adapt TRELLIS`,
  `i_direct_mv_pred AUTO`, PSUB8x8 inter partitions, weighted bipred detail.

**Alignment rule:** an x264 parameter must keep the *same value* in the oracle
even when its implementation is incomplete. Disabling a parameter because its
code is unfinished breaks the bitstream in a different way and hides the real
gap. E45/E47/E48/E49/E70/E74 all follow this rule.

---

## 3. Per-preset status and measured parity

Metrics below are from `build/progress_metrics.py` (first differing byte of the
first IDR NAL) and `build/audit_presets.py` (120 cases = 10 presets × 2
entropies × 6 clip/frame groups). Current baseline: **12/120 byte-exact**,
**3/20 NAL2 pairs**, **18 NAL-count mismatches** (down from 24 in E81).

| preset | x264 tier | oracle status | first-diff (CABAC) | byte-exact | main missing work |
|---|---|---|---:|---:|---|
| **ultrafast** | subme 0, no B, no CABAC, no I4/I8, trellis 0 | **COMPLETE** — the oracle implements exactly this tier | EXACT | 12/12 | none |
| **superfast** | subme 1, SAD mbcmp, 8x8dct on, refs 1 | CABAC byte-exact (IDR); CAVLC first-diff byte4→byte10/17 (E77) | EXACT (IDR) | 0/12 | CAVLC coefficient syntax (MB0 byte 10/17) |
| **veryfast** | subme 2, SATD mbcmp, trellis 0, lookahead 10 | I16/I8/I4 analysis largely faithful; E74 SA8D + E79 exact SATD | **48** | 0/12 | MB11/MB15 type mismatch; later-MB coefficient syntax |
| **faster** | subme 4, trellis 1, refs 2 | I8/I4 decisions aligned; trellis primitive block-exact 17/17 but write-time integration regresses (E68–E70) | **30** | 0/12 | trellis-aware coefficient writing; write-time state timing |
| **fast** | subme 6, trellis 1, psy-RD, chroma QP −2 | RD paths present; CAVLC coefficients still non-trellis | **41** | 0/12 | trellis-aware CAVLC coefficients; MB-level RD refinement |
| **medium** | subme 7, 3 refs, defaults | same gaps as fast, plus 3-ref P-frame behaviour | **41** | 0/12 | multi-ref, B-slice emission |
| **slow** | subme 8, 5 refs, trellis 2, direct AUTO | effective values aligned; analysis partial | **41** | 0/12 | 5-ref, direct AUTO, B slices |
| **slower** | subme 9, UMH, 8 refs, b-adapt TRELLIS, PSUB8x8 | DIA-only ME; b-adapt TRELLIS and PSUB8x8 absent | **41** | 0/12 | UMH, b-adapt TRELLIS, PSUB8x8 |
| **veryslow** | subme 10, UMH, 16 refs, me_range 24, B=8 | me_range aligned; UMH/16-ref/8-B not implemented | **42** | 0/12 | UMH, 16 refs, 8 B-frames |
| **placebo** | subme 11, TESA, 16 refs, B=16, fast_pskip 0 | fast_pskip aligned; TESA and 16-B not implemented | **42** | 0/12 | TESA, 16 B-frames |

### Verdict tiers (2-frame audit slice)

| preset | CAVLC NAL2 | CABAC NAL2 |
|---|---|---|
| ultrafast | EXACT | EXACT |
| superfast | byte4 → byte10/17 | EXACT (first IDR) |
| veryfast | byte4 → byte17 | byte79 → **48** |
| faster | byte4 → byte17 | byte17 → **30** |
| fast / medium / slow / slower | byte10 | byte15 → **41** |
| veryslow / placebo | byte10 | byte16 → **42** |

The gap is **preset-gated, not content-gated**: the same tier result holds for
every clip × frame-count group. Work = complete missing tiers, not chase
arbitrary content bugs.

---

## 4. Feature implementation map

Legend: **DONE** = byte-exact contribution in at least one verified case;
**PARTIAL** = code exists and is faithful in a subset; **MISSING** = no
implementation; **ROADMAP** = planned, not started.

| feature | presets that need it | oracle location | status |
|---|---|---|---|
| SPS/PPS header fields | all | `nal.c:145–230` | **DONE** — 120/120 headers exact |
| slice header write | all | `slice.c:258–304` | **DONE** for I/P; B partial |
| I16 mode decision + exact SATD | superfast+ | `cabac_enc.c` `i16_satd*`, `intra16.c` | **PARTIAL** — exact costs E79; MB11/MB15 remain |
| I8 mode decision + SA8D/subme gate | superfast+ | `i8x8decide.c`, `cabac_enc.c` | **PARTIAL** — E74; E77 shares decisions with CAVLC |
| I4 decision + DC variants | superfast+ | `i4x4.c`, `cabac_enc.c` | **PARTIAL** — E72 DC variant, E70 cost_sig |
| CAVLC I4/I8 MB emit | superfast+ | `i4x4enc.c`, `i8x8enc.c` | **PARTIAL** — E77 wired to CABAC decisions; coefficient syntax diverges at byte10/17 |
| CABAC trellis primitive | faster+ | `trellis.c`, `cabac_enc.c` | **PARTIAL** — block replay 17/17, `cost_sig` fixed E70; write-time integration regresses |
| CAVLC trellis coefficients | faster+ | `cavlc.c` | **ROADMAP** — fast/medium still byte4; needs trellis quant |
| P-slice inter ME (DIA) | all B-bearing | `lowres.c`, `pframe.c` | **PARTIAL** — CAVLC P-slice 1360/1360, CABAC partial |
| HEX / UMH / TESA ME | veryfast+ | — | **ROADMAP** — only DIA exists |
| b-adapt FAST | superfast+ | `slicetype.c` | **PARTIAL** — 16/16 frame-type agreement |
| b-adapt TRELLIS | slower+ | — | **ROADMAP** |
| scenecut IDR placement | superfast+ (B≠0) | `slicetype.c` `vk_slicetype_idr_flags`, `lowres.c` exact costs | **PARTIAL** — E81: 24→18 NAL-count mismatches; c043 exact 0/9/23 |
| MB-tree | medium+ | `mbtree.c` | **MISSING** in lookahead |
| multi-pass lookahead | veryfast+ | `slicetype.c` | **MISSING** — single pass |
| B-slice emission | all B-bearing | `bslice.c` | **PARTIAL** — frame types decided, slices not emitted |
| weighted prediction detail | superfast+ | `pframe.c`, `nal.c` | **PARTIAL** |
| direct MV prediction AUTO | slow+ | — | **MISSING** |
| PSUB8x8 inter partitions | slower+ | — | **MISSING** |

---

## 5. Roadmap (priority order)

1. **Trellis-aware CAVLC coefficients** for faster…placebo — closes the
   fast/medium byte4 CAVLC gap and the remaining MB0 byte10/17 divergence.
2. **Write-time trellis integration** for CABAC with correct live `cb->state`
   timing (E68–E70): analysis-time trellis works per block but write-time
   regresses; this is the main faster/fast byte-30/41 cause.
3. **x264 lookahead pruning fidelity** (E80/E81): replicate the sequential
   `frames[1]` check and flash-detection window so IDRs land at the right
   frames for every preset, not only c043.
4. **B-slice emission + b-adapt TRELLIS** — needed for slower/veryslow/placebo
   and for the remaining NAL-count mismatches.
5. **ME method coverage** (HEX → UMH → TESA) and multi-ref (5/8/16 refs).
6. **MB-tree** wiring into lowres edge-MB gating.
7. **PSUB8x8** inter partitions and direct AUTO for slow+.

Retracted (do not repeat): unconditional SA8D (E74 — breaks superfast recon),
direct naive SATD_16x16 (E75 — buggy cross-check, retried correctly in E79),
per-frame scenecut ratio without flash detection (E78/E80 — over-triggers),
I16-only CAVLC IDR fallback (E76 — replaced in E77).

---

## 6. How to verify a change

```powershell
python build\audit_presets.py          # regenerates 120 cases
python build\progress_metrics.py       # NAL2 first-diff per preset/entropy
python build\recon_pair.py c004        # must stay 0/4096
python build\compare_slicetype.py      # must stay 16/16
python build\cavlc_parity_sweep.py     # must stay 1360/1360
$env:VK264_X264_EXE=(Resolve-Path 'build/x264-ref-src/x264.exe').Path
ctest --test-dir Vk264-rewrite/build -C Release
ctest --test-dir build -C Release
```

Preset verification must always pass explicit `--cabac` / `--no-cabac` to x264;
the oracle CLI takes entropy as argv[6] (`1` = CABAC, `0` = CAVLC).
`VK_TRELLIS` overrides the aligned preset value only for A/B experiments; never
land a change that depends on it.
