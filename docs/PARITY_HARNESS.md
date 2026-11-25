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
- Real-video fixtures: `tools/make_video_fixtures.py` crops four MP4s from
  `D:\Videos` into deterministic 64x64 / 60-frame I420 files, listed in
  `build/parity/video_manifest.json`.
- Phoenix encoder: `PHX_EXE=...` (not available until the encoder modules are
  ported). Without it, the harness only builds the reference corpus.

## What it does

1. Writes a deterministic fixture (`build/parity/fixture.yuv`): 64x64, 8
   frames, I420, luma `(x*7 + y*13 + f*29) & 255`.
2. For each fixture, each of the 10 presets and both entropies, runs x264 with
   `--qp 28 --keyint 30 --min-keyint 30 --scenecut 0` and an **explicit**
   `--cabac` / `--no-cabac`. Never rely on preset defaults for entropy mode.
3. Stores `build/parity/<fixture>-<preset>-<entropy>.ref.h264`.
4. If `PHX_EXE` is set, encodes the same fixture with Phoenix, compares bytes,
   records `byte_exact` and the first differing byte offset in
   `build/parity/parity_report.json`.
5. Success requires byte-exact parity for every case; any mismatch is a failure.

## Current status (measured)

```
python tools/build_reference_x264.py   -> built build/x264_ref/x264.exe
python tools/make_video_fixtures.py    -> 4 real-video 64x64/60f YUV fixtures
python tools/parity_harness.py         -> 100 reference streams, all x264 rc=0
                                          byte-exact 0/100 (no PHX_EXE yet)
```

The 100 cases are 5 fixtures (synthetic + four `D:\Videos` crops) × 10 presets
× 2 entropies. No Phoenix encoder exists yet, so 0/100 is a corpus-generation
result, not a Phoenix parity result. The historical baseline from the
discarded oracle was 12/120 short-clip and 0/240 extended; Phoenix starts from
zero and must earn every case back.

Raw source lists and MSVC defines for the reference build are in
`tools/build_reference_x264.py` — do not compile by globbing all `.c` files:
that pulls in PPC/ARM/ffms/lavf/gpac sources and does not reproduce the binary.

## Gates after the encoder lands

```
PHX_EXE=... python tools/parity_harness.py   -> 100/100 byte-exact
python tools/run_tests.py                    -> all unit tests
```

Longer/multi-region parity (the 240-case extended territory) will be a
follow-up harness using the same byte-comparison core.
