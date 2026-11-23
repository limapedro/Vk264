# I8x8 diagnosis — final state this turn

## Definitive findings

1. **FDEC matches for flat blocks**: x264 frame-0 I-slice MB5 idx0 and
   idx2 FDEC are flat 16, oracle `rmb[]` identical (`XFD`/`OFD` dumps).
2. **Top-right samples were missing**: oracle `tr8[k]` was hardcoded 128
   for `by==0` blocks although the top neighbour exists. x264's
   `edge[24..31]` for MB5 idx1 is ~69. Fixed to read from the top MB.
   Oracle edge now: `t8=30,63,70,71,69,69,69,69`, `tr8=69...` — closely
   matching x264's `edge[16..23]=33,57,69,70,70,69,69,69`,
   `edge[24..31]=69...`.
3. **VR/DDR predictors verified correct**: with x264's exact edge and the
   correct 8x8 source, DDR SAD=1002 matches x264 exactly; VR SAD=189 vs
   x264 176 (bias scale). See `I8_PREDICTORS_VERIFIED.md`.
4. **Top-sample column offset fixed** (`mx*16+bx+k`) in prior turn.

## Remaining

With all inputs corrected, the combined filtered-edge + x3 + shortcut
mode-decision path still moves the CABAC divergence earlier (#343 ->
#329), i.e. the *decision path* itself (how x3 costs feed the shortcut and
`-3*lambda` bias) is not yet reproduced. Reverted; only the two verified
neighbour-sample fixes retained.

Stream unchanged: divergence #343, NAL2 prefix 49/130, audit 12/120,
all gates green.
