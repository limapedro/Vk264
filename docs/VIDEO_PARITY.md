# 6000-kbps video parity harness

`tools/video_parity.py` runs the user's explicit test: encode real videos with
the authoritative x264 reference **and** with Phoenix at **6000 kb/s**, for
**all 10 presets × both entropy modes** (CABAC and CAVLC), save every stream in
the workspace, and report **byte-exact** parity — plus rc, byte counts, Annex-B
NAL counts/types, first differing byte offset and achieved average bitrate.

It uses only the Python standard library. It never claims parity from similar
bitrate, similar size, successful decode, or matching NAL counts: **only byte
identical `.h264` files count**.

## Usage

```powershell
python tools/video_parity.py --help
python tools/video_parity.py --input D:/Videos/clip.mp4
python tools/video_parity.py --input build/video_encode_test/<clip>_640x360_60f.yuv --x264-only
python tools/video_parity.py --all-videos --max-videos 3
```

Key options (all optional except one input source):

| Flag | Meaning |
| --- | --- |
| `--input FILE` | Video file (decoded with ffmpeg/ffprobe) or an existing raw I420 `.yuv` file. |
| `--all-videos` | Sweep a deterministic few files from `D:\Videos` (smallest 200 KB–12 MB files, ties by name). |
| `--max-videos N` | How many files `--all-videos` selects (default 3). |
| `--raw` | Treat `--input` as I420 even without a `.yuv` suffix. |
| `--width/--height/--fps/--frames` | Decode/encode geometry and length (default `640 360 25 60`). Width/height must be even. |
| `--bitrate N` | Target kb/s for both encoders (default **6000**). |
| `--qp N` | Positional fallback QP that `phx_cli` still requires in bitrate mode (default 28). |
| `--gop phx\|x264-default` | GOP policy; see below (default `phx`). |
| `--x264-only` | Produce only x264 references; phx cases are `SKIP` (used for verification). |
| `--out-dir DIR` | Output root (default `build/video_parity`). |
| `--x264/--phx/--ffmpeg/--ffprobe` | Explicit tool paths. |
| `--timeout/--decode-timeout` | Per-encode / per-decode seconds (default 600 / 300). |
| `--warn-ratio F` | Warn when achieved/target average is below `F` (default 0.5). |

Tool resolution order: explicit flag → `X264_EXE` / `PHX_EXE` / `FFMPEG_EXE` /
`FFPROBE_EXE` → known build directories → `PATH`. Missing tools are reported,
never crash the run. Invalid explicit paths produce a warning and fall back to
auto-detection.

## Inputs

- **Video**: ffprobe records source metadata; ffmpeg decodes exactly
  `--frames` pictures with `fps=<fps>,scale=<w>:<h>:flags=bicubic` and
  `pix_fmt=yuv420p` to `<out-dir>/<video>/input.yuv`. Other streams are dropped
  (`-map 0:v:0 -an -sn -dn`). Short sources yield fewer frames; the actual
  count is used for encoding and for the achieved-bitrate calculation.
- **Raw I420**: the source is read in place if it contains exactly `--frames`
  pictures. If it is longer, exactly `--frames` pictures are copied to
  `<out-dir>/<video>/input.yuv`; the original is never modified. Fewer frames
  is a `BLOCKED` input.

## Encoding contract

For each video × preset × entropy the harness runs (default `--gop phx`):

```text
x264 --input-res <w>x<h> --input-csp i420 --fps <fps> --frames <n> \
     --bitrate 6000 --keyint 30 --min-keyint 30 --scenecut 0 \
     --cabac|--no-cabac --preset <preset> <input.yuv> -o <case>.ref.h264

phx_cli <input.yuv> <w> <h> <qp> <preset> cabac|cavlc \
        <case>.phx.h264 --bitrate 6000
```

The extra x264 GOP flags are intentional: `tools/phx_cli.c` pins
`keyint=30`, `min-keyint=30` and `scenecut=0`, so without them byte-exact
parity is impossible by construction. `--gop x264-default` omits them and
reproduces the older plain-ABR command behind
`build/video_encode_test/*-6000k.ref.h264`; those archived streams are **not**
a parity contract with the current `phx_cli`. `phx_cli` has no fps argument and
hardcodes 25 fps in its ABR rate control, so a non-25 `--fps` marks phx cases
`BLOCKED` rather than pretending the comparison is meaningful.

Phoenix bitrate capability is probed before encoding: the CLI usage output must
advertise `--bitrate` **and** an 8-frame 64×64 smoke encode must succeed. If
Phoenix is absent or bitrate mode has not landed, every phx case is recorded as
`BLOCKED` with the reason, `byte_exact` stays `null`, and the full x264 reference
corpus is still produced. `--x264-only` uses `SKIP` with the same no-claim rule.
A non-zero encoder rc, timeout, or missing stream is `REF_FAIL` / `PHX_FAIL` /
`PHX_TIMEOUT`; a produced-but-different phx stream is `DIFF` with its first
differing byte offset.

## Output layout

```text
build/video_parity/
  <video>/
    input.yuv                       # decoded or trimmed I420 (when needed)
    <preset>-<entropy>.ref.h264     # x264 stream
    <preset>-<entropy>.phx.h264     # Phoenix stream (if run)
  report.json                       # machine-readable, all cases + summaries
  report.md                         # human-readable tables + warnings
  _probe/                           # phx --bitrate smoke evidence
```

Each case in `report.json` records both full commands, rc, byte counts, elapsed
seconds, `ref_nal_count` / `phx_nal_count`, `ref_nal_types` / `phx_nal_types`
(keyed by Annex-B NAL type code, e.g. `5`=IDR, `1`=slice, `6`=SEI, `7`=SPS,
`8`=PPS), a compact RLE NAL sequence, `byte_exact`, `first_diff`
(`-1` when identical), achieved kb/s, achieved percentage of target and status.
`report.md` has per-video, per-preset, per-entropy and per-case tables.

Exit codes: `0` report written with no encode failure or byte mismatch
(blocked/skipped phx cases are allowed but explicitly **not** a parity claim),
`1` an encode failed or any produced phx stream differs, `2` x264 reference
binary unavailable or no usable input.

## Target 6000 kb/s vs achieved average: important caveat

The harness requests `--bitrate 6000` from both encoders, but it reports the
**achieved average** as `bytes * 8 / (frames / fps)` in kb/s. A short clip
cannot reach steady-state ABR: x264 needs time to converge, so a 60-frame
clip at 25 fps lasts only 2.4 s.

Measured with the verification run below (60 frames, 640×360, 25 fps, 20
x264 references), achieved averages ranged from **405.94 to 838.91 kb/s**,
i.e. roughly **7–14% of the 6000 kb/s target**, for every case. The harness
warns on stdout and in `report.md` whenever achieved is below 50% of target
(configurable with `--warn-ratio`). This warning is a measurement caveat:
it is **not** evidence that bitrate mode failed, and it is **not** a parity
result. Use longer clips for bitrate-accuracy checks; use byte identity for
parity checks. Neither is inferred from the other.

## Verification run (x264 only, existing YUV)

Command and output (abbreviated only in the middle case table; all 20 cases
were produced):

```text
> python tools/video_parity.py --input build/video_encode_test/ssstwitter.com_1731881880284_640x360_60f.yuv --x264-only
x264: build\x264_ref\x264.exe
phx:  build\tests\Release\phx_cli.exe (not probed (--x264-only))
20 cases; x264 rc=0: 20/20; phx rc=0: 0/20; byte-exact: 0/0 compared
NO byte-exact parity claim: only identical byte streams count; blocked/skipped/failed cases are not parity.

  video                  preset     ent        refB      phxB   NAL ref/phx     kb/s  status
  ssstwitter.com_1731881 ultrafast  cabac    219598         0          65/0   731.99  SKIP
  ssstwitter.com_1731881 ultrafast  cavlc    251672         0          65/0   838.91  SKIP
  ssstwitter.com_1731881 superfast  cabac    150307         0          65/0   501.02  SKIP
  ssstwitter.com_1731881 superfast  cavlc    165713         0          65/0   552.38  SKIP
  ... (veryfast, faster, fast, medium, slow, slower, veryslow, placebo; both entropies) ...
  ssstwitter.com_1731881 placebo    cabac    122288         0          65/0   407.63  SKIP
  ssstwitter.com_1731881 placebo    cavlc    132265         0          65/0   440.88  SKIP

WARNING: 20/20 cases achieved far below the 6000 kb/s target. Short clips (60 frames at 25 fps = 2.4 s) cannot reach steady-state ABR; see report.md.

reports: build\video_parity\report.json, build\video_parity\report.md
```

Result: 20/20 x264 references encoded (`rc=0`), 20 `.ref.h264` streams saved
under `build/video_parity/ssstwitter.com_1731881880284_640x360_60f/`, zero phx
cases run (as requested), **zero byte-exact claims**. Full output is in
`build/video_parity/report.json` and `build/video_parity/report.md`.

## Limitations

- The harness is a measurement/reporting tool; it does not fix parity failures.
  A full two-encoder run can legitimately exit 1 because streams differ.
- Byte-exact parity needs identical encoder configuration everywhere, including
  x264 SEI/option strings and GOP policy. Matching the target bitrate or NAL
  counts does not imply byte identity.
- `phx_cli` accepts only 25 fps in ABR mode, so other frame rates are blocked
  for phx comparison; the x264 references are still produced.
- ffmpeg/ffprobe are optional only for raw YUV input; video input requires both.
- All child processes have timeouts; timed-out cases are recorded, not retried.
