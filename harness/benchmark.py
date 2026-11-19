"""Run same-input H.264 streaming comparisons and record achieved bitrate, speed and quality."""

import argparse
import json
import math
import os
import re
import statistics
import subprocess
import time
from pathlib import Path

from check import check_reconstruction, run, verify_stream

PRESETS = ("fast", "medium", "slow", "veryslow")
X264_PRESETS = ("ultrafast", "superfast", "veryfast", "faster", *PRESETS[:3],
                "slower", "veryslow", "placebo")
VULKAN_PRESETS = ("ultrafast", "superfast", "veryfast", "faster", "fast")
ENCODERS = (("vk264", "vk264-vulkan", "vk264-vulkan-cavlc")
            + tuple(f"vk264-vulkan-{preset}" for preset in VULKAN_PRESETS)
            + tuple(f"vk264-{preset}" for preset in PRESETS)
            + tuple(f"x264-{preset}" for preset in X264_PRESETS)
            + tuple(f"nvenc-p{preset}" for preset in range(1, 8)))


def run_logged(command, path):
    start = time.perf_counter()
    with path.open("wb") as log:
        result = subprocess.run([str(value) for value in command], stdout=log, stderr=subprocess.STDOUT,
                                cwd=path.parent)
    if result.returncode:
        raise RuntimeError(f"Command exited {result.returncode}. See {path}\n{path.read_text(errors='replace')[-3000:]}")
    return time.perf_counter() - start


def raw_input(args):
    return ["-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", f"{args.width}x{args.height}",
            "-framerate", args.fps, "-i", args.input]


def quality(args, stream, directory, name):
    psnr = f"{name}.psnr.txt"
    ssim = f"{name}.ssim.txt"
    vmaf = f"{name}.vmaf.json"
    clock = f"settb=1/{args.fps},setpts=N"
    branches = "psv" if args.vmaf else "ps"
    ref = "".join(f"[ref{key}]" for key in branches)
    dis = "".join(f"[dis{key}]" for key in branches)
    graph = (f"[0:v]trim=end_frame={args.frames},{clock},split={len(branches)}{ref};"
             f"[1:v]{clock},split={len(branches)}{dis};"
             f"[disp][refp]psnr=stats_file={psnr}:shortest=1:repeatlast=0[p];"
             f"[diss][refs]ssim=stats_file={ssim}:shortest=1:repeatlast=0[s]")
    maps = ["-map", "[p]", "-map", "[s]"]
    if args.vmaf:
        graph += f";[disv][refv]libvmaf=log_fmt=json:log_path={vmaf}:n_threads=4:shortest=1:repeatlast=0[v]"
        maps.extend(["-map", "[v]"])
    log = directory / f"{name}.metrics.log"
    input_timing = ["-r", args.fps] if stream.suffix == ".h264" else []
    run_logged([args.ffmpeg, "-hide_banner", "-loglevel", "error", "-xerror", *raw_input(args),
                *input_timing, "-err_detect", "explode", "-i", stream, "-filter_complex", graph, *maps,
                "-fps_mode", "passthrough", "-f", "null", os.devnull], log)

    def records(name):
        rows = [dict(re.findall(r"(\w+):\s*([\d.]+|inf)", line))
                for line in (directory / name).read_text().splitlines()]
        indices = [int(row["n"]) for row in rows]
        if indices != list(range(1, args.frames + 1)):
            raise ValueError(f"Unexpected metric frame indices in {name}")
        return rows

    psnr_records = records(psnr)
    result = {}
    for key, field in (("y", "y"), ("u", "u"), ("v", "v"), ("average", "avg")):
        mse = statistics.mean(float(row[f"mse_{field}"]) for row in psnr_records)
        result[f"psnr_{key}"] = round(10 * math.log10(255 ** 2 / mse), 4) if mse else "inf"
    result["ssim"] = round(statistics.mean(float(row["All"]) for row in records(ssim)), 6)
    if args.vmaf:
        values = json.loads((directory / vmaf).read_text())
        if [frame["frameNum"] for frame in values["frames"]] != list(range(args.frames)):
            raise ValueError("Unexpected VMAF frame indices")
        result["vmaf"] = values["pooled_metrics"]["vmaf"]["mean"]
    return result


def build_command(args, name, stream):
    codec, _, preset = name.partition("-")
    if codec == "vk264":
        command = [args.encoder, args.input, stream, args.width, args.height, args.frames, args.fps,
                   "--qp", args.qp, "--gop", args.gop, "--bitrate", args.bitrate]
        if preset in ("vulkan", "vulkan-cavlc"):
            command.extend(["--backend", "vulkan"])
            if preset == "vulkan-cavlc":
                command.extend(["--entropy", "cavlc"])
        elif preset.startswith("vulkan-"):
            vulkan_preset = preset[len("vulkan-"):]
            if vulkan_preset not in VULKAN_PRESETS:
                raise ValueError(f"Unsupported vk264-vulkan preset: {vulkan_preset!r} "
                                 f"(supported: {', '.join(VULKAN_PRESETS)})")
            command.extend(["--backend", "vulkan", "--preset", vulkan_preset])
        elif preset:
            command.extend(["--preset", preset])
        for option, value in (("--search", args.search), ("--subpel", args.subpel),
                              ("--partitions", args.partitions), ("--p-slice-rows", args.p_slice_rows),
                              ("--satd-candidates", args.satd_candidates), ("--ref", args.ref),
                              ("--deblock", args.deblock), ("--flat-intra", args.flat_intra), ("--aq", args.aq)):
            if value is not None:
                command.extend([option, value])
        return command
    codec_args = (["-c:v", "libx264", "-preset", preset, "-sc_threshold", 0] if codec == "x264" else
                  ["-c:v", "h264_nvenc", "-preset", preset, "-tune", "hq", "-rc", "cbr", "-no-scenecut", 1])
    container = "mp4" if stream.suffix == ".mp4" else "h264"
    return [args.ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "warning", "-y", *raw_input(args),
            "-frames:v", args.frames, "-an", *codec_args, "-g", args.gop, "-b:v", f"{args.bitrate}k",
            "-maxrate", f"{args.bitrate}k", "-bufsize", f"{2 * args.bitrate}k", "-f", container, stream]


def x264_settings(stream):
    # libx264 normally includes its effective settings in an unregistered SEI.
    with stream.open("rb") as file:
        headers = file.read(32 * 1024)
    options = re.search(rb"x264 - core [^\x00]+? - options: ([\x20-\x7e]+)", headers)
    return dict(re.findall(r"(\w+)=([^\s]+)", options[1].decode("ascii"))) if options else None


def packet_stats(args, stream):
    probe = run([args.ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
                 "packet=size,flags,pts:stream=time_base", "-of", "json", stream])
    info = json.loads(probe.stdout)
    packets = info["packets"]
    if len(packets) != args.frames or probe.stderr:
        raise ValueError(f"Incorrect access-unit count for {stream}")
    if stream.suffix == ".mp4":
        time_num, time_den = map(int, info["streams"][0]["time_base"].split("/"))
        timestamps = sorted(int(packet["pts"]) for packet in packets)
        if any((pts - timestamps[0]) * time_num * args.fps != index * time_den
               for index, pts in enumerate(timestamps)):
            raise ValueError(f"Unexpected presentation timestamps for {stream}")
    sizes = [int(packet["size"]) for packet in packets]
    window = min(args.fps, len(sizes))
    window_bytes = sum(sizes[:window])
    peak = window_bytes
    for new, old in zip(sizes[window:], sizes):
        window_bytes += new - old
        peak = max(peak, window_bytes)
    return {"video_packet_bytes": sum(sizes), "keyframes": sum("K" in packet["flags"] for packet in packets),
            "largest_frame_bytes": max(sizes),
            "peak_window_seconds": window / args.fps, "peak_window_kbps": peak * 8 * args.fps / (1000 * window)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--encoder", default=Path("build/Release/vk264.exe"), type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path, help="New results directory")
    parser.add_argument("--width", default=1920, type=int)
    parser.add_argument("--height", default=1080, type=int)
    parser.add_argument("--fps", default=60, type=int)
    parser.add_argument("--frames", default=120, type=int)
    parser.add_argument("--bitrate", default=6000, type=int, help="Target in kbit/s")
    parser.add_argument("--gop", type=int, help="GOP length, defaults to two seconds")
    parser.add_argument("--qp", default=34, type=int, help="Vk264 initial QP")
    parser.add_argument("--search", type=int, help="Override the Vk264 integer search radius")
    parser.add_argument("--subpel", type=int, choices=[0, 1], help="Override Vk264 quarter-pel refinement")
    parser.add_argument("--partitions", type=int, choices=[0, 1], help="Enable experimental Vk264 8x8 inter partitions")
    parser.add_argument("--p-slice-rows", type=int, choices=[1, 2], help="Override Vk264 P-slice rows in 16x16 mode")
    parser.add_argument("--satd-candidates", type=int, choices=range(9), help="Override Vk264 full-resolution motion candidates")
    parser.add_argument("--ref", type=int, choices=range(1, 17), help="Override the Vk264 reference-picture budget")
    parser.add_argument("--deblock", type=int, choices=[0, 1, 2], help="Vk264 filter: off, within slices, or full frame")
    parser.add_argument("--flat-intra", type=int, choices=[0, 1], help="Test DC-only P-frame intra on flat dark blocks")
    parser.add_argument("--aq", type=int, choices=[0, 1], help="Toggle spatial adaptive quantization in Vk264")
    parser.add_argument("--runs", default=1, type=int)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--encoders", nargs="+", choices=ENCODERS)
    selection.add_argument("--presets", nargs="+", choices=PRESETS, help="Pair each selected Vk264 and x264 preset")
    parser.add_argument("--vmaf", action="store_true")
    parser.add_argument("--mp4", action=argparse.BooleanOptionalAction, default=True,
                        help="Save playable MP4s (default); --no-mp4 keeps raw .h264 intermediates only")
    parser.add_argument("--check-recon", action="store_true", help="Run a separate untimed Vk264 decode-back check")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    args.input, args.out, args.encoder = (path.resolve() for path in (args.input, args.out, args.encoder))
    args.gop = args.gop if args.gop is not None else 2 * args.fps
    if args.presets:
        args.encoders = [f"{codec}-{preset}" for preset in args.presets for codec in ("vk264", "x264")]
    if not args.encoders:
        args.encoders = ["vk264", "x264-slow", "nvenc-p4", "nvenc-p6"]
    if (min(args.width, args.height, args.fps, args.frames, args.runs, args.bitrate, args.gop) <= 0 or
        (args.width | args.height) & 1 or len(set(args.encoders)) != len(args.encoders)):
        parser.error("Use positive counts, even YUV420 dimensions, and unique encoders")
    if args.input.stat().st_size < args.width * args.height * 3 // 2 * args.frames:
        parser.error("The input is shorter than the requested frame count")
    args.out.mkdir(parents=True)
    summary = {"input": str(args.input), "width": args.width, "height": args.height, "frames": args.frames,
               "fps": args.fps, "gop_frames": args.gop, "target_kbps": args.bitrate,
               "vk264_rate_control": "average-rate feedback", "reference_buffer_kbits": 2 * args.bitrate,
               "results": []}
    for name in args.encoders:
        codec, _, preset = name.partition("-")
        suffix = ".mp4" if args.mp4 and codec != "vk264" else ".h264"
        stream = args.out / f"{name}{suffix}"
        command = build_command(args, name, stream)
        timings, reports = [], []
        for repeat in range(args.runs):
            log = args.out / f"{name}.run{repeat + 1}.log"
            timings.append(run_logged(command, log))
            if codec == "vk264":
                reports.append(json.loads(log.read_text().splitlines()[-1]))
        verify_stream(args.ffprobe, stream, args.width, args.height, args.frames)
        recon_verified = False
        if codec == "vk264" and args.check_recon:
            recon, checked_stream = args.out / f"{name}.padded-recon.yuv", args.out / f"{name}.recon-check.h264"
            check_command = build_command(args, name, checked_stream) + ["--recon", recon]
            run_logged(check_command, args.out / f"{name}.recon-check.log")
            if checked_stream.read_bytes() != stream.read_bytes():
                raise ValueError("Timed and reconstruction-check streams differ")
            check_reconstruction(args.ffmpeg, checked_stream, recon, args.width, args.height, args.frames,
                                 padded=not name.startswith("vk264-vulkan"))
            recon_verified = True
            recon.unlink()
            checked_stream.unlink()
        video = stream
        if args.mp4 and codec == "vk264":
            # The raw .h264 is only a recon-check intermediate; mux it and
            # drop it so the output directory contains only the playable .mp4.
            video = stream.with_suffix(".mp4")
            run_logged([args.ffmpeg, "-nostdin", "-v", "error", "-xerror", "-fflags", "+genpts",
                        "-r", args.fps, "-i", stream, "-map", "0:v:0", "-c:v", "copy", video],
                       args.out / f"{name}.mux.log")
            verify_stream(args.ffprobe, video, args.width, args.height, args.frames)
            stream.unlink()
        size = video.stat().st_size
        row = {"encoder": name, "command": list(map(str, command)), "bytes": size,
               "wall_seconds": timings, "process_fps": args.frames / statistics.median(timings),
               "decoded_frames": args.frames, "gpu_reconstruction_verified": recon_verified}
        if reports:
            row["gpu_runs"] = reports
            row["preset"] = reports[-1]["preset"]
            row["effective_settings"] = reports[-1]["settings"]
        elif codec == "x264":
            row["preset"] = preset
            row["effective_settings"] = x264_settings(stream)
        row["video"] = video.name
        row.update(packet_stats(args, video))
        row["achieved_kbps"] = row["video_packet_bytes"] * 8 * args.fps / (1000 * args.frames)
        row.update(quality(args, video, args.out, name))
        summary["results"].append(row)
        (args.out / "summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n")
        print(f"{name:11s} {row['achieved_kbps']:8.1f} kbit/s  {row['process_fps']:6.1f} process fps"
              f"  PSNR-Y {row['psnr_y']}  SSIM {row['ssim']}"
              + (f"  VMAF {row['vmaf']:.3f}" if args.vmaf else ""), flush=True)


if __name__ == "__main__":
    main()
