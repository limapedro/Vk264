#!/usr/bin/env python3
"""Verify that the parity/*.inc modules reconstruct a pre-split dct_parity.cpp.

The parity fixtures were moved out of a single 6,700-line dct_parity.cpp into
domain modules without changing code. This script proves that mechanically:
it concatenates the module bodies and compares them byte for byte to a base
revision of dct_parity.cpp (default: the commit before the split).

    python harness/check_parity_layout.py --base 1c6c50b

A UTF-8 BOM, if present in the base revision, is ignored (it is invisible to
the compiler). Exits 0 on byte equality, 1 otherwise.
"""

import argparse
import pathlib
import subprocess
import sys

MODULES = [
    "x264_reference.inc",
    "fixtures_transform.inc",
    "fixtures_intra.inc",
    "fixtures_deblock.inc",
    "fixtures_cabac.inc",
    "fixtures_cavlc.inc",
    "fixtures_motion.inc",
    "fixtures_syntax.inc",
    "fixtures_macroblock.inc",
    "resident_cavlc.inc",
    "resident_cabac.inc",
    "resident_pipeline.inc",
    "fixtures_aq.inc",
    "dispatch.inc",
]

HEADER_LINES = 4  # 3 comment lines + blank line prepended to each module


def normalize(data: bytes) -> bytes:
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    return data.replace(b"\r\n", b"\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", default="1c6c50b", help="git rev of the pre-split dct_parity.cpp")
    args = ap.parse_args()

    root = pathlib.Path(__file__).resolve().parents[1]
    parity = root / "Vk264" / "src" / "parity"

    base = normalize(
        subprocess.check_output(
            ["git", "show", f"{args.base}:Vk264/vulkan/dct_parity.cpp"], cwd=root
        )
    )
    parts = []
    for name in MODULES:
        lines = normalize((parity / name).read_bytes()).split(b"\n")
        if len(lines) < HEADER_LINES or not lines[0].startswith(b"// Vk264 parity module:"):
            print(f"ERROR: {name} is missing its module header", file=sys.stderr)
            return 1
        parts.append(b"\n".join(lines[HEADER_LINES:]))

    rebuilt = b"".join(parts)
    print(f"base bytes:     {len(base)}")
    print(f"rebuilt bytes:  {len(rebuilt)}")
    if base == rebuilt:
        print("BYTE-IDENTICAL: parity modules exactly reconstruct the pre-split source")
        return 0

    print("MISMATCH: modules do not reconstruct the base source", file=sys.stderr)
    a, b = base.split(b"\n"), rebuilt.split(b"\n")
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            print(f"first differing line {i + 1}", file=sys.stderr)
            print(f"  base:    {a[i][:100]!r}", file=sys.stderr)
            print(f"  rebuilt: {b[i][:100]!r}", file=sys.stderr)
            break
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
