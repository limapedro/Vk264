# Definitive encode-pass FDEC differs from analysis-pass FDEC

Dumped `h->mb.pic.p_fdec[0]` from `macroblock_write_cabac_internal`
(the actual stream-producing call) for frame-0 MB5:

```
XWF mb=5 fdec = all 64 samples = 16
XWF mb=5 right column = 16,16,16,16,16,16,16,16
```

Yet analysis-pass dumps (`X5FD`, `XCID call=6`) showed MB5 idx1 FDEC
= 19,52,67,72,... and MB6's edge[14]=69. The two passes disagree
because `mb_analyse_intra` runs multiple times per MB with different
reconstruction state; only the CABAC writer's FDEC is the encoded truth.

Consequence: the earlier "MB6 left edge 67 vs 69" diagnosis was based
on analysis-pass values. Based on the writer FDEC (flat 16), MB6's real
left edge should be 16, not 67/69. Do not change reconstruction based
on analysis-pass edge dumps.

No source change landed. Verified state (PR #238): MB0-5 match at the
decision level, MB5 idx1 analysis-pass prediction/FDEC byte-exact,
NAL2 prefix 50/130, all gates green, audit 12/120, parity 2/10.

Next: dump the oracle's final reconstructed frame at the same point as
x264's CABAC writer (after all residual coding for the MB) and compare
there; only then adjust reconstruction.
