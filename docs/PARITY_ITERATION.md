# Byte-exact parity iteration loop

Phoenix is correct only when its output stream is **byte-identical** to the
authoritative x264 reference for every case in the 100-case corpus. This
document describes the loop used to drive that work and how to read the first
divergence reported by `tools/first_frame_probe.py`.

## The loop

```
probe -> fix the x264-source-aligned function -> golden test -> re-run the 100-case harness
```

1. **Probe the first divergence.** Generate (or refresh) the reference corpus
   with `python tools/parity_harness.py`, then probe one case:

   ```powershell
   python tools/first_frame_probe.py --preset medium --entropy cabac
   python tools/first_frame_probe.py --preset all --entropy cavlc
   ```

   The probe reads `build/parity/synth-<preset>-<entropy>.ref.h264` and
   `build/parity/synth-<preset>-<entropy>.phx.h264`. Use `--refs-dir` /
   `--phx-dir` for streams in another directory and `--all-slices` to print
   full headers for every slice NAL. It prints, per NAL: type, payload length,
   byte-exact status (`EXACT` / `DIFF@<offset>` / `MISSING` / `EXTRA`), the
   first differing byte, and a hexdump window around it. For NAL type 1/5 it
   decodes the slice header in the exact syntax order of
   `src/phx_slice.c::phx_slice_header_write` and
   `reference/x264/encoder/encoder.c:213-364`, then states whether the
   divergence is `HEADER MISMATCH` or `ENTROPY/MB MISMATCH`.

   Exit codes: `0` = probe says byte-exact; `1` = divergence found; `2` = file
   missing or stream unparsable. Exit `0` from the probe is still only a
   diagnostic — the harness is the authority (see the rule below).

2. **Fix the one x264-source-aligned function.** Port the discrepancy into the
   corresponding Phoenix function, keeping the source-line comments that tie
   each syntax element to its x264 line. Do not change x264 flags, fixtures, or
   the reference corpus to make a case pass. Typical mappings:

   | Divergence | Function to fix | x264 source |
   | --- | --- | --- |
   | SPS fields / length | `phx_sps_init`, `phx_sps_write` | `encoder/set.c:101,305-475` |
   | PPS fields / tail stop bit | `phx_pps_init`, `phx_pps_write` | `encoder/set.c:479,505-570` (note the second `chroma_qp_index_offset` at `:567`) |
   | Slice header fields | `phx_slice_header_init`, `phx_slice_header_write` | `encoder/encoder.c:122-211,213-364` |
   | NAL start codes / trailing bytes | `phx_nal_encode` / wrapper | `common/bitstream.c:55` |
   | Entropy after a matching header | `phx_cabac*`, `phx_cavlc`, `phx_predict`, `phx_dct`, `phx_quant`, `phx_mbcache` | per-function source comments |

3. **Add a golden test.** Before touching the next function, lock the fix in
   with a focused test/golden vector under `tests/` (for example
   `tests/test_phx_slice.c` or the header-byte oracles in
   `tests/compare_headers.py`): assert the exact expected RBSP bytes or field
   values derived from the reference source, not merely "decodes correctly".
   Run it via `python tools/run_tests.py` and confirm the regression cannot
   silently return.

4. **Re-run the 100-case harness.** `python tools/parity_harness.py` followed by
   `python tools/compare_streams.py`. Only a `byte_exact` result for every case
   ends the loop; then re-probe the previously failing case as a sanity check.

## Reading the probe

### Header-only mismatch

`HEADER MISMATCH` means the slice-header field *values* differ between ref and
Phoenix while the bytes following the header may still match. The probe lists
each differing field with its reference and Phoenix value, e.g.
`frame_num ref=0 phx=2`. This points at a parameter-set or header writer:
check `phx_sps_init/write`, `phx_pps_init/write`, and
`phx_slice_header_init/write` in that order (a wrong `log2_max_frame_num`,
`pic_order_cnt_type`, PPS default ref count, or `cabac_init_idc` presence
shifts the entire following header). Fix the header function; do not tune CABAC
contexts or MB decisions to compensate. A common special case is a length-only
difference with all decoded fields equal — that is an RBSP stop-bit / trailing
-byte bug in the header writer (see set.c:567 for PPS 8x8dct).

### First-slice / macroblock mismatch

`HEADER MATCH ... ENTROPY/MB MISMATCH at post-header byte N` means every slice
header field matched, but the first differing byte lies after the header. If it
is in the first slice NAL (typically NAL type 5, IDR), use the reported NAL byte
offset and hexdump to locate the first macroblock that diverges. This implicates
the entropy/MB pipeline (prediction, transform, quant, scan, CABAC/CAVLC state,
MB decision), not the header. Later slices matching while the first does not
usually indicates a first-frame state problem (IDR/intra prediction, QP or POC
initialization); all slices diverging from the same offset indicates a shared
syntax/context bug.

### No slice NAL / missing NALs

`NO SLICE NAL in Phoenix stream` means the Phoenix encoder never reached slice
emission. The probe still prints the expected first slice header from the
reference and reports `parameter sets: 2/2 byte-exact` when SPS/PPS match. This
is the current state of `phx_cli` (it writes SPS+PPS and returns code 3 with
`full-frame encoding is not implemented yet`); it is **not** parity progress.
Likewise, `MISSING in Phoenix` for an SEI/AUD NAL means the NAL assembly or
encoder loop is incomplete, even if SPS/PPS and slice payloads would match.

## The rule: no case counts until byte-exact

A case counts only when the whole Phoenix `.h264` file equals the reference
`.h264` file byte for byte. A matching decoded picture, a late first-diff,
matching SPS/PPS, matching slice headers, or "visually identical" output are all
**mismatches**. `compare_streams.py` / `parity_harness.py` report `byte_exact`
per case; 100/100 must be true before any parity claim is made. The probe's
`BYTE-EXACT` verdict is a local explanation of *where* a case stands, never a
substitute for the full 100-case re-run.
