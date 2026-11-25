# Byte-exact parity harness (Phoenix)

The authoritative check is a **byte-for-byte** comparison of Annex-B streams,
not decode plausibility. This harness replaces the discarded ad-hoc audit
scripts with one deterministic, preset-complete procedure.

## Location and inputs

- Script: `tools/parity_harness.py`
- Authoritative encoder: `tools/build_reference_x264.py` builds the 8-bit
  C-only reference from `reference/x264/` into `build/x264_ref/x264.exe` with
  the copied `config.h`; the harness auto-detects it (or `X264_EXE=...`). It
  exits 2 with a clear message when absent, so it is **not** part of `ctest`.
- Fixtures: the 100-case corpus is **5 fixtures × 10 presets × 2 entropies**.
  1. `synth`: deterministic 64×64, 8-frame I420 pattern
     (`(x*7 + y*13 + f*29) & 255`) written to `build/parity/fixture.yuv`.
  2. Four real-video crops from `D:\Videos`, created by
     `tools/make_video_fixtures.py`: deterministic 64×64 / 60-frame I420
     centre crops of four MP4s, listed in `build/parity/video_manifest.json`.
- Presets: all 10 (`ultrafast`, `superfast`, `veryfast`, `faster`, `fast`,
  `medium`, `slow`, `slower`, `veryslow`, `placebo`).
- Entropies: CABAC and CAVLC, always requested **explicitly** with `--cabac`
  or `--no-cabac`; never rely on preset defaults for entropy mode.
- Phoenix encoder: `tools/phx_cli.c` builds to
  `build/tests/Release/phx_cli.exe`, which the harness auto-detects. An
  explicit `PHX_EXE=/path/to/phx_cli.exe` always takes precedence; the search
  then checks `build/tests/Release/`, the repo root, `build/`,
  `build/x264_ref/`, `reference/` and `C:/x64/bin`. Without it, the harness
  only builds the reference corpus and reports `"encoder not available"` for
  every case — that is **not** a failure (exit 0), and is not a parity claim.

## Phoenix CLI contract

The harness invokes the encoder once per case as:

```
phx_cli <yuv> <width> <height> <qp> <preset> <cabac|cavlc> <out.h264>
```

For example, `phx_cli build/parity/fixture.yuv 64 64 28 veryfast cabac
build/parity/synth-veryfast-cabac.phx.h264`. The entropy argument is the
lowercase label `cabac` or `cavlc` (the harness maps `--no-cabac` to `cavlc`).
`tools/phx_cli.c` must accept this positional form for the parity gate to run.

## What it does

1. Writes/regenerates the synthetic fixture and reads the real-video manifest.
2. For every fixture, every one of the 10 presets and both entropies, runs
   x264 with the unchanged authoritative flags `--input-res 64x64
   --input-csp i420 --fps 25 --frames <n> --qp 28 --keyint 30 --min-keyint 30
   --scenecut 0 --preset <preset> --cabac|--no-cabac`.
3. Stores `build/parity/<fixture>-<preset>-<entropy>.ref.h264` (100 streams).
4. If a Phoenix encoder is available, encodes the same fixture to
   `build/parity/<case>.phx.h264` and byte-compares it with the reference,
   recording `phx_rc`, byte counts, `byte_exact`, and the **first differing
   byte offset** (`-1` when identical; the shorter length when one stream is
   a prefix of the other).
5. Writes two reports: `build/parity/parity_report.json` (machine-readable)
   and `tests/parity_report.md` (human-readable). Both include all 100
   per-case rows plus per-fixture and per-preset summaries (cases, x264 rc=0,
   byte-exact count, mismatches, earliest first-diff offset).
6. Exit codes: `0` = corpus built and either no Phoenix encoder or 100/100
   byte-exact; `1` = an x264 encode failed or Phoenix streams mismatch;
   `2` = x264 reference binary unavailable.

## Current status (measured)

```
python tools/build_reference_x264.py   -> built build/x264_ref/x264.exe
python tools/make_video_fixtures.py    -> 4 real-video 64x64/60f YUV fixtures
python tools/parity_harness.py         -> 100 reference streams, all x264 rc=0
                                          byte-exact 0/100 (encoder not available)
```

The 100 cases are 5 fixtures (synthetic + four `D:\Videos` crops) × 10
presets × 2 explicit entropies. No Phoenix encoder has produced streams yet,
so 0/100 is a corpus-generation result, **not** a Phoenix parity result. The
historical baseline from the discarded oracle was 12/120 short-clip and 0/240
extended; Phoenix starts from zero and must earn every case back.

Raw source lists and MSVC defines for the reference build are in
`tools/build_reference_x264.py` — do not compile by globbing all `.c` files:
that pulls in PPC/ARM/ffms/lavf/gpac sources and does not reproduce the binary.

## Standalone stream comparison

`tools/compare_streams.py` compares a directory of Phoenix `.phx.h264` streams
with the saved refs without needing the harness or its x264 build:

```
python tools/compare_streams.py                       # build/parity by default
python tools/compare_streams.py <stream-dir> --refs build/parity
python tools/compare_streams.py <stream-dir> --mismatches-only
python tools/compare_streams.py <stream-dir> --json
```

It prints a compact per-case table (ref bytes, Phoenix bytes, first-diff
offset, status) plus totals; it accepts partial directories (missing streams
are reported, not counted as mismatches) and exits 1 on any byte mismatch.

## Gates after the encoder lands

```
python tools/parity_harness.py             -> 100/100 byte-exact (auto-detects
                                              build/tests/Release/phx_cli.exe)
python tools/compare_streams.py build/parity -> compact 100-case table
python tools/run_tests.py                  -> all unit tests
```

Longer/multi-region parity (the 240-case extended territory) will be a
follow-up harness using the same byte-comparison core.
