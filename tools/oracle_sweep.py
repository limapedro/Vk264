#!/usr/bin/env python3
"""Encode a real clip with Phoenix and x264 through tools/oracle_video.py.

For each (preset, entropy) pair this writes two streams with identical encode
parameters, verifies both decode with ffmpeg, and reports stream sizes plus the
first differing byte.  Makes NO parity claim: it only measures the oracle.

Usage: python tools/oracle_sweep.py [--video PATH] [--width W] [--height H]
       [--frames N] [--qp N] [--fps N] [--presets a,b,c] [--out-dir DIR]
"""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
PRESETS = ('ultrafast', 'superfast', 'veryfast', 'faster', 'fast', 'medium',
           'slow', 'slower', 'veryslow', 'placebo')


def first_diff(a: bytes, b: bytes) -> int | None:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return None if len(a) == len(b) else n


def nal_count(path: pathlib.Path) -> int:
    b = path.read_bytes()
    return b.count(b'\x00\x00\x01')


def run(args: list[str]) -> tuple[int, str]:
    p = subprocess.run(args, cwd=str(ROOT), capture_output=True, text=True,
                       timeout=1800)
    return p.returncode, (p.stdout + p.stderr).strip()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--video', default='D:/Videos/ssstwitter.com_1748999992229.mp4')
    ap.add_argument('--width', type=int, default=64)
    ap.add_argument('--height', type=int, default=64)
    ap.add_argument('--frames', type=int, default=30)
    ap.add_argument('--qp', type=int, default=28)
    ap.add_argument('--fps', type=int, default=25)
    ap.add_argument('--presets', default='ultrafast,superfast,veryfast,medium')
    ap.add_argument('--out-dir', default='build/oracle_sweep')
    a = ap.parse_args(argv)

    out = ROOT / a.out_dir
    out.mkdir(parents=True, exist_ok=True)
    results = []
    for preset in a.presets.split(','):
        for entropy in ('cabac', 'cavlc'):
            tag = f'{preset}-{entropy}'
            phx = out / f'{tag}.phx.h264'
            ref = out / f'{tag}.x264.h264'
            common = ['--width', str(a.width), '--height', str(a.height),
                      '--frames', str(a.frames), '--preset', preset,
                      '--qp', str(a.qp), '--entropy', entropy, '--fps', str(a.fps)]
            rp_phx, txt = run([sys.executable, 'tools/oracle_video.py', a.video,
                               str(phx), *common, '--report', str(out / f'{tag}.phx.json')])
            ok_phx = rp_phx == 0 and 'verify: ffmpeg -f null - : OK' in txt
            rp_ref, txt2 = run([sys.executable, 'tools/oracle_video.py', a.video,
                                str(ref), *common, '--x264',
                                '--report', str(out / f'{tag}.x264.json')])
            ok_ref = rp_ref == 0 and 'verify: ffmpeg -f null - : OK' in txt2
            fd = None
            if phx.exists() and ref.exists():
                fd = first_diff(phx.read_bytes(), ref.read_bytes())
            rec = dict(preset=preset, entropy=entropy, phx_decode_ok=ok_phx,
                       x264_decode_ok=ok_ref,
                       phx_bytes=phx.stat().st_size if phx.exists() else None,
                       x264_bytes=ref.stat().st_size if ref.exists() else None,
                       phx_nals=nal_count(phx) if phx.exists() else None,
                       x264_nals=nal_count(ref) if ref.exists() else None,
                       first_diff=fd)
            results.append(rec)
            print(f'{tag:<22} decode {str(ok_phx):>5}/{str(ok_ref):<5} '
                  f'bytes {rec["phx_bytes"]!s:>6}/{rec["x264_bytes"]!s:<6} '
                  f'first_diff={fd}')

    report = out / 'sweep_report.json'
    report.write_text(json.dumps(dict(video=a.video, width=a.width,
                                      height=a.height, frames=a.frames,
                                      qp=a.qp, fps=a.fps,
                                      parity_claim=False, results=results),
                                 indent=2), encoding='utf-8')
    print('report:', report)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
