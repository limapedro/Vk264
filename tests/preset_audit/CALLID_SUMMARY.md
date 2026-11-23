# Summary: unique call ID for x264 I8 dumps

Implemented in `build/dump_with_callid.py`: a global `vkd_call` counter
incremented at each `x264_macroblock_analyse` call, tagging all I8 dumps,
plus CABAC-writer FDEC dumps for the same MB. This removes the cross-pass
ambiguity that produced several false findings (1-LSB reconstruction,
DDR prediction).

Encode-pass results confirm:
- MB5 pred/FDEC match the oracle byte-exact after the VR/recon fix.
- MB6's left edge differs (oracle 67 vs x264 69) — the next concrete fix.

This is the prerequisite tooling for all further I8 edge/reconstruction
work. Gates green; audit 12/120; NAL2 prefix 50/130; parity 2/10.
