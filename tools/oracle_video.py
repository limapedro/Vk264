#!/usr/bin/env python3
"""Encode any container video through the Phoenix / x264 oracle CLI.

Why this wrapper exists
-----------------------
The oracle CLIs (``build/tests/Release/phx_cli.exe`` and
``build/x264_ref/x264.exe``) consume *raw I420 YUV*, not mp4/mkv/webm/mov/avi.
This script is a stdlib-only front end that:

  1. probes the source with ffprobe (width/height/fps when not given),
  2. decodes it to a temporary raw I420 YUV with ffmpeg,
  3. invokes the chosen H.264 encoder CLI with matched flags,
  4. optionally verifies the Annex-B stream by decoding it with
     ``ffmpeg -f null -``,
  5. writes a JSON report and removes the temporary YUV.

ffmpeg is used ONLY for container decode / verification.  The H.264 encoder is
the oracle CLI itself; this script never encodes pixels.  It makes no parity
claim: it encodes with one encoder per run and reports stream facts.

Usage examples
--------------
  # authoritative reference, one preset, all frames
  python tools/oracle_video.py D:/Videos/clip.mp4 --x264 --preset veryfast

  # fixed QP with Phoenix (default encoder)
  python tools/oracle_video.py clip.mkv --width 640 --height 360 \
      --fps 25 --frames 60 --qp 28 --entropy cabac

  # bitrate target with the reference; explicit output file and no verify
  python tools/oracle_video.py clip.webm --bitrate 6000 --preset medium \
      --out build/oracle_video/ref.h264 --no-verify

See docs/ORACLE_VIDEO_CLI.md for the full contract and limitations.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
from fractions import Fraction

REPO = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_OUT_DIR = REPO / "build" / "oracle_video"
DEFAULT_FFMPEG = pathlib.Path("C:/ffmpeg/ffmpeg-master-latest-win64-gpl/bin/ffmpeg.EXE")
DEFAULT_FFPROBE = pathlib.Path("C:/ffmpeg/ffmpeg-master-latest-win64-gpl/bin/ffprobe.EXE")
DEFAULT_X264 = REPO / "build" / "x264_ref" / "x264.exe"
DEFAULT_PHX = REPO / "build" / "tests" / "Release" / "phx_cli.exe"
DEFAULT_REPORT = DEFAULT_OUT_DIR / "report.json"
PRESETS = ("ultrafast", "superfast", "veryfast", "faster", "fast",
           "medium", "slow", "slower", "veryslow", "placebo")
VIDEO_EXTS = {".mp4", ".mkv", ".webm", ".mov", ".avi"}
START_CODE = re.compile(rb"\x00\x00\x01")


class OracleError(Exception):
    """Actionable, user-facing failure (exit 2)."""


def run(cmd, timeout):
    """Run a subprocess with captured text; raise OracleError on spawn/timeout."""
    try:
        return subprocess.run([str(c) for c in cmd], capture_output=True,
                              text=True, errors="replace", timeout=timeout)
    except FileNotFoundError as exc:
        raise OracleError(f"executable not found: {cmd[0]}\n  {exc}") from exc
    except subprocess.TimeoutExpired as exc:
        raise OracleError(f"timed out after {timeout}s: {cmd[0]}") from exc
    except OSError as exc:
        raise OracleError(f"cannot launch {cmd[0]}: {exc}") from exc


def tail(text, limit=1200):
    """Keep the useful tail of a tool's stderr/stdout."""
    text = (text or "").strip()
    return text[-limit:] if len(text) > limit else text


def ensure_exe(path, label):
    p = pathlib.Path(path)
    if not p.is_file():
        raise OracleError(
            f"{label} not found: {p}\n"
            f"  Pass --{label} PATH or build it. "
            "See docs/ORACLE_VIDEO_CLI.md.")
    return p.resolve()


def fps_text(value):
    """Render a float fps without trailing zeros (25, 29.97, 23.976, ...)."""
    return f"{float(value):.6f}".rstrip("0").rstrip(".")


def probe(video, ffprobe, report, timeout):
    """Probe stream 0:v with ffprobe; return dict or {} on failure."""
    cmd = [ffprobe, "-v", "error", "-select_streams", "v:0",
           "-show_entries",
           "stream=width,height,avg_frame_rate,r_frame_rate,nb_frames,duration",
           "-of", "json", video]
    r = run(cmd, timeout)
    info = {}
    if r.returncode == 0:
        try:
            streams = json.loads(r.stdout).get("streams", [])
            if streams:
                info = streams[0]
        except (ValueError, AttributeError):
            pass
    report["probe"] = {"cmd": [str(c) for c in cmd], "rc": r.returncode,
                       "info": info, "stderr": tail(r.stderr, 400)}
    return info


def probed_fps(info):
    for key in ("avg_frame_rate", "r_frame_rate"):
        raw = str(info.get(key, "") or "")
        if not raw or raw.startswith("0"):
            continue
        try:
            value = float(Fraction(raw))
            if 0 < value < 1000:
                return value
        except (ValueError, ZeroDivisionError):
            continue
    return None


def resolve_geometry(args, info, report):
    """Pick width/height/fps/crop, warning when overriding the source."""
    sw = int(info.get("width") or 0)
    sh = int(info.get("height") or 0)
    w = args.width or sw
    h = args.height or sh
    if not w or not h:
        raise OracleError(
            "could not determine video dimensions. Pass --width and --height "
            "(ffprobe returned no usable width/height).")
    if w % 2 or h % 2:
        raise OracleError(f"width and height must be even for I420 (got {w}x{h}).")
    fps = args.fps if args.fps is not None else probed_fps(info)
    if not fps or fps <= 0:
        fps = 25.0
        report.setdefault("warnings", []).append(
            "fps could not be probed; defaulted to 25 (parity harness uses 25).")
    if args.width is None and args.height is None:
        fit = "none"          # native size: no filter, pixel-exact decode
    elif args.fit == "scale":
        fit = "scale"         # user asked to resample
    elif sw and sh and w <= sw and h <= sh:
        fit = "crop"          # center crop preserves source pixels
    else:
        fit = "scale"         # requested size larger than source: must scale
        report.setdefault("warnings", []).append(
            f"requested {w}x{h} exceeds source {sw}x{sh}; using scale instead of crop.")
    crop = {"x": max(0, (sw - w) // 2), "y": max(0, (sh - h) // 2)} if fit == "crop" else None
    geometry = {"source": {"width": sw or None, "height": sh or None},
                "width": w, "height": h, "fps": fps, "fit": fit, "crop": crop}
    report["geometry"] = geometry
    return geometry


def detect_sync(ffmpeg, timeout, report):
    """ffmpeg >= 5 uses -fps_mode; older builds use -vsync 0."""
    r = run([ffmpeg, "-hide_banner", "-h", "full"], timeout)
    if "-fps_mode" in r.stdout:
        sync = ["-fps_mode", "passthrough"]
    else:
        sync = ["-vsync", "0"]
    report["ffmpeg_sync"] = sync
    return sync


def decode_to_yuv(video, ffmpeg, geo, frames, tmpdir, timeout, report, sync):
    """Decode video to a temporary raw I420 YUV file with ffmpeg."""
    yuv = pathlib.Path(tmpdir) / "input.i420.yuv"
    cmd = [ffmpeg, "-v", "error", "-y", "-i", video,
           "-map", "0:v:0", "-an", "-sn", "-dn"]
    vf = []
    if geo["fit"] == "crop":
        vf.append(f"crop={geo['width']}:{geo['height']}:"
                  f"{geo['crop']['x']}:{geo['crop']['y']}")
    elif geo["fit"] == "scale":
        vf.append(f"scale={geo['width']}:{geo['height']}")
    if vf:
        cmd += ["-vf", ",".join(vf)]
    cmd += sync
    if frames is not None:
        cmd += ["-frames:v", str(frames)]
    cmd += ["-pix_fmt", "yuv420p", "-f", "rawvideo", yuv]
    r = run(cmd, timeout)
    if r.returncode or not yuv.is_file():
        raise OracleError(
            f"ffmpeg decode failed (rc={r.returncode}).\n"
            f"  command: {' '.join(str(c) for c in cmd)}\n"
            f"  stderr: {tail(r.stderr)}")
    nbytes = yuv.stat().st_size
    frame_bytes = geo["width"] * geo["height"] * 3 // 2
    if nbytes < frame_bytes:
        raise OracleError(
            f"decode produced no complete I420 frame ({nbytes} bytes, "
            f"one frame = {frame_bytes} bytes).")
    decoded = nbytes // frame_bytes
    report["decode"] = {"cmd": [str(c) for c in cmd], "rc": r.returncode,
                        "yuv_bytes": nbytes, "frame_bytes": frame_bytes,
                        "frames_decoded": decoded, "stderr": tail(r.stderr, 400)}
    return yuv, decoded


def phx_supports_bitrate(phx, timeout, report):
    """Detect --bitrate support from the CLI's own usage text (never guess)."""
    r = run([phx], timeout)
    usage = f"{r.stdout}\n{r.stderr}"
    supported = "--bitrate" in usage
    report["phx_bitrate_detected"] = supported
    return supported


def encode(cmd, timeout, report):
    r = run(cmd, timeout)
    report["encode"] = {"cmd": [str(c) for c in cmd], "rc": r.returncode,
                        "stdout": tail(r.stdout), "stderr": tail(r.stderr)}
    return r


def count_nals(path):
    """Count Annex-B NAL units (00 00 01 / 00 00 00 01) in a stream file."""
    count = 0
    zeros = 0
    with path.open("rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            for b in chunk:
                if zeros >= 2 and b == 1:
                    count += 1
                    zeros = 0
                elif b == 0:
                    zeros += 1
                else:
                    zeros = 0
    return count


def verify_stream(out, ffmpeg, timeout, report):
    """Decode the Annex-B H.264 stream with ffmpeg -f null -."""
    cmd = [ffmpeg, "-v", "error", "-i", out, "-f", "null", "-"]
    r = run(cmd, timeout)
    result = {"cmd": [str(c) for c in cmd], "rc": r.returncode,
              "decoded": r.returncode == 0, "stderr": tail(r.stderr)}
    report["verify"] = result
    return result


def write_report(report, report_path):
    try:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    except OSError as exc:
        print(f"warning: could not write report {report_path}: {exc}",
              file=sys.stderr)


def make_output(args, stem):
    """Resolve the positional output (file or directory) / --out-dir."""
    explicit_file = None
    directory = None
    if args.output:
        p = pathlib.Path(args.output).expanduser()
        if p.suffix.lower() in (".h264", ".264", ".h265", ".hevc"):
            explicit_file = p
        else:
            directory = p
    if args.out_dir:
        directory = pathlib.Path(args.out_dir).expanduser()
    base = (directory or DEFAULT_OUT_DIR).resolve()
    if explicit_file:
        return explicit_file.expanduser().resolve(), base
    tag_parts = [stem, args.encoder]
    tag_parts += [args.preset, args.entropy]
    tag_parts += [f"bitrate{args.bitrate}k" if args.bitrate is not None
                  else f"qp{args.qp}"]
    name = "-".join(re.sub(r"[^A-Za-z0-9._-]", "_", str(p)) for p in tag_parts)
    return base / f"{name}.h264", base


def build_parser():
    p = argparse.ArgumentParser(
        prog="oracle_video.py",
        description="Decode any video to I420 with ffmpeg and encode it with "
                    "the Phoenix or x264 oracle CLI. No external Python deps.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""examples:
  python tools/oracle_video.py D:/Videos/clip.mp4 --x264 --preset veryfast
  python tools/oracle_video.py clip.mkv --frames 60 --qp 28 --entropy cavlc
  python tools/oracle_video.py clip.webm --bitrate 6000 ref.h264

Exit codes: 0 success, 1 encode/verify failure, 2 bad args or missing tool.
""")
    p.add_argument("video", help="input video (mp4/mkv/webm/mov/avi; other "
                                  "container extensions are attempted with a warning)")
    p.add_argument("output", nargs="?",
                   help="output .h264 file path, OR a directory name. "
                        "Default: build/oracle_video/<name>.h264")
    p.add_argument("--out-dir", help="output directory (default build/oracle_video/)")
    p.add_argument("--width", type=int, help="output width; default: probe source")
    p.add_argument("--height", type=int, help="output height; default: probe source")
    p.add_argument("--fit", choices=("crop", "scale"), default="crop",
                   help="when width/height differ from source: center crop "
                        "(default, preserves pixels) or scale")
    p.add_argument("--fps", type=float, help="frame rate; default: probe with ffprobe, "
                                             "fallback 25 (Phoenix CLI is fixed at 25)")
    p.add_argument("--frames", type=int, help="number of frames; default: all frames")
    p.add_argument("--preset", choices=PRESETS, default="medium",
                   help="x264 preset (default: medium)")
    p.add_argument("--entropy", choices=("cabac", "cavlc"), default="cabac",
                   help="entropy coding (default: cabac)")
    rc = p.add_mutually_exclusive_group()
    rc.add_argument("--qp", type=int, default=28,
                    help="constant QP [0..51] (default: 28); used as the "
                         "positional fallback in Phoenix bitrate mode")
    rc.add_argument("--bitrate", type=int, metavar="KBPS",
                    help="target bitrate in kbps (requires CLI support; "
                         "Phoenix support is detected at runtime)")
    p.add_argument("--x264", action="store_true",
                   help="use the authoritative x264 reference instead of Phoenix")
    p.add_argument("--verify", dest="verify", action="store_true", default=True,
                   help="verify stream with ffmpeg -f null - (default: on)")
    p.add_argument("--no-verify", dest="verify", action="store_false",
                   help="skip the ffmpeg decode verification")
    p.add_argument("--ffmpeg", default=str(DEFAULT_FFMPEG))
    p.add_argument("--ffprobe", default=str(DEFAULT_FFPROBE))
    p.add_argument("--x264-exe", default=os.environ.get("X264_EXE", str(DEFAULT_X264)))
    p.add_argument("--phx-exe", default=os.environ.get("PHX_EXE", str(DEFAULT_PHX)))
    p.add_argument("--report", default=str(DEFAULT_REPORT), help="JSON report path")
    p.add_argument("--timeout", type=int, default=1800,
                   help="per-tool timeout in seconds (default: 1800)")
    p.add_argument("--keep-temp", action="store_true",
                   help="do not delete the temporary I420 YUV (debugging)")
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.encoder = "x264" if args.x264 else "phoenix"
    if args.frames is not None and args.frames <= 0:
        print("ERROR: --frames must be positive", file=sys.stderr)
        return 2
    if args.qp < 0 or args.qp > 51:
        print("ERROR: --qp must be between 0 and 51", file=sys.stderr)
        return 2
    if args.bitrate is not None and args.bitrate <= 0:
        print("ERROR: --bitrate must be positive kbps", file=sys.stderr)
        return 2

    video = pathlib.Path(args.video).expanduser().resolve()
    if not video.is_file():
        print(f"ERROR: input video not found: {video}", file=sys.stderr)
        return 2
    if video.suffix.lower() not in VIDEO_EXTS:
        print(f"warning: unusual extension '{video.suffix}'; trying ffmpeg anyway",
              file=sys.stderr)

    report = {"tool": "oracle_video",
              "generated_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
              "parity_claim": False,
              "input": str(video), "encoder_kind": args.encoder,
              "warnings": [], "status": "error", "error": None}
    report_path = pathlib.Path(args.report).expanduser().resolve()
    out_path, out_dir = make_output(args, video.stem)
    report["output"] = str(out_path)
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
        out_path.parent.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        print(f"ERROR: cannot create output directory: {exc}", file=sys.stderr)
        return 2

    try:
        ffmpeg = ensure_exe(args.ffmpeg, "ffmpeg")
        ffprobe = ensure_exe(args.ffprobe, "ffprobe")
        report["ffmpeg"] = str(ffmpeg)
        report["ffprobe"] = str(ffprobe)
        probe_info = probe(video, ffprobe, report, args.timeout)
        geo = resolve_geometry(args, probe_info, report)

        encoder = ensure_exe(args.x264_exe if args.x264 else args.phx_exe,
                             "x264-exe" if args.x264 else "phx-exe")
        report["encoder_exe"] = str(encoder)
        report["settings"] = {"preset": args.preset, "entropy": args.entropy,
                              "qp": args.qp, "bitrate_kbps": args.bitrate,
                              "fps": geo["fps"], "frames": args.frames,
                              "fit": geo["fit"]}
    except OracleError as exc:
        report["error"] = str(exc)
        print(f"ERROR: {exc}", file=sys.stderr)
        write_report(report, report_path)
        return 2

    print(f"input:  {video}")
    print(f"decode: ffmpeg -> I420 {geo['width']}x{geo['height']} "
          f"@ {fps_text(geo['fps'])} fps ({geo['fit']})")
    print(f"encode: {'x264 (authoritative reference)' if args.x264 else 'Phoenix phx_cli'}"
          f" --preset {args.preset} --{args.entropy} "
          f"--{('bitrate ' + str(args.bitrate) + 'k') if args.bitrate is not None else ('qp ' + str(args.qp))}")

    tmp = None
    rc = 0
    try:
        tmp = tempfile.mkdtemp(prefix="oracle_video_")
        sync = detect_sync(ffmpeg, args.timeout, report)
        yuv, decoded = decode_to_yuv(video, ffmpeg, geo, args.frames, tmp,
                                     args.timeout, report, sync)

        if args.x264:
            cmd = [encoder, "--input-res", f"{geo['width']}x{geo['height']}",
                   "--input-csp", "i420", "--fps", fps_text(geo["fps"])]
            if args.frames is not None:
                cmd += ["--frames", str(args.frames)]
            cmd += (["--bitrate", str(args.bitrate)] if args.bitrate is not None
                    else ["--qp", str(args.qp)])
            cmd += ["--keyint", "30", "--min-keyint", "30", "--scenecut", "0",
                    "--preset", args.preset,
                    "--cabac" if args.entropy == "cabac" else "--no-cabac",
                    yuv, "-o", out_path]
        else:
            if args.bitrate is not None:
                if not phx_supports_bitrate(encoder, args.timeout, report):
                    raise OracleError(
                        "Phoenix CLI does not support --bitrate (its usage shows "
                        "only the fixed-QP form).\n"
                        "  U1 is adding bitrate support to tools/phx_cli.c: rebuild "
                        "build/tests/Release/phx_cli.exe and retry.\n"
                        "  Refusing to silently fall back to QP. Use --qp, or run "
                        "the same command with --x264 (authoritative reference).")
                cmd = [encoder, yuv, geo["width"], geo["height"], args.qp,
                       args.preset, args.entropy, out_path,
                       "--bitrate", args.bitrate]
            else:
                cmd = [encoder, yuv, geo["width"], geo["height"], args.qp,
                       args.preset, args.entropy, out_path]
            if geo["fps"] != 25.0:
                report.setdefault("warnings", []).append(
                    "phx_cli has no fps argument and uses a fixed 25 fps VUI; "
                    f"requested {fps_text(geo['fps'])} fps was NOT encoded into the stream.")

        r = encode(cmd, args.timeout, report)
        if r.returncode or not out_path.is_file():
            raise OracleError(
                f"encoder failed (rc={r.returncode}).\n"
                f"  command: {' '.join(str(c) for c in cmd)}\n"
                f"  stderr: {tail(r.stderr)}")

        size = out_path.stat().st_size
        nals = count_nals(out_path)
        report["stream"] = {"path": str(out_path), "bytes": size, "nal_count": nals}
        print(f"stream: {out_path} ({size} bytes, {nals} NALs)")

        if args.verify:
            v = verify_stream(out_path, ffmpeg, args.timeout, report)
            print(f"verify: ffmpeg -f null - : {'OK' if v['decoded'] else 'FAILED'}")
            if not v["decoded"]:
                print(f"  {tail(v['stderr'])}", file=sys.stderr)
                rc = 1
        else:
            report["verify"] = {"enabled": False, "decoded": None}

        report["status"] = "ok" if rc == 0 else "verify_failed"
        print(f"report: {report_path}")
    except OracleError as exc:
        report["error"] = str(exc)
        report["status"] = "error"
        print(f"ERROR: {exc}", file=sys.stderr)
        rc = 2
    finally:
        if tmp and not args.keep_temp:
            import shutil
            shutil.rmtree(tmp, ignore_errors=True)
        elif tmp:
            report["temp_dir"] = tmp
        write_report(report, report_path)
    return rc


if __name__ == "__main__":
    sys.exit(main())
