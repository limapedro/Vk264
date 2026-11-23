# Next: dump full reconstructed frame at end of frame 0

Per-MB FDEC is rebased between MBs, so compare complete reconstructed
luma frames instead:

1. Patch x264's encoder to write the reconstructed luma plane after
   frame 0 is encoded (frame 0, `--frames 2`, single thread), to a file.
2. Dump the oracle's final `rbase` luma plane for frame 0 (after all 16
   MB codes) via the CLI, to a file.
3. Byte-compare the two 64x64 planes, list differing pixels, and trace
   the first difference back to the MB that produced it.
4. Fix that MB's reconstruction, then validate with
   `build/wholeframe_diff2.py` (MB0-6 matching) and full gates.
