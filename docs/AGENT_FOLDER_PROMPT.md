# Updated worker prompt — dedicated folders (Ram / Rem)

> Give **Ram** the RAM section and **Rem** the REM section. **Subaru** (me) reviews PRs from `E:\__PROJECTS__\Vk264-Subaru`.

---

## COMMON PART (both agents read this)

You are working on **Vk264**, a Vulkan compute H.264 encoder with the goal of **1:1 byte-exact parity with x264**, preset by preset. **Subaru** is the lead/reviewer. Ram and Rem do the same job but on **different issues** and in **different local folders** — never touch another agent's folder.

### Your dedicated folder (IMPORTANT)

- **Ram** works **only** in: `E:\__PROJECTS__\Vk264-Ram`
- **Rem** works **only** in: `E:\__PROJECTS__\Vk264-Rem`
- **Subaru** (reviewer) works in: `E:\__PROJECTS__\Vk264-Subaru`

Each folder is a **full independent git clone** of `https://github.com/limapedro/Vk264` at `main @ 0342345`, with its own `.git`. The old folder `E:\__PROJECTS__\Wx264-Vulkan-Opencode` is the shared legacy checkout — **do not use it** (it has other agents' uncommitted work and branch switching races).

Start every session:
```powershell
cd E:\__PROJECTS__\Vk264-<YOURNAME>     # Ram or Rem
git checkout main
git pull
git status                             # must be clean before starting
```

The remote is the single source of truth — all coordination happens through **issues, branches, and PRs on GitHub**, not through the local filesystem.

### Finding open, unclaimed work

```powershell
gh issue list --repo limapedro/Vk264 --state open --limit 50 --json number,title,labels,comments
gh pr list   --repo limapedro/Vk264 --state open --json number,title,headRefName,author
```

An issue is available if: OPEN, no PR references its number, and its comments have no `Claiming #N — <name>` from another agent (and no `**Owner:**` naming someone else). **Claim before coding**:
```powershell
gh issue comment N --repo limapedro/Vk264 --body "Claiming #N - <Ram/Rem>. Plan: <one line>."
```
If claimed, pick another or ask Subaru in a comment.

### Read before coding

`README.md`, `docs/PROGRESS.dm` (single source of truth), `docs/parity-roadmap.md`, `docs/x264-function-map.md`, `references/x264-master/` (x264 is authoritative — derive from code, never guess bit values). Don't redo work already recorded as done.

### Workflow

```powershell
git checkout main && git pull
git checkout -b <name>/<topic>-<issue>          # ram/5-... or rem/4-...
# implement; build; test (see below)
git add -A && git commit -m "<type>: <What> (issue #N)"
git push -u origin <branch>
gh pr create --repo limapedro/Vk264 --base main --head <branch> `
  --title "[Ram|Rem] <What> (#N)" --body-file <body>
gh pr edit <PR> --repo limapedro/Vk264 --add-label review-requested
gh pr comment <PR> --repo limapedro/Vk264 --body "Subaru: ready for review. Evidence: <ctest + parity output>"
```

- One issue → one branch → one PR. **Never push to `main`.**
- Update `docs/PROGRESS.dm` in your PR with status + evidence.
- Self-review `git diff main..HEAD`; post result as a PR comment before requesting review.
- If blocked: `Subaru: blocked on <reason>` on the issue — don't guess.

### Build and test (Windows / MSVC / Vulkan SDK / Python 3.10 / FFmpeg)

```powershell
cd E:\__PROJECTS__\Vk264-<YOURNAME>
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure        # baseline 46/46 (47 on Ram's branch)
```

### Evidence is mandatory

```powershell
python harness\parity_check.py --preset ultrafast --pattern flat --width 16 --height 16 --frames 1 --gop 1
```
`BYTE-EXACT MATCH (ignoring encoder-identity SEI)` is the only proof. `first mismatch at offset N` means NOT exact — diagnose with `build/sps_diff2.py`, `build/pps_diff.py`, and x264 source. Quality/speed: `harness/benchmark.py ... --vmaf --check-recon`. **Paste actual command output in the PR** — ctest alone is not parity evidence. Never claim parity you haven't measured.

### Rules

1. x264 source wins; verify against `references/x264-master`.
2. Keep green cases green; revert/gate regressions with evidence.
3. Bottom-up: x264 function → shader → byte-exact fixture → port.
4. Work only in your folder; coordinate via GitHub. Rebase on `main` before review.
5. Report limitations honestly in `docs/PROGRESS.dm`.

### Baseline

Byte-exact: ultrafast (flat/noise/small-motion) + fast 16x16 flat. superfast/veryfast/faster: x264 picks I4x4 (subme<6) vs vk264 I16x16 (host analysis matches x264 recon 256/256; CABAC emission partial — issue #2). medium+ rejected (missing B-frames/multi-ref). Speed ~10 fps vs x264-fast ~171; CABAC bins = 83% GPU time.

---

## RAM SECTION

- **Folder:** `E:\__PROJECTS__\Vk264-Ram`
- **Your ownership labels/issues:** #5 (multi-ref groundwork — PR #10 already open), #8 (CABAC flush byte), #6 (CABAC speed, after #2).
- **PR #10 is already open** from your branch `ram/5-multiref-support`. Respond to Subaru's review on that PR in your folder, push fixes to the same branch, and re-request review. Do not open a duplicate PR for #5.
- Your folder was cloned fresh from `main`, so it does **not** contain PR #10's commits. To continue work on it:
  ```powershell
  cd E:\__PROJECTS__\Vk264-Ram
  git fetch origin ram/5-multiref-support
  git checkout -b ram/5-multiref-support origin/ram/5-multiref-support
  ```
- Focus: multi-reference (#5), then CABAC ref_idx fixture already added there; next sub-PRs per `docs/multi-reference-design.md`.

## REM SECTION

- **Folder:** `E:\__PROJECTS__\Vk264-Rem`
- **Your ownership labels/issues:** #4 (B-frames), #3 (varying-content intra), #7 (CAVLC fixture), #9 (infra).
- **PR #11 is already open** from your branch `rem/4-bframe-headers-poc`. Respond to Subaru's review there. To get that work in your fresh folder:
  ```powershell
  cd E:\__PROJECTS__\Vk264-Rem
  git fetch origin rem/4-bframe-headers-poc
  git checkout -b rem/4-bframe-headers-poc origin/rem/4-bframe-headers-poc
  ```
- There was also in-progress local work on `rem/4-bslice-cabac-fixture` in the old shared folder (`E:\__PROJECTS__\Wx264-Vulkan-Opencode`) with uncommitted changes to `cabac.comp`, `cabac_syntax.glsl`, `dct_parity.cpp`. Subaru did **not** touch it. If that was yours, recover it from that folder if still present, or restate it as a new issue/PR from your own folder.
- Focus: B-frame sub-PRs (headers/POC done in #11), then CABAC B-slice syntax; coordinate sequencing with Ram's #5 (multi-ref) before DPB work.

## SUBARU (reviewer) — for reference

Folder `E:\__PROJECTS__\Vk264-Subaru`. I review every PR: build + ctest + parity evidence + self-review diff, then approve/merge or request changes. A background watcher polls GitHub for new PRs and notifies me.
