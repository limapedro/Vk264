#!/usr/bin/env python3
"""Analyse the extended video parity corpus (fixtures x presets x entropies).

This is an aggregate diagnostic, not a parity gate.  It reads the machine
readable report written by tools/parity_harness.py and the Annex-B streams in
build/parity/, then prints:

  * per-fixture byte-exact counts and first-diff distributions
    (min / median / max plus fixed histogram bins);
  * per-preset breakdowns, including which presets fail earliest and which
    have the largest Phoenix/x264 byte-size ratio;
  * per-case NAL-type sequences (slice counts, B slices, SEI presence);
  * compact clusters of cases sharing the same first-diff/NAL signature.

Missing streams are reported as missing and never treated as an error: the
report remains authoritative for byte counts and first-diff offsets, while the
streams supply the NAL structure when present.  Nothing is encoded, regenerated
or modified by this tool; Python standard library only.

Usage:
    python tools/extended_video_probe.py
    python tools/extended_video_probe.py --report build/parity/parity_report.json
    python tools/extended_video_probe.py --streams-dir build/parity --json
    python tools/extended_video_probe.py --markdown

By default a compact human-readable summary goes to stdout.  --json and
--markdown select the stdout format (both may be given).  Either way the same
two files are (re)written: build/video_corpus_report.json and
build/video_corpus_report.md.

Exit codes:
    0  analysis completed (0/100 byte-exact still exits 0 -- this tool reports,
       it does not certify parity)
    2  the report is missing, unreadable, or contains no cases
"""
from __future__ import annotations

import argparse
import json
import pathlib
import statistics
import sys
from collections import Counter
from typing import Any, Dict, List, Optional, Sequence, Tuple

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_REPORT = ROOT / 'build' / 'parity' / 'parity_report.json'
DEFAULT_STREAMS_DIR = ROOT / 'build' / 'parity'
JSON_OUT = ROOT / 'build' / 'video_corpus_report.json'
MD_OUT = ROOT / 'build' / 'video_corpus_report.md'

REF_SUFFIX = '.ref.h264'
PHX_SUFFIX = '.phx.h264'
PLAIN_SUFFIX = '.h264'

# Fixed, comparable bins.  Values are offsets of the first differing byte.
DIFF_BINS: Tuple[Tuple[int, Optional[int], str], ...] = (
    (0, 0, '0'),
    (1, 15, '1-15'),
    (16, 31, '16-31'),
    (32, 47, '32-47'),
    (48, 63, '48-63'),
    (64, 127, '64-127'),
    (128, 255, '128-255'),
    (256, 1023, '256-1023'),
    (1024, None, '1024+'),
)
EXTRA_BINS = ('exact', 'unknown')
HISTOGRAM_KEYS = tuple(label for _, _, label in DIFF_BINS) + EXTRA_BINS

NAL_NAMES = {
    1: 'non-IDR slice', 5: 'IDR slice', 6: 'SEI', 7: 'SPS', 8: 'PPS',
    9: 'AUD', 10: 'end of sequence', 11: 'end of stream', 12: 'filler',
    14: 'prefix', 20: 'slice extension',
}
SLICE_TYPES = {0: 'P', 1: 'B', 2: 'I', 3: 'SP', 4: 'SI',
               5: 'P', 6: 'B', 7: 'I', 8: 'SP', 9: 'SI'}


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def nal_name(nal_type: int) -> str:
    return NAL_NAMES.get(nal_type, 'reserved/other')


def rle(values: Sequence[Any]) -> str:
    """Compact run-length rendering: [7,8,1,1,1] -> '7,8,1x3'."""
    if not values:
        return ''
    parts: List[str] = []
    current, count = values[0], 1
    for value in values[1:]:
        if value == current:
            count += 1
        else:
            parts.append(str(current) if count == 1 else f'{current}x{count}')
            current, count = value, 1
    parts.append(str(current) if count == 1 else f'{current}x{count}')
    return ','.join(parts)


def fmt(value: Optional[float]) -> str:
    if value is None:
        return '-'
    if isinstance(value, int):
        return str(value)
    return f'{value:g}'


def first_difference(a: bytes, b: bytes) -> int:
    """Offset of the first differing byte, -1 when identical (as in harness)."""
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b)) if len(a) != len(b) else -1


# ---------------------------------------------------------------------------
# Annex-B stream inspection
# ---------------------------------------------------------------------------

def annexb_nals(data: bytes) -> List[bytes]:
    """Split an Annex-B byte stream into NAL payloads (header byte included).

    Handles 3- and 4-byte start codes and drops trailing_zero_8bits.  This is
    the same light-weight scan used by tools/first_frame_probe.py; full syntax
    decoding is deliberately out of scope for a corpus-wide sweep.
    """
    starts: List[Tuple[int, int]] = []
    i, n = 0, len(data)
    while i + 2 < n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            starts.append((i - 1 if i > 0 and data[i - 1] == 0 else i, i + 3))
            i += 3
        else:
            i += 1
    out: List[bytes] = []
    for k, (_, payload_pos) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else n
        payload = data[payload_pos:end].rstrip(b'\x00')
        if payload:
            out.append(payload)
    return out


def rbsp_unescape(nal: bytes) -> bytes:
    """Remove emulation prevention bytes from a NAL payload (00 00 03)."""
    out = bytearray()
    zeros = 0
    for byte in nal[1:]:                       # skip the one-byte NAL header
        if zeros >= 2 and byte == 3:
            zeros = 0
            continue
        out.append(byte)
        zeros = zeros + 1 if byte == 0 else 0
    return bytes(out)


def read_ue(data: bytes, pos: int) -> Tuple[Optional[int], int]:
    """One unsigned Exp-Golomb code; (None, pos) on underflow."""
    zeros = 0
    nbits = len(data) * 8
    while True:
        if pos >= nbits:
            return None, pos
        bit = (data[pos >> 3] >> (7 - (pos & 7))) & 1
        pos += 1
        if bit:
            break
        zeros += 1
        if zeros > 31:
            return None, pos
    value = (1 << zeros) - 1
    if zeros:
        suffix = 0
        for _ in range(zeros):
            if pos >= nbits:
                return None, pos
            suffix = (suffix << 1) | ((data[pos >> 3] >> (7 - (pos & 7))) & 1)
            pos += 1
        value += suffix
    return value, pos


def slice_type_of(nal: bytes) -> str:
    """Decode just first_mb_in_slice and slice_type from a slice NAL.

    Labels are normalised (5..9 collapse to 0..4) so a B slice is reported as
    'B' regardless of whether x264 wrote the 'all slices in picture' form.
    """
    rbsp = rbsp_unescape(nal)
    _, pos = read_ue(rbsp, 0)
    code, _ = read_ue(rbsp, pos)
    return SLICE_TYPES.get(code, '?') if code is not None else '?'


def inspect_stream(path: pathlib.Path) -> Dict[str, Any]:
    """Return a JSON-friendly description; a missing file is not an error."""
    info: Dict[str, Any] = {
        'available': False, 'path': str(path), 'bytes': None, 'reason': None,
        'nal_count': None, 'nal_types': None, 'nal_rle': None,
        'nal_counts': None, 'slice_count': None, 'b_slices': None,
        'sei_count': None, 'slice_types': None, 'slice_types_rle': None,
    }
    if not path.is_file():
        info['reason'] = 'missing'
        return info
    try:
        data = path.read_bytes()
    except OSError as exc:
        info['reason'] = f'unreadable: {exc}'
        return info
    nals = annexb_nals(data)
    types = [nal[0] & 0x1f for nal in nals]
    slices = [nal for nal in nals if (nal[0] & 0x1f) in (1, 5)]
    slice_types = [slice_type_of(nal) for nal in slices]
    info.update({
        'available': True,
        'bytes': len(data),
        'nal_count': len(nals),
        'nal_types': types,
        'nal_rle': rle(types),
        'nal_counts': {str(k): v for k, v in sorted(Counter(types).items())},
        'slice_count': len(slices),
        'b_slices': sum(1 for t in slice_types if t == 'B'),
        'sei_count': types.count(6),
        'slice_types': slice_types,
        'slice_types_rle': rle(slice_types),
    })
    return info


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------

def distribution(values: Sequence[float]) -> Dict[str, Optional[float]]:
    return {
        'count': len(values),
        'min': min(values) if values else None,
        'median': statistics.median(values) if values else None,
        'max': max(values) if values else None,
    }


def first_diff_histogram(records: Sequence[Dict[str, Any]]) -> Dict[str, int]:
    hist = {label: 0 for label in HISTOGRAM_KEYS}
    for rec in records:
        diff = rec['first_diff']
        if rec['byte_exact']:
            hist['exact'] += 1
            continue
        if not isinstance(diff, int) or diff < 0:
            hist['unknown'] += 1
            continue
        for lo, hi, label in DIFF_BINS:
            if diff >= lo and (hi is None or diff <= hi):
                hist[label] += 1
                break
    return hist


def summarise(records: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    diffs = [r['first_diff'] for r in records
             if isinstance(r['first_diff'], int) and r['first_diff'] >= 0]
    ratios = [r['size_ratio'] for r in records if r['size_ratio'] is not None]
    return {
        'cases': len(records),
        'byte_exact': sum(1 for r in records if r['byte_exact']),
        'mismatches': sum(1 for r in records
                          if not r['byte_exact'] and r['first_diff'] is not None),
        'missing_ref_streams': sum(1 for r in records if not r['ref']['available']),
        'missing_phx_streams': sum(1 for r in records if not r['phx']['available']),
        'first_diff': distribution(diffs),
        'first_diff_histogram': first_diff_histogram(records),
        'size_ratio': distribution(ratios),
    }


def build_records(cases: Sequence[Dict[str, Any]],
                  streams_dir: pathlib.Path) -> List[Dict[str, Any]]:
    records: List[Dict[str, Any]] = []
    for case in cases:
        name = str(case.get('case', ''))
        ref_path = streams_dir / f'{name}{REF_SUFFIX}'
        phx_path = streams_dir / f'{name}{PHX_SUFFIX}'
        if not phx_path.is_file():
            alt = streams_dir / f'{name}{PLAIN_SUFFIX}'
            if alt.is_file() and not alt.name.endswith(REF_SUFFIX):
                phx_path = alt
        ref = inspect_stream(ref_path)
        phx = inspect_stream(phx_path)

        rep_ref = case.get('ref_bytes')
        rep_phx = case.get('phx_bytes')
        rep_ref = rep_ref if isinstance(rep_ref, int) else None
        rep_phx = rep_phx if isinstance(rep_phx, int) else None
        diff = case.get('first_diff')
        diff = diff if isinstance(diff, int) and diff >= 0 else None

        ratio = None
        if rep_ref and rep_ref > 0 and rep_phx is not None:
            ratio = rep_phx / rep_ref

        size_check = None
        if ref['available'] and phx['available']:
            measured = first_difference(phx_path.read_bytes(), ref_path.read_bytes())
            measured = measured if measured >= 0 else None
            size_check = (measured == diff) if diff is not None else None

        if not ref['available'] and not phx['available']:
            status = 'both streams missing'
        elif not ref['available']:
            status = 'reference stream missing'
        elif not phx['available']:
            status = 'Phoenix stream missing'
        else:
            status = 'streams present'

        records.append({
            'case': name,
            'fixture': str(case.get('fixture', '')),
            'preset': str(case.get('preset', '')),
            'entropy': str(case.get('entropy', '')),
            'ref_bytes': rep_ref,
            'phx_bytes': rep_phx,
            'byte_exact': bool(case.get('byte_exact', False)),
            'first_diff': diff,
            'x264_rc': case.get('x264_rc'),
            'phx_rc': case.get('phx_rc'),
            'size_ratio': ratio,
            'stream_status': status,
            'stream_first_diff_matches_report': size_check,
            'ref': ref,
            'phx': phx,
        })
    return records


def signature_of(rec: Dict[str, Any]) -> Tuple[Any, ...]:
    """The clustering key: first diff plus both compact NAL signatures.

    A signature is the run-length NAL-type sequence plus *counts* of decoded
    slice types (not their order).  Counts keep B-slice differences visible
    while keeping the cluster table readable for 60-slice streams.
    """
    def nal_sig(info: Dict[str, Any]) -> str:
        if not info['available']:
            return f'missing({info["reason"]})'
        counts = Counter(info['slice_types'] or [])
        mix = ','.join(f'{k}{counts[k]}' for k in ('I', 'P', 'B', 'SP', 'SI', '?')
                       if counts[k])
        return f'{info["nal_rle"]}|{mix or "no slices"}'

    return (rec['first_diff'], nal_sig(rec['ref']), nal_sig(rec['phx']))


def build_clusters(records: Sequence[Dict[str, Any]]) -> List[Dict[str, Any]]:
    groups: Dict[Tuple[Any, ...], List[Dict[str, Any]]] = {}
    for rec in records:
        groups.setdefault(signature_of(rec), []).append(rec)
    out: List[Dict[str, Any]] = []
    for sig, members in groups.items():
        example = members[0]
        out.append({
            'cases': len(members),
            'first_diff': sig[0],
            'ref_signature': sig[1],
            'phx_signature': sig[2],
            'fixtures': sorted({m['fixture'] for m in members}),
            'presets': sorted({m['preset'] for m in members}),
            'entropies': sorted({m['entropy'] for m in members}),
            'examples': [m['case'] for m in members[:5]],
            'byte_exact': sum(1 for m in members if m['byte_exact']),
        })
    out.sort(key=lambda c: (-c['cases'],
                            c['first_diff'] if c['first_diff'] is not None else -1,
                            c['ref_signature'], c['phx_signature']))
    return out


def preset_breakdown(records: Sequence[Dict[str, Any]],
                     presets: Sequence[str]) -> Dict[str, Any]:
    found = sorted({r['preset'] for r in records} | set(presets))
    out: Dict[str, Any] = {}
    for preset in found:
        members = [r for r in records if r['preset'] == preset]
        if not members:
            continue
        summary = summarise(members)
        by_diff = sorted((r for r in members if r['first_diff'] is not None),
                         key=lambda r: (r['first_diff'], r['case']))
        by_ratio = sorted((r for r in members if r['size_ratio'] is not None),
                          key=lambda r: (-r['size_ratio'], r['case']))
        summary.update({
            'earliest_cases': [r['case'] for r in by_diff[:3]],
            'largest_ratio_cases': [
                {'case': r['case'], 'ratio': r['size_ratio']}
                for r in by_ratio[:3]
            ],
        })
        out[preset] = summary
    return out


def analyse(report_path: pathlib.Path,
            streams_dir: pathlib.Path) -> Dict[str, Any]:
    raw = json.loads(report_path.read_text(encoding='utf-8'))
    cases = raw.get('cases', [])
    if not isinstance(cases, list) or not cases:
        raise ValueError('report has no cases')
    corpus = raw.get('corpus', {}) if isinstance(raw.get('corpus'), dict) else {}
    records = build_records(cases, streams_dir)

    fixtures = [str(f) for f in corpus.get('fixtures', [])] or sorted(
        {r['fixture'] for r in records})
    presets = [str(p) for p in corpus.get('presets', [])] or sorted(
        {r['preset'] for r in records})

    by_fixture = {
        name: summarise([r for r in records if r['fixture'] == name])
        for name in fixtures
    }
    return {
        'tool': 'extended_video_probe',
        'inputs': {
            'report': str(report_path),
            'streams_dir': str(streams_dir),
        },
        'corpus': {
            'fixtures': fixtures,
            'presets': presets,
            'entropies': [str(e) for e in corpus.get('entropies', [])],
            'expected_cases': corpus.get('expected_cases', len(records)),
        },
        'method': {
            'first_diff_source': 'parity_report.json (streams cross-checked when both present)',
            'histogram_bins': [label for _, _, label in DIFF_BINS] + list(EXTRA_BINS),
            'cluster_signature': 'first_diff + (ref NAL RLE | slice-type counts) + (phx NAL RLE | slice-type counts)',
            'slice_type_labels': ['I', 'P', 'B', 'SP', 'SI', '?'],
        },
        'totals': summarise(records),
        'by_fixture': by_fixture,
        'by_preset': preset_breakdown(records, presets),
        'cases': records,
        'clusters': build_clusters(records),
    }


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def hist_line(hist: Dict[str, int], indent: str = '    ') -> str:
    return '\n'.join(f'{indent}{label:<10} {hist[label]:>4}'
                     for label in HISTOGRAM_KEYS)


def render_text(result: Dict[str, Any]) -> str:
    lines: List[str] = []
    t = result['totals']
    lines.append('Extended video corpus probe')
    lines.append(f'  report:  {result["inputs"]["report"]}')
    lines.append(f'  streams: {result["inputs"]["streams_dir"]}')
    lines.append('')
    lines.append('CORPUS TOTALS')
    lines.append(f'  cases {t["cases"]}; byte-exact {t["byte_exact"]}/{t["cases"]}; '
                 f'mismatches {t["mismatches"]}')
    lines.append(f'  missing streams: ref={t["missing_ref_streams"]} '
                 f'phx={t["missing_phx_streams"]}')
    fd = t['first_diff']
    sr = t['size_ratio']
    lines.append(f'  first-diff min/median/max: {fmt(fd["min"])} / '
                 f'{fmt(fd["median"])} / {fmt(fd["max"])} '
                 f'(n={fd["count"]})')
    lines.append(f'  size ratio min/median/max: {fmt(sr["min"])} / '
                 f'{fmt(sr["median"])} / {fmt(sr["max"])}')
    lines.append('  first-diff histogram:')
    lines.append(hist_line(t['first_diff_histogram']))

    lines.append('')
    lines.append('BY FIXTURE')
    lines.append(f'  {"fixture":<32} {"cases":>5} {"exact":>5} '
                 f'{"min":>5} {"med":>5} {"max":>5} {"ratio med":>10}')
    for name, s in result['by_fixture'].items():
        fd, sr = s['first_diff'], s['size_ratio']
        lines.append(f'  {name:<32} {s["cases"]:>5} {s["byte_exact"]:>5} '
                     f'{fmt(fd["min"]):>5} {fmt(fd["median"]):>5} '
                     f'{fmt(fd["max"]):>5} {fmt(sr["median"]):>10}')
    for name, s in result['by_fixture'].items():
        lines.append(f'  {name} histogram:')
        lines.append(hist_line(s['first_diff_histogram'], '      '))

    lines.append('')
    lines.append('BY PRESET (earliest failure / largest size ratio)')
    lines.append(f'  {"preset":<12} {"exact":>5} {"min":>5} {"med":>5} '
                 f'{"max":>5} {"ratio med/max":>15}  earliest  largest ratio')
    for name, s in result['by_preset'].items():
        fd, sr = s['first_diff'], s['size_ratio']
        earliest = s['earliest_cases'][0] if s['earliest_cases'] else '-'
        biggest = s['largest_ratio_cases']
        largest = f'{biggest[0]["case"]} ({fmt(biggest[0]["ratio"])})' \
            if biggest else '-'
        lines.append(f'  {name:<12} {s["byte_exact"]:>5} {fmt(fd["min"]):>5} '
                     f'{fmt(fd["median"]):>5} {fmt(fd["max"]):>5} '
                     f'{fmt(sr["median"]) + "/" + fmt(sr["max"]):>15}  '
                     f'{earliest}  {largest}')

    lines.append('')
    lines.append('NAL SEQUENCES PER CASE (RLE; I/P/B = decoded slice types)')
    lines.append(f'  {"case":<48} {"ref NALs":<22} {"phx NALs":<22} '
                 f'{"slices":>8} {"B":>5} {"SEI":>5}')
    for rec in result['cases']:
        ref, phx = rec['ref'], rec['phx']
        slices = (f'{fmt(ref["slice_count"])}/{fmt(phx["slice_count"])}'
                  if ref['available'] and phx['available'] else '-')
        b_slices = (f'{fmt(ref["b_slices"])}/{fmt(phx["b_slices"])}'
                    if ref['available'] and phx['available'] else '-')
        sei = (f'{fmt(ref["sei_count"])}/{fmt(phx["sei_count"])}'
               if ref['available'] and phx['available'] else '-')
        missing = '' if rec['stream_status'] == 'streams present' \
            else f' [{rec["stream_status"]}]'
        lines.append(f'  {rec["case"]:<48} '
                     f'{(ref["nal_rle"] or "-") if ref["available"] else "MISSING":<22} '
                     f'{(phx["nal_rle"] or "-") if phx["available"] else "MISSING":<22} '
                     f'{slices:>8} {b_slices:>5} {sei:>5}{missing}')

    lines.append('')
    lines.append('CLUSTERS (identical first-diff + NAL/slice signature)')
    lines.append(f'  {"#":>3} {"cases":>5} {"fd":>5}  {"ref signature":<40} '
                 f'{"phx signature":<40}  examples')
    for i, c in enumerate(result['clusters'], 1):
        lines.append(f'  {i:>3} {c["cases"]:>5} {fmt(c["first_diff"]):>5}  '
                     f'{c["ref_signature"]:<40} {c["phx_signature"]:<40}  '
                     f'{", ".join(c["examples"])}')

    lines.append('')
    lines.append('NOTE: 0/' + str(t['cases']) + ' byte-exact is NOT success. '
                 'It means no case has been proven identical to x264; clusters '
                 'show where to start fixing writers/entropy, presets show '
                 'which configurations fail earliest.')
    return '\n'.join(lines) + '\n'


def _md_table(header: Sequence[str], rows: Sequence[Sequence[Any]]) -> str:
    out = ['| ' + ' | '.join(header) + ' |',
           '|' + '|'.join('---' for _ in header) + '|']
    for row in rows:
        out.append('| ' + ' | '.join(str(v) for v in row) + ' |')
    return '\n'.join(out)


def render_markdown(result: Dict[str, Any]) -> str:
    t = result['totals']
    fd, sr = t['first_diff'], t['size_ratio']
    lines = [
        '# Extended video corpus analysis',
        '',
        f'- Report: `{result["inputs"]["report"]}`',
        f'- Streams: `{result["inputs"]["streams_dir"]}`',
        f'- Cases: {t["cases"]}; byte-exact: {t["byte_exact"]}/{t["cases"]}; '
        f'mismatches: {t["mismatches"]}',
        f'- Missing streams: ref={t["missing_ref_streams"]}, '
        f'phx={t["missing_phx_streams"]}',
        f'- First-diff min/median/max: {fmt(fd["min"])} / {fmt(fd["median"])} '
        f'/ {fmt(fd["max"])}',
        f'- Size ratio min/median/max: {fmt(sr["min"])} / {fmt(sr["median"])} '
        f'/ {fmt(sr["max"])}',
        '',
        '> **0/' + str(t['cases']) + ' byte-exact is NOT success.** '
        'It means zero cases have been proven byte-identical to x264. '
        'Use the clusters and preset table below to prioritise fixes.',
        '',
        '## First-diff histogram (whole corpus)',
        '',
        _md_table(['bin', 'cases'],
                  [(label, t['first_diff_histogram'][label])
                   for label in HISTOGRAM_KEYS]),
        '',
        '## Per fixture',
        '',
    ]
    rows = []
    for name, s in result['by_fixture'].items():
        f, r = s['first_diff'], s['size_ratio']
        rows.append((name, s['cases'], s['byte_exact'],
                     fmt(f['min']), fmt(f['median']), fmt(f['max']),
                     fmt(r['median']), fmt(r['max'])))
    lines += [_md_table(['fixture', 'cases', 'byte-exact', 'fd min', 'fd med',
                         'fd max', 'ratio med', 'ratio max'], rows), '']
    lines += ['### First-diff histogram by fixture', '']
    hist_rows = []
    for name, s in result['by_fixture'].items():
        for label in HISTOGRAM_KEYS:
            hist_rows.append((name, label, s['first_diff_histogram'][label]))
    lines += [_md_table(['fixture', 'bin', 'cases'], hist_rows), '',
              '## Per preset (earliest failure / largest size ratio)', '']
    rows = []
    for name, s in result['by_preset'].items():
        f, r = s['first_diff'], s['size_ratio']
        earliest = s['earliest_cases'][0] if s['earliest_cases'] else '-'
        biggest = s['largest_ratio_cases']
        largest = f'{biggest[0]["case"]} ({fmt(biggest[0]["ratio"])})' \
            if biggest else '-'
        rows.append((name, s['byte_exact'], fmt(f['min']), fmt(f['median']),
                     fmt(f['max']), fmt(r['median']), fmt(r['max']),
                     earliest, largest))
    lines += [_md_table(['preset', 'byte-exact', 'fd min', 'fd med', 'fd max',
                         'ratio med', 'ratio max', 'earliest case',
                         'largest ratio case'], rows), '',
              '## NAL sequences per case', '',
              'RLE means run-length encoding (`1x7` = seven consecutive '
              'slice NALs). Slice types are decoded from the first two '
              'Exp-Golomb fields of each slice header.', '']
    rows = []
    for rec in result['cases']:
        ref, phx = rec['ref'], rec['phx']
        rows.append((
            rec['case'],
            (ref['nal_rle'] or '-') if ref['available'] else 'MISSING',
            (phx['nal_rle'] or '-') if phx['available'] else 'MISSING',
            f'{ref["slice_count"]}/{phx["slice_count"]}'
            if ref['available'] and phx['available'] else '-',
            f'{ref["b_slices"]}/{phx["b_slices"]}'
            if ref['available'] and phx['available'] else '-',
            f'{ref["sei_count"]}/{phx["sei_count"]}'
            if ref['available'] and phx['available'] else '-',
            rec['stream_status'],
        ))
    lines += [_md_table(['case', 'ref NALs', 'phx NALs', 'slices ref/phx',
                         'B ref/phx', 'SEI ref/phx', 'status'], rows), '',
              '## Clusters', '',
              'A cluster groups cases with the same first-diff offset and the '
              'same reference/Phoenix NAL-type and slice-type signatures. '
              'A large cluster is one fix target, not many independent bugs.', '']
    rows = []
    for i, c in enumerate(result['clusters'], 1):
        rows.append((i, c['cases'], fmt(c['first_diff']),
                     c['ref_signature'], c['phx_signature'],
                     ', '.join(c['examples']),
                     ', '.join(c['presets'][:4])))
    lines += [_md_table(['#', 'cases', 'fd', 'ref signature', 'phx signature',
                         'examples', 'presets (first 4)'], rows), '']
    return '\n'.join(lines)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description='Analyse the video parity corpus: first-diff '
                    'distributions, preset breakdown, NAL sequences and '
                    'failure clusters.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Examples:\n'
               '  python tools/extended_video_probe.py\n'
               '  python tools/extended_video_probe.py --json\n'
               '  python tools/extended_video_probe.py --markdown\n'
               'Writes build/video_corpus_report.json and '
               'build/video_corpus_report.md in every mode.')
    ap.add_argument('--report', type=pathlib.Path, default=DEFAULT_REPORT,
                    help='parity report JSON (default: '
                         'build/parity/parity_report.json)')
    ap.add_argument('--streams-dir', type=pathlib.Path,
                    default=DEFAULT_STREAMS_DIR,
                    help='directory with <case>.ref.h264 / <case>.phx.h264 '
                         '(default: build/parity); missing streams are '
                         'reported, not fatal')
    ap.add_argument('--json', action='store_true',
                    help='print machine-readable JSON to stdout')
    ap.add_argument('--markdown', action='store_true',
                    help='print Markdown to stdout')
    args = ap.parse_args(argv)

    report_path = args.report.resolve()
    streams_dir = args.streams_dir.resolve()
    try:
        result = analyse(report_path, streams_dir)
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
        print(f'ERROR: cannot analyse corpus: {exc}', file=sys.stderr)
        return 2

    json_text = json.dumps(result, indent=2) + '\n'
    md_text = render_markdown(result)
    try:
        JSON_OUT.parent.mkdir(parents=True, exist_ok=True)
        JSON_OUT.write_text(json_text, encoding='utf-8')
        MD_OUT.write_text(md_text, encoding='utf-8')
    except OSError as exc:
        print(f'ERROR: cannot write report files: {exc}', file=sys.stderr)
        return 2

    if args.json:
        sys.stdout.write(json_text)
    if args.markdown:
        sys.stdout.write(md_text)
    if not args.json and not args.markdown:
        sys.stdout.write(render_text(result))
    if not streams_dir.is_dir():
        print(f'NOTE: streams directory not found ({streams_dir}); '
              'all streams reported as missing.', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
