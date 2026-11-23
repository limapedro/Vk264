# Current verified state and honest assessment

Byte-exact preset parity remains 2/10 (ultrafast only):
- audit 12/120; progress_metrics 2/20 NAL2 pairs
- superfast NAL2 first diff 50/130; other presets 5-16
- compare_slicetype 13/16
- gates: CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56

Progress this session:
- I4 CABAC path with exact mb_type/pred modes/CBP/residual/nnz cache
- subme-gated I4/I8 analysis; header parity for all 10 presets
- whole-frame per-MB differential harness
- gated interior-MB I8 decision path: MB0-5 all match, NAL2 49->50
- exact I8 reconstruction predictor + VR odd-diagonal fix: MB5 idx1
  analysis-pass prediction/FDEC byte-exact
- unique call-ID tooling for x264 dumps

Remaining gap: MB6 onward. Repeated attempts showed x264's
`mb_analyse_intra` runs multiple passes per MB with different FDEC
state; analysis-pass dumps are unreliable. The CABAC writer's final
FDEC for MB5 is flat 16, so further edge/reconstruction comparisons
must use writer-pass values, not analysis-pass dumps.

This is a large, interconnected feature (I8 residual/CABAC for interior
MBs); the verified MB0-5 frontier is preserved. No user input is
required; the work is purely implementation/differential debugging.
