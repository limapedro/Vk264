# MB6 I8 decision — edge input diagnosed, not yet fixed

With the gated interior-MB path, MB0-5 all match and the superfast NAL2
prefix is 50/130. MB6 is the next divergence:

```
whole-frame: MB6 x264 I8 modes 2,2,5,2 cbpL=14
                  oracle I4 modes 0,2,0,0 cbpL=4
```

Pass-tagged x264 x3 dump for frame-0 I-slice MB6 (`build/dump_i8_mb6.py`):

```
idx0 nb8=15 cand9=1 favor=1 x3=20,90,8   -> mode2
idx1 nb8=15 cand9=1 favor=1 x3=55,63,51  -> mode2
idx2 nb8=15 cand9=1 favor=1 x3=425,1819,1069 -> mode5
idx3 nb8=11 cand9=1 favor=1 x3=65,97,53  -> mode2
```

Oracle x3 for the same blocks (`OX3` debug):

```
idx0: 20,202,82    (V matches x264; H/DC differ)
idx1: 55,63,63     (V/H match; DC differs)
idx2: 423,1635,1033
```

x264's filtered edge for MB6 idx0 (`build/dump_x264_mb6edge.py`):
left samples edge[14..7] = 69,70,71,71,71,70,69,66; lt=69; top all 69.
The oracle's left samples come from the MB5 reconstruction in `rmb[]`;
the H-mode discrepancy (90 vs 202) means those samples differ from
x264's FDEC, likely because MB5's own reconstruction is not byte-exact
even though its mode/CBP decisions are.

Next: byte-compare x264 FDEC and oracle rec8[] for MB5 blocks (pass-tagged)
before changing the MB6 decision logic. Gates green; audit 12/120.
