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
vk264-phoenix/                 everything for this project lives under here
  reference/x264/              176 source files copied from the exact x264 build
                               used for all parity measurements (config.h included)
  src/                         the new oracle source (Phoenix port)
  tests/                       unit tests for ported functions
  tools/                       inventory generator + test runner
  README.md                    top-level overview
  docs/
    README.md                  this file
    X264_PIPELINE.md           how x264 encodes, function by function, with source lines
    DATA_STRUCTURES.md         the structs that must be ported, field by field
    FUNCTION_INVENTORY.md      mechanical inventory of 814 x264 C functions + oracle status
    ORACLE_DESIGN.md           how the phoenix oracle must be structured
    SHADER_MAPPING.md          CPU function -> shader classification (the main goal)
    PORTING_ROADMAP.md         ordered work packages and their acceptance gates
```

`reference/x264/` is the **authoritative** copy for this branch.
Do not read a different x264 tree when porting: the rest of the repository
contains two other copies (`references/x264-master`, `build/x264-ref-src`)
that have different configurations. All line numbers in these documents refer
to `reference/x264/`.

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
5. **A PR must include** the x264 source lines followed, the oracle diff, the
   gate output, and the parity metric before/after.

## Existing gates (must stay green while porting)

```
build/audit_presets.py        12/120 byte-exact, NAL2 3/20
build/progress_metrics.py     84/30/41/41/41/41/42/42 first-diff bytes
build/recon_pair.py c004      0/4096
build/compare_slicetype.py    16/16 frame types
build/cavlc_parity_sweep.py   1360/1360
ctest --test-dir Vk264-rewrite/build   29/29
ctest --test-dir build                 65/65
build/extended_audit.py       0/240 byte-exact (the real target)
```

Phoenix is expected to preserve these while it grows; the success criterion is
that the extended map reaches byte-exact parity for every preset, plus a clean
function-for-function correspondence that can be handed to the shader port.
