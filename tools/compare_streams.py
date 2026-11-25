#!/usr/bin/env python3
"""Compare Phoenix streams against the saved x264 reference streams.

Standalone: Python standard library only; reads the .h264 files under
build/parity/ (no imports from the harness, no third-party packages).

Stream files are named <case>.phx.h264 (or <case>.h264); each is matched with
<refs>/<case>.ref.h264. Both complete and partial Phoenix directories can be
compared: missing streams are reported but are not treated as mismatches.

Usage:
    python tools/compare_streams.py                       # build/parity by default
    python tools/compare_streams.py <stream-dir> [--refs build/parity]
    python tools/compare_streams.py <stream-dir> --mismatches-only
    python tools/compare_streams.py <stream-dir> --json

Exit codes:
    0  no mismatches among the streams that are present
    1  at least one byte mismatch (or a stream with no matching reference)
    2  references directory missing or contains no *.ref.h264 files
"""
import argparse, json, pathlib, sys

R = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_DIR = R / 'build' / 'parity'
REF_SUFFIX = '.ref.h264'
PHX_SUFFIX = '.phx.h264'
H264_SUFFIX = '.h264'


def first_difference(got, want):
    """Offset of the first differing byte, -1 when both streams are identical."""
    for i, (a, b) in enumerate(zip(got, want)):
        if a != b:
            return i
    if len(got) != len(want):
        return min(len(got), len(want))
    return -1


def stream_case(name):
    if name.endswith(PHX_SUFFIX):
        return name[:-len(PHX_SUFFIX)]
    if name.endswith(H264_SUFFIX):
        return name[:-len(H264_SUFFIX)]
    return pathlib.Path(name).stem


def collect(streams_dir, refs_dir):
    refs = {}
    for p in sorted(refs_dir.glob('*' + REF_SUFFIX)):
        refs[p.name[:-len(REF_SUFFIX)]] = p
    streams = {}
    for p in sorted(streams_dir.glob('*' + PHX_SUFFIX)):
        streams[stream_case(p.name)] = p
    if not streams:  # also accept plain <case>.h264 naming
        for p in sorted(streams_dir.glob('*' + H264_SUFFIX)):
            if not p.name.endswith(REF_SUFFIX):
                streams.setdefault(stream_case(p.name), p)
    return refs, streams


def canonical_cases(refs_dir):
    """The 100 case names recorded by the harness, if its report is present.

    build/parity also holds older unprefixed refs and scratch .h264 files; the
    report is the authoritative case list and keeps those out of the table.
    """
    report = refs_dir / 'parity_report.json'
    if report.is_file():
        try:
            cases = [c['case'] for c in
                     json.loads(report.read_text(encoding='utf-8')).get('cases', [])]
            if cases:
                return cases
        except (OSError, ValueError, TypeError, KeyError):
            pass
    return None


def compare(refs, streams, cases):
    rows = []
    for case in sorted(cases):
        ref_path, stream_path = refs.get(case), streams.get(case)
        row = {'case': case,
               'ref_bytes': ref_path.stat().st_size if ref_path else None,
               'phx_bytes': stream_path.stat().st_size if stream_path else None,
               'first_diff': None, 'status': ''}
        if ref_path is None:
            row['status'] = 'NO REF'
        elif stream_path is None:
            row['status'] = 'NO STREAM'
        else:
            want, got = ref_path.read_bytes(), stream_path.read_bytes()
            d = first_difference(got, want)
            row['first_diff'] = d if d >= 0 else None
            row['status'] = 'EXACT' if got == want else f'DIFF@{d}'
        rows.append(row)
    return rows


def print_table(rows):
    print(f'  {"case":<48} {"ref":>7} {"phx":>7} {"first diff":>10}  status')
    for r in rows:
        ref = str(r['ref_bytes']) if r['ref_bytes'] is not None else '-'
        phx = str(r['phx_bytes']) if r['phx_bytes'] is not None else '-'
        d = str(r['first_diff']) if r['first_diff'] is not None else '-'
        print(f'  {r["case"]:<48} {ref:>7} {phx:>7} {d:>10}  {r["status"]}')


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Byte-compare Phoenix streams with saved x264 references.')
    ap.add_argument('streams_dir', nargs='?', type=pathlib.Path,
                    default=DEFAULT_DIR,
                    help='directory of <case>.phx.h264 streams '
                         '(default: build/parity)')
    ap.add_argument('--refs', type=pathlib.Path, default=DEFAULT_DIR,
                    help='directory of <case>.ref.h264 references '
                         '(default: build/parity)')
    ap.add_argument('--mismatches-only', action='store_true',
                    help='show only mismatched/missing cases')
    ap.add_argument('--json', action='store_true',
                    help='emit machine-readable JSON instead of the table')
    args = ap.parse_args(argv)

    refs_dir, streams_dir = args.refs.resolve(), args.streams_dir.resolve()
    if not refs_dir.is_dir() or not list(refs_dir.glob('*' + REF_SUFFIX)):
        print(f'SKIP: no reference streams under {refs_dir}; run '
              'tools/parity_harness.py first.', file=sys.stderr)
        return 2
    if not streams_dir.is_dir():
        print(f'SKIP: streams directory not found: {streams_dir}',
              file=sys.stderr)
        return 2

    refs, streams = collect(streams_dir, refs_dir)
    canonical = canonical_cases(refs_dir)
    if canonical:
        wanted = set(canonical)
        # Scratch <case>.h264 files are only streams when they match a known
        # case; .phx.h264 files are included even for unknown cases so a
        # misnamed Phoenix stream is still visible in the table.
        streams = {k: v for k, v in streams.items()
                   if k in wanted or v.name.endswith(PHX_SUFFIX)}
        cases = wanted | set(streams)
    else:
        cases = set(refs) | set(streams)
    rows = compare(refs, streams, cases)
    if args.json:
        print(json.dumps(rows, indent=2))
    else:
        shown = [r for r in rows if r['status'] != 'EXACT'] \
            if args.mismatches_only else rows
        print_table(shown)
    exact = sum(1 for r in rows if r['status'] == 'EXACT')
    diffs = sum(1 for r in rows if r['status'].startswith('DIFF@'))
    missing_streams = sum(1 for r in rows if r['status'] == 'NO STREAM')
    missing_refs = sum(1 for r in rows if r['status'] == 'NO REF')
    if not args.json:
        print(f'{len(rows)} cases: {exact} byte-exact, {diffs} mismatches, '
              f'{missing_streams} missing streams, {missing_refs} missing refs')
    return 1 if (diffs or missing_refs) else 0


if __name__ == '__main__':
    sys.exit(main())
