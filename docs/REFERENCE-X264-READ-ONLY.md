# x264 reference tree is READ-ONLY

The directory `references/x264-master/` (and any other upstream x264 source
checkout used as a reference) is **read-only**. It is ground truth.

## Rules

- **READ** source files there as much as needed.
- **NEVER EDIT, WRITE, PATCH, OR DELETE** any file in that tree.
- **NEVER BUILD IN PLACE.** Do not run compilers, `make`, `nmake`, `cmake`,
  `lib`, or `link` with that directory as the working/output directory. Object
  files, `.lib`s, executables, generated headers — all of it — must be built in
  a separate scratch/build directory outside the reference tree.
- **NEVER USE `git checkout -- .`** or any other git write operation inside the
  reference tree. That overwrites whatever copy of the source is present.
- Do not add temporary debug `fprintf`s there, even if you plan to revert them.
  Reverting is not reliable: binaries and build artifacts get overwritten and
  `git status` cannot restore untracked build outputs.

## Why

This happened once: temporary `fprintf` instrumentation was added to
`encoder/cavlc.c`, `encoder/macroblock.c` and `encoder/analyse.c` to dump
per-block coefficients and CAVLC `nC` values. It produced useful findings, but:

- the instrumented `x264.exe` / `libx264.lib` were built **in the reference
  tree**, overwriting the originals;
- backup copies were themselves overwritten on subsequent instrumentation
  runs;
- `git checkout -- .` had to be used to restore sources, which is unsafe when
  a user-supplied copy of the source differs from git HEAD.

The reference tree must remain pristine so byte-exact comparisons against
`x264.exe` mean anything. If the reference is modified, the oracle could be
"matching" a modified encoder rather than x264.

## How to debug parity instead

1. Read x264's source in place (allowed) to understand the algorithm.
2. Copy the files you need into a scratch directory (e.g. `build/x264-probe/`)
   and patch/build **there**.
3. Keep the reference `x264.exe` untouched; use a separate probe binary for
   coefficient dumps.
4. Compare the oracle's output against the pristine reference binary.

Alternative: use the existing `harness/` tools and the C oracle itself to
generate intermediate values; no reference-tree modification is needed.

## Current state note

The instrumentation was reverted; no debug markers remain in the reference
sources. The in-place built binaries were lost during cleanup. If a pristine
`x264.exe` is not available, rebuild from a **copy** of the source into
`build/`, never from `references/x264-master/` itself.
