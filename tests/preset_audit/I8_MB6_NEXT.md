# Next: MB6 is the earliest divergence

With the gated I8 path, MB0-5 all match and the superfast NAL2 first byte
diff improved 49 -> 50. The whole-frame table shows:

```
MB6: x264 type=1 (I8), modes 2,2,5,2, cbpL=14
     oracle type=0 (I4), modes 0,2,0,0, cbpL=4
```

MB6 (my=1,mx=2) has all three MB neighbours, so it takes the new gated
path, but the chosen modes are wrong. Use `build/dump_i8_candidates.py`
(with mb=6) to compare x264's x3 costs/favor/chosen modes with the
oracle's, then adjust only the interior-MB path (MB0-5 must stay matching).
