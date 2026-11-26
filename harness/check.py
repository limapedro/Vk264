"""Exercise ordinary encoder entry points and compare GPU recon with an independent decoder."""

import argparse
import json
import random
import re
import subprocess
import tempfile
from pathlib import Path


def run(command, **kwargs):
    return subprocess.run([str(arg) for arg in command], check=True, capture_output=True, **kwargs)


def verify_stream(ffprobe, path, width, height, frames):
    metadata = run([
        ffprobe, "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries",
        "stream=codec_name,width,height,pix_fmt,nb_read_frames", "-of", "json", path,
    ])
    stream, = json.loads(metadata.stdout)["streams"]
    expected = {"codec_name": "h264", "width": width, "height": height, "pix_fmt": "yuv420p",
                "nb_read_frames": str(frames)}
    if stream != expected or metadata.stderr:
        raise ValueError(f"Decoded stream mismatch: {stream}, expected {expected}\n{metadata.stderr.decode()}")


def check_reconstruction(ffmpeg, stream, recon, width, height, frames, padded=True):
    padded_w, padded_h = ((width + 15) // 16 * 16, (height + 15) // 16 * 16) if padded else (width, height)
    command = [ffmpeg, "-v", "error", "-xerror", "-err_detect", "explode", "-i", str(stream),
               "-fps_mode", "passthrough", "-pix_fmt", "yuv420p", "-f", "rawvideo", "-"]
    with recon.open("rb") as gpu, subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as decoder:
        try:
            for frame in range(frames):
                decoded = decoder.stdout.read(width * height * 3 // 2)
                padded = gpu.read(padded_w * padded_h * 3 // 2)
                if len(decoded) != width * height * 3 // 2 or len(padded) != padded_w * padded_h * 3 // 2:
                    stderr = decoder.stderr.read().decode(errors="replace") if not decoded else ""
                    raise ValueError(f"Incomplete reconstruction at frame {frame}. {stderr}")
                d = p = 0
                for plane in range(3):
                    shift = int(plane != 0)
                    w, h, pw, ph = width >> shift, height >> shift, padded_w >> shift, padded_h >> shift
                    for y in range(h):
                        cpu_row = decoded[d + w * y:d + w * (y + 1)]
                        gpu_row = padded[p + pw * y:p + pw * y + w]
                        if cpu_row != gpu_row:
                            x = next(x for x, (a, b) in enumerate(zip(cpu_row, gpu_row)) if a != b)
                            raise ValueError(f"Frame {frame}, plane {plane}, ({x}, {y}) differs: "
                                             f"decoder {cpu_row[x]}, shader {gpu_row[x]}")
                    d += w * h
                    p += pw * ph
            if decoder.stdout.read(1) or gpu.read(1):
                raise ValueError("Reconstruction has more frames than requested")
            stderr = decoder.stderr.read().decode(errors="replace")
            if decoder.wait() or stderr:
                raise ValueError(f"FFmpeg could not decode the complete stream: {stderr}")
        finally:
            if decoder.poll() is None:
                decoder.kill()


def make_source(path, width, height, frames, pattern):
    rng = random.Random(491)
    with path.open("wb") as output:
        for frame in range(frames):
            for plane in range(3):
                w, h = (width, height) if plane == 0 else (width // 2, height // 2)
                if pattern == "noise":
                    output.write(rng.randbytes(w * h))
                elif pattern == "repeat16":
                    # Frame 16 can recover frame 0 via the oldest reference.
                    output.write(random.Random(1009 + 3 * (frame % 16) + plane).randbytes(w * h))
                elif pattern == "flat":
                    output.write(bytes([0, 250, 30][plane:plane + 1]) * (w * h))
                elif pattern == "flat-grid":
                    unit = 16 if plane == 0 else 8
                    base = (18, 104, 124)[plane]
                    output.write(bytes(base + 7*((x//unit + 3*(y//unit) + 5*frame) % 7)
                                       for y in range(h) for x in range(w)))
                else:
                    output.write(bytes((27 * ((x - 3 * frame) // 5) + 4 * (y + frame) + 57 * plane) % 256
                                       for y in range(h) for x in range(w)))


def check_vulkan_backend(args, root):
    for frames, gop, bitrate in ((1, 1, 0), (19, 9, 75)):
        width, height, fps = 98, 70, "60000/1001"
        source, stream, recon = (root / f"vulkan-{frames}.{ext}" for ext in ("yuv", "h264", "recon.yuv"))
        make_source(source, width, height, frames, "motion")
        command = [args.encoder.resolve(), source, stream, width, height, frames, fps,
                   "--backend", "vulkan", "--entropy", args.entropy, "--qp", 34, "--gop", gop, "--recon", recon]
        if bitrate:
            command.extend(["--bitrate", bitrate])
        report = json.loads(run(command).stdout)
        if report["backend"] != "x264-vulkan" or report["settings"]["entropy"] != args.entropy:
            raise ValueError("Wrong backend or entropy mode")
        if bitrate and report["min_qp"] == report["max_qp"]:
            raise ValueError("Rate-control fixture did not change quantizers")
        verify_stream(args.ffprobe, stream, width, height, frames)
        check_reconstruction(args.ffmpeg, stream, recon, width, height, frames, padded=False)
        fields = run([args.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
                      "frame=key_frame", "-of", "json", stream])
        metadata = json.loads(fields.stdout)
        keyframes = [i for i, frame in enumerate(metadata["frames"]) if frame["key_frame"]]
        if keyframes != list(range(0, frames, gop)):
            raise ValueError("Vulkan backend GOP differs from the request")
        headers = run([args.ffmpeg, "-v", "info", "-i", stream, "-c:v", "copy", "-bsf:v", "trace_headers",
                       "-frames:v", 1, "-f", "null", "-"]).stderr.decode(errors="replace")
        for field, value in (("num_units_in_tick", 1001), ("time_scale", 120000), ("fixed_frame_rate_flag", 1),
                             ("entropy_coding_mode_flag", int(args.entropy == "cabac"))):
            if set(re.findall(rf"\b{field}\s+[01]+\s+= (\d+)", headers)) != {str(value)}:
                raise ValueError(f"Vulkan stream has incorrect {field}")
        print(f"Vulkan {args.entropy.upper()}: {frames} frames and GPU reconstruction match", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--encoder", required=True, type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--out", type=Path, help="Keep generated test files in this directory")
    parser.add_argument("--vulkan", action="store_true", help="Test the Vulkan CABAC backend")
    parser.add_argument("--entropy", choices=("cabac", "cavlc"), default="cabac")
    args = parser.parse_args()
    cases = [
        ("flat", 16, 16, 4, 0, 2, 0, 0),
        ("noise", 2, 2, 3, 0, 2, 1, 0),
        ("noise", 34, 30, 8, 0, 3, 8, 0),
        ("motion", 1062, 34, 12, 17, 9, 24, 0),
        ("motion", 162, 30, 61, 30, 60, 16, 0),
        ("motion", 178, 46, 12, 49, 9, 0, 0),
        ("noise", 128, 64, 16, 29, 8, 8, 300),
        ("flat", 144, 18, 5, 51, 5, 0, 0),
        ("motion", 32, 34, 5, 26, 1, 0, 0),
        ("motion", 16, 48, 6, 29, 5, 8, 0),
        ("repeat16", 48, 34, 47, 20, 23, 8, 0),
        ("motion", 162, 98, 9, 38, 7, 16, 0),
        ("flat-grid", 96, 80, 8, 48, 7, 0, 0),
    ]
    with tempfile.TemporaryDirectory() as temp:
        root = args.out or Path(temp)
        root.mkdir(parents=True, exist_ok=True)
        if args.vulkan:
            check_vulkan_backend(args, root)
            return
        for index, (pattern, w, h, frames, qp, gop, search, bitrate) in enumerate(cases):
            stem = f"{index}-{pattern}-{w}x{h}-qp{qp}"
            source, stream, recon = (root / f"{stem}.{ext}" for ext in ("yuv", "h264", "recon.yuv"))
            make_source(source, w, h, frames, pattern)
            command = [args.encoder.resolve(), source, stream, w, h, frames, 60, "--qp", qp,
                       "--gop", gop, "--search", search, "--recon", recon]
            if bitrate:
                command.extend(["--bitrate", bitrate])
            if index == 4:
                command.extend(["--subpel", 0, "--partitions", 0])
            elif index in (2, 3, 6):
                command.extend(["--partitions", 1])
            elif index == 5:
                command.extend(["--p-slice-rows", 1])
            if pattern == "repeat16":
                command.extend(["--preset", "veryslow"])
            if index in (3, 5, 10, 11):
                command.extend(["--deblock", 2])
            elif index == 4:
                command.extend(["--deblock", 1])
            elif index == 7:
                command.extend(["--deblock", 0])
            result = run(command)
            if result.stderr:
                raise ValueError(result.stderr.decode(errors="replace"))
            report = json.loads(result.stdout)
            if report["frames"] != frames:
                raise ValueError("Encoder reported a different frame count")
            if pattern == "repeat16" and report["selected_reference_depth"] != 16:
                raise ValueError("The repeat fixture did not select the 16th reference")
            if pattern == "flat-grid" and report["flat_intra_macroblocks"] == 0:
                raise ValueError("The flat fixture did not exercise P-frame intra")
            verify_stream(args.ffprobe, stream, w, h, frames)
            check_reconstruction(args.ffmpeg, stream, recon, w, h, frames)
            if index == 3:
                original, original_recon = stream.read_bytes(), recon.read_bytes()
                run(command)
                if stream.read_bytes() != original or recon.read_bytes() != original_recon:
                    raise ValueError("Repeated encodes produced different output")
            print(f"{stem}: {frames} frames match", flush=True)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"{error}\n{error.stdout.decode(errors='replace')}\n{error.stderr.decode(errors='replace')}")
