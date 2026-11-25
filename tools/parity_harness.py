#!/usr/bin/env python3
"""Byte-exact parity harness (rebuilt under the Phoenix repository).

Generates a deterministic raw YUV fixture, encodes it with the authoritative
x264 reference at all 10 presets x 2 entropies (explicit --cabac/--no-cabac),
and byte-compares Phoenix output when a Phoenix encoder is available.

Usage:
    X264_EXE=/path/to/x264.exe python tools/parity_harness.py
    X264_EXE=... PHX_EXE=/path/to/phx_cli.exe python tools/parity_harness.py

Exit 2 means "reference binary unavailable" (not a parity failure). The harness
is intentionally NOT part of ctest until the Phoenix encoder exists.
"""
import json, os, pathlib, subprocess, sys

R = pathlib.Path(__file__).resolve().parents[1]
WORK = R / 'build' / 'parity'
PRESETS = ['ultrafast', 'superfast', 'veryfast', 'faster', 'fast',
           'medium', 'slow', 'slower', 'veryslow', 'placebo']
ENTROPIES = [('cabac', '--cabac'), ('cavlc', '--no-cabac')]
W, H, FRAMES = 64, 64, 8


def find_exe(env, names):
    p = os.environ.get(env)
    if p and pathlib.Path(p).is_file():
        return pathlib.Path(p)
    for n in names:
        for d in [R, R / 'build', R / 'reference', pathlib.Path('C:/x64/bin')]:
            q = d / n
            if q.is_file():
                return q
    return None


def make_yuv(path):
    w, h, n = W, H, FRAMES
    y = bytearray(w*h*n)
    u = bytearray(w*h//4*n)
    v = bytearray(w*h//4*n)
    for f in range(n):
        for j in range(h):
            for i in range(w):
                y[f*w*h + j*w + i] = (i*7 + j*13 + f*29) & 255
        for j in range(h//2):
            for i in range(w//2):
                u[f*w*h//4 + j*(w//2) + i] = (128 + i*5 + f*11) & 255
                v[f*w*h//4 + j*(w//2) + i] = (128 + j*5 + f*7) & 255
    path.write_bytes(bytes(y + u + v))
    return path


def main():
    x264 = find_exe('X264_EXE', ['x264.exe', 'x264'])
    if not x264:
        print('SKIP: x264 reference binary not found. Set X264_EXE to the '
              'authoritative 8-bit build from reference/x264/.')
        return 2
    phx = find_exe('PHX_EXE', ['phx_cli.exe', 'phx_cli'])
    WORK.mkdir(parents=True, exist_ok=True)
    yuv = make_yuv(WORK / 'fixture.yuv')
    report = {'x264': str(x264), 'phx': str(phx) if phx else None, 'cases': []}

    for preset in PRESETS:
        for ename, eflag in ENTROPIES:
            tag = f'{preset}-{ename}'
            ref = WORK / f'{tag}.ref.h264'
            cmd = [str(x264), '--input-res', f'{W}x{H}', '--input-csp', 'i420',
                   '--fps', '25', '--frames', str(FRAMES), '--qp', '28',
                   '--keyint', '30', '--min-keyint', '30', '--scenecut', '0',
                   '--preset', preset, eflag, str(yuv), '-o', str(ref)]
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
            case = {'case': tag, 'x264_rc': r.returncode,
                    'ref_bytes': ref.stat().st_size if ref.exists() else 0}
            if phx:
                out = WORK / f'{tag}.phx.h264'
                r2 = subprocess.run([str(phx), str(yuv), str(W), str(H), '28',
                                     preset, ename, str(out)],
                                    capture_output=True, text=True, timeout=300)
                got = out.read_bytes() if out.exists() else b''
                want = ref.read_bytes() if ref.exists() else b''
                case.update({'phx_rc': r2.returncode,
                             'byte_exact': got == want,
                             'first_diff': next((i for i,(a,b) in enumerate(zip(got,want))
                                                 if a != b), -1)})
            report['cases'].append(case)

    (WORK / 'parity_report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    exact = sum(1 for c in report['cases'] if c.get('byte_exact'))
    print(f'{len(report["cases"])} reference streams; byte-exact {exact}/'
          f'{len(report["cases"])}; report: {WORK / "parity_report.json"}')
    return 0 if phx and exact == len(report['cases']) else 1


if __name__ == '__main__':
    sys.exit(main())
