#!/usr/bin/env python3
"""Run the full verification stack and write a compact morning-readable report.

One command runs six gates in order, survives tools that exit non-zero by
design (the probe and the bin-diff report divergence, not tool failure) and
writes two files under build/:

    build/nightly_status.json   machine-readable, per-gate counts + divergences
    build/nightly_status.md     the same facts in a short table

Only the Python standard library is used.  Every child process has a timeout;
a timed-out or unstartable tool is recorded as a SKIP/FAIL gate instead of
killing the rest of the stack.

Usage:
    python tools/nightly_gates.py
    python tools/nightly_gates.py --quick        # skip the bin-diff golden build
    python tools/nightly_gates.py --help

Exit status is 0 when the report was written, regardless of gate results:
divergence is data during parity iteration, not a harness error.  CI wrappers
that want to fail on a gate result should inspect nightly_status.json.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import re
import subprocess
import sys
import time
from typing import Any, Dict, List, Optional

ROOT = pathlib.Path(__file__).resolve().parents[1]
PY_DISPLAY = "python"
PY_EXE = sys.executable
PARITY_JSON = ROOT / "build" / "parity" / "parity_report.json"

DEFAULT_TIMEOUTS = {
    "ctest": 1800,       # cmake configure + build + ctest (each capped at 600s)
    "headers": 600,      # 20 small header dumps
    "parity": 1800,      # up to 100 x264 reference encodes
    "probe": 300,        # two streams, one case
    "bin_diff": 1800,    # two instrumented builds + two trace runs
    "git": 60,           # three cheap git queries
}

GATE_ORDER = ("ctest", "headers", "parity", "probe", "bin_diff", "git")
GATE_TITLES = {
    "ctest": "ctest (unit tests)",
    "headers": "header-exact SPS/PPS",
    "parity": "parity harness (byte-exact)",
    "probe": "first-frame probe (medium/cabac)",
    "bin_diff": "CABAC bin diff (first divergent bin)",
    "git": "git status / HEAD / branch",
}


def txt(v: Any) -> str:
    if v is None:
        return ""
    if isinstance(v, bytes):
        return v.decode("utf-8", "replace")
    return str(v)


def clean(v: Any, limit: int = 200) -> str:
    """One-line, ASCII-safe copy of tool output (Windows consoles are cp1252)."""
    s = " ".join(txt(v).encode("ascii", "replace").decode("ascii").split())
    if limit and len(s) > limit:
        s = s[:limit - 3] + "..."
    return s


def last_lines(out: Any, n: int = 3, limit: int = 200) -> List[str]:
    ls = [ln for ln in txt(out).splitlines() if ln.strip()]
    return [clean(ln, limit) for ln in ls[-n:]]


def new_gate(gid: str, command: str, run: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    g: Dict[str, Any] = {
        "id": gid,
        "title": GATE_TITLES[gid],
        "command": command,
        "status": "SKIP",
        "exit_code": run["rc"] if run else None,
        "timed_out": run["timed_out"] if run else False,
        "seconds": run["seconds"] if run else 0.0,
        "counts": {},
        "divergence": None,
        "evidence": [],
        "error": None,
    }
    return g


class Runner:
    def __init__(self, override: Optional[int] = None):
        self.timeouts = dict(DEFAULT_TIMEOUTS)
        if override:
            self.timeouts = {k: override for k in self.timeouts}

    def run(self, gid: str, command: List[str]) -> Dict[str, Any]:
        timeout = self.timeouts[gid]
        started = time.time()
        try:
            p = subprocess.run(command, cwd=str(ROOT), capture_output=True,
                               text=True, errors="replace", timeout=timeout)
            rc, out, err = p.returncode, txt(p.stdout), txt(p.stderr)
            timed_out = False
        except subprocess.TimeoutExpired as e:
            rc = 124
            out, err = txt(e.stdout), txt(e.stderr)
            err += "\nTIMEOUT after %ds" % timeout
            timed_out = True
        except OSError as e:
            rc, out, err = 127, "", "cannot start %s: %s" % (command[0], e)
            timed_out = False
        return {"rc": rc, "out": out, "err": err, "timed_out": timed_out,
                "seconds": round(time.time() - started, 1),
                "start": started}


# ---------------------------------------------------------------------------
# Gates
# ---------------------------------------------------------------------------

def gate_ctest(runner: Runner) -> Dict[str, Any]:
    cmd = f"{PY_DISPLAY} tools/run_tests.py"
    r = runner.run("ctest", [PY_EXE, str(ROOT / "tools" / "run_tests.py")])
    g = new_gate("ctest", cmd, r)

    m = re.search(r"(\d+)% tests passed,\s*(\d+) tests failed out of\s*(\d+)", r["out"])
    if m:
        pct, failed, total = (int(x) for x in m.groups())
        g["counts"] = {"tests_total": total, "tests_passed": total - failed,
                       "tests_failed": failed, "passed_pct": pct}
    else:
        rows = [ln for ln in r["out"].splitlines()
                if re.match(r"\s*\d+/\d+\s+Test\s", ln)]
        passed = sum(1 for ln in rows if re.search(r"\bPassed\b", ln, re.I))
        failed = sum(1 for ln in rows if re.search(r"\bFailed\b", ln, re.I))
        if rows:
            g["counts"] = {"tests_total": len(rows), "tests_passed": passed,
                           "tests_failed": failed}

    ev: List[str] = []
    for ln in r["out"].splitlines():
        if re.search(r"tests passed|Test #|CMake Error|error:|FAILED", ln, re.I):
            ev.append(clean(ln, 180))
            if len(ev) >= 4:
                break
    if not ev:
        ev = last_lines(r["out"], 3)
    if r["rc"] != 0:
        ev += last_lines(r["err"], 2)
    g["evidence"] = ev[:6]

    if r["timed_out"]:
        g["status"] = "FAIL"
        g["error"] = "ctest gate timed out"
    elif r["rc"] != 0:
        g["status"] = "FAIL"
        g["error"] = clean(r["err"] or (r["out"].splitlines() or ["non-zero exit"])[-1])
    elif g["counts"].get("tests_failed", 0) > 0:
        g["status"] = "FAIL"
        g["error"] = "ctest reported failing tests"
    else:
        g["status"] = "PASS"
        if not g["counts"]:
            g["error"] = "ctest returned 0 but no test count was parsed"
    return g


def gate_headers(runner: Runner) -> Dict[str, Any]:
    cmd = f"{PY_DISPLAY} tests/compare_headers.py"
    r = runner.run("headers", [PY_EXE, str(ROOT / "tests" / "compare_headers.py")])
    g = new_gate("headers", cmd, r)

    m = re.search(r"header-exact\s+(\d+)/(\d+)\s+pps-tail-verified\s+(\d+)/(\d+)",
                  r["out"])
    if m:
        exact, total, tails, tails_total = (int(x) for x in m.groups())
        g["counts"] = {"header_exact": exact, "header_total": total,
                       "pps_tail_verified": tails, "pps_tail_total": tails_total}
    for ln in r["out"].splitlines():
        mm = re.match(r"^(\S+)\s+(cabac|cavlc)\s+SPS prefix\s+(\d+)/(\d+)\s+"
                      r"PPS prefix\s+(\d+)/(\d+)", ln)
        if mm:
            case, ent, sps_p, sps_n, pps_p, pps_n = mm.groups()
            if (int(sps_p) < int(sps_n) or int(pps_p) < int(pps_n)
                    or "EXACT" not in ln):
                g["divergence"] = {"case": f"{case}-{ent}",
                                   "sps_prefix_bytes": int(sps_p),
                                   "sps_ref_bytes": int(sps_n),
                                   "pps_prefix_bytes": int(pps_p),
                                   "pps_ref_bytes": int(pps_n)}
                break
    g["evidence"] = [clean(ln, 180) for ln in r["out"].splitlines()
                     if "header-exact" in ln or "SPS prefix" in ln][:4]

    if r["timed_out"]:
        g["status"] = "FAIL"
        g["error"] = "header gate timed out"
    elif r["rc"] == 2 and "SKIP" in r["out"]:
        g["status"] = "SKIP"
        g["error"] = clean(last_lines(r["out"], 1, 200)[0]) if r["out"].strip() else "build missing"
    elif r["rc"] != 0:
        g["status"] = "FAIL"
        g["error"] = clean(r["err"] or "header compare exited non-zero")
    elif (g["counts"].get("header_exact") == g["counts"].get("header_total")
          and g["counts"].get("pps_tail_verified") == g["counts"].get("pps_tail_total")
          and g["counts"]):
        g["status"] = "PASS"
    else:
        g["status"] = "FAIL"
        g["error"] = "header-exact / pps-tail counts are not complete"
    return g


def gate_parity(runner: Runner) -> Dict[str, Any]:
    cmd = f"{PY_DISPLAY} tools/parity_harness.py"
    r = runner.run("parity", [PY_EXE, str(ROOT / "tools" / "parity_harness.py")])
    g = new_gate("parity", cmd, r)

    fresh = (PARITY_JSON.is_file()
             and PARITY_JSON.stat().st_mtime >= r["start"] - 2.0)
    data: Optional[Dict[str, Any]] = None
    if fresh:
        try:
            data = json.loads(PARITY_JSON.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            g["error"] = "cannot read parity_report.json: " + clean(e)
    else:
        g["error"] = "parity_report.json was not regenerated by this run"

    if isinstance(data, dict):
        totals = data.get("totals") or {}
        cases = data.get("cases") or []
        x_hist: Dict[str, int] = {}
        p_hist: Dict[str, int] = {}
        for c in cases:
            if "x264_rc" in c:
                k = str(c["x264_rc"])
                x_hist[k] = x_hist.get(k, 0) + 1
            if "phx_rc" in c:
                k = str(c["phx_rc"])
                p_hist[k] = p_hist.get(k, 0) + 1
        g["counts"] = {
            "reference_cases": totals.get("cases"),
            "expected_cases": (data.get("corpus") or {}).get("expected_cases"),
            "byte_exact": totals.get("byte_exact"),
            "compared": totals.get("compared"),
            "mismatches": totals.get("mismatches"),
            "x264_rc_hist": x_hist,
            "phx_rc_hist": p_hist,
            "phx_available": bool(data.get("phx")),
            "x264": clean(data.get("x264"), 160),
            "phx": clean(data.get("phx"), 160) if data.get("phx") else None,
        }
        diffs = [(int(c["first_diff"]), c) for c in cases
                 if c.get("first_diff", -1) >= 0]
        if diffs:
            off, case = min(diffs, key=lambda pair: pair[0])
            g["counts"]["first_diff_min"] = off
            g["divergence"] = {
                "first_diff_byte": off,
                "case": case.get("case"),
                "fixture": case.get("fixture"),
                "preset": case.get("preset"),
                "entropy": case.get("entropy"),
            }
        if g["divergence"]:
            needle = str(g["divergence"]["case"])
            for ln in r["out"].splitlines():
                if needle in ln:
                    g["evidence"].append(clean(ln, 200))
                    break
    g["evidence"] += [clean(ln, 200) for ln in r["out"].splitlines()
                      if re.search(r"reference streams|rc=0|byte-exact|FAIL:|SKIP:", ln)][:4]

    n_cases = g["counts"].get("reference_cases") or 0
    x_ok = g["counts"].get("x264_rc_hist", {}).get("0", 0)
    if r["timed_out"]:
        g["status"] = "FAIL"
        g["error"] = "parity harness timed out"
    elif data is None:
        g["status"] = "FAIL" if r["rc"] == 1 else "SKIP"
        g["error"] = g["error"] or clean(last_lines(r["err"] or r["out"], 2, 200))
    elif x_ok != n_cases:
        g["status"] = "FAIL"
        g["error"] = f"x264 encodes failed (rc=0: {x_ok}/{n_cases}); corpus invalid"
    elif r["rc"] != 0:
        g["status"] = "FAIL"
        g["error"] = (f"byte-exact {g['counts'].get('byte_exact')}/{n_cases}: "
                      f"parity not achieved (harness exit {r['rc']})")
    elif not g["counts"].get("phx_available"):
        g["status"] = "SKIP"
        g["error"] = "no Phoenix encoder: reference corpus only, no parity claim"
    elif n_cases and g["counts"].get("byte_exact") == n_cases:
        g["status"] = "PASS"
    else:
        g["status"] = "FAIL"
        g["error"] = ("byte-exact count is below the case count; "
                      "0/N is not success")
    return g


def gate_probe(runner: Runner) -> Dict[str, Any]:
    cmd = (f"{PY_DISPLAY} tools/first_frame_probe.py "
           "--preset medium --entropy cabac")
    r = runner.run("probe", [PY_EXE, str(ROOT / "tools" / "first_frame_probe.py"),
                             "--preset", "medium", "--entropy", "cabac"])
    g = new_gate("probe", cmd, r)
    out = r["out"]

    m = re.search(r"VERDICT: DIVERGENCE \(first stream byte diff (\d+)\)", out)
    stream_diff: Optional[int] = int(m.group(1)) if m else None
    at_end = "first stream byte diff at end of stream" in out
    exact = "VERDICT: BYTE-EXACT" in out
    idr_diff: Optional[int] = None
    for ln in out.splitlines():
        if "IDR slice" in ln and "DIFF@" in ln:
            mm = re.search(r"DIFF@(\d+)", ln)
            if mm:
                idr_diff = int(mm.group(1))
                break
    m2 = re.search(r"stream bytes: ref=(\d+), phx=(\d+); NALs: ref=(\d+), phx=(\d+)", out)
    m3 = re.search(r"parameter sets: (\d+)/(\d+) byte-exact", out)
    g["counts"] = {
        "stream_first_diff_byte": stream_diff if stream_diff is not None else ("end" if at_end else None),
        "idr_nal_first_diff_byte": idr_diff,
        "ref_bytes": int(m2.group(1)) if m2 else None,
        "phx_bytes": int(m2.group(2)) if m2 else None,
        "ref_nals": int(m2.group(3)) if m2 else None,
        "phx_nals": int(m2.group(4)) if m2 else None,
        "parameter_sets_exact": int(m3.group(1)) if m3 else None,
        "parameter_sets_total": int(m3.group(2)) if m3 else None,
    }
    g["divergence"] = {"first_stream_byte": stream_diff, "idr_nal_byte": idr_diff,
                       "at_end_of_stream": at_end}

    last, context = "", ""
    hex_rows: List[str] = []
    for ln in out.splitlines():
        s = ln.strip()
        if "DIFF@" in s and "IDR slice" in s:
            context = "IDR slice"
        elif s in ("ref:", "phx:"):
            last = s[:-1]
        if "<== first diff" in s and last:
            hex_rows.append(clean(f"{last} [{context or 'stream'}]: {s}", 200))
            if len(hex_rows) >= 4:
                break
    g["evidence"] = [clean(ln, 200) for ln in out.splitlines()
                     if ln.strip().startswith("VERDICT:")
                     or ("IDR slice" in ln and "DIFF@" in ln)
                     or "NO SLICE NAL" in ln][:3] + hex_rows
    missing = ("ERROR: missing stream" in out or "ERROR: reference directory" in out
               or "ERROR: Phoenix directory" in out)
    if r["timed_out"] or r["rc"] == 2 or missing:
        g["status"] = "SKIP"
        g["error"] = (clean(([ln for ln in out.splitlines() if "ERROR:" in ln] or ["probe could not decide"])[0])
                      if missing else "probe could not decide")
    elif exact and r["rc"] == 0:
        g["status"] = "PASS"
    elif r["rc"] == 1 and (stream_diff is not None or idr_diff is not None or at_end):
        g["status"] = "DIFF"       # divergence is expected data, not a tool failure
    else:
        g["status"] = "FAIL"
        g["error"] = clean(last_lines(r["err"] or r["out"], 2, 200))
    return g


def gate_bin_diff(runner: Runner, quick: bool) -> Dict[str, Any]:
    cmd = f"{PY_DISPLAY} tools/cabac_bin_diff.py --all"
    if quick:
        g = new_gate("bin_diff", cmd + "  [skipped by --quick]")
        g["status"] = "SKIP"
        g["counts"] = {"golden_build_skipped": 1}
        g["divergence"] = None
        g["error"] = "--quick: bin-diff golden build skipped"
        return g

    r = runner.run("bin_diff", [PY_EXE, str(ROOT / "tools" / "cabac_bin_diff.py"),
                                "--all"])
    g = new_gate("bin_diff", cmd, r)
    out, err = r["out"], r["err"]
    m = re.search(r"FIRST DIVERGENT BIN: index (\d+) \(0-based, bin #(\d+)\)", out)
    m2 = re.search(r"x264 golden bins: (\d+); Phoenix bins: (\d+)", out)
    identical = "first MB0 bin streams are IDENTICAL" in out
    g["counts"] = {
        "first_divergent_bin_index": int(m.group(1)) if m else None,
        "x264_bins": int(m2.group(1)) if m2 else None,
        "phx_bins": int(m2.group(2)) if m2 else None,
        "ordered_bins_identical": identical,
    }
    g["divergence"] = ({"first_divergent_bin": int(m.group(1)),
                        "bin_number": int(m.group(2))} if m else None)
    g["evidence"] = [clean(ln, 200) for ln in out.splitlines()
                     if re.search(r"FIRST DIVERGENT BIN|first MB0 bin streams|"
                                  r"x264 record:|phx  record:|coder state before bin",
                                  ln)][:6]
    err_lines = [ln for ln in (out + "\n" + err).splitlines() if "ERROR:" in ln]
    if r["timed_out"]:
        g["status"] = "FAIL"
        g["error"] = "bin-diff gate timed out"
    elif r["rc"] == 2:
        g["status"] = "SKIP"
        g["error"] = clean(err_lines[0] if err_lines else
                           "golden/build step failed; bin diff unavailable")
        g["evidence"] += last_lines(err_lines or err, 2, 200)
    elif r["rc"] == 1 and m:
        g["status"] = "DIFF"
    elif r["rc"] == 0 and identical:
        g["status"] = "PASS"
    else:
        g["status"] = "FAIL"
        g["error"] = clean(last_lines(err or out, 2, 200))
    return g


def gate_git(runner: Runner) -> Dict[str, Any]:
    commands = {
        "head": ["git", "rev-parse", "HEAD"],
        "branch": ["git", "branch", "--show-current"],
        "status": ["git", "status", "--porcelain=v1"],
    }
    results = {k: runner.run("git", cmd) for k, cmd in commands.items()}
    g = new_gate("git", "git rev-parse HEAD; git branch --show-current; "
                        "git status --porcelain=v1", results["head"])
    head = txt(results["head"]["out"]).strip().splitlines()
    branch = txt(results["branch"]["out"]).strip().splitlines()
    status_lines = [ln for ln in txt(results["status"]["out"]).splitlines() if ln.strip()]
    g["counts"] = {
        "head": clean(head[0], 80) if head else None,
        "branch": clean(branch[0], 80) if branch else None,
        "changed_files": len(status_lines),
    }
    g["evidence"] = [clean(ln, 160) for ln in status_lines[:6]]
    if len(status_lines) > 6:
        g["evidence"].append(f"... +{len(status_lines) - 6} more changed paths")
    if any(results[k]["rc"] != 0 for k in commands):
        g["status"] = "FAIL"
        g["error"] = "a git query failed"
    else:
        g["status"] = "PASS"
    return g


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def gate_summary(g: Dict[str, Any]) -> str:
    c = g["counts"]
    d = g["divergence"] or {}
    if g["id"] == "ctest":
        return (f"{c.get('tests_passed', '?')}/{c.get('tests_total', '?')} passed, "
                f"{c.get('tests_failed', '?')} failed")
    if g["id"] == "headers":
        return (f"header-exact {c.get('header_exact', '?')}/{c.get('header_total', '?')}, "
                f"pps-tail {c.get('pps_tail_verified', '?')}/{c.get('pps_tail_total', '?')}")
    if g["id"] == "parity":
        phx = c.get("phx_available")
        p_total = sum(c.get("phx_rc_hist", {}).values())
        p_ok = c.get("phx_rc_hist", {}).get("0", 0)
        return (f"byte-exact {c.get('byte_exact', '?')}/{c.get('reference_cases', '?')}; "
                f"x264 rc0 {c.get('x264_rc_hist', {}).get('0', 0)}/{c.get('reference_cases', '?')}; "
                + (f"phx rc0 {p_ok}/{p_total}, rc!=0 {p_total - p_ok}"
                   if phx else "no phx encoder"))
    if g["id"] == "probe":
        return (f"stream byte {c.get('stream_first_diff_byte', '?')}, "
                f"IDR NAL byte {c.get('idr_nal_first_diff_byte', '?')}, "
                f"ref/phx bytes {c.get('ref_bytes', '?')}/{c.get('phx_bytes', '?')}")
    if g["id"] == "bin_diff":
        if g["status"] == "SKIP":
            return "--quick: golden build skipped"
        return (f"first divergent bin index {c.get('first_divergent_bin_index', '?')}; "
                f"x264/phx bins {c.get('x264_bins', '?')}/{c.get('phx_bins', '?')}")
    if g["id"] == "git":
        return (f"HEAD {str(c.get('head') or '?')[:12]}, branch "
                f"{c.get('branch') or '?'}, {c.get('changed_files', '?')} changed")
    return clean(g["error"] or "", 120)


def divergence_lines(g: Dict[str, Any]) -> List[str]:
    c, d = g["counts"], g["divergence"] or {}
    out: List[str] = []
    if g["id"] == "headers" and d:
        out.append(f"- headers: {d['case']} SPS prefix "
                   f"{d['sps_prefix_bytes']}/{d['sps_ref_bytes']} bytes, "
                   f"PPS prefix {d['pps_prefix_bytes']}/{d['pps_ref_bytes']} bytes")
    elif g["id"] == "parity" and d:
        out.append(f"- parity: case {d['case']} ({d['preset']}/{d['entropy']}) "
                   f"first differing byte {d['first_diff_byte']}; earliest across corpus "
                   f"{c.get('first_diff_min', d['first_diff_byte'])}")
        if c.get("phx_rc_hist"):
            out.append(f"  phx rc histogram {c['phx_rc_hist']}; "
                       f"x264 rc histogram {c.get('x264_rc_hist', {})}")
    elif g["id"] == "probe":
        hex_rows = [e for e in g["evidence"] if "<== first diff" in e]
        out.append(f"- probe medium/cabac: stream first diff byte "
                   f"{c.get('stream_first_diff_byte')}, IDR NAL payload byte "
                   f"{c.get('idr_nal_first_diff_byte')}")
        out += [f"  {h}" for h in hex_rows[:4]]
    elif g["id"] == "bin_diff" and d:
        out.append(f"- bin diff: first divergent bin index "
                   f"{d['first_divergent_bin']} (bin #{d['bin_number']})")
        out += [f"  {e}" for e in g["evidence"]
                if "record:" in e or "coder state" in e][:3]
    return out


def build_markdown(payload: Dict[str, Any]) -> str:
    gated = {g["id"]: g for g in payload["gates"]}
    c = gated["git"]["counts"]
    overall = payload["overall"]
    claim = payload["byte_exact_parity_claim"]
    suffix = "" if claim == "none" else f" ({claim})"
    L = [
        f"# Nightly status{' (quick)' if payload['quick'] else ''} - {payload['generated_utc']}",
        f"branch {c.get('branch') or '?'} @ {c.get('head') or '?'}",
        f"overall: **{overall}**{suffix}" if claim != "none"
        else f"overall: **{overall}** (no byte-exact parity claim)",
        "",
        "| Gate | Status | Counts / result |",
        "| --- | --- | --- |",
    ]
    for g in payload["gates"]:
        L.append(f"| {g['title']} | {g['status']} | "
                 f"{clean(gate_summary(g), 220).replace('|', '/')} |")
    L += ["", "## First divergences (exact)", ""]
    divs: List[str] = []
    for g in payload["gates"]:
        divs += divergence_lines(g)
    L += divs or ["- none reported"]
    notes: List[str] = []
    for g in payload["gates"]:
        if g["error"]:
            notes.append(f"- {g['id']}: {clean(g['error'], 220)}")
    if notes:
        L += ["", "## Notes", ""] + notes
    L += ["", "- Byte-exact count is the only parity claim; 0/100 is not success. "
          "See docs/NIGHTLY_STATUS.md.", ""]
    return "\n".join(L)


def status_rank(status: str) -> int:
    return {"FAIL": 0, "DIFF": 1, "SKIP": 2, "PASS": 3}.get(status, 4)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Exit 0 means the report was written; gate results are data, "
               "not harness errors. Read build/nightly_status.json for "
               "automated checks.")
    ap.add_argument("--quick", action="store_true",
                    help="skip the bin-diff golden build (gate 5 = SKIP)")
    ap.add_argument("--output-dir", type=pathlib.Path, default=ROOT / "build",
                    help="directory for nightly_status.json/.md (default: build)")
    ap.add_argument("--timeout", type=int, default=None,
                    help="override every per-gate subprocess timeout (seconds)")
    args = ap.parse_args(argv)

    runner = Runner(args.timeout)
    gates = [
        gate_ctest(runner),
        gate_headers(runner),
        gate_parity(runner),
        gate_probe(runner),
        gate_bin_diff(runner, args.quick),
        gate_git(runner),
    ]
    overall = min((g["status"] for g in gates), key=status_rank)
    parity = next(g for g in gates if g["id"] == "parity")
    pcount = parity["counts"]
    claim = "none"
    if (parity["status"] == "PASS" and pcount.get("byte_exact")
            == pcount.get("reference_cases")):
        claim = f"{pcount['byte_exact']}/{pcount['reference_cases']} byte-exact"

    payload: Dict[str, Any] = {
        "schema": 1,
        "generated_utc": dt.datetime.now(dt.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"),
        "root": str(ROOT),
        "quick": bool(args.quick),
        "overall": overall,
        "byte_exact_parity_claim": claim,
        "gates": gates,
        "by_id": {g["id"]: g for g in gates},
    }
    md = build_markdown(payload)
    out_dir = args.output_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "nightly_status.json").write_text(
        json.dumps(payload, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    (out_dir / "nightly_status.md").write_text(md, encoding="utf-8")
    print(md)
    print(f"reports: {out_dir / 'nightly_status.json'}")
    print(f"         {out_dir / 'nightly_status.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
