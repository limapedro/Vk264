# Prompt for Rem — issue #3 (byte-exact varying-content intra SATD)

Copy-paste everything below to Rem.

---

You are resuming Vk264 parity work on issue #3. Work ONLY in
`E:\__PROJECTS__\Vk264-Rem`. Never touch Subaru's or Ram's folders. Never push
to `main`.

## 0. Update your repo first (you are ~29 PRs behind)

```powershell
cd E:\__PROJECTS__\Vk264-Rem
git fetch origin
git checkout main
git pull --ff-only          # main is now 913c0b2
git checkout rem/3-intra-satd-parity
git rebase main
```

Your branch has no issue #3 commits yet. Claim the issue:

```powershell
gh issue comment 3 --repo limapedro/Vk264 --body "Claiming issue #3 (Rem). Rebased onto main 913c0b2. Will port x264's exact SATD/mode-cost decision into the GPU bridge shader and prove byte-exact parity on flat-grid/motion."
```

## 1. Verified state (do not re-derive)

```
main @ 913c0b2, clean
ctest --test-dir build -C Release          -> 60/60 passed   (issue text says 46/46 — outdated)
ctest -R x264_parity_                       -> 4/4 passed
ultrafast flat/noise/small-motion           -> BYTE-EXACT (issue #30 CLOSED)
```

## 2. Issue #3 — what is actually failing

- `parity_check.py --preset ultrafast --pattern flat-grid --width 64 --height 64
  --frames 1 --gop 1` → first mismatch.
- `motion` at 64x64 and multi-frame also mismatch.
- `dcMode()` in `Vk264/vulkan/inter_frame.comp` is a neighbour-availability
  heuristic, not x264's min-SATD decision with mode-cost tie-breaking.
- Chroma differs by 1–2 samples on flat-grid; luma mode decision is the bigger
  factor.

Relevant code:
- `Vk264/vulkan/inter_frame.comp` — `selectIntra16Mode()`, `selectChromaMode()`,
  `dcMode()`. (Issue text says `intra_frame.comp` — that is a typo.)
- `Vk264/vulkan/predict_intra_math.glsl` — 16x16/8x8 predict functions.
- `Vk264/vulkan/hadamard4.glsl` — `satd4x4()` primitive already available.
- x264 reference: `references/x264-master/encoder/analyse.c::mb_analyse_intra`
  (mode tables, `bs_size_ue` mode costs, `COPY2_IF_LT` tie-break),
  `predict_16x16_mode_available`, `chroma_mode_available`.

## 3. Critical subtlety — do NOT implement a naive min-SATD

x264's `intra_satd_x9_4x4` mode selection is NOT "evaluate 9 modes, pick min
SATD". V mode goes through a separate cost path. The same class of subtlety
applies to 16x16/chroma. Match x264's exact tables and costs:

- `cost_i4x4_mode[i] = 3*lambda*(i!=8)` (analyse.c:171-173) — mode 8 (VL) and
  the predicted mode get effectively free bit cost.
- `intra_analysis_shortcut` table (analyse.c:673) when SIMD is unavailable.
- Real decision loop: analyse.c:890-950.
- Host model in `fixtures_intra.inc` omits DDL mode 3 and is NOT a faithful
  oracle (produces `{2,9,9,9,...}` vs x264 `{2,2,3,2,...}`) — PR #46.

This is the SAME family as the remaining issue #2 blocker. Coordinate with Ram
(issue #8, CABAC neighbour contexts) so you don't duplicate work, but you own
the intra mode-decision port.

## 4. Hard constraints

- Macroblock record stride is **394 words**, hardcoded in 4 places:
  `fixtures_cabac.inc:8`, `inter_frame.comp:51`, `cavlc_frame.comp:16`,
  `cabac_syntax.glsl`. Do NOT change it (a 410 attempt broke CAVLC).
- `x264ModeSelection` (frameInput[13] bit1) and `useSad` (bit2, PR #43) already
  control the I16 metric at subme<=1 — preserve them. Four gates already prove
  SAD-vs-SATD at ultrafast; do not regress them.
- Byte comparison against x264 output is the ONLY trustworthy oracle.
- Finish every change with: full `ctest`, the relevant `parity_check.py`, and a
  clean `git status`. Quote command output in the PR body.

## 5. Acceptance criteria (from the issue, ctest count updated)

- `parity_check.py --preset ultrafast --pattern flat-grid --width 64 --height 64
  --frames 1 --gop 1` → BYTE-EXACT.
- Same for `motion` 64x64 and a multi-frame case.
- No regression on currently-exact cases (flat/noise/16x16 motion, fast 16x16 flat).
- 60/60 ctest green; evidence in PR body.

## 6. PR loop (mandatory)

find mismatch → open PR → implement → prove with command output → merge → close → next.
No speculative fixes. If blocked, document exact command output in
`docs/PROGRESS.dm` and open a docs PR. Do not claim 100% parity — MISSING
families remain (lookahead/slicetype, RD/trellis, mbtree, CQM, B-frames).
