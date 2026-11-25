# Vk264 Phoenix

A from-scratch, function-for-function CPU port of x264. This is the reference
that the eventual **shader version of Vk264** will be derived from.

## Layout

```
vk264-phoenix/
  README.md            this file
  reference/x264/      176 source files copied from the exact x264 build used
                       for all parity measurements (authoritative)
  src/                 the new oracle source (Phoenix port)
    phx_param.h/.c     x264_param_t defaults + 10 presets  [WP1 — done]
  tests/               unit tests for ported functions
    test_param.c       10-preset parameter parity         [passing]
  tools/
    gen_function_inventory.py   regenerate docs/FUNCTION_INVENTORY.md
    run_tests.py               build + run unit tests (MSVC)
  docs/
    README.md            mission, rules, gates
    X264_PIPELINE.md     how x264 encodes, function by function
    DATA_STRUCTURES.md   structs that must be ported, field by field
    FUNCTION_INVENTORY.md  814 x264 functions + oracle status
    ORACLE_DESIGN.md     required structure and API of the new oracle
    SHADER_MAPPING.md    CPU->shader classes A/B/C (the main goal)
    PORTING_ROADMAP.md   WP0-WP9 with acceptance gates
```

## Current state

- **WP0 complete**: branch created, x264 reference copied, all seven documents
  written, mechanical function inventory generated (814 functions).
- **WP1 in progress**: `phx_param` ported from `reference/x264/common/base.c`;
  all 10 presets verified against the x264 parameter matrix (`OK: 10 presets,
  0 failures`).
- No other source ported yet. The old oracle (`Vk264-rewrite`) remains the
  verification baseline: 12/120 short-clip byte-exact, 0/240 extended, all
  gates green.

## Rules

1. Port x264's functions verbatim — including its data structures, tables and
   state ownership. No approximations to make a metric move.
2. Every function gets a doc comment with x264 source lines and a
   `SHADER: A/B/C` classification (see `docs/SHADER_MAPPING.md`).
3. Every ported function gets a unit test comparing directly with x264.
4. Work packages are sequential; see `docs/PORTING_ROADMAP.md`.

## Verify

```
python vk264-phoenix/tools/gen_function_inventory.py
python vk264-phoenix/tools/run_tests.py
```
