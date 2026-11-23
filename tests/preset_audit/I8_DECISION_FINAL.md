# I8x8 decision control flow — final diagnostic

## Pass-tagged x264 decision dump (frame-0 I-slice, pass 6)

`build/dump_x264_i8decision.py` produced the exact I8 decisions:

```
MB5 idx=0 predmode=2 cand9=1 favor=0 x3V=49 x3H=1 x3DC=-11 -> mode2
MB5 idx=1 predmode=2 cand9=1 favor=1 x3V=891 x3H=2179 x3DC=1547 -> mode5 cost176
MB5 idx=2 predmode=2 cand9=1 favor=0 x3V=8 x3H=0 x3DC=-12 -> mode2
MB5 idx=3 predmode=2 cand9=1 favor=0 x3V=1045 x3H=383 x3DC=743 -> mode4 cost131
MB3 idx=0 -> mode9 cost52; idx=1 -> mode9 cost-12; idx=2 -> mode2; idx=3 -> mode1
```

## Replicating the loop verbatim

Implemented x264's loop with: x3 costs, `-3*lambda` bias, full candidate
lists for edge classes, `intra_analysis_shortcut[cand9=1][favor]`, and the
true top-left sample `lt` from the neighbour MB. Result:

```
MB5 modes: 2,5,2,4  -- EXACT match with x264
MB3 modes: 9,8,2,1  -- only idx=1 differs (x264 9, oracle 8)
```

MB3 idx1's DC_LEFT raw cost is 131 in the oracle vs ~3 in x264, because
the left neighbour's reconstructed samples (from MB3 idx0) differ.
This is reconstruction-state, not decision logic.

Because MB3 regressed, the overall CABAC stream moved from #343 to #299,
so the change was reverted. All gates green; audit 12/120; NAL2 49/130.

## Conclusion

Three of four MB5 I8 blocks and three of four MB3 blocks now match with
the verbatim loop; the remaining mismatches are all traceable to 8x8
reconstruction samples. Full parity requires byte-exact 8x8 DCT/quant/
dequant/IDCT reconstruction in the IDR encoder, not more decision logic.
