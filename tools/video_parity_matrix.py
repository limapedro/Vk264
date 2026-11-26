#!/usr/bin/env python3
"""Periodic byte-exact parity matrix over a manifest of real videos.

For every video in the manifest this drives tools/oracle_video.py twice with
identical encode parameters (Phoenix and x264), at the video's NATIVE size
(no crop/scale), then measures:

  * both streams' ffmpeg decode error counts (stderr, not just exit status)
  * stream sizes and Annex-B NAL counts
  * first differing byte
  * byte-exact status (only this counts as parity)

Makes NO parity claim by itself: it is the measurement loop used to drive the
port and to detect regressions.  All artifacts are written under --out-dir.

Usage:
  python tools/video_parity_matrix.py --manifest build/oracle_matrix/videos16.json
  python tools/video_parity_matrix.py --limit 4 --presets ultrafast,medium
"""
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
FF = pathlib.Path(r'C:/ffmpeg/ffmpeg-master-latest-win64-gpl/bin/ffmpeg.EXE')
FFPROBE = pathlib.Path(r'C:/ffmpeg/ffmpeg-master-latest-win64-gpl/bin/ffprobe.EXE')
PRESETS = ('ultrafast', 'superfast', 'veryfast', 'faster', 'fast', 'medium',
           'slow', 'slower', 'veryslow', 'placebo')


def run(args: list[str]) -> tuple[int, str]:
    p = subprocess.run(args, cwd=str(ROOT), capture_output=True, text=True,
                       timeout=3600)
    return p.returncode, (p.stdout + p.stderr)


def probe(video: str) -> dict:
    p = subprocess.run([str(FFPROBE), '-v', 'error', '-select_streams', 'v:0',
                        '-show_entries', 'stream=width,height,nb_frames',
                        '-of', 'json', video], capture_output=True, text=True)
    s = json.loads(p.stdout)['streams'][0]
    return dict(width=int(s.get('width', 0)), height=int(s.get('height', 0)),
                frames=int(s.get('nb_frames', 0)))


def decode_errors(path: pathlib.Path) -> tuple[int, str | None]:
    p = subprocess.run([str(FF), '-v', 'error', '-i', str(path), '-f', 'null', '-'],
                       capture_output=True, text=True, timeout=3600)
    errs = [ln.strip() for ln in p.stderr.splitlines() if ln.strip()]
    return len(errs), errs[0] if errs else None


def first_diff(a: bytes, b: bytes) -> int | None:
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return None if len(a) == len(b) else n


def nals(path: pathlib.Path) -> int:
    return path.read_bytes().count(b'\x00\x00\x01')


def encode(video: str, out: pathlib.Path, preset: str, entropy: str,
           frames: int, qp: int, use_x264: bool,
           width: int = 0, height: int = 0) -> tuple[bool, str]:
    args = [sys.executable, 'tools/oracle_video.py', video, str(out),
            '--frames', str(frames), '--preset', preset, '--qp', str(qp),
            '--entropy', entropy, '--fps', '25',
            '--report', str(out.with_suffix('.json'))]
    if width and height:
        args += ['--width', str(width), '--height', str(height),
                 '--fit', 'scale']
    if use_x264:
        args.append('--x264')
    rc, text = run(args)
    return rc == 0, text


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--presets', default='ultrafast')
    ap.add_argument('--entropies', default='cabac')
    ap.add_argument('--frames', type=int, default=30)
    ap.add_argument('--width', type=int, default=0,
                    help='optional scaled width (0 = native)')
    ap.add_argument('--height', type=int, default=0,
                    help='optional scaled height (0 = native)')
    ap.add_argument('--qp', type=int, default=28)
    ap.add_argument('--out-dir', default='build/oracle_matrix')
    a = ap.parse_args(argv)

    manifest = json.loads((ROOT / a.manifest).read_text(encoding='utf-8'))
    videos = manifest[:a.limit] if a.limit else manifest
    out = ROOT / a.out_dir
    out.mkdir(parents=True, exist_ok=True)

    results = []
    for entry in videos:
        name = entry['name']
        video = entry['source']
        geo = probe(video)
        frames = min(a.frames, geo['frames'] or a.frames)
        if a.width and a.height:
            geo['width'], geo['height'] = a.width, a.height
        for preset in a.presets.split(','):
            for entropy in a.entropies.split(','):
                tag = f'{name}-{preset}-{entropy}'
                phx = out / f'{tag}.phx.h264'
                ref = out / f'{tag}.x264.h264'
                ok_p, _ = encode(video, phx, preset, entropy, frames, a.qp,
                                 False, a.width, a.height)
                ok_r, _ = encode(video, ref, preset, entropy, frames, a.qp,
                                 True, a.width, a.height)
                pe, perr = decode_errors(phx) if phx.exists() else (-1, None)
                re_, rerr = decode_errors(ref) if ref.exists() else (-1, None)
                pb = phx.stat().st_size if phx.exists() else None
                rb = ref.stat().st_size if ref.exists() else None
                both_exist = (phx.exists() and phx.stat().st_size > 0 and
                              ref.exists() and ref.stat().st_size > 0)
                fd = (first_diff(phx.read_bytes(), ref.read_bytes())
                      if both_exist else None)
                rec = dict(name=name, preset=preset, entropy=entropy,
                           width=geo['width'], height=geo['height'],
                           frames=frames, phx_rc0=ok_p, x264_rc0=ok_r,
                           phx_bytes=pb, x264_bytes=rb,
                           phx_decode_errors=pe, x264_decode_errors=re_,
                           phx_first_error=perr, x264_first_error=rerr,
                           phx_nals=nals(phx) if phx.exists() else None,
                           x264_nals=nals(ref) if ref.exists() else None,
                           first_diff=fd,
                           byte_exact=bool(both_exist and fd is None))
                results.append(rec)
                print(f'{name[:28]:<28} {preset:<10} {entropy:<5} '
                      f'{geo["width"]}x{geo["height"]:<5} '
                      f'err {pe:>3}/{re_:<3} first_diff={fd} '
                      f'bytes {pb!s:>7}/{rb!s:<7}')

    exact = sum(1 for r in results if r['byte_exact'])
    clean = sum(1 for r in results if r['phx_decode_errors'] == 0)
    report = out / 'matrix_report.json'
    report.write_text(json.dumps(dict(video_count=len(videos),
                                      params=dict(presets=a.presets,
                                                  entropies=a.entropies,
                                                  frames=a.frames, qp=a.qp),
                                      byte_exact=exact, phx_clean_decode=clean,
                                      total=len(results),
                                      parity_claim=bool(exact == len(results)),
                                      results=results), indent=2),
                      encoding='utf-8')
    print(f'\nbyte_exact {exact}/{len(results)}  phx_clean_decode '
          f'{clean}/{len(results)}  report: {report}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
