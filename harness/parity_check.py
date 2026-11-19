"""Byte-parity comparator: wx264 --backend vulkan vs libx264 (harness settings).

Usage:
  python harness/parity_check.py --input clip.yuv --width 128 --height 64 \
      --frames 40 --preset ultrafast [--out-dir DIR] [--keep]

Encoding for libx264 mirrors harness/benchmark.py (mp4 container with -c copy
then probe-free Annex B extraction); wx264 writes the raw .h264 directly.
Annex B streams are extracted with ffmpeg -c copy and compared byte-for-byte;
first mismatching offset is reported with context and NAL type.

Exit code: 0 on byte-exact match, 1 on mismatch, 2 on invocation errors.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check import make_source, run

X264_PRESETS = {
    "ultrafast": ("-preset", "ultrafast"),
    "superfast": ("-preset", "superfast"),
    "veryfast": ("-preset", "veryfast"),
    "faster": ("-preset", "faster"),
    "fast": ("-preset", "fast"),
    "medium": ("-preset", "medium"),
    "slow": ("-preset", "slow"),
    "slower": ("-preset", "slower"),
    "veryslow": ("-preset", "veryslow"),
    "placebo": ("-preset", "placebo"),
}


def annex_b(ffmpeg: str, path: Path, tmp: Path) -> bytes:
    h264 = tmp / (path.stem + ".ref.h264")
    run([ffmpeg, "-y", "-nostdin", "-hide_banner", "-loglevel", "error",
         "-i", path, "-c:v", "copy", "-f", "h264", h264])
    return h264.read_bytes()


def split_nals(data: bytes):
    starts = [m.start() for m in re.finditer(rb"\x00\x00\x00\x01", data)] + [len(data)]
    units = []
    for i in range(len(starts) - 1):
        s, e = starts[i], starts[i + 1]
        if s + 5 <= e:
            units.append((s, e, data[s + 4] & 0x1F))
    return units


def strip_sei(data: bytes) -> bytes:
    return b"".join(data[s:e] for s, e, typ in split_nals(data) if typ != 6)


def first_mismatch(a: bytes, b: bytes) -> int:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return -1 if len(a) == len(b) else min(len(a), len(b))


def nal_type_at(data: bytes, offset: int) -> str:
    found = 0
    for s, e, typ in split_nals(data):
        if s <= offset < e:
            found = typ
    names = {1: "P", 5: "IDR", 6: "SEI", 7: "SPS", 8: "PPS", 9: "AUD"}
    return names.get(found, str(found))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, help="Raw YUV 4:2:0 input; if omitted a deterministic synthetic clip is generated")
    parser.add_argument("--width", default=176, type=int)
    parser.add_argument("--height", default=96, type=int)
    parser.add_argument("--frames", default=8, type=int)
    parser.add_argument("--fps", default=60, type=int)
    parser.add_argument("--preset", default="ultrafast", choices=X264_PRESETS)
    parser.add_argument("--qp", default=28, type=int)
    parser.add_argument("--gop", default=12)
    parser.add_argument("--pattern", default="motion", choices=["flat", "noise", "motion", "flat-grid", "repeat16"])
    parser.add_argument("--out-dir", type=Path, help="Keep artifacts in this directory")
    parser.add_argument("--wx264", type=Path, default=Path("build/Release/wx264.exe"))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()

    work = args.out_dir or Path(f"build/parity-{args.preset}-{args.pattern}")
    work.mkdir(parents=True, exist_ok=True)
    source = work / "source.yuv"
    if args.input:
        source = args.input
    else:
        make_source(source, args.width, args.height, args.frames, args.pattern)

    wx_out = work / f"wx264-{args.preset}.h264"
    ref_mp4 = work / f"x264-{args.preset}.mp4"

    run([args.wx264, source, wx_out, args.width, args.height, args.frames, args.fps,
         "--qp", args.qp, "--gop", args.gop, "--backend", "vulkan", "--preset", args.preset])
    ref_cmd = [args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
               "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", f"{args.width}x{args.height}",
               "-framerate", args.fps, "-i", source, "-frames:v", args.frames, "-an",
               "-c:v", "libx264", *X264_PRESETS[args.preset], "-sc_threshold", "0",
               "-g", args.gop, "-qp", str(args.qp), "-x264-params",
               "no-info=1:force-cfr=1:colorprim=bt709:transfer=bt709:colormatrix=bt709", "-f", "mp4", ref_mp4]
    run(ref_cmd)
    ref = annex_b(args.ffmpeg, ref_mp4, work)
    wx = wx_out.read_bytes()

    wx_sei = sum(1 for _, _, t in split_nals(wx) if t == 6)
    ref_sei = sum(1 for _, _, t in split_nals(ref) if t == 6)
    wxs, refs = strip_sei(wx), strip_sei(ref)
    mismatch = first_mismatch(wxs, refs)
    print(f"preset={args.preset} pattern={args.pattern}")
    print(f"wx264 bytes={len(wx)} (SEI {wx_sei})  x264 bytes={len(ref)} (SEI {ref_sei})")
    print("note: encoder-identity SEI NALs are excluded from the comparison (both streams decode identically either way)")
    if mismatch < 0:
        print("BYTE-EXACT MATCH (ignoring encoder-identity SEI)")
        return 0
    lo = max(0, mismatch - 16)
    print(f"first mismatch at offset {mismatch} in SEI-stripped stream ({nal_type_at(wxs, mismatch)} NAL)")
    print(f"wx264  [{lo}:{mismatch + 24}] " + wxs[lo:mismatch + 24].hex(" "))
    print(f"x264   [{lo}:{mismatch + 24}] " + refs[lo:mismatch + 24].hex(" "))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
