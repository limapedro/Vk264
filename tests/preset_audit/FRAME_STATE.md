# Current state (honest) — frame-level comparison is the next tool

Byte-exact preset parity remains 2/10:
- audit 12/120; progress_metrics 2/20 NAL2 pairs
- superfast NAL2 first diff 50/130; compare_slicetype 13/16
- gates green: CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56

Verified frontier: MB0-5 of frame 0 match x264 at the decision level;
MB5 idx1 prediction/reconstruction byte-exact after the VR/recon fix.
MB6 onward diverges; per-MB FDEC dumps are unreliable because the FDEC
pointer is rebased between MBs, so a full reconstructed-frame comparison
is required to locate the first differing sample.

All sources of parity progress this session (header parity, I4 path,
subme gate, gated I8 path, VR/recon fix) are preserved and gated.
No user input is needed; this is implementation work.
