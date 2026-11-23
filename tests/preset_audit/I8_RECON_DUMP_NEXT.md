# Next: align x264's I8 decision dumps to one call

`XCHO`/`XI8D` (decision costs) and `X3FD` (FDEC) dumps need a shared,
unique call identifier — `vkd_pass` alone is ambiguous because it counts
`mb_analyse_intra` invocations across MBs and frames.

1. Add a global `vkd_seq` incremented once per encoded frame and a
   `vkd_mb` set in `x264_macroblock_analyse`, then tag every I8 dump with
   `(vkd_seq, frame, slice, mb_xy, idx)`.
2. Re-run with `--frames 2`; select the single encode-pass call for
   frame 0, MB3 idx0..3 and MS5 idx0..3.
3. Recompare x264's per-mode costs against oracle per-mode SADs using the
   exact FDEC from the same call.
4. If the costs then match, re-land `build/apply_i8_exact.py` verbatim;
   it already produces MB5 modes 2,5,2,4 and MB3 idx0/2/3 correctly.
