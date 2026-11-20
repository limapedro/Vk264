# Vk264 agent handoff — Subaru → Ram / Rem (2026-11-20)

## 0. FIRST: update your repo (you are both on stale bases)

Both of your branches are based on an old `main` (before PRs #41–#46).
Your local branches contain only pre-#41 work:

- Ram: `ram/2-i4x4-cabac-final` @ `5ea24a1` (docs-only "blocked" note, issue #2)
- Rem: `rem/3-intra-satd-parity` @ `aaa5a39` (no issue #3 commits yet)

Do this first, in YOUR OWN folder only (never touch another agent's folder):

```powershell
# Ram: cd E:\__PROJECTS__\Vk264-Ram ; Rem: cd E:\__PROJECTS__\Vk264-Rem
git fetch origin
git checkout main
git pull --ff-only            # main is now 913c0b2
git checkout <your-branch>
git rebase main               # expect conflicts in docs/PROGRESS.dm — resolve, keep both notes
```

Never push to `main`. Every change goes through a PR into `main`.

## 1. Verified state of `main` (do not re-derive; this is proven)

```
main @ 913c0b2, tree clean
ctest --test-dir build -C Release          -> 60/60 passed
ctest -R x264_parity_                       -> 4/4 passed
parity_check.py --preset ultrafast <real footage> -> BYTE-EXACT MATCH
```

Merged this session (do NOT redo): #41 exact host I4x4 bin sequence, #42 scope
note, #43 SAD flag in CABAC path, #44 byte-exact host emitter as CTest,
#45 **byte-exact GPU I4x4 CABAC macroblock encoder** (`x264_i4x4_frame_parity`),
#46 root-cause finding below.

What actually works:
- All 4 byte-exact parity gates, 60 CTests (DCT/quant/predict/deblock/motion/entropy/headers).
- **ultrafast** full real-footage byte-exact parity (issue #30 CLOSED): 1080p
  1 and 3 frames, 32x32/64x64 crops, 10/10 sampled crops, 2 other clips.
- `encodeIntra4Macroblock` in `Vk264/vulkan/cabac_syntax.glsl` is byte-exact:
  given a correct I4x4 record it emits x264's exact 20-byte superfast flat slice.

## 2. What is NOT working / open (evidence, not guesses)

- **Issue #2 (OPEN)**: `parity_check.py --preset superfast --pattern flat
  --width 16 --height 16 --frames 1 --gop 1` still reports
  `first mismatch at offset 41` (PPS NAL). vk264 slice
  `88 84 3f fd af 4f 81 ...` (16 bytes, I16 path) vs x264
  `88 84 3f ea 40 35 70 ...` (20 bytes, I4x4 path).
  **Proven root cause of the remaining blocker**: the mode decision, not the
  encoder. x264's real I4x4 decision is `encoder/analyse.c:890-950`, using
  `intra_mbcmp_x9_4x4` (SSSE3) with bit-cost table
  `cost_i4x4_mode[i] = 3*lambda*(i!=8)` (`analyse.c:171-173`) and
  `intra_analysis_shortcut` (`:673`). The host model in
  `Vk264/src/parity/fixtures_intra.inc` (`verifyI4x4Decision`,
  `encodeI4x4Frame`) uses candidate array `{0,1,2,4,5,6,7,8,9}` which **omits
  DDL (mode 3)**, and a simplified `satd + (fixed==pred ? -3lambda : 0)` cost.
  Diagnostic: host model produces modes `{2,9,9,9, 4,2,2,2,...}`; x264 ground
  truth is `{2,2,3,2, 2,2,2,2,...}`. It is NOT a faithful oracle.
- **Issue #3 (OPEN)**: byte-exact varying-content intra — reproduce x264 SATD mode decision.
- Other open issues: #18 (1:1 shader mirror), #8 (64x64 CABAC tail divergence),
  #6 (parallelize CABAC, 82% GPU time), #5 (multi-ref), #4 (B-frames).
- MISSING function families in `docs/function-parity-matrix.md`:
  lookahead/slicetype (~43), RD/trellis (~26), mbtree, CQM, B-frames.
  100% function parity is NOT met — do not claim it.

## 2b. Trustworthiness caveats (full audit, 2026-11-20 — see PROGRESS.dm corrigendum)

- `harness/cabac_decoder.py` has 3 verified bugs: off-by-one mode fix (`:33,95-96`,
  x264 uses `fix[t+1]`), truncated qp_delta (`:133`), and intra cbf context 93
  instead of 96 (`:148-151`). It is NOT an oracle; do not derive syntax from it.
- `parity_check.py` strips ALL NAL type 6 (`:55-57`) and only splits 4-byte start
  codes (`:45-52`), so "PPS NAL" in mismatch messages is mislabelled slice data.
- The harness compares against installed ffmpeg/libx264 (`N-126335…20260830`),
  NOT the pinned `references/x264-master` `c24e06c2` used by the fixtures.
- Byte comparison against x264's actual stream is the only trustworthy oracle.

## 3. CRITICAL implementation constraints (learned the hard way)

1. The macroblock record stride is **394 words**, hardcoded in 4 places:
   `Vk264/src/parity/fixtures_cabac.inc:8`, `Vk264/vulkan/inter_frame.comp:51`
   (`levelWords`), `Vk264/vulkan/cavlc_frame.comp:16` (`mbWords`), and
   `cabac_syntax.glsl`. **Do NOT change it to 410** — last attempt broke CAVLC
   fixtures (empty slice / corrupt decode). PR #45 stores I4x4 pred modes in
   the **unused frame-bank tail** instead: `frameBase + macroblocks*394 + mb*16`.
2. `inter_frame.comp` GPU analysis currently produces only I16 mode via
   `selectIntra16Mode` / `dcMode`. There is no 16-block I4x4 decision and no
   4x4 predictor set in `predict_intra_math.glsl`.
3. `x264ModeSelection` (frameInput[13] bit1) and `useSad` (bit2, PR #43) already
   control the I16 metric; preserve them.
4. Byte comparison against x264 output is the ONLY trustworthy oracle. The bin
   decoder's syntax-ordering assumptions proved unreliable (Ram's finding).
5. Always finish with: full `ctest`, the byte-exact `parity_check.py` for the
   relevant preset, and `git status` clean.

## 4. Assigned, non-overlapping work

- **Ram — issue #8** (CABAC 64x64 tail divergence, `agent-kepler`). Your issue #2
  investigation is superseded by PR #45/#46 findings; close/deprecate your stale
  branch after rebasing and open a fresh branch `ram/8-cabac-tail`. Note issue
  #8's premise was already partly disproven (flush ruled out) in your own
  earlier commit `d8ab519` — start there.
- **Rem — issue #3** (byte-exact varying-content intra SATD, `agent-turing`),
  branch `rem/3-intra-satd-parity`. This is the same x9 cost/shortcut family as
  the issue #2 blocker: port `intra_mbcmp_x9_4x4` + `cost_i4x4_mode` +
  `intra_analysis_shortcut` faithfully into a shader, prove byte-exact against
  real intra crops. Coordinate with Ram so you don't duplicate the decision port.

## 5. PR loop (mandatory)

find mismatch → open PR → implement → prove with command output → merge → close → next.
Never push directly to `main`. Subaru reviews via PR comments (self-approval blocked).
When you hit a genuine blocker, document the exact command output in
`docs/PROGRESS.dm` and open a docs PR rather than guessing.
