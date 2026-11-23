# I8 CABAC stream diff — final result this turn

Landed the verbatim I8 decision path and diffed the CABAC decision stream
from the first divergence rather than reverting immediately.

Findings with call-aligned dumps:

- MB3 (frame 0) is byte-exact in the CABAC stream with the verbatim path.
- Divergence moves to MB4 (#326): oracle modes 3,2,3,1 vs x264 10,2,10,2.
- MB4 idx0 top samples are correct: oracle `rbase` row 15 = all 15, x264
  `edge[16..31]` = all 15. The wrong modes came from an out-of-bounds
  `lt8` read (`rbase[...,-1]`) for MB-edge blocks with no left neighbour.
  Fixed, giving MB4 idx0 mode 10 (matches x264).
- MB3 idx1 then changed to mode 8 (x264 9) due to the lt change; refined
  lt for top-edge blocks to use `l8[0]` when left is available, but MB3
  idx1 still differs.
- MB type decisions also differ for 4 MBs (6, 10, 11, 14).

With all of these, the NAL2 byte prefix stayed at 49/130 and the CABAC
divergence remained earlier than the baseline #343, so the changes were
reverted. Gates green; audit 12/120; parity 2/10.

Conclusion: individual I8 pieces are now understood, but changing them
as a group shifts divergences between MBs without improving the byte
prefix — the remaining mismatch is spread across multiple interacting
MB decisions, not a single isolated symbol. Further progress needs a
whole-frame, per-MB differential harness (compare every MB's type/modes/
CBP/residual against x264 in one run) rather than one-MB-at-a-time fixes.
