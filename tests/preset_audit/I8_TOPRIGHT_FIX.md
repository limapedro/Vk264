# Fixed: I8 top-right neighbour availability

## Root cause
The oracle built the 8x8 prediction edge with `have_tr = 1`
unconditionally (both in `vk_i8x8_decide` and
`vk_predict8_filtered`). x264's `predict_8x8_filter` is called with
`i_filters = i_neighbour8[idx] & MB_TOPRIGHT`, which is set only for:

- idx0 when TOP exists,
- idx1 when the MB's top-right neighbour exists,
- idx2 always,
- idx3 **never**.

For an interior MB (e.g. MB6 idx3), the top-right sample belongs to
the MB to the right, which has not been reconstructed yet. x264
replicates the last top sample; the oracle used uninitialised padding
(128), producing a badly wrong filtered edge.

## Fix
- Added `vk_i8x8_decide_ex` / `vk_predict8_filtered_ex` taking an
  explicit `have_tr`; the original functions remain wrappers with
  `have_tr=1`.
- `cabac_enc.c` computes `have_tr8` per idx exactly as x264's
  `i_neighbour8[idx]` and passes it to both decision and reconstruction.

## Verified effect
```
c004 frame-0 luma recon:  355 -> 277 / 4096
per-MB diffs: {5:1,6:54,10:153,14:72,15:75}
           -> {10:103,14:110,15:64}   (MB0-9 byte-exact)
wholeframe_diff2: decisions match through MB9
audit superfast NAL2 first diff: 67 -> 80 / 130
CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56
```
The idx3 final-encode re-prediction was tested again in combination
with this fix and made no difference; it was removed. This confirms
the real cause of the earlier "raw left samples differ" measurement
was the bogus top-right edge, not a second reconstruction state.

Preset parity is still 2/10; remaining diffs are in MB10/14/15.
