# Concrete next steps for 04 Analysis

1. **I8x8 VR/DDR predictors (unblocks preset parity).** x264's per-block
   costs for MB5 are `X8C idx=1 mode=5 cost=176` and `X8C idx=3 mode=4
   cost=131`. The oracle's combined path picked mode 7 (cost 1812) and
   mode 1 (395). Dump the oracle's per-mode SADs next to x264's and fix
   the `ET()`/`EL()` sample selection in the VR/DDR predictors before
   re-landing the filtered-edge + x3 + shortcut path.

2. **mb_analyse_inter_p* (4 PARTIAL).** 4 functions partial; these gate
   P-slice macroblock types beyond the all-skip cases used by the current
   gates.

3. **rd_cost/trellis (26 MISSING).** Largest numerical gap but only used
   at subme>=6; it will not improve superfast NAL2 parity. Sequence it
   after byte-exact parity for superfast/veryfast.

4. **mb_analyse_inter_b* (9 MISSING).** Requires a B-frame reference-state
   port; stage 12 orchestration is also still ungated.
