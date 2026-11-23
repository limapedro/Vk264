# Real next step after retracting the 1-LSB finding

Reconstruction and quant are verified byte-exact for MB4 idx0 (same-pass
x264 FDEC=16 == oracle rec8=16). The whole-frame harness shows MB0-4
match and MB5 is the first divergence: oracle I4, x264 I8 modes 2,5,2,4.

The remaining work is purely the I8 mode-decision path for MB5:
1. Use `build/dump_i8_candidates.py` to get x264's per-block x3 costs,
   favor_vertical, candidate lists and chosen modes for MB5 (all 4 blocks).
2. Implement exactly that for the all-neighbour class: filtered edge,
   x3 V/H/DC, `-3*lambda` bias, `intra_analysis_shortcut[avoid=0][cand9][favor]`.
3. Critical: keep edge-class blocks (MB3/MB4 with missing neighbours) on
   the baseline path — the earlier class-4 patch changed MB4 idx3 because
   within-MB blocks of edge MBs also have class-4 neighbour flags.
   Gate on the MB-level `have_l && have_t && topleft_available` condition
   (i.e. `mx>0 && my>0`), not the per-block class, so only MB5+ are affected.
4. Validate with `build/wholeframe_diff2.py`: MB0-4 unchanged, MB5 I8.
