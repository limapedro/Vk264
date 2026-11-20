# Prompt for Ram — issue #8 (CABAC 64x64 tail divergence)

Copy-paste everything below to Ram.

---

You are resuming Vk264 parity work on issue #8. Work ONLY in
`E:\__PROJECTS__\Vk264-Ram`. Never touch Subaru's or Rem's folders. Never push
to `main`.

## 0. Update your repo first (you are ~29 PRs behind)

```powershell
cd E:\__PROJECTS__\Vk264-Ram
git fetch origin
git checkout main
git pull --ff-only          # main is now 913c0b2
git checkout ram/2-i4x4-cabac-final
git rebase main             # resolve docs/PROGRESS.dm conflicts, keep both notes
```

Your stale issue #2 branch (`5ea24a1`, docs-only "blocked" note) is SUPERSEDED.
Document that in your rebase commit, then abandon it and start fresh:

```powershell
git checkout -b ram/8-cabac-tail
gh issue comment 8 --repo limapedro/Vk264 --body "Claiming issue #8 (Ram). Rebased onto main 913c0b2; will identify the exact differing CABAC bin/context in the 64x64 flat multi-MB case, not the flush."
```

## 1. Verified state (do not re-derive)

```
main @ 913c0b2, clean
ctest --test-dir build -C Release          -> 60/60 passed
ctest -R x264_parity_                       -> 4/4 passed
ultrafast real footage                      -> BYTE-EXACT (issue #30 CLOSED)
```

Issue #2 is STILL OPEN, but its encoder half is now DONE: PR #45 added a
byte-exact GPU I4x4 CABAC encoder (`x264_i4x4_frame_parity`). Do NOT redo it.
The remaining #2 blocker is mode decision (PR #46), which overlaps Rem's #3.

## 2. Issue #8 — revised scope (the original premise is disproven)

Do NOT chase the flush constant or frame index. Evidence from your own PR #13
and the owner's reclassification:

- `cabac.comp::encodeFlush` is line-for-line identical to x264's
  `x264_cabac_encode_flush` (common/cabac.c:165-183), including the
  `0x35a4e4f5 >> (frame & 31)` bit. `x264_cabac_parity` exercises it at every
  `(frame & 31)` and passes.
- 8-frame tail dump: final-byte LSB follows x264's flush bit identically in
  both encoders; only **bit 7** differs, constant across frames → frame index
  ruled out.
- Divergence starts at **2 macroblocks** (1 MB is byte-exact) and touches only
  the last 1–2 slice bytes. CAVLC (`ultrafast`) IS byte-exact at the same sizes
  → MB decisions/modes/residual levels are identical. The delta is **CABAC-only
  bin/context emission in the multi-MB neighbour path**.
- Adding an extra terminal at end-of-slice made it worse and broke the exact
  16x16 case (already tried, reverted — do not repeat).

Reproduce with `fast`, flat, 1 frame, gop=1:
- 16x16: BYTE-EXACT
- 64x64: `vk264 [... d6 94 bb e1]` vs `x264 [... d6 94 d1 61]`

**Your task:** identify the exact first differing CABAC bin/context at the tail
of the 64x64 flat case. Check `lumaCodedFlag` / `chromaAcFlag` neighbour contexts
in `Vk264/vulkan/cabac_syntax.glsl` and the multi-MB neighbour record layout.

**WARNING (CORRECTION to earlier advice):** `harness/cabac_decoder.py` has THREE
verified bugs and is NOT an authoritative oracle — do NOT derive ground truth
from it. See the corrigendum in `docs/PROGRESS.dm` (2026-11-20):

- `:33,95-96` — `FIX[]` indexed as `fix[t]` instead of x264's `fix[t+1]`
  (`common/predict.h:92`); every neighbour-mode prediction is off by one.
- `:133` — only the first qp_delta bin is consumed; x264 emits more when
  delta != 0 (`encoder/cabac.c:168-181`).
- `:148-151` — cbf context uses unavailable-neighbour = 0 giving 93; x264
  intra counts them as available giving 96 for the first MB
  (`encoder/cabac.c:612-643`). This is why the decoder's decoded syntax
  (transform_size=1, cbp=0xe, dqp=1, blocks 4/5/7/10/13/15) contradicts the
  byte-proven PR #41 target (transform_size=0, cbp=1, blocks 0/1).

UPDATE: Subaru has since FIXED all four bugs (including a fourth: x264 stores
context states as (min(s,127-s)<<1)|(s>>6), not the raw clamped value). The
I4x4 HEADER now decodes exactly to PR #41 ground truth and is gated by
`x264_cabac_decoder_oracle`. You may trust header syntax (modes/cbp/dqp), but
residual levels >=15 still need ue_bypass and are NOT trustworthy — use raw
byte comparison for residual work. Feed the decoder the CABAC payload only
(bytes after the 88 84 3f slice header).

## 3. Hard constraints

- Macroblock record stride is **394 words**, hardcoded in 4 places:
  `fixtures_cabac.inc:8`, `inter_frame.comp:51`, `cavlc_frame.comp:16`,
  `cabac_syntax.glsl`. Do NOT change it (a 410 attempt broke CAVLC).
- Byte comparison against x264 output is the ONLY trustworthy oracle — bin
  decoder syntax-ordering assumptions proved unreliable in your #2 work.
- Finish every change with: full `ctest`, the relevant `parity_check.py`, and a
  clean `git status`. Quote the command output in the PR body.

## 4. PR loop (mandatory)

find mismatch → open PR → implement → prove with command output → merge → close → next.
No speculative fixes. If blocked, document exact command output in
`docs/PROGRESS.dm` and open a docs PR. Coordinate with Rem (issue #3) — both
touch the CABAC neighbour-context path. Do not claim parity you did not measure.
