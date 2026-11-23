# Milestone: all I8 modes match x264 (call-aligned dumps)

With a shared `(frame, mb_xy, per-MB call counter)` tag on every x264 I8
dump and the CABAC writer dump, the encode-pass data is unambiguous. The
verbatim I8 decision path with corrected top-left samples produced:

```
MB3 modes: 9,9,2,1  == x264
MB5 modes: 2,5,2,4  == x264
```

All 8 I8 modes across the two diagnostic MBs match for the first time.
The CABAC stream diverged at #326 on pred-mode bins rather than #343;
since the NAL2 byte prefix stayed at 49/130 the change was reverted.
Next: x264's cache-based I8 pred-mode context (`I8_PREDMODE_NEXT.md`).

Gates: 29/29, 65/65, 1360/1360, 56/56, 13/16, audit 12/120. Parity 2/10.
