#!/usr/bin/env python3
"""Compare Phoenix SPS/PPS NALs to the saved x264 reference streams.

Usage: python tests/compare_headers.py
"""
import pathlib, subprocess, sys

R = pathlib.Path(__file__).resolve().parents[1]
DUMP = R / 'build/tests/Release/phx_header_dump.exe'
REF = R / 'build/parity'
PRESETS = ['ultrafast', 'superfast', 'veryfast', 'faster', 'fast',
           'medium', 'slow', 'slower', 'veryslow', 'placebo']


def nals(data):
    out, i = [], 0
    while True:
        j = data.find(b'\x00\x00\x01', i)
        if j < 0: break
        s = j+3
        k = data.find(b'\x00\x00\x01', s)
        if k < 0: k = len(data)
        out.append(data[s:k]); i = k
    return out


def common_prefix(a, b):
    return next((i for i,(x,y) in enumerate(zip(a,b)) if x != y), min(len(a),len(b)))


def main():
    if not DUMP.exists():
        print('SKIP: build phx_header_dump first (python tools/run_tests.py)')
        return 2
    exact = 0
    for preset in PRESETS:
        for ent in ['cabac', 'cavlc']:
            ref = REF / f'synth-{preset}-{ent}.ref.h264'
            if not ref.exists():
                print('SKIP', ref.name); continue
            got = bytes.fromhex(subprocess.run(
                [str(DUMP), preset, '28', ent], capture_output=True,
                text=True, check=True).stdout.strip())
            target = nals(ref.read_bytes())
            want_sps, want_pps = target[0], target[1]
            # Our dump emits SPS+PPS: split at the second start code.
            k = got.find(b'\x00\x00\x01', 4)
            got_sps, got_pps = got[4:k], got[k+3:]
            ps = common_prefix(got_sps, want_sps)
            pp = common_prefix(got_pps, want_pps)
            ok = got == b'\x00\x00\x00\x01'+want_sps+b'\x00\x00\x00\x01'+want_pps
            exact += ok
            print(f'{preset:10s} {ent:5s} SPS prefix {ps:2d}/{len(want_sps):2d}'
                  f' PPS prefix {pp}/{len(want_pps)} {"EXACT" if ok else ""}')
    print(f'header-exact {exact}/20')
    return 0 if exact == 20 else 1


if __name__ == '__main__':
    sys.exit(main())
