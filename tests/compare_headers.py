#!/usr/bin/env python3
"""Compare Phoenix SPS/PPS NALs to the saved x264 reference streams.

Usage: python tests/compare_headers.py
"""
import pathlib, subprocess, sys

from parse_sps import parse_pps, pps_tail_bits

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
        nal = data[s:k].rstrip(b'\x00')   # Annex-B trailing_zero_8bits
        out.append(nal); i = k
    return out


def common_prefix(a, b):
    return next((i for i,(x,y) in enumerate(zip(a,b)) if x != y), min(len(a),len(b)))


def verify_pps_tail(got_pps, want_pps):
    """Focused regression check for the PPS tail / RBSP stop-bit position.

    Source: reference/x264/encoder/set.c:551-567 — when transform_8x8_mode is
    set, chroma_qp_index_offset is written a *second* time after the two
    transform flags; set.c:570 and common/bitstream.h:272 then append the rbsp
    stop bit.  A simple writer that omits the :567 write still decodes to the
    same field values, but places the stop bit one bit too early, so this
    check compares the encoded tail bits against the set.c-derived oracle.

    SHADER: C — focused verification of the serial header stage.
    """
    ref = parse_pps(want_pps)
    got = parse_pps(got_pps)
    expect = pps_tail_bits(ref['cqo'], ref['transform'], ref['tail_pos'])
    return (ref['tail'] == expect and got['tail_pos'] == ref['tail_pos']
            and got['tail'] == expect
            and (not ref['transform'] or ref['cqo2'] == ref['cqo']))


def main():
    if not DUMP.exists():
        print('SKIP: build phx_header_dump first (python tools/run_tests.py)')
        return 2
    exact = 0
    tails_ok = 0
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
            tail_ok = verify_pps_tail(got_pps, want_pps)
            tails_ok += tail_ok
            print(f'{preset:10s} {ent:5s} SPS prefix {ps:2d}/{len(want_sps):2d}'
                  f' PPS prefix {pp}/{len(want_pps)}'
                  f' {"EXACT" if ok else ""} tail-ok={int(tail_ok)}')
    print(f'header-exact {exact}/20  pps-tail-verified {tails_ok}/20')
    return 0 if exact == 20 and tails_ok == 20 else 1


if __name__ == '__main__':
    sys.exit(main())
