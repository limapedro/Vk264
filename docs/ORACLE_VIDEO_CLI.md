# Oracle video CLI (`tools/oracle_video.py`)

`oracle_video.py` is a **stdlib-only Python front end** that makes the oracle
usable on **any video file the user provides** — mp4, mkv, webm, mov, avi —
instead of requiring a pre-made raw YUV fixture. It is a pipeline driver, not a
new encoder, and it makes **no parity claim**.

Run it from the repository root:

```powershell
python tools/oracle_video.py --help
```

## Why ffmpeg is used, and what it is used for

The oracle CLIs consume **raw I420 YUV** only:

- Phoenix: `build/tests/Release/phx_cli.exe`
  `<yuv> <w> <h> <qp> <preset> <cabac|cavlc> <out.h264> [--bitrate <kbps>]`
- Reference: `build/x264_ref/x264.exe`
  `--input-res WxH --input-csp i420 --fps N --qp N --keyint 30 --min-keyint 30
  --scenecut 0 --preset P --cabac|--no-cabac <yuv> -o <out.h264>`

Neither reads containers. **ffmpeg is used only for container decode** (and for
the optional decode-verify step). It extracts video stream 0 as planar I420;
the H.264 bitstream is produced by the chosen oracle CLI. ffmpeg never encodes
H.264 in this pipeline, so the artifact under test remains the Phoenix port (or
the authoritative x264 reference via `--x264`).

ffprobe supplies width/height/fps when they are not given explicitly.

## Decode -> encode -> verify pipeline

1. **Probe** — `ffprobe -v error -select_streams v:0 -show_entries
   stream=width,height,avg_frame_rate,r_frame_rate,nb_frames,duration -of json`
   on the input. Probed fps falls back to 25 if unavailable (the parity harness
   and the Phoenix CLI both use 25).
2. **Decode** — `ffmpeg -v error -y -i INPUT -map 0:v:0 -an -sn -dn`
   with a frame-sync flag (`-fps_mode passthrough` on modern ffmpeg, `-vsync 0`
   on older builds), optional `-frames:v N`, `-pix_fmt yuv420p -f rawvideo` into
   a temporary `input.i420.yuv`. If `--width/--height` differ from the source,
   the default is a **center crop** (`--fit crop`, preserves source pixels);
   `--fit scale` resamples instead. When no size is given the native size is
   decoded with no filter. The temporary YUV is deleted in a `finally` block
   even on failure (`--keep-temp` retains it for debugging).
3. **Encode** — invoke x264 (with `--x264`) or `phx_cli` with the *same*
   width/height/frame count/preset/entropy and either `--qp` or `--bitrate`.
   Frame count defaults to all frames. `qp` defaults to 28.
4. **Verify** (on by default; `--no-verify` disables) — decode the Annex-B
   stream with `ffmpeg -v error -i OUT.h264 -f null -`; success means the stream
   is decodable. No container is muxed by the verifier.
5. **Report** — print stream size and NAL count (Annex-B start-code scan) and
   write `build/oracle_video/report.json` (override with `--report`). The JSON
   contains the probe, geometry, decode, encode, stream, verify, warnings,
   status and error fields, plus `"parity_claim": false`.

Exit codes: `0` success, `1` encode/verify failure, `2` bad arguments, missing
binary, or unsupported Phoenix bitrate mode.

## Exact usage

```
oracle_video.py [-h] [--out-dir OUT_DIR] [--width WIDTH] [--height HEIGHT]
                [--fit {crop,scale}] [--fps FPS] [--frames FRAMES]
                [--preset {ultrafast,superfast,veryfast,faster,fast,medium,
                           slow,slower,veryslow,placebo}]
                [--entropy {cabac,cavlc}] [--qp QP | --bitrate KBPS]
                [--x264] [--verify] [--no-verify] [--ffmpeg FFMPEG]
                [--ffprobe FFPROBE] [--x264-exe X264_EXE]
                [--phx-exe PHX_EXE] [--report REPORT] [--timeout TIMEOUT]
                [--keep-temp]
                video [output]
```

- `video` — required input path. Listed extensions are mp4/mkv/webm/mov/avi;
  others are attempted with a warning.
- `output` — optional. A `.h264`/`.264` suffix means an exact output file;
  otherwise it is treated as a directory name. Default:
  `build/oracle_video/<stem>-<encoder>-<preset>-<entropy>-<qp28|bitrateNk>.h264`.
  `--out-dir DIR` is an alternative way to specify the directory.
- `--width`/`--height` — output I420 size; default: probed source size.
- `--fit crop|scale` — how to reconcile a requested size with the source
  (default `crop`).
- `--fps N` — default: probed from the source, fallback 25. **Phoenix has no
  fps argument**: its VUI is fixed at 25, so a non-25 request produces a
  warning in the report; x264 receives the requested fps.
- `--frames N` — default: all frames. Use it for quick smoke tests.
- `--qp N` (0..51, default 28) or `--bitrate KBPS` — mutually exclusive.
  In Phoenix bitrate mode `qp` is still passed as the positional fallback.
- `--x264` — use the authoritative reference instead of Phoenix.
- `--verify` (default) / `--no-verify`, `--timeout` (per tool, default 1800s).
- `--ffmpeg`, `--ffprobe`, `--x264-exe`, `--phx-exe` — binary overrides.
  `X264_EXE` / `PHX_EXE` environment variables are honoured as defaults.
- `--report PATH` — default `build/oracle_video/report.json`.
- `--keep-temp` — retain the temporary I420 YUV instead of cleaning it up.

Examples:

```powershell
# Authoritative reference, one preset, 30 frames, fixed QP
python tools/oracle_video.py D:/Videos/clip.mp4 --x264 --width 64 --height 64 --frames 30 --preset veryfast --qp 28 --entropy cabac

# Phoenix, explicit output directory, native size
python tools/oracle_video.py clip.mkv --out-dir build/oracle_video --frames 60 --qp 28

# Bitrate target with the reference, explicit output file
python tools/oracle_video.py clip.webm --bitrate 6000 ref.h264
```

## Verification performed

The required smoke test used a real small video from `D:\Videos` and the
authoritative reference (Phoenix is **not** claimed to work):

```
> python tools/oracle_video.py "D:/Videos/ssstwitter.com_1748999992229.mp4" --x264 --width 64 --height 64 --frames 30 --preset veryfast --qp 28 --entropy cabac
input:  D:\Videos\ssstwitter.com_1748999992229.mp4
decode: ffmpeg -> I420 64x64 @ 30 fps (crop)
encode: x264 (authoritative reference) --preset veryfast --cabac --qp 28
stream: E:\__PROJECTS__\Vk264\build\oracle_video\ssstwitter.com_1748999992229-x264-veryfast-cabac-qp28.h264 (2299 bytes, 33 NALs)
verify: ffmpeg -f null - : OK
report: E:\__PROJECTS__\Vk264\build\oracle_video\report.json
EXIT=0
```

Bitrate mode with x264 was also exercised successfully:

```
> python tools/oracle_video.py "D:/Videos/ssstwitter.com_1748999992229.mp4" --x264 --width 64 --height 64 --frames 30 --bitrate 6000 --preset ultrafast --entropy cavlc
stream: ...-x264-ultrafast-cavlc-bitrate6000k.h264 (50040 bytes, 33 NALs)
verify: ffmpeg -f null - : OK
EXIT=0
```

The Phoenix bitrate guard was exercised with a stand-in CLI that does not
advertise `--bitrate` (so this is a test of the guard, **not** a Phoenix result):

```
ERROR: Phoenix CLI does not support --bitrate (its usage shows only the fixed-QP form).
  U1 is adding bitrate support to tools/phx_cli.c: rebuild build/tests/Release/phx_cli.exe and retry.
  Refusing to silently fall back to QP. Use --qp, or run the same command with --x264 (authoritative reference).
EXIT=2
```

The guard probes the CLI's own usage output at runtime; it never silently
substitutes QP for an unsupported bitrate request.

## Current limitations (no parity claim)

- **Phoenix bitrate support is being added by U1.** If the built `phx_cli.exe`
  does not advertise `--bitrate`, the wrapper refuses to encode in bitrate mode
  with an actionable error (rebuild the CLI, use `--qp`, or use `--x264`). A
  rebuilt CLI that advertises `--bitrate` is used directly.
- **Phoenix at 640x360 is currently broken**: `phx_encoder_open` fails with
  `encoder open failed` (see `docs/VIDEO_CORPUS.md`, "6000 kbps real-video
  test"). The wrapper reports the encoder failure verbatim; it does not work
  around it or claim a successful encode.
- **Phoenix fps is fixed at 25**; `x264` honours the probed/requested fps.
- Because of the two points above, this tool is **not a parity test**. It
  produces streams and stream facts; byte-exact comparison against x264 is a
  separate concern (`tools/parity_harness.py`). Nothing here establishes that
  Phoenix matches x264.

Only the Python standard library is used (`argparse`, `json`, `subprocess`,
`pathlib`, `tempfile`, `fractions`, etc.). No pip packages are required.
