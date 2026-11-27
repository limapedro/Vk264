#!/usr/bin/env python3
"""Run the standard Vk264 quality/rate matrix on one master.

Usage:
    python harness/matrix.py --input build/obs_new_src.yuv --fps 60 \
        --gop 120 --preset medium --out build/matrix_new \
        --rates 2000 4000 6000 8000
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--gop", type=int, default=120)
    ap.add_argument("--qp", type=int, default=14)
    ap.add_argument("--preset", default="slow")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--rates", nargs="+", type=int, default=[2000, 4000, 6000, 8000])
    ap.add_argument("--encoders", nargs="+",
                    default=[f"wx264-{P}" for P in ("slow",)] +
                            ["x264-slow", "nvenc-p5"])
    a = ap.parse_args()

    width, height = 1920, 1080
    rows = []
    for rate in a.rates:
        out = Path(a.out) / f"r{rate}"
        cmd = [sys.executable, str(ROOT / "harness" / "benchmark.py"),
               "--input", a.input, "--out", str(out),
               "--width", str(width), "--height", str(height),
               "--fps", str(a.fps), "--frames", str(a.frames),
               "--bitrate", str(rate), "--gop", str(a.gop), "--qp", str(a.qp),
               "--encoders", *a.encoders, "--vmaf"]
        print("+", " ".join(cmd), flush=True)
        subprocess.run(cmd, cwd=ROOT, check=True)
        summary = json.loads((out / "summary.json").read_text())
        for r in summary["results"]:
            gpu = r.get("gpu_runs") or [{}]
            rows.append({
                "target": rate, "encoder": r["encoder"],
                "kbps": round(r["achieved_kbps"], 1),
                "psnr_y": round(r["psnr_y"], 3),
                "ssim": round(r["ssim"], 5),
                "vmaf": round(r.get("vmaf", 0), 3),
                "process_fps": round(r["process_fps"], 1),
                "loop_fps": round(gpu[0].get("encode_loop_fps", 0), 1),
            })

    print("\n| target | encoder | kb/s | PSNR-Y | SSIM | VMAF | proc fps | loop fps |")
    print("|---:|---|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        print(f"| {r['target']} | {r['encoder']} | {r['kbps']} | {r['psnr_y']} "
              f"| {r['ssim']} | {r['vmaf']} | {r['process_fps']} | {r['loop_fps']} |")
    (Path(a.out) / "matrix.json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
