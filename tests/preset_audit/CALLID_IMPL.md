# Implementation note for unique call ID

In `encoder/analyse.c`, add at file scope:
```c
static int vkd_call = 0;
```
Increment `vkd_call` at the top of `x264_macroblock_analyse` (once per MB
per frame). Add `extern int vkd_call;` in any other instrumented file
(e.g. `encoder/macroblock.h`) and tag every dump with it.

To identify the encode-pass call: dump `p_dst_by` and `h->mb.i_type`
from `encoder/cabac.c::macroblock_write_cabac_internal` for the same MB,
and select the I8 dump whose FDEC matches. Then the MB5 idx3 DDR
prediction from that call can be compared to the oracle's `p8[]`.
