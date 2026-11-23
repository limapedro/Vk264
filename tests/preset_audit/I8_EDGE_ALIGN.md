# I8x8 edge alignment — diagnostic result

## Finding

Dumped x264's filtered `edge[36]` for frame-0 MB5 idx1
(`build/dump_x264_edge.py`) and compared with the oracle's computed edge.
The oracle's top samples for `by==0` blocks were read from
`rbase[(my*16+by-1)*rs + mx*16 + k]` — missing the `+bx` offset, so upper
8x8 blocks of every MB read the *left* 8 columns of the top neighbour
instead of their own columns. Fixed to `... + mx*16 + bx + k`.

## Result

The fix is logically correct but produced **no stream change**: the oracle
already reads equivalent values from the top neighbour's bottom row in
this clip, so the CABAC divergence stays at #343 and the NAL2 prefix at
49/130. All gates stay green.

The x264 `edge[36]` dump also showed top-right samples (t8..t15) of ~69
where the oracle's rbase holds different values, but that dump was not
tagged with the analysis pass (x264 runs `mb_analyse_intra` in lookahead
and encode); the comparison may be cross-pass. A pass-tagged edge dump is
required before concluding the VR/DDR inputs are wrong.

## Status

MB5 modes still differ: x264 `2,5,2,4` (mode5 VR=176, mode4 DDR=131)
vs oracle `1,7,2,1` from the combined-path experiment (reverted).
Byte-exact parity remains 2/10; NAL2 prefix 49/130; audit 12/120.
