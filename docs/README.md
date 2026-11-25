# Vk264 Phoenix — restart the oracle from the x264 source

## Why this branch exists

The incremental `Vk264-rewrite` oracle has reached a hard ceiling:

| measurement | short audit | extended map (E101) |
|---|---:|---:|
| byte-exact | 12/120 (10%, ultrafast only) | **0/240 (0%)** |
| NAL-count mismatch | 6/120 | 120/240 (50%) |
| CABAC first-diff | 84 / 30 / 41 / 42 | — |

Patches keep finding real bugs (E89 I8 cost, E91 I4 pred cache, E96 scenecut),
but each one fixes a local symptom. The structure of the encoder — its
function set, state ownership, and call order — was never ported faithfully.
Phoenix restarts with the opposite rule:

> **Port x264 function-by-function, in source order, with x264's own data
> structures and state semantics. Never approximate a function to make a
> metric move.**

The reason for the care is the project's real goal: **the shader version of
Vk264**. A shader backend needs a complete, explicit description of every
function's inputs, outputs, state, and determinism. A collection of
metric-driven approximations cannot be translated to GPU shaders; an exact
CPU reference can.

## What is in this branch

```
.                              repository root; vk264-phoenix is the only branch
  CMakeLists.txt               build definition (library + unit tests)
  reference/x264/              176 source files copied from the exact x264 build
                               used for all parity measurements (config.h included)
  src/                         the new oracle source (Phoenix port)
  tests/                       unit tests for ported functions
  tools/                       inventory generator, test runner, branch cleanup
  README.md                    top-level overview
  docs/
    README.md                  this file
    X264_PIPELINE.md           how x264 encodes, function by function, with source lines
    DATA_STRUCTURES.md         the structs that must be ported, field by field
    FUNCTION_INVENTORY.md      mechanical inventory of 1049 x264 C functions + status
    ORACLE_DESIGN.md           how the phoenix oracle must be structured
    SHADER_MAPPING.md          CPU function -> shader classification (the main goal)
    PORTING_ROADMAP.md         ordered work packages and their acceptance gates
```

`reference/x264/` is the **only** authoritative x264 tree for this project;
other copies elsewhere on disk have different configurations. All line numbers
in these documents refer to `reference/x264/`.

## Ground rules

1. **Value alignment is mandatory.** An x264 parameter must have the same
   value in the oracle even if its implementation is unfinished (E45/E47/E48/E49).
2. **No metric-driven shortcuts.** A change that improves first-diff but
   diverges from x264's source is not a port.
3. **State is explicit.** CABAC contexts, neighbour caches, lowres MVs, QP
   offsets and lookahead state must live in named structures, not globals
   patched per experiment.
4. **Every function gets a doc comment** saying what it reads, what it writes,
   what state it mutates, and whether it is seek-safe/shader-able.
5. **A commit must include** the x264 source lines followed, the new oracle
   code, the test/gate output, and the parity metric before/after. There are no
   side branches or PRs: `vk264-phoenix` is the single default branch. The 362
   old branches are archived as `archive/*` tags (see
   `tools/archive_old_branches.py`) and can be restored from there if needed.

## Gates

Current, runnable gates for this repository:

```
python tools/run_tests.py       CMake configure + build + ctest (all unit tests)
python tools/gen_function_inventory.py   regenerate the 1049-function inventory
```

Historical baseline from the discarded incremental oracle (its harness scripts
were removed from this branch but remain in git history):

```
12/120 short-clip byte-exact · 0/240 extended · NAL2 3/20
first-diff 84/30/41/41/41/41/42/42 · recon c004 0/4096
slicetype 16/16 · CAVLC P-slice 1360/1360 · CTest 29/29 + 65/65
```

Each work package adds a new parity gate under `tests/` before it is considered
complete. The final success criterion is byte-exact parity for every preset on
both the short and extended clip sets, plus a function-for-function
correspondence that can be handed to the shader port.
