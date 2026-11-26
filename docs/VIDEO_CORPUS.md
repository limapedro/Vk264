# Video corpus analysis (`extended_video_probe.py`)

`tools/extended_video_probe.py` is an **aggregate diagnostic** over the
100-case parity corpus (5 fixtures × 10 presets × 2 entropies). It does not
encode, regenerate streams, change x264 flags, or replace the byte-exact gate in
`tools/parity_harness.py`. It uses only the Python standard library.

> **0/100 byte-exact is NOT success.** It means *zero* cases have been proven
> byte-identical to x264. A run that reports 0/100 is a failure inventory, not
> a parity result and not a progress percentage. This warning is printed by the
> tool itself and repeated in `build/video_corpus_report.md`.

## Inputs

| Input | Role |
| --- | --- |
| `build/parity/parity_report.json` (`--report`) | Authoritative case list, `ref_bytes`, `phx_bytes`, `byte_exact` and `first_diff`. The harness's first-diff convention is used: `-1` = identical, otherwise the offset of the first differing byte (or the shorter length when one stream is a prefix). |
| `build/parity/` (`--streams-dir`) | Annex-B streams `<case>.ref.h264` and `<case>.phx.h264`, supplying NAL structure. Missing files are reported as `MISSING` and never abort the analysis. |
| `build/parity/video_manifest.json` | Describes the four real 64×64 / 60-frame crops from `D:\Videos` plus the synthetic fixture; the corpus fixture list in the report is used for grouping. |

## Usage

```
python tools/extended_video_probe.py
python tools/extended_video_probe.py --report build/parity/parity_report.json
python tools/extended_video_probe.py --streams-dir build/parity --json
python tools/extended_video_probe.py --markdown
python tools/extended_video_probe.py --help
```

| Flag | Meaning |
| --- | --- |
| `--report PATH` | Parity report to read (default `build/parity/parity_report.json`). |
| `--streams-dir DIR` | Directory with the `.ref.h264` / `.phx.h264` streams (default `build/parity`). A `.h264` file is accepted as a Phoenix fallback. |
| `--json` | Print the machine-readable result to stdout. |
| `--markdown` | Print the Markdown result to stdout. Both flags may be combined. |
| `--help` | Show the flag reference and examples. |

With no format flag, a compact human-readable summary is printed. **Both**
`build/video_corpus_report.json` and `build/video_corpus_report.md` are
(re)written in every mode, so a JSON-stdout run still refreshes the Markdown
file. Exit status is `0` when the analysis completes (even at 0/100) and `2`
only when the report is missing/unreadable or contains no cases. A missing
streams directory is not an error: every case is still listed with the byte
data from the report and a `both streams missing` status.

## Sections of the output

### Corpus totals and per-fixture distribution

Byte-exact counts, mismatch counts, missing-stream counts and the
min / median / max of `first_diff`, plus a fixed histogram so runs are
comparable:

```
0  1-15  16-31  32-47  48-63  64-127  128-255  256-1023  1024+  exact  unknown
```

`exact` counts report-flagged byte-exact cases; `unknown` counts cases with no
usable first-diff offset. Per-fixture tables repeat the same statistics for
each of the five fixtures. A small `first_diff` means the divergence is early
in the stream — typically in SPS/PPS or the first slice header — while a large
one means the prefix is shared and the divergence is in later headers or
entropy data.

The size ratio is `phx_bytes / ref_bytes` (when both are non-zero). Values well
above 1 mean Phoenix is emitting more data than x264, which is expected while
headers/entropy differ but is a useful ranking signal.

### Per-preset breakdown

Each preset reports byte-exact count, first-diff min/median/max, median/max
size ratio, the case that **fails earliest** (smallest `first_diff`) and the
case with the **largest size ratio**. Sorting these across presets answers:

- which configurations diverge in the very first bytes (fix the writer shared
  by those presets first);
- which configurations produce the most bloated streams (look for extra NALs,
  repeated parameter sets or over-long entropy data).

### NAL sequences per case

Sequences are run-length encoded: `7,8,6,5,1x29,7,8,5,1x29` means SPS, PPS,
SEI, IDR slice, 29 slices, then a second SPS/PPS/IDR group and 29 slices. The
table also shows slice counts, B-slice counts and SEI counts as
`ref/phx`. Slice types are decoded from the first two Exp-Golomb fields of each
slice header and normalised to `I`, `P`, `B`, `SP`, `SI` (`?` if truncated).
B-slices being present at all is therefore directly visible, as is a mismatch
in how many were emitted.

### Clusters

A cluster groups cases whose **signature** is identical:

```
first_diff + (ref NAL RLE | slice-type counts) + (phx NAL RLE | slice-type counts)
```

Slice types are summarised as counts (`I2,P19,B39`) rather than order, so the
summary stays compact for 60-slice streams while still separating cases where
Phoenix emits a different number of B slices. A cluster is a *diagnostic*
group, not a claim of bitwise equality: cases in it diverge at the same offset
and have the same NAL/slice structure, but their payload bytes may differ.

How to use clusters for prioritisation:

1. **Largest clusters first.** One writer or decision bug typically explains
   every case in a cluster. The snapshot below has 43 clusters: an 8-case
   `first_diff=35` cluster (all four real fixtures at `ultrafast`, CABAC and
   CAVLC), six 6-case clusters at `first_diff=39` (five real-video groups and
   one synthetic group), and only 18 singleton clusters. The large clusters
   are better first targets than the singletons.
2. **Empty-Phoenix cluster.** The 4-case cluster with an empty NAL sequence and
   `first_diff=0` is the `synth` `veryslow`/`placebo` encodes where `phx_cli`
   crashed. Fix the crash path separately; no entropy work applies there yet.
3. **NAL-sequence differences.** Real-video cases have two SPS/PPS groups in
   the reference but only one in Phoenix, and truncated Phoenix streams at
   `veryslow`/`placebo` have fewer slices. That points at parameter-set
   insertion / keyframe handling and at encoder completion, not at CABAC.
4. **B-count differences.** Clusters where ref and phx have the same NAL types
   but different B counts (for example `I2,P19,B39` vs `I2,P16,B42`) point at
   lookahead / slice-type decision logic; clusters with matching counts point
   at bitstream writing or entropy coding.
5. **Cross-check with `tools/first_frame_probe.py`.** Pick one representative
   case from a large cluster and drill into per-NAL byte diffs and slice-header
   fields. Do not drill into all members of a cluster independently.

## Verification snapshot

Command (run from the repo root on branch `vk264-phoenix`):

```
python tools/extended_video_probe.py
```

Key output from the current `build/parity` contents:

```
CORPUS TOTALS
  cases 100; byte-exact 0/100; mismatches 100
  missing streams: ref=0 phx=0
  first-diff min/median/max: 0 / 39 / 41 (n=100)
  size ratio min/median/max: 0 / 2.20091 / 3.44674
  first-diff histogram:
    0             4
    32-47        96
    exact         0
    unknown       0
```

The four `first_diff=0` cases are the empty Phoenix `synth`
`veryslow`/`placebo` streams; the other 96 cases first diverge between bytes 35
and 41. By preset, `ultrafast` fails earliest (`first_diff=35`) and has the
largest observed ratio (3.447×); `veryslow`/`placebo` also show `first_diff=0`
because of the crash cases. The two largest clusters contain 8 and 6 cases.
Again: **0/100 byte-exact is NOT success** — use these numbers to choose the
next fix, then re-run `tools/parity_harness.py` to regenerate the authoritative
report.
