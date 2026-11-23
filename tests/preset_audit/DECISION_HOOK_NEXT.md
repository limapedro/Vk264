# Next: unmodified-encoder analysis decision dump

Since `i_skip_intra=1`, the final reconstruction equals analysis. The
remaining 1460-byte frame difference must come from analysis-pass mode
or residual decisions. Previous mode dumps were unreliable because
they required disabling early termination (which changed the state).

Next: dump x264's chosen I4 modes and quantized coefficients for MB1
from the UNMODIFIED encoder by hooking the point where it saves
`i4x4_dct_buf` / `i4x4_nnz_buf` (`mb_analyse_intra`, near the
`i4x4_fdec_buf` save at analyse.c:967). Compare those DCT buffers with
the oracle's `i4q[16][16]` for MB1. That is the first comparison with
both sides from the actual encoded state.
