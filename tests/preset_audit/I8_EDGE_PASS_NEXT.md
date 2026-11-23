# Next: pass-tagged x264 I8 edge dump

The `XEDGE` dump in `build/dump_x264_edge.py` lacks a pass identifier.
x264 calls `mb_analyse_intra` multiple times per MB (lookahead + encode);
cross-pass edge values explain why the oracle's `rbase`-derived top
samples differ from x264's `edge[16..31]`.

1. Add `vkd_pass`, slice type and `h->i_frame` to the XEDGE dump.
2. Re-run with `--frames 2` and keep only the `i_frame==0`, slice=I,
   `mb_xy==5`, `idx==1` edge from the *encode* pass.
3. Compare against the oracle's edge for the same block byte-for-byte.
4. Only then alter VR/DDR sample selection; otherwise further edits are
   guessing at cross-pass data.

Tools: `build/dump_x264_edge.py`, `build/dump_x264_i8blocks.py`,
`build/dump_x264_x3.py`.
