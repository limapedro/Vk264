# Agent onboarding — Vk264

Copy the prompt in this file to any agent that will work on the codebase.

---

## Prompt

You are working on **Vk264**, a Vulkan compute H.264 encoder whose goal is **1:1 functional and byte-level parity with x264**, preset by preset. Work like a real programmer: claim an issue, branch, implement with evidence, open a PR, self-review, iterate on review.

### 1. Find and clone the repo

The repository is **private**: `https://github.com/limapedro/Vk264` (GitHub CLI is authenticated as `limapedro` in this environment).

```powershell
git clone https://github.com/limapedro/Vk264.git
cd Vk264
git checkout main
git pull
```

If `gh` is not on PATH in your shell, it lives at `C:\Users\multi\AppData\Local\Programs\gh\gh.exe` (also copied to `C:\VulkanSDK\1.4.363.0\Bin\gh.exe`).

Local workspace path used by the main agent: `E:\__PROJECTS__\Wx264-Vulkan-Opencode` (folder name is legacy; repo/project name is `Vk264`).

### 2. Read these before touching code

- `README.md` — goal, layout, build/test commands, parity checks.
- `docs/PROGRESS.dm` — **single source of truth** for per-preset/per-function status, evidence, and open items.
- `docs/parity-roadmap.md` — phase plan and dependencies.
- `docs/x264-function-map.md` — bottom-up x264 function → shader map.
- `references/x264-master/` — pinned x264 source. **x264 is the master reference**; derive behavior from it, not from guesses.

### 3. Pick work

Open issues: `gh issue list --repo limapedro/Vk264`.

**Claim an issue by commenting on it before starting** so parallel agents don't collide. Several issues share `Vk264/vulkan/dct_parity.cpp` (#2, #3, #7) — coordinate there.

Suggested parallel split:
- #2 — I4x4 byte-exact superfast (critical path)
- #3 — varying-content intra mode decision
- #6/#7/#8 — small self-contained tasks (good first issues)
- #9 — infra/parity gate
- #4/#5 — blocked on dependencies; plan before coding

### 4. Workflow (mandatory)

```powershell
git checkout main
git pull
git checkout -b <short-topic>-<issue-number>   # e.g. feature/i4x4-cabac-2
# implement, build, test
git add -A
git commit -m "..."
git push -u origin <branch>
gh pr create --repo limapedro/Vk264 --base main --head <branch> --title "..." --body-file <file>
```

- **Never push directly to `main`.** One issue → one branch → one PR.
- Keep PRs focused; a big task (#4 B-frames) should be split into sub-PRs with a plan comment.
- Update `docs/PROGRESS.dm` inside the PR with new status and evidence.
- Self-review your diff (`git diff main..HEAD`) before requesting review; post the result as a PR comment.

### 5. Build and test (Windows / MSVC / Vulkan SDK / Python 3.10 / FFmpeg)

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Baseline: **46/46 tests pass**. Any regression must be fixed or explained.

### 6. Prove byte-exact parity (this is the objective)

```powershell
python harness/parity_check.py --preset ultrafast --pattern flat --width 16 --height 16 --frames 1 --gop 1
python harness/parity_check.py --preset superfast --pattern flat --width 16 --height 16 --frames 1 --gop 1
```

- `BYTE-EXACT MATCH (ignoring encoder-identity SEI)` is the only accepted proof of parity. The harness compares SPS, PPS, slice header, entropy-coded macroblock data and deblocking byte-for-byte; it strips only encoder-identity SEI NALs.
- `first mismatch at offset N` means **not** parity — investigate with `build/sps_diff2.py`, `build/pps_diff.py` and x264 source.
- Quality/speed: `python harness/benchmark.py --input <clip.yuv> --out <dir> --frames 240 --encoders vk264-vulkan x264-fast --vmaf --check-recon`.
- `--check-recon` verifies the GPU reconstruction against an independent FFmpeg decode byte-for-byte.

**Never claim parity without pasting the actual command output in the PR.** ctest passing alone is not parity evidence.

### 7. Rules

1. x264 source is authoritative. Verify assumptions against `references/x264-master`; don't "fix" behavior by guessing bit values.
2. Keep known-green behavior green. A change that breaks an exact case must be reverted or gated behind a flag with evidence.
3. Work bottom-up: trace an x264 C function → its shader → add a byte-exact fixture → port.
4. Report limitations honestly. If something is not byte-exact, say so and record it in `docs/PROGRESS.dm`.
5. Evidence in every PR: build output, ctest result, parity command output.
6. Don't duplicate another agent's issue — claim first.

### 8. Current baseline (for context, verify yourself)

- Byte-exact: `ultrafast` (flat/noise/small-motion) and `fast` (16x16 flat).
- `superfast`/`veryfast`/`faster`: diverge because x264 picks I4x4 (subme<6) while vk264 emits I16x16. Host I4x4 analysis already matches x264 reconstruction 256/256; CABAC emission is partial (issue #2).
- `medium`+: rejected at CLI (missing B-frames/multi-ref/etc.).
- Speed: vk264 ~10 fps vs x264-fast ~171 fps at 1080p60; CABAC bins take 83% of GPU time (issue #6).
