# Vk264 Phoenix

![Phoenix parity progress — x264 weapon level](docs/phoenix-progress.svg)

A from-scratch, function-for-function CPU port of x264. This repository **is**
the Phoenix project: the old incremental oracle and its harnesses were removed from this branch.
`vk264-phoenix` is the single, default branch; the 362 old branches were
**archived as tags** (`archive/pr<N>-<branch>` and `archive/pre-phoenix-main`)
by `tools/archive_old_branches.py`, so nothing was lost. This CPU reference is
what the eventual **shader version of Vk264** will be derived from.

## Layout

```
.                            repository root (= the old vk264-phoenix/ tree)
  CMakeLists.txt             build definition for the library + tests
  README.md                  this file
  reference/x264/            176 source files copied from the exact x264 build
                             used for all parity measurements (authoritative)
  src/                       the new oracle source (Phoenix port)
    phx_param.h/.c           x264_param_t defaults + 10 presets  [WP1 — done]
    phx_tables.h/.c          generated x264 tables (lambda, ue, chroma) [done]
    phx_cost.h/.c            analyse.c cost tables + QP init      [WP1 — done]
    phx_frame.h/.c           frame allocation + lowres planes     [WP1 — done]
    phx_mbcache.h/.c         scan8 + progressive MB cache load/save [WP2 — done]
    phx_bitstream.h/.c       bs_* Golomb writer + NAL escape        [done]
    phx_cabac.h/.c           CABAC arithmetic coder + context init  [done]
    phx_headers.h/.c         SPS/PPS init + write + NAL assembly    [in progress]
  tests/                     unit tests for ported functions
    test_param.c             10-preset parameter parity          [passing]
    test_cost.c              cost tables against x264 goldens    [passing]
    test_frame.c             lowres planes + border expansion    [passing]
    test_mbcache.c           scan8 + cache load/save round-trip  [passing]
    test_bitstream.c         Exp-Golomb patterns + NAL escape     [passing]
    test_cabac.c             decision/bypass vs x264 golden bytes [passing]
    tests/compare_headers.py SPS 20/20 exact; SPS+PPS 2/20 exact
  tools/
    gen_function_inventory.py   regenerate docs/FUNCTION_INVENTORY.md
    gen_tables.py              regenerate src/phx_tables.{h,c} from x264 source
    make_video_fixtures.py      crop D:\Videos MP4s into raw YUV fixtures
    parity_harness.py          20-case byte-exact x264 comparison
    run_tests.py               configure + build + ctest (CMake)
    close_branches.py          one-time branch cleanup utility
    archive_old_branches.py    one-time archival of old branches as tags
  docs/
    README.md                  mission, rules, gates
    X264_PIPELINE.md           how x264 encodes, function by function
    DATA_STRUCTURES.md         structs that must be ported, field by field
    FUNCTION_INVENTORY.md      mechanical inventory of 1386 x264 C functions + status
    ORACLE_DESIGN.md           required structure and API of the new oracle
    SHADER_MAPPING.md          CPU->shader classes A/B/C (the main goal)
    PARITY_HARNESS.md          byte-exact comparison procedure and status
    PORTING_ROADMAP.md         WP0-WP9 with acceptance gates
```

## Current state

- **WP0 complete**: x264 reference copied, seven documents written, mechanical
  function inventory generated (1386 functions), old repository contents removed.
- **WP1 complete**: `phx_param` (10/10 presets), the analyse cost tables, and
  frame allocation / `x264_frame_init_lowres` are ported with source-line
  comments; x264 tables are generated mechanically. All unit tests pass
  (`ctest`: 6/6).
- **Parity harness rebuilt**: `tools/parity_harness.py` compares 10 presets ×
  2 entropies byte-for-byte once `X264_EXE` (and later `PHX_EXE`) is supplied;
  see `docs/PARITY_HARNESS.md`. No Phoenix encoder exists yet, so no byte-exact
  number can be reported — the historical 12/120 short and 0/240 extended are
  context, not Phoenix results.
- MBAFF cache paths ported (field-pair neighbour selection, cbp/MV/MVD
  conversion, skip flag, intra border backup) with unit tests.
- SPS writer is byte-exact for all 20 reference streams; SPS+PPS is 2/20
  exact. PPS differences are trailing word-flush padding in x264's shared
  bitstream buffer (`tests/compare_headers.py`).
- Next: finish SPS/PPS parity, then CABAC/CAVLC slice syntax and phx_cli.
- The CABAC coder matches x264 golden bytes; porting it also exposed and fixed
  a `PHX_CLIP3` argument-order bug in the cost tables.

## Rules

1. Port x264's functions verbatim — including its data structures, tables and
   state ownership. No approximations to make a metric move.
2. Every function gets a doc comment with its x264 source lines and a
   `SHADER: A/B/C` classification (see `docs/SHADER_MAPPING.md`).
3. Every ported function gets a unit test comparing directly with x264.
4. Work packages are sequential; see `docs/PORTING_ROADMAP.md`.

## Verify

```
python tools/gen_tables.py
python tools/gen_function_inventory.py
python tools/run_tests.py
```
