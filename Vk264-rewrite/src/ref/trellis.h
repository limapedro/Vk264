/* SPDX-License-Identifier: GPL-2.0-or-later
 * CABAC trellis quantization, ported from x264 encoder/rdo.c C fallback. */
#ifndef VK264_TRELLIS_H
#define VK264_TRELLIS_H

#include <stdint.h>

/* Trellis-quantize a 4x4 block in-place. dct[16] is input/output (raster order).
 * orig[16] is the original unquantized DCT (raster). Returns nnz count.
 * cabac_state is the live CABAC state[1024] (or a copy) - NOT modified except
 * through the normal trellis state tracking; caller's state is not advanced. */
int vk_trellis_quant_4x4(int dct[16], const int orig[16],
                         const int mf[16], const int bias[16],
                         const int deq[16], int qp,
                         int lambda2, int b_ac,
                         uint8_t *cabac_state, int ctx_block_cat,
                         int b_interlaced);

/* Trellis-quantize 4x4 DC (16 coeffs, luma I16 DC). */
int vk_trellis_quant_4x4_dc(int dct[16], const int orig[16],
                            int dcmf, int dcbias, int dcunquant_mf,
                            int lambda2, uint8_t *cabac_state,
                            int b_interlaced);

/* Trellis-quantize 2x2 chroma DC (4 coeffs). */
int vk_trellis_quant_chroma_dc(int dct[4], const int orig[4],
                               int dcmf, int dcbias, int dcunquant_mf,
                               int lambda2, uint8_t *cabac_state,
                               int b_interlaced);

int vk_trellis_lambda2(int qp, int b_intra);
void vk_trellis_init(void);

#endif
