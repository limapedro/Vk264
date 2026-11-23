# Next: unique call identifier for x264 I8 dumps

Multiple `mb_analyse_intra` calls per (frame, mb, idx) produce different
FDEC/edge states; filtering by frame/slice/mb/idx is not sufficient.
Prior false findings (1-LSB reconstruction, DDR prediction) came from
cross-pass comparisons.

Implement a single global counter incremented in
`x264_macroblock_analyse` (once per MB per frame) and tag every I8 dump
(XEDGE, XCHO, X3FD, prediction, FDEC) with it. Then select the call whose
dumped FDEC matches the CABAC writer's FDEC for that MB (dumped from
`macroblock_write_cabac_internal`), guaranteeing the encode pass.

Only after that, re-run the MB5 idx3 DDR byte-compare and fix any real
difference. Validate with `build/wholeframe_diff2.py` (MB0-6 matching).
