#!/usr/bin/env python3
"""Byte-exact parity harness (rebuilt under the Phoenix repository).

Generates deterministic raw YUV fixtures — a synthetic pattern plus real-video
crops from D:/Videos — encodes them with the authoritative x264 reference at
all 10 presets x 2 entropies (explicit --cabac/--no-cabac), and byte-compares
Phoenix output when a Phoenix encoder is available.

Phoenix CLI contract (tools/phx_cli.c, built to build/tests/Release/phx_cli.exe):
    phx_cli <yuv> <width> <height> <qp> <preset> <cabac|cavlc> <out.h264>

Auto-detection order for PHX_EXE: env override, build/tests/Release, repo root,
build/, build/x264_ref/, reference/, C:/x64/bin. X264_EXE uses the same env
override with the usual build locations (not build/tests/Release).

Outputs:
    build/parity/parity_report.json   machine-readable, per-case + summaries
    tests/parity_report.md            human-readable, per-case + summaries

Exit codes:
    0  reference corpus generated; either no Phoenix encoder (report only,
       "encoder not available") or full byte-exact parity
    1  a x264 encode failed, or Phoenix streams exist but are not byte-exact
    2  x264 reference binary unavailable (not a parity failure)

Usage:
    X264_EXE=/path/to/x264.exe python tools/parity_harness.py
    X264_EXE=... PHX_EXE=... python tools/parity_harness.py
    python tools/make_video_fixtures.py   # optional: add real-video fixtures

The harness is intentionally NOT part of ctest until the Phoenix encoder exists.
"""
import json, os, pathlib, subprocess, sys

R = pathlib.Path(__file__).resolve().parents[1]
WORK = R / 'build' / 'parity'
REPORT_JSON = WORK / 'parity_report.json'
REPORT_MD = R / 'tests' / 'parity_report.md'
PRESETS = ['ultrafast', 'superfast', 'veryfast', 'faster', 'fast',
           'medium', 'slow', 'slower', 'veryslow', 'placebo']
ENTROPIES = [('cabac', '--cabac'), ('cavlc', '--no-cabac')]
W, H, FRAMES = 64, 64, 8
X264_DIRS = (R, R / 'build', R / 'build' / 'x264_ref', R / 'reference',
             pathlib.Path('C:/x64/bin'))
PHX_DIRS = (R / 'build' / 'tests' / 'Release',) + X264_DIRS


def find_exe(env, names, dirs):
    p = os.environ.get(env)
    if p:
        q = pathlib.Path(p)
        if q.is_file():
            return q.resolve()
        print(f'warning: {env}={p} is not a file; falling back to auto-detection',
              file=sys.stderr)
    for n in names:
        for d in dirs:
            q = d / n
            if q.is_file():
                return q.resolve()
    return None


def make_synthetic(path):
    w, h, n = W, H, FRAMES
    buf = bytearray()
    for f in range(n):
        for j in range(h):
            for i in range(w):
                buf.append((i*7 + j*13 + f*29) & 255)
        for j in range(h//2):
            for i in range(w//2):
                buf.append((128 + i*5 + f*11) & 255)
        for j in range(h//2):
            for i in range(w//2):
                buf.append((128 + j*5 + f*7) & 255)
    path.write_bytes(bytes(buf))
    return path


def fixtures():
    WORK.mkdir(parents=True, exist_ok=True)
    out = [('synth', make_synthetic(WORK / 'fixture.yuv'), W, H, FRAMES)]
    manifest = WORK / 'video_manifest.json'
    if manifest.exists():
        for e in json.loads(manifest.read_text(encoding='utf-8')):
            p = pathlib.Path(e['yuv'])
            if p.is_file():
                out.append((e['name'], p, e['width'], e['height'], e['frames']))
    return out


def first_difference(got, want):
    """Offset of the first differing byte, -1 when the streams are identical.

    A common prefix with different lengths reports the offset of the first
    missing byte (the shorter length), which is the most useful diagnostic.
    """
    for i, (a, b) in enumerate(zip(got, want)):
        if a != b:
            return i
    if len(got) != len(want):
        return min(len(got), len(want))
    return -1


def summarize(cases):
    """Totals for a group of cases (all, one fixture, or one preset)."""
    compared = [c for c in cases if 'byte_exact' in c]
    exact = sum(1 for c in compared if c['byte_exact'])
    diffs = [c['first_diff'] for c in compared if c.get('first_diff', -1) >= 0]
    return {'cases': len(cases),
            'x264_ok': sum(1 for c in cases if c['x264_rc'] == 0),
            'x264_failures': sum(1 for c in cases if c['x264_rc'] != 0),
            'compared': len(compared),
            'byte_exact': exact,
            'mismatches': len(compared) - exact,
            'first_diff_min': min(diffs) if diffs else None}


def _status(c):
    if 'byte_exact' not in c:
        return 'encoder not available'
    if c['byte_exact']:
        return 'byte-exact'
    return f'DIFF (phx rc={c["phx_rc"]})'


def print_report(r):
    t = r['totals']
    print(f'x264: {r["x264"]}')
    print('phx:  ' + (r['phx'] or 'not available (set PHX_EXE or build '
                      'build/tests/Release/phx_cli.exe)'))
    print(f'{t["cases"]} reference streams; byte-exact {t["byte_exact"]}/'
          f'{t["cases"]}; report: {REPORT_JSON}')
    print(f'x264 rc=0: {t["x264_ok"]}/{t["cases"]}')
    if not r['phx']:
        print('Phoenix encoder not available: "encoder not available" for all '
              'cases; no byte-exact parity claim.')
    print()
    print('Per-case results:')
    print(f'  {"case":<48} {"ref":>7} {"phx":>7} {"first diff":>10}  status')
    for c in r['cases']:
        phx_bytes = str(c['phx_bytes']) if 'phx_bytes' in c else '-'
        diff = str(c['first_diff']) if c.get('first_diff', -1) >= 0 else '-'
        print(f'  {c["case"]:<48} {c["ref_bytes"]:>7} {phx_bytes:>7} '
              f'{diff:>10}  {_status(c)}')
    for label, key in (('Per-fixture summary', 'by_fixture'),
                       ('Per-preset summary', 'by_preset')):
        print()
        print(f'{label}:')
        print(f'  {"group":<40} {"cases":>6} {"exact":>6} {"mismatch":>9} '
              f'{"first diff":>11}')
        for name, s in r[key].items():
            diff = str(s['first_diff_min']) if s['first_diff_min'] is not None else '-'
            print(f'  {name:<40} {s["cases"]:>6} {s["byte_exact"]:>6} '
                  f'{s["mismatches"]:>9} {diff:>11}')


def write_markdown(r):
    """Keep a human-readable copy of the same report next to the unit tests."""
    t = r['totals']
    L = ['# Parity report (100-case corpus)', '',
         'Generated by `python tools/parity_harness.py`. Machine-readable '
         'source: `build/parity/parity_report.json`.', '',
         f'- Reference encoder: `{r["x264"]}`',
         f'- Phoenix encoder: `{r["phx"]}`' if r['phx']
         else '- Phoenix encoder: **not available** (set `PHX_EXE` or build '
              '`build/tests/Release/phx_cli.exe`)',
         f'- Reference streams: {t["cases"]}; x264 rc=0: {t["x264_ok"]}/'
         f'{t["cases"]}',
         f'- Byte-exact: {t["byte_exact"]}/{t["cases"]}; mismatches: '
         f'{t["mismatches"]}; compared: {t["compared"]}', '']
    if not r['phx']:
        L += ['> **No byte-exact parity claim.** No Phoenix encoder was '
              'available; every case is marked "encoder not available". '
              'This is a reference-corpus result only.', '']
    elif t['byte_exact'] != t['cases']:
        L += ['> **Parity not achieved.** See the per-case table for the first '
              'differing byte offset.', '']
    else:
        L += ['> Byte-exact parity: all cases match.', '']
    for title, key, first in (('Per-fixture summary', 'by_fixture', 'Fixture'),
                              ('Per-preset summary', 'by_preset', 'Preset')):
        L += [f'## {title}', '',
              f'| {first} | Cases | x264 rc=0 | Byte-exact | Mismatches | '
              'First diff |', '| --- | ---: | ---: | ---: | ---: | ---: |']
        for name, s in r[key].items():
            d = s['first_diff_min'] if s['first_diff_min'] is not None else '-'
            L.append(f'| {name} | {s["cases"]} | {s["x264_ok"]} | '
                     f'{s["byte_exact"]} | {s["mismatches"]} | {d} |')
        L.append('')
    L += ['## Per-case results', '',
          '| Case | Fixture | Preset | Entropy | Ref bytes | Phoenix bytes | '
          'First diff | Status |', '| --- | --- | --- | --- | ---: | ---: | '
          '---: | --- |']
    for c in r['cases']:
        phx = c['phx_bytes'] if 'phx_bytes' in c else '-'
        d = c['first_diff'] if c.get('first_diff', -1) >= 0 else '-'
        L.append(f'| {c["case"]} | {c["fixture"]} | {c["preset"]} | '
                 f'{c["entropy"]} | {c["ref_bytes"]} | {phx} | {d} | '
                 f'{_status(c)} |')
    L.append('')
    REPORT_MD.write_text('\n'.join(L), encoding='utf-8')


def main():
    x264 = find_exe('X264_EXE', ['x264.exe', 'x264'], X264_DIRS)
    if not x264:
        print('SKIP: x264 reference binary not found. Set X264_EXE (or build it '
              'with tools/build_reference_x264.py).')
        return 2
    phx = find_exe('PHX_EXE', ['phx_cli.exe', 'phx_cli'], PHX_DIRS)
    fx = fixtures()
    report = {'x264': str(x264), 'phx': str(phx) if phx else None,
              'encoder_status': 'available' if phx else 'encoder not available',
              'corpus': {'fixtures': [e[0] for e in fx], 'presets': PRESETS,
                         'entropies': [e for e, _ in ENTROPIES],
                         'expected_cases': len(fx) * len(PRESETS) * len(ENTROPIES)},
              'cases': []}

    for fname, yuv, w, h, frames in fx:
        for preset in PRESETS:
            for ename, eflag in ENTROPIES:
                tag = f'{fname}-{preset}-{ename}'
                ref = WORK / f'{tag}.ref.h264'
                cmd = [str(x264), '--input-res', f'{w}x{h}', '--input-csp', 'i420',
                       '--fps', '25', '--frames', str(frames), '--qp', '28',
                       '--keyint', '30', '--min-keyint', '30', '--scenecut', '0',
                       '--preset', preset, eflag, str(yuv), '-o', str(ref)]
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
                case = {'case': tag, 'fixture': fname, 'preset': preset,
                        'entropy': ename, 'x264_rc': r.returncode,
                        'ref_bytes': ref.stat().st_size if ref.exists() else 0}
                if phx:
                    out = WORK / f'{tag}.phx.h264'
                    try:
                        r2 = subprocess.run([str(phx), str(yuv), str(w), str(h),
                                             '28', preset, ename, str(out)],
                                            capture_output=True, text=True,
                                            timeout=600)
                        phx_rc = r2.returncode
                    except (OSError, subprocess.TimeoutExpired) as exc:
                        phx_rc = -1
                        case['error'] = str(exc)[:200]
                    got = out.read_bytes() if out.exists() else b''
                    want = ref.read_bytes() if ref.exists() else b''
                    case.update({'phx_rc': phx_rc,
                                 'phx_bytes': len(got),
                                 'byte_exact': got == want,
                                 'first_diff': first_difference(got, want)})
                report['cases'].append(case)

    report['totals'] = summarize(report['cases'])
    report['by_fixture'] = {name: summarize([c for c in report['cases']
                                             if c['fixture'] == name])
                            for name, *_ in fx}
    report['by_preset'] = {preset: summarize([c for c in report['cases']
                                              if c['preset'] == preset])
                           for preset in PRESETS}
    REPORT_JSON.write_text(json.dumps(report, indent=2), encoding='utf-8')
    write_markdown(report)
    print_report(report)

    t = report['totals']
    if t['x264_failures']:
        print(f'FAIL: {t["x264_failures"]} x264 encodes returned non-zero; '
              'reference corpus is invalid.')
        return 1
    if not phx:
        return 0
    return 0 if t['byte_exact'] == t['cases'] else 1


if __name__ == '__main__':
    sys.exit(main())
