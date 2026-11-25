# Vk264 Phoenix

A from-scratch, function-for-function CPU port of x264. This repository **is**
the Phoenix project: the old incremental oracle, its harnesses and its branch
zoo were removed from this branch. `vk264-phoenix` is the single, default
branch. This CPU reference is what the eventual **shader version of Vk264**
will be derived from.

## Layout

```
.                            repository root (= the old vk264-phoenix/ tree)
  CMakeLists.txt             build definition for the library + tests
  README.md                  this file
  reference/x264/            176 source files copied from the exact x264 build
                             used for all parity measurements (authoritative)
  src/                       the new oracle source (Phoenix port)
    phx_param.h/.c           x264_param_t defaults + 10 presets  [WP1 — done]
  tests/                     unit tests for ported functions
    test_param.c             10-preset parameter parity          [passing]
  tools/
    gen_function_inventory.py   regenerate docs/FUNCTION_INVENTORY.md
    run_tests.py               configure + build + ctest (CMake)
    close_branches.py          one-time branch cleanup utility
  docs/
    README.md                  mission, rules, gates
    X264_PIPELINE.md           how x264 encodes, function by function
    DATA_STRUCTURES.md         structs that must be ported, field by field
    FUNCTION_INVENTORY.md      mechanical inventory of 814 x264 C functions + status
    ORACLE_DESIGN.md           required structure and API of the new oracle
    SHADER_MAPPING.md          CPU->shader classes A/B/C (the main goal)
    PORTING_ROADMAP.md         WP0-WP9 with acceptance gates
```

## Current state

- **WP0 complete**: x264 reference copied, seven documents written, mechanical
  function inventory generated (814 functions), old repository contents removed.
- **WP1 in progress**: `phx_param` ported from `reference/x264/common/base.c`;
  all 10 presets verified (`OK: 10 presets, 0 failures`).
- Historical baseline from the discarded incremental oracle: 12/120 short-clip
  byte-exact, 0/240 extended, first-diff 84/30/41/42. Its harnesses are gone
  from this branch (they remain in git history); Phoenix will rebuild parity
  checks as part of each work package.

## Rules

1. Port x264's functions verbatim — including its data structures, tables and
   state ownership. No approximations to make a metric move.
2. Every function gets a doc comment with its x264 source lines and a
   `SHADER: A/B/C` classification (see `docs/SHADER_MAPPING.md`).
3. Every ported function gets a unit test comparing directly with x264.
4. Work packages are sequential; see `docs/PORTING_ROADMAP.md`.

## Verify

```
python tools/gen_function_inventory.py
python tools/run_tests.py
```
