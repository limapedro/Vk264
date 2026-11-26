#!/usr/bin/env python3
"""6000-kbps video parity harness: x264 vs Phoenix, all presets/entropies.

Encodes real videos (or an existing raw I420 file) with the authoritative x264
reference and with Phoenix at one-pass ABR --bitrate 6000, for every one of the
10 presets x {CABAC, CAVLC}.  All streams are saved in the workspace:

    build/video_parity/<video>/<preset>-<entropy>.ref.h264     (x264)
    build/video_parity/<video>/<preset>-<entropy>.phx.h264     (Phoenix)
    build/video_parity/report.json
    build/video_parity/report.md

Only a byte-for-byte equal pair counts as parity.  When Phoenix is missing, or
its --bitrate mode has not landed yet, every phx case is recorded as BLOCKED
with a reason and no byte_exact value is fabricated; the x264 reference corpus
is still produced in full.

The x264 command is aligned with the fixed GOP policy that phx_cli.c applies
(--keyint 30 --min-keyint 30 --scenecut 0) because otherwise byte-exact parity
is impossible by construction.  Pass --gop x264-default to reproduce the older
plain-ABR command used for build/video_encode_test/*-6000k.ref.h264 (that
command is *not* a parity contract with the current phx_cli).

Achieved average bitrate is computed as bytes*8/(frames/fps).  A short clip
(60 frames at 25 fps = 2.4 s) cannot reach a steady 6000 kb/s, so the harness
warns explicitly whenever the achieved average is far below the target.  A low
average is not evidence that bitrate mode failed, and it is never a substitute
for byte-exact comparison.

Usage examples:
    python tools/video_parity.py --help
    python tools/video_parity.py --input D:/Videos/clip.mp4
    python tools/video_parity.py --input build/video_encode_test/foo_640x360_60f.yuv --x264-only
    python tools/video_parity.py --all-videos --max-videos 3

Environment overrides: X264_EXE, PHX_EXE, FFMPEG_EXE, FFPROBE_EXE.
Exit codes: 0 report written cleanly, 1 encode failure / byte mismatch,
2 x264 reference binary unavailable.  Python standard library only.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import time
from collections import Counter
from typing import Any, Dict, List, Optional, Sequence, Tuple

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_WORK = ROOT / "build" / "video_parity"
VIDEO_SOURCE_DIR = pathlib.Path("D:/Videos")

PRESETS: Tuple[str, ...] = (
    "ultrafast", "superfast", "veryfast", "faster", "fast",
    "medium", "slow", "slower", "veryslow", "placebo",
)
ENTROPIES: Tuple[Tuple[str, str], ...] = (
    ("cabac", "--cabac"), ("cavlc", "--no-cabac"),
)
# phx_cli.c pins these values in phx_param_t; x264 must use the same GOP policy.
PHX_GOP_ARGS: Tuple[str, ...] = ("--keyint", "30", "--min-keyint", "30", "--scenecut", "0")

DEFAULT_BITRATE = 6000
DEFAULT_WIDTH, DEFAULT_HEIGHT = 640, 360
DEFAULT_FPS, DEFAULT_FRAMES = 25, 60
DEFAULT_TIMEOUT, DEFAULT_DECODE_TIMEOUT = 600, 300
DEFAULT_WARN_RATIO = 0.5
DEFAULT_MAX_VIDEOS = 3

X264_NAMES = ("x264.exe", "x264")
PHX_NAMES = ("phx_cli.exe", "phx_cli")
FFMPEG_NAMES = ("ffmpeg.exe", "ffmpeg")
FFPROBE_NAMES = ("ffprobe.exe", "ffprobe")
X264_DIRS = (ROOT, ROOT / "build", ROOT / "build" / "x264_ref",
             ROOT / "reference", pathlib.Path("C:/x64/bin"))
PHX_DIRS = (ROOT / "build" / "tests" / "Release", ROOT / "build" / "tests",
            ROOT, ROOT / "build", ROOT / "build" / "x264_ref",
            pathlib.Path("C:/x64/bin"))
FF_DIRS = (pathlib.Path("C:/ffmpeg/ffmpeg-master-latest-win64-gpl/bin"),
           ROOT, ROOT / "build")

NAL_NAMES = {
    1: "non-IDR slice", 5: "IDR slice", 6: "SEI", 7: "SPS", 8: "PPS",
    9: "AUD", 10: "end of sequence", 11: "end of stream", 12: "filler",
    14: "prefix", 20: "slice extension",
}
VIDEO_SUFFIXES = (".mp4", ".mkv", ".mov", ".avi", ".m4v", ".webm")


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def tail(text: Any, limit: int = 600) -> str:
    """One-line, bounded, console-safe copy of tool output."""
    if not text:
        return ""
    s = " ".join(str(text).split())
    if len(s) > limit:
        s = "..." + s[-limit:]
    return s


def rel(path: Optional[pathlib.Path]) -> Optional[str]:
    if path is None:
        return None
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)


def cmdline(argv: Sequence[Any]) -> str:
    return shlex.join([str(a) for a in argv])


def find_exe(explicit: Optional[str], env: str, names: Sequence[str],
             dirs: Sequence[pathlib.Path]) -> Optional[pathlib.Path]:
    """Resolve a tool from --flag, env var, known directories, then PATH."""
    for candidate in (explicit, os.environ.get(env)):
        if candidate:
            p = pathlib.Path(candidate).expanduser()
            if p.is_file():
                return p.resolve()
            print(f"warning: {candidate!r} is not a file (ignored)", file=sys.stderr)
    for name in names:
        for d in dirs:
            p = d / name
            if p.is_file():
                return p.resolve()
        found = shutil.which(name)
        if found:
            return pathlib.Path(found).resolve()
    return None


def run(argv: Sequence[Any], timeout: int) -> Dict[str, Any]:
    """Run a child with a hard timeout; never raise on timeout/missing tool."""
    started = time.time()
    try:
        p = subprocess.run([str(a) for a in argv], cwd=str(ROOT),
                           capture_output=True, text=True, errors="replace",
                           timeout=timeout)
        rc, out, err, timed_out, error = p.returncode, p.stdout, p.stderr, False, None
    except subprocess.TimeoutExpired as e:
        rc, out, err, timed_out = 124, e.stdout or "", e.stderr or "", True
        error = f"TIMEOUT after {timeout}s"
    except OSError as e:
        rc, out, err, timed_out = 127, "", str(e), False
        error = f"cannot start {argv[0]}: {e}"
    return {"rc": rc, "out": out, "err": err, "timed_out": timed_out,
            "error": error, "seconds": round(time.time() - started, 2)}


def tool_version(exe: Optional[pathlib.Path], timeout: int = 15) -> Optional[str]:
    if not exe:
        return None
    r = run([exe, "--version"], timeout)
    text = (r["out"] or r["err"]).strip().splitlines()
    return tail(text[0], 160) if text else None


def first_difference(a: bytes, b: bytes) -> int:
    """Offset of first differing byte; -1 when equal, shorter length on prefix."""
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b)) if len(a) != len(b) else -1


def rle(values: Sequence[int], limit: int = 140) -> str:
    """Compact NAL type sequence, e.g. '7,8,6,5,1x59'."""
    if not values:
        return ""
    parts: List[str] = []
    cur, count = values[0], 1
    for v in values[1:]:
        if v == cur:
            count += 1
        else:
            parts.append(str(cur) if count == 1 else f"{cur}x{count}")
            cur, count = v, 1
    parts.append(str(cur) if count == 1 else f"{cur}x{count}")
    s = ",".join(parts)
    return s if len(s) <= limit else s[:limit - 3] + "..."


def annexb_nals(data: bytes) -> List[bytes]:
    """Split an Annex-B stream into NAL payloads (3/4-byte start codes)."""
    starts: List[Tuple[int, int]] = []
    i, n = 0, len(data)
    while i + 2 < n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            starts.append((i - 1 if i > 0 and data[i - 1] == 0 else i, i + 3))
            i += 3
        else:
            i += 1
    out: List[bytes] = []
    for k, (_, pos) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else n
        payload = data[pos:end].rstrip(b"\x00")
        if payload:
            out.append(payload)
    return out


def nal_info(data: bytes) -> Dict[str, Any]:
    types = [nal[0] & 0x1F for nal in annexb_nals(data) if nal]
    counts = Counter(types)
    return {
        "nal_count": len(types),
        "nal_types": {str(k): v for k, v in sorted(counts.items())},
        "nal_type_names": {str(k): NAL_NAMES.get(k, "reserved/other")
                           for k in sorted(counts)},
        "nal_sequence": rle(types),
    }


def achieved_kbps(num_bytes: int, frames: int, fps: int) -> Optional[float]:
    if not frames or fps <= 0:
        return None
    return round(num_bytes * 8 * fps / (frames * 1000.0), 2)


def sanitize(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("._")
    return (s or "input")[:60]


def unique_name(base: str, used: set) -> str:
    name = sanitize(base)
    if name not in used:
        used.add(name)
        return name
    for i in range(2, 1000):
        candidate = f"{name}_{i}"
        if candidate not in used:
            used.add(candidate)
            return candidate
    raise RuntimeError("too many inputs with the same name")


# ---------------------------------------------------------------------------
# Input preparation
# ---------------------------------------------------------------------------

def probe_video(ffprobe: Optional[pathlib.Path], src: pathlib.Path,
                timeout: int) -> Dict[str, Any]:
    if not ffprobe:
        return {"error": "ffprobe not available"}
    cmd = [ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
           "stream=width,height,codec_name,r_frame_rate,nb_frames:format=duration",
           "-of", "json", src]
    r = run(cmd, timeout)
    if r["rc"]:
        return {"error": tail(r["err"], 300) or f"ffprobe rc={r['rc']}"}
    try:
        payload = json.loads(r["out"] or "{}")
        stream = (payload.get("streams") or [{}])[0]
        fmt = payload.get("format") or {}
        return {"width": stream.get("width"), "height": stream.get("height"),
                "codec": stream.get("codec_name"),
                "r_frame_rate": stream.get("r_frame_rate"),
                "nb_frames": stream.get("nb_frames"),
                "duration_s": fmt.get("duration")}
    except (ValueError, IndexError) as e:
        return {"error": f"ffprobe JSON parse failed: {e}"}


def copy_frames(src: pathlib.Path, dst: pathlib.Path, frame_bytes: int,
                frames: int) -> None:
    """Write exactly `frames` I420 pictures; never modify the source."""
    remaining = frame_bytes * frames
    with src.open("rb") as fin, dst.open("wb") as fout:
        while remaining > 0:
            chunk = fin.read(min(1 << 20, remaining))
            if not chunk:
                raise ValueError("raw YUV has fewer frames than requested")
            fout.write(chunk)
            remaining -= len(chunk)


def prepare_inputs(args: argparse.Namespace, work: pathlib.Path,
                   ffmpeg: Optional[pathlib.Path],
                   ffprobe: Optional[pathlib.Path]) -> List[Dict[str, Any]]:
    specs: List[Dict[str, Any]] = []
    used: set = set()

    if args.all_videos:
        if not VIDEO_SOURCE_DIR.is_dir():
            print(f"warning: {VIDEO_SOURCE_DIR} is not a directory", file=sys.stderr)
            candidates: List[pathlib.Path] = []
        else:
            candidates = [p for p in VIDEO_SOURCE_DIR.iterdir()
                          if p.is_file() and p.suffix.lower() in VIDEO_SUFFIXES
                          and 200_000 <= p.stat().st_size <= 12_000_000]
            # Deterministic sweep: the smallest few videos, ties by name.
            candidates.sort(key=lambda p: (p.stat().st_size, p.name.lower()))
            candidates = candidates[:args.max_videos]
    else:
        candidates = [pathlib.Path(args.input).expanduser()]

    for src in candidates:
        name = unique_name(src.stem, used)
        vdir = work / name
        vdir.mkdir(parents=True, exist_ok=True)
        is_raw = args.raw or src.suffix.lower() == ".yuv"
        spec: Dict[str, Any] = {
            "name": name, "source": str(src), "kind": "raw-i420" if is_raw else "video",
            "yuv": None, "width": args.width, "height": args.height,
            "fps": args.fps, "frames": args.frames, "available_frames": None,
            "probe": None, "error": None,
        }
        if not src.is_file():
            spec["error"] = f"input not found: {src}"
            specs.append(spec)
            continue

        if is_raw:
            spec["probe"] = {"source_bytes": src.stat().st_size}
            frame_bytes = args.width * args.height * 3 // 2
            available = src.stat().st_size // frame_bytes if frame_bytes else 0
            spec["available_frames"] = available
            if available < args.frames:
                spec["error"] = (f"raw YUV has {available} frames; "
                                 f"{args.frames} requested")
                specs.append(spec)
                continue
            if available == args.frames:
                spec["yuv"] = src.resolve()
            else:
                dst = vdir / "input.yuv"
                try:
                    copy_frames(src, dst, frame_bytes, args.frames)
                except OSError as e:
                    spec["error"] = f"cannot trim raw YUV: {e}"
                else:
                    spec["yuv"] = dst
                    spec["probe"]["trimmed_from"] = available
        else:
            spec["probe"] = probe_video(ffprobe, src, args.decode_timeout)
            if not ffmpeg:
                spec["error"] = "ffmpeg not available (cannot decode video input)"
                specs.append(spec)
                continue
            dst = vdir / "input.yuv"
            vf = f"fps={args.fps},scale={args.width}:{args.height}:flags=bicubic"
            cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                   "-i", src, "-map", "0:v:0", "-an", "-sn", "-dn",
                   "-vf", vf, "-frames:v", str(args.frames),
                   "-pix_fmt", "yuv420p", "-f", "rawvideo", dst]
            r = run(cmd, args.decode_timeout)
            spec["probe"]["decode_cmd"] = cmdline(cmd)
            spec["probe"]["decode_rc"] = r["rc"]
            spec["probe"]["decode_stderr"] = tail(r["err"], 300)
            if r["rc"] or not dst.is_file():
                spec["error"] = (f"ffmpeg decode failed (rc={r['rc']}): "
                                 f"{tail(r['err'], 200)}")
                specs.append(spec)
                continue
            spec["yuv"] = dst
            frame_bytes = args.width * args.height * 3 // 2
            available = dst.stat().st_size // frame_bytes if frame_bytes else 0
            spec["available_frames"] = available
            if available < args.frames:
                spec["frames"] = available
                spec["probe"]["short_source"] = True
            if not available:
                spec["error"] = "ffmpeg produced no complete I420 frames"
        specs.append(spec)
    return specs


# ---------------------------------------------------------------------------
# Phoenix capability probe
# ---------------------------------------------------------------------------

def probe_phx_bitrate(phx: Optional[pathlib.Path], work: pathlib.Path,
                      timeout: int, bitrate: int) -> Dict[str, Any]:
    """Decide whether phx_cli really accepts --bitrate, with a smoke encode."""
    result: Dict[str, Any] = {
        "path": rel(phx), "available": False, "reason": None,
        "usage_rc": None, "smoke_rc": None, "smoke_bytes": None,
    }
    if not phx:
        result["reason"] = "phx_cli not found (set PHX_EXE or build build/tests/Release/phx_cli.exe)"
        return result
    usage = run([phx], 15)
    result["usage_rc"] = usage["rc"]
    if "--bitrate" not in ((usage["out"] or "") + (usage["err"] or "")):
        result["reason"] = ("phx_cli has no --bitrate option in its usage output; "
                            "bitrate mode has not landed yet")
        return result

    pdir = work / "_probe"
    pdir.mkdir(parents=True, exist_ok=True)
    yuv, out = pdir / "smoke.yuv", pdir / "smoke.h264"
    w = h = 64
    frames = 8
    frame = bytearray()
    for f in range(frames):
        for j in range(h):
            for i in range(w):
                frame.append((i * 7 + j * 13 + f * 29) & 255)
        for j in range(h // 2):
            for i in range(w // 2):
                frame.append((128 + i * 5 + f * 11) & 255)
        for j in range(h // 2):
            for i in range(w // 2):
                frame.append((128 + j * 5 + f * 7) & 255)
    yuv.write_bytes(bytes(frame))
    cmd = [phx, yuv, w, h, 28, "ultrafast", "cabac", out,
           "--bitrate", str(bitrate)]
    r = run(cmd, min(timeout, 180))
    result["smoke_cmd"] = cmdline(cmd)
    result["smoke_rc"] = r["rc"]
    result["smoke_stderr"] = tail(r["err"], 300)
    result["smoke_seconds"] = r["seconds"]
    if r["rc"] or not out.is_file() or out.stat().st_size == 0:
        result["reason"] = (f"phx_cli --bitrate smoke encode failed "
                            f"(rc={r['rc']}): {tail(r['err'], 200)}")
        return result
    result["smoke_bytes"] = out.stat().st_size
    result["available"] = True
    result["reason"] = "usage advertises --bitrate and smoke encode succeeded"
    return result


# ---------------------------------------------------------------------------
# Per-case encoding and reporting
# ---------------------------------------------------------------------------

def summarize(cases: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    achieved = [c["achieved_kbps"] for c in cases
                if c.get("achieved_kbps") is not None]
    diffs = [c["first_diff"] for c in cases
             if c.get("first_diff") is not None and c["first_diff"] >= 0]
    byte_exact = [c["byte_exact"] for c in cases
                  if c.get("byte_exact") is not None]
    return {
        "cases": len(cases),
        "ref_ok": sum(1 for c in cases if c.get("ref_rc") == 0),
        "ref_fail": sum(1 for c in cases if c.get("status") == "REF_FAIL"),
        "ref_blocked": sum(1 for c in cases if c.get("ref_rc") is None),
        "phx_ok": sum(1 for c in cases if c.get("phx_rc") == 0),
        "phx_fail": sum(1 for c in cases if c.get("status") in
                        ("PHX_FAIL", "PHX_TIMEOUT")),
        "phx_blocked": sum(1 for c in cases if c.get("phx_rc") is None),
        "compared": len(byte_exact),
        "byte_exact": sum(1 for v in byte_exact if v),
        "mismatches": sum(1 for v in byte_exact if not v),
        "low_bitrate_warnings": sum(1 for c in cases
                                    if c.get("low_bitrate_warning")),
        "achieved_kbps_min": min(achieved) if achieved else None,
        "achieved_kbps_max": max(achieved) if achieved else None,
        "first_diff_min": min(diffs) if diffs else None,
    }


def encode_case(spec: Dict[str, Any], preset: str, ename: str, eflag: str,
                args: argparse.Namespace, work: pathlib.Path,
                x264: Optional[pathlib.Path], phx: Optional[pathlib.Path],
                phx_capable: bool, phx_reason: str) -> Dict[str, Any]:
    stem = f"{preset}-{ename}"
    ref_path = work / spec["name"] / f"{stem}.ref.h264"
    phx_path = work / spec["name"] / f"{stem}.phx.h264"
    frames, fps = spec["frames"], spec["fps"]
    case: Dict[str, Any] = {
        "video": spec["name"], "source": spec["source"],
        "preset": preset, "entropy": ename,
        "width": spec["width"], "height": spec["height"],
        "fps": fps, "frames": frames, "duration_s": round(frames / fps, 3),
        "target_kbps": args.bitrate,
        "ref_path": rel(ref_path), "phx_path": rel(phx_path),
        "ref_rc": None, "ref_bytes": 0, "ref_seconds": None, "ref_error": None,
        "phx_rc": None, "phx_bytes": 0, "phx_seconds": None, "phx_error": None,
        "phx_reason": None, "byte_exact": None, "first_diff": None,
        "achieved_kbps": None, "achieved_pct": None,
        "low_bitrate_warning": False, "status": "BLOCKED",
    }

    if spec["error"]:
        case["status"] = "BLOCKED"
        case["phx_reason"] = spec["error"]
        case["ref_error"] = spec["error"]
        return case
    if not x264:
        case["status"] = "BLOCKED"
        case["ref_error"] = case["phx_reason"] = "x264 reference binary not found"
        return case

    gop = list(PHX_GOP_ARGS) if args.gop == "phx" else []
    cmd = [x264, "--input-res", f"{spec['width']}x{spec['height']}",
           "--input-csp", "i420", "--fps", str(fps), "--frames", str(frames),
           "--bitrate", str(args.bitrate)] + gop + \
          [eflag, "--preset", preset, spec["yuv"], "-o", ref_path]
    case["x264_cmd"] = cmdline(cmd)
    r = run(cmd, args.timeout)
    case["ref_rc"] = r["rc"]
    case["ref_seconds"] = r["seconds"]
    case["ref_error"] = tail(r["err"], 400) if r["rc"] else None
    if r["rc"] or not ref_path.is_file():
        case["status"] = "REF_FAIL"
        case["ref_error"] = case["ref_error"] or "x264 produced no output file"
        case["phx_reason"] = "reference stream missing/failed; no comparison made"
        return case
    ref_data = ref_path.read_bytes()
    case["ref_bytes"] = len(ref_data)
    case.update({f"ref_{k}": v for k, v in nal_info(ref_data).items()})
    case["achieved_kbps"] = achieved_kbps(len(ref_data), frames, fps)
    if case["achieved_kbps"] is not None and args.bitrate > 0:
        case["achieved_pct"] = round(100.0 * case["achieved_kbps"] / args.bitrate, 1)
        case["low_bitrate_warning"] = case["achieved_pct"] < args.warn_ratio * 100.0

    if args.x264_only:
        case["status"] = "SKIP"
        case["phx_reason"] = "--x264-only: Phoenix cases intentionally not run"
        return case
    if not phx_capable:
        case["status"] = "BLOCKED"
        case["phx_reason"] = phx_reason
        return case
    if fps != 25:
        case["status"] = "BLOCKED"
        case["phx_reason"] = ("phx_cli hardcodes 25 fps in ABR rate control; "
                              f"--fps {fps} cannot be byte-compared")
        return case

    pcmd = [phx, spec["yuv"], spec["width"], spec["height"], args.qp,
            preset, ename, phx_path, "--bitrate", args.bitrate]
    case["phx_cmd"] = cmdline(pcmd)
    p = run(pcmd, args.timeout)
    case["phx_rc"] = p["rc"]
    case["phx_seconds"] = p["seconds"]
    case["phx_error"] = tail(p["err"], 400) if p["rc"] else None
    if p["rc"] or not phx_path.is_file():
        case["status"] = "PHX_TIMEOUT" if p["timed_out"] else "PHX_FAIL"
        case["phx_error"] = case["phx_error"] or "phx_cli produced no output file"
        return case
    phx_data = phx_path.read_bytes()
    case["phx_bytes"] = len(phx_data)
    case.update({f"phx_{k}": v for k, v in nal_info(phx_data).items()})
    case["first_diff"] = first_difference(phx_data, ref_data)
    # Never claim parity unless both encodes completed successfully.
    case["byte_exact"] = (not p["timed_out"] and ref_data == phx_data)
    case["status"] = "BYTE-EXACT" if case["byte_exact"] else "DIFF"
    return case


def print_report(report: Dict[str, Any]) -> None:
    meta, cases = report["metadata"], report["cases"]
    t = report["totals"]
    print(f'x264: {meta["x264"] or "NOT FOUND"}')
    print(f'phx:  {meta["phx"] or "NOT FOUND"} '
          f'({meta["phx_bitrate_probe"]["reason"]})')
    print(f'{t["cases"]} cases; x264 rc=0: {t["ref_ok"]}/{t["cases"]}; '
          f'phx rc=0: {t["phx_ok"]}/{t["cases"]}; '
          f'byte-exact: {t["byte_exact"]}/{t["compared"]} compared')
    if t["byte_exact"] != t["cases"]:
        print("NO byte-exact parity claim: only identical byte streams count; "
              "blocked/skipped/failed cases are not parity.")
    print()
    print(f'  {"video":<22} {"preset":<10} {"ent":<5} {"refB":>9} {"phxB":>9} '
          f'{"NAL ref/phx":>13} {"kb/s":>8}  status')
    for c in cases:
        nals = f'{c.get("ref_nal_count", 0)}/{c.get("phx_nal_count", 0)}'
        rate = c["achieved_kbps"] if c["achieved_kbps"] is not None else "-"
        print(f'  {c["video"][:22]:<22} {c["preset"]:<10} {c["entropy"]:<5} '
              f'{c["ref_bytes"]:>9} {c["phx_bytes"]:>9} {nals:>13} '
              f'{str(rate):>8}  {c["status"]}')
    for label, key in (("Per video", "by_video"), ("Per preset", "by_preset"),
                       ("Per entropy", "by_entropy")):
        print()
        print(f"{label}:")
        print(f'  {"group":<24} {"cases":>5} {"refOK":>5} {"phxOK":>5} '
              f'{"exact":>5} {"diff":>5} {"lowRate":>7}')
        for name, s in report[key].items():
            print(f'  {name[:24]:<24} {s["cases"]:>5} {s["ref_ok"]:>5} '
                  f'{s["phx_ok"]:>5} {s["byte_exact"]:>5} '
                  f'{s["mismatches"]:>5} {s["low_bitrate_warnings"]:>7}')
    low = t["low_bitrate_warnings"]
    if low:
        print()
        print(f"WARNING: {low}/{t['cases']} cases achieved far below the "
              f"{meta['target_kbps']} kb/s target. Short clips ({meta['frames']} "
              f"frames at {meta['fps']} fps = {meta['duration_s']} s) cannot "
              "reach steady-state ABR; see report.md.")


def write_reports(report: Dict[str, Any], work: pathlib.Path) -> None:
    (work / "report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    meta, t = report["metadata"], report["totals"]
    L = [
        "# 6000-kbps video parity report", "",
        f'Generated: {meta["generated_utc"]}',
        f'Command: `{meta["command"]}`', "",
        f'- Target bitrate: **{meta["target_kbps"]} kb/s** (x264 '
        "`--bitrate`, phx_cli `--bitrate`)",
        f'- Encoders: x264 `{meta["x264"] or "NOT FOUND"}`; '
        f'Phoenix `{meta["phx"] or "NOT FOUND"}`',
        f'- GOP policy: `{meta["gop"]}` '
        f'({meta["x264_gop_args"] or "x264 defaults"})',
        f'- Presets: {", ".join(meta["presets"])}',
        "- Entropies: cabac (`--cabac`), cavlc (`--no-cabac`)",
        f'- Cases: {t["cases"]}; x264 rc=0: {t["ref_ok"]}; '
        f'phx rc=0: {t["phx_ok"]}',
        f'- Byte-exact: **{t["byte_exact"]}/{t["compared"]} compared** '
        f'({t["mismatches"]} mismatches, '
        f'{t["cases"] - t["compared"]} not comparable)', "",
        "> **Only byte-identical streams count as parity.** A skipped, "
        "blocked, failed, smaller/larger or merely similar stream is never "
        "reported as byte-exact.", "",
    ]
    if meta["phx_bitrate_probe"]["reason"]:
        L += [f'- Phoenix bitrate mode: {meta["phx_bitrate_probe"]["reason"]}', ""]
    if t["byte_exact"] != t["cases"]:
        L += ["**NO byte-exact parity claim** for this run.", ""]
    else:
        L += ["Byte-exact parity: all cases match.", ""]
    if t["low_bitrate_warnings"]:
        L += [
            "## Bitrate caveat: target vs achieved average", "",
            f'**WARNING:** {t["low_bitrate_warnings"]}/{t["cases"]} cases have '
            f'an achieved average below {int(meta["warn_ratio"] * 100)}% of the '
            f'{meta["target_kbps"]} kb/s target '
            f'(min {t["achieved_kbps_min"]} kb/s, '
            f'max {t["achieved_kbps_max"]} kb/s).',
            "",
            f'The default clip is only {meta["frames"]} frames at '
            f'{meta["fps"]} fps = {meta["duration_s"]} s. ABR needs time to '
            "converge, so the average measured as `bytes*8/duration` stays far "
            "below 6000 kb/s on such a short clip. This is a measurement "
            "caveat, **not** evidence that bitrate mode works, and **not** a "
            "parity result; use longer clips for bitrate-accuracy checks and "
            "byte identity for parity checks.",
            "",
        ]

    def summary_table(title: str, key: str, heading: str) -> None:
        L.extend([f"## {title}", "",
                  f"| {heading} | Cases | ref rc=0 | phx rc=0 | Byte-exact | "
                  "Mismatches | Not comparable | Low avg. rate | "
                  "Achieved kb/s (min-max) |",
                  "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |"])
        for name, s in report[key].items():
            rate = ("-" if s["achieved_kbps_min"] is None else
                    f'{s["achieved_kbps_min"]}-{s["achieved_kbps_max"]}')
            not_comp = s["cases"] - s["compared"]
            L.append(f'| {name} | {s["cases"]} | {s["ref_ok"]} | {s["phx_ok"]} '
                     f'| {s["byte_exact"]} | {s["mismatches"]} | {not_comp} '
                     f'| {s["low_bitrate_warnings"]} | {rate} |')
        L.append("")

    summary_table("Per-video summary", "by_video", "Video")
    summary_table("Per-preset summary", "by_preset", "Preset")
    summary_table("Per-entropy summary", "by_entropy", "Entropy")
    L += ["## Per-case results", "",
          "| Video | Preset | Entropy | x264 rc | Ref bytes | Ref NALs | "
          "phx rc | phx bytes | phx NALs | Byte-exact | First diff | "
          "Achieved kb/s | Status |",
          "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | "
          "--- | ---: | ---: | --- |"]
    for c in report["cases"]:
        yn = "yes" if c["byte_exact"] is True else (
            "no" if c["byte_exact"] is False else "-")
        fd = "-" if c["first_diff"] is None else c["first_diff"]
        rate = "-" if c["achieved_kbps"] is None else c["achieved_kbps"]
        ref_nals = c.get("ref_nal_sequence", "-") or "-"
        phx_nals = c.get("phx_nal_sequence", "-") or "-"
        reason = f' ({c["phx_reason"]})' if c["phx_reason"] else ""
        L.append(f'| {c["video"]} | {c["preset"]} | {c["entropy"]} | '
                 f'{c["ref_rc"] if c["ref_rc"] is not None else "-"} | '
                 f'{c["ref_bytes"]} | {ref_nals} | '
                 f'{c["phx_rc"] if c["phx_rc"] is not None else "-"} | '
                 f'{c["phx_bytes"]} | {phx_nals} | {yn} | {fd} | {rate} | '
                 f'{c["status"]}{reason} |')
    L += ["", "NAL sequences are run-length encoded Annex-B type codes: "
          "5=IDR slice, 1=non-IDR slice, 6=SEI, 7=SPS, 8=PPS. `NALs` counts "
          "are in `report.json` as `ref_nal_types` / `phx_nal_types`.", ""]
    (work / "report.md").write_text("\n".join(L), encoding="utf-8")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Encode videos with x264 and Phoenix at 6000 kb/s across "
                    "all 10 presets x CABAC/CAVLC, save every stream and report "
                    "byte-exact parity (stdlib only).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="examples:\n"
               "  python tools/video_parity.py --input D:/Videos/clip.mp4\n"
               "  python tools/video_parity.py --input clip.yuv --x264-only\n"
               "  python tools/video_parity.py --all-videos --max-videos 3\n")
    p.add_argument("--input", help="video file (decoded to I420 by ffmpeg) or "
                   "an existing raw I420 .yuv file")
    p.add_argument("--all-videos", action="store_true",
                   help="sweep a deterministic few videos from D:/Videos")
    p.add_argument("--max-videos", type=int, default=DEFAULT_MAX_VIDEOS,
                   help="how many videos --all-videos selects (default: 3)")
    p.add_argument("--raw", action="store_true",
                   help="treat --input as raw I420 even without a .yuv suffix")
    p.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    p.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    p.add_argument("--fps", type=int, default=DEFAULT_FPS,
                   help="must be 25 for phx byte comparison (phx_cli hardcodes 25)")
    p.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    p.add_argument("--bitrate", type=int, default=DEFAULT_BITRATE,
                   help="target kb/s for both encoders (default: 6000)")
    p.add_argument("--qp", type=int, default=28,
                   help="fallback QP argument required by phx_cli (default: 28)")
    p.add_argument("--gop", choices=("phx", "x264-default"), default="phx",
                   help="'phx' adds --keyint 30 --min-keyint 30 --scenecut 0 "
                   "so x264 matches phx_cli (default); 'x264-default' uses "
                   "plain x264 ABR defaults")
    p.add_argument("--x264-only", action="store_true",
                   help="run only the x264 references; phx cases are SKIP")
    p.add_argument("--out-dir", default=str(DEFAULT_WORK),
                   help=f"output root (default: {rel(DEFAULT_WORK)})")
    p.add_argument("--x264", help="x264 executable (or X264_EXE)")
    p.add_argument("--phx", help="phx_cli executable (or PHX_EXE)")
    p.add_argument("--ffmpeg", help="ffmpeg executable (or FFMPEG_EXE)")
    p.add_argument("--ffprobe", help="ffprobe executable (or FFPROBE_EXE)")
    p.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT,
                   help="per-encode timeout in seconds (default: 600)")
    p.add_argument("--decode-timeout", type=int, default=DEFAULT_DECODE_TIMEOUT,
                   help="ffmpeg/ffprobe timeout in seconds (default: 300)")
    p.add_argument("--warn-ratio", type=float, default=DEFAULT_WARN_RATIO,
                   help="warn when achieved/target is below this fraction "
                   "(default: 0.5)")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.input and not args.all_videos:
        print("error: specify --input FILE or --all-videos (see --help)",
              file=sys.stderr)
        return 2
    if args.all_videos and args.input:
        print("warning: --input ignored because --all-videos was given",
              file=sys.stderr)
    if args.width <= 0 or args.height <= 0 or args.fps <= 0 or args.frames <= 0:
        print("error: --width/--height/--fps/--frames must be positive",
              file=sys.stderr)
        return 2
    if args.height % 2 or args.width % 2:
        print("error: I420 requires even --width and --height", file=sys.stderr)
        return 2
    if args.bitrate <= 0 or args.timeout <= 0 or args.decode_timeout <= 0:
        print("error: --bitrate and timeouts must be positive", file=sys.stderr)
        return 2

    work = pathlib.Path(args.out_dir).expanduser().resolve()
    work.mkdir(parents=True, exist_ok=True)
    x264 = find_exe(args.x264, "X264_EXE", X264_NAMES, X264_DIRS)
    phx = find_exe(args.phx, "PHX_EXE", PHX_NAMES, PHX_DIRS)
    ffmpeg = find_exe(args.ffmpeg, "FFMPEG_EXE", FFMPEG_NAMES, FF_DIRS)
    ffprobe = find_exe(args.ffprobe, "FFPROBE_EXE", FFPROBE_NAMES, FF_DIRS)
    if not x264:
        print("warning: x264 reference binary not found; all cases will be "
              "BLOCKED (set X264_EXE or build build/x264_ref/x264.exe)",
              file=sys.stderr)

    phx_probe = ({"path": rel(phx), "available": False,
                  "reason": "not probed (--x264-only)"}
                 if args.x264_only else
                 probe_phx_bitrate(phx, work, args.timeout, args.bitrate))
    gop_args = list(PHX_GOP_ARGS) if args.gop == "phx" else []
    specs = prepare_inputs(args, work, ffmpeg, ffprobe)
    if not specs:
        print("error: no inputs selected", file=sys.stderr)
        return 2

    cases: List[Dict[str, Any]] = []
    for spec in specs:
        if spec["error"]:
            print(f"warning: {spec['name']}: {spec['error']}", file=sys.stderr)
        for preset in PRESETS:
            for ename, eflag in ENTROPIES:
                cases.append(encode_case(
                    spec, preset, ename, eflag, args, work, x264, phx,
                    phx_probe["available"], phx_probe["reason"] or ""))

    report: Dict[str, Any] = {
        "metadata": {
            "generated_utc": dt.datetime.now(dt.timezone.utc).isoformat(
                timespec="seconds"),
            "command": cmdline([sys.executable, __file__] + list(argv or sys.argv[1:])),
            "target_kbps": args.bitrate,
            "duration_s": round(args.frames / args.fps, 3),
            "fps": args.fps, "frames": args.frames,
            "width": args.width, "height": args.height,
            "warn_ratio": args.warn_ratio,
            "presets": list(PRESETS), "entropies": [e for e, _ in ENTROPIES],
            "gop": args.gop, "x264_gop_args": gop_args,
            "x264": rel(x264), "x264_version": tool_version(x264),
            "phx": rel(phx),
            "phx_bitrate_probe": phx_probe,
            "ffmpeg": rel(ffmpeg), "ffprobe": rel(ffprobe),
            "x264_only": args.x264_only,
        },
        "inputs": [{k: (rel(v) if isinstance(v, pathlib.Path) else v)
                    for k, v in s.items()} for s in specs],
        "cases": cases,
    }
    report["totals"] = summarize(cases)
    for label, key in (("by_video", "video"), ("by_preset", "preset"),
                       ("by_entropy", "entropy")):
        names = [s["name"] for s in specs] if key == "video" else (
            list(PRESETS) if key == "preset" else ["cabac", "cavlc"])
        report[label] = {name: summarize([c for c in cases if c[key] == name])
                         for name in names}
    write_reports(report, work)
    print_report(report)
    print(f"\nreports: {rel(work / 'report.json')}, {rel(work / 'report.md')}")

    t = report["totals"]
    if not x264:
        return 2
    return 1 if (t["ref_fail"] or t["phx_fail"] or t["mismatches"]) else 0


if __name__ == "__main__":
    sys.exit(main())
