# Nightly status (one-command verification)

`tools/nightly_gates.py` runs the whole verification stack in a fixed order and
writes a compact report that can be read in the morning without digging through
logs. It uses only the Python standard library.

```bat
python tools/nightly_gates.py            :: full stack, including the CABAC bin-diff golden build
python tools/nightly_gates.py --quick    :: skip the bin-diff golden build (gate 5 = SKIP)
python tools/nightly_gates.py --help     :: all options
```

Outputs (always rewritten):

- `build/nightly_status.json` — machine-readable: generated time, branch/HEAD,
  overall status, and for each gate its command, exit code, timeout flag,
  counts, exact first-divergence values and a few evidence lines.
- `build/nightly_status.md` — the same facts as a small table plus a focused
  "First divergences (exact)" section. The script also prints this text.

The script exits **0 whenever the report was written**, even when gates fail:
during parity iteration a divergence is expected data, not a harness error.
Automated checks should read `build/nightly_status.json`. Every child process
has a timeout (`--timeout SECONDS` overrides all of them); a timed-out tool is
recorded as a failed or unavailable gate and the remaining gates still run.

## What each gate means

| Gate | Command | What it measures | Status rule |
| --- | --- | --- | --- |
| `ctest` | `python tools/run_tests.py` | CMake configure/build + ctest: number of tests passed and failed. | PASS only if ctest exits 0 with no failed tests. |
| `headers` | `python tests/compare_headers.py` | Byte-exact SPS/PPS NALs against the x264 reference, plus the focused PPS-tail/stop-bit check; reports `header-exact X/20` and `pps-tail-verified Y/20`. | PASS only if both counts are 20/20; a missing `phx_header_dump` is SKIP. |
| `parity` | `python tools/parity_harness.py` | The 100-case corpus (5 fixtures x 10 presets x 2 entropies): byte-exact streams, x264/phx return-code histograms and the earliest first differing byte. | PASS only when all cases are byte-exact; no Phoenix encoder is SKIP (reference corpus only); x264 failures or any mismatch are FAIL. |
| `probe` | `python tools/first_frame_probe.py --preset medium --entropy cabac` | For the medium/CABAC IDR case: stream byte offset of the first difference, NAL-by-NAL status, the IDR slice payload offset and hexdump windows with the exact bytes. | PASS if byte-exact; **DIFF** if a divergence was found (exit 1 is by design); SKIP if streams/directories are missing or the probe times out. |
| `bin_diff` | `python tools/cabac_bin_diff.py --all` | Ordered CABAC bin trace of MB0 from the instrumented x264 golden harness vs Phoenix: index of the first divergent bin, both `ctx/b` pairs, coder `low/range` state and x264/phx records. | PASS if bin streams are identical; **DIFF** if a bin differs (exit 1 by design); SKIP if the instrumented golden build fails (the error is recorded and the stack continues). `--quick` sets this gate to SKIP without building anything. |
| `git` | `git rev-parse HEAD`, `git branch --show-current`, `git status --porcelain=v1` | Which branch and commit produced the report, and which paths were modified. A dirty tree is recorded as a fact, not a failure. | PASS when all three queries succeed. |

`overall` is FAIL if any gate FAILs, else DIFF if any gate reports a
divergence, else SKIP if anything was skipped, else PASS.

## The only parity claim

**The byte-exact count from the parity gate is the only parity claim.**
`byte-exact 0/100` means zero of the 100 reference streams matched byte for
byte; it is **not** success and is not evidence of parity. Likewise, a
PPS-tail check, a matching slice header, a plausible decode, an "encoder not
available" corpus or a 0/N result from any other gate does not establish
parity. This report deliberately states "no byte-exact parity claim" unless
the parity gate reports `byte_exact == reference_cases`. It makes no claim that
Phoenix is byte-exact with x264.

`first_diff` values are diagnostic offsets (the first differing byte, or the
shorter length when one stream is a prefix). They tell you *where* to look;
they are not a pass criterion.

## Notes for the orchestrator

- Gate 5 is expensive: it compiles an instrumented copy of the x264 golden
  harness and a `-DPHX_CABAC_BIN_TRACE` Phoenix CLI in a temp directory. Use
  `--quick` for a fast morning check and run the full form when bin-level
  diagnosis is needed.
- Gate 3 regenerates `build/parity/parity_report.json` and the existing
  `tests/parity_report.md` (the harness owns that path); gate 4 then reads the
  freshly generated streams in `build/parity/`.
- Gate 1 rebuilds the CMake test tree, so later gates see the current sources.
