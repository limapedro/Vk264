# Project status summary

Byte-exact preset parity: 2/10 (ultrafast only).
- audit 12/120; NAL2 pairs 2/20; superfast first diff 50/130;
  compare_slicetype 13/16.
- Gates: CTest 29/29 + 65/65, CAVLC 1360/1360, MP4 56/56.

Confirmed this session:
- Header parity for all 10 presets.
- I4 CABAC path, subme gating, gated interior I8 path (MB0-5 decisions
  match), NAL2 prefix 49->50.
- Full reconstructed-frame comparison (1460/4096 luma bytes differ) shows
  reconstruction is the systematic remaining gap, not MB6 alone.
- x264 analysis runs multiple passes per MB; only writer/full-frame dumps
  are reliable for comparisons.

No user input required; remaining work is a systematic reconstruction
byte-exactness pass documented in RECON_PASS_NEXT.md.
