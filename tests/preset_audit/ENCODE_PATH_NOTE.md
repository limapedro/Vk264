# I4 encode-path dump blocked by x264 skip-intra optimization

Tried instrumenting `encoder/macroblock.c::x264_macroblock_encode`
around `x264_mb_encode_i4x4(h, p, i, i_qp, i_mode, 1)` for MB1 idx1.
No dump fired because x264 superfast sets `h->mb.i_skip_intra`, and the
loop becomes `for(i = 15; i < 16; i++)` — only the last sub-block is
encoded through this path; the other 15 use cached DCT from analysis.

The analysis path (`mb_analyse_intra`) also skips MB1 idx1 after early
termination. So neither hook can dump the prediction/DCT for that block.

To get it, disable the skip-intra optimization for the instrumented run
(e.g. patch `h->mb.i_skip_intra = 0` in `macroblock_encode`) — but that
changes the encoder state, so any values dumped must be treated as
diagnostic only, not bit-exact stream values.

No source change landed. Verified frontier preserved: MB0-5 decision
match, full-frame luma diff 1460/4096, NAL2 prefix 50/130, gates green,
audit 12/120, parity 2/10.
