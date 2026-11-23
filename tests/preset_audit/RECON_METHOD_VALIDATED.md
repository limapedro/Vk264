# Reconstruction comparison method validated

`build/validate_recon_method.py` encodes both presets with the exact
audit flags and compares oracle `ORACLE_DUMP_RECON` against x264
`--dump-yuv`:

```
ultrafast: diff = 0 / 4096 luma bytes    <-- method validated
superfast: diff = 1460 / 4096 luma bytes  <-- real divergence
```

Since ultrafast is byte-exact at the bitstream level and its reconstructed
frames match perfectly, the dump comparison itself is sound; the 1460-byte
superfast difference is genuine.

Root localization: the earliest differences are only two pixels in MB0:
```
(15,3) x264=14 oracle=15
(15,4) x264=14 oracle=15
```
Both are the rightmost column of MB0's DC-mode I4 blocks. These feed
MB1's left-neighbour prediction, which explains MB1's 72 differing bytes
and the cascade across the frame.

x264's writer-pass neighbour dump for MB0 idx3 reported top samples
199,82,85,65 — clearly a wrong pointer offset into FDEC padding, so the
exact DC-prediction rounding remains unresolved by that dump. No source
change landed; gates green; audit 12/120; NAL2 prefix 50/130; parity 2/10.
