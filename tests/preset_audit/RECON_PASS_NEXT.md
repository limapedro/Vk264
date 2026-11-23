# Next: systematic reconstruction pass

The full-frame compare (`FULL_RECON_COMPARE.md`) shows 1460/4096 luma
bytes differ, starting at MB0/MB1 by 1 LSB. Plan:

1. Use `build/compare_recon.py` as the primary metric (target: 0 differing
   bytes) alongside `wholeframe_diff2.py` for decisions.
2. Start with MB0/MB1 (I4 blocks): byte-compare their 4x4 prediction,
   quant/dequant and IDCT against x264 using the same full-frame dumps.
3. Fix the earliest differing reconstruction primitive (likely a rounding
   or filter-tap detail shared by many MBs) and re-measure the total diff
   count — a correct fix should reduce the count substantially.
4. Re-run NAL2 prefix metric, wholeframe harness, audit and all gates.
