# Next: compare writer-pass reconstructed frame

x264's `macroblock_write_cabac_internal` FDEC for frame-0 MB5 is flat 16,
while analysis-pass dumps showed gradients. To find the real remaining
difference:

1. Dump x264's post-write reconstructed luma row (frame rows 16-23,
   cols 32-47) from the encoder after MB5 is written, with frame index.
2. Dump the oracle's final `rbase` for the same region (after MB5 coding).
3. Byte-compare. Only fix reconstruction stages whose writer-pass values
   actually differ.
4. Validate with `build/wholeframe_diff2.py` (MB0-6 matching) and re-run
   the full gates.
