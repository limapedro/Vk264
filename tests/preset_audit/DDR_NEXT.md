# Next: MB5 idx3 DDR one-LSB difference

After the VR/recon fix, MB5 idx1 is byte-exact. MB5 idx3 (mode 4 DDR)
still differs at one sample: x264 FDEC sample 7 = 75, oracle = 78;
all other 63 samples match.

Next: dump x264's DDR-predicted 8x8 block for MB5 idx3 (pass-tagged,
`predict_8x8_ddr_c` output) and the oracle's `p8[]`, byte-compare, and
correct the specific DDR sample index. Then re-validate with
`build/wholeframe_diff2.py` (MB0-6 matching) and re-run the full gates.
