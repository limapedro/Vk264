# Byte-exact parity harness (Phoenix)

The authoritative check is a **byte-for-byte** comparison of Annex-B streams,
not decode plausibility. This harness replaces the discarded ad-hoc audit
scripts with one deterministic, preset-complete procedure.

## Location and inputs

- Script: `tools/parity_harness.py`
- Authoritative encoder: the executable built from `reference/x264/`
  (8-bit 4:2:0, the exact configuration used for all historical measurements).
  Supply it with `X264_EXE=...`; the script exits 2 with a clear message when
  it is unavailable, so it is **not** part of `ctest`.
- Phoenix encoder: `PHX_EXE=...` (not available until the encoder modules are
  ported). Without it, the harness only builds the reference corpus.

## What it does

1. Writes a deterministic fixture (`build/parity/fixture.yuv`): 64x64, 8
   frames, I420, luma `(x*7 + y*13 + f*29) & 255`.
2. For each of the 10 presets and both entropies — 20 cases — runs x264 with
   `--qp 28 --keyint 30 --min-keyint 30 --scenecut 0` and an **explicit**
   `--cabac` / `--no-cabac`. Never rely on preset defaults for entropy mode.
3. Stores `build/parity/<preset>-<entropy>.ref.h264`.
4. If `PHX_EXE` is set, encodes the same fixture with Phoenix, compares bytes,
   records `byte_exact` and the first differing byte offset in
   `build/parity/parity_report.json`.
5. Success requires 20/20 byte-exact; any mismatch is a failure.

## Current status

- Harness code and fixture generation are implemented.
- Reference corpus generation is blocked only by the absence of the x264
  executable in this repository. It can be supplied externally (`X264_EXE`)
  or built with the project's normal x264 configure/MSVC flow — do **not**
  compile the reference by globbing all `.c` files: that pulls in platform
  sources (PPC/ARM/ffms/lavf/gpac) and does not reproduce the authoritative
  binary.
- No Phoenix encoder exists yet, so no parity number can be reported. The
  historical baseline from the discarded oracle was 12/120 short-clip and
  0/240 extended; Phoenix starts from zero and must earn every case back.

## Gates after the encoder lands

```
X264_EXE=... PHX_EXE=... python tools/parity_harness.py   -> 20/20 byte-exact
python tools/run_tests.py                                 -> all unit tests
```

Longer/multi-region parity (the 240-case extended territory) will be a
follow-up harness using the same byte-comparison core.
