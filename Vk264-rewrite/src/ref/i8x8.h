/* SPDX-License-Identifier: GPL-2.0-or-later
 * x264-faithful 8x8 DCT / quant / IDCT / CAVLC-interleave primitives.
 * Ported from x264 common/dct.c, common/set.c, common/quant.c. */
#ifndef VK264_I8X8_H
#define VK264_I8X8_H
#include <stdint.h>

/* 8x8 forward DCT of (src - pred), both tight 8x8 blocks (stride 8).
 * Output dct[64] is in natural row-major order (dct[y*8+x]). */
void vk_ref_dct8x8(int dct[64], const uint8_t src[64], const uint8_t pred[64]);

/* 8x8 inverse DCT + add to dst (tight stride 8). dct is in natural order. */
void vk_ref_idct8x8_add(uint8_t dst[64], int dct[64]);

/* Flat-CQM 8x8 intra quant tables (list CQM_8IY), mirroring
 * x264 common/set.c::x264_cqm_init. */
void vk_ref_quant_tables8(int qp, int mf[64], int bias[64], int dequant[64]);

/* Quantize a zigzag-scanned 8x8 block in place; returns nonzero flag. */
int vk_ref_quant8(int dct[64], const int mf[64], const int bias[64]);

/* Dequantize an 8x8 block in natural order, then IDCT-add. */
void vk_ref_dequant8_idct_add(uint8_t dst[64], int dct[64],
                              const int dequant[64], int qp);

/* Zigzag-scan an 8x8 natural-order block into zigzag order (frame scan). */
void vk_ref_zigzag8(int level[64], const int dct[64]);

/* De-interleave a zigzag-scanned 8x8 block into 4 zigzag 4x4 blocks for
 * CAVLC; fills nnz[4] in x264 scan8 order. */
void vk_ref_interleave8x8_cavlc(int dst[4][16], const int src[64],
                                uint8_t nnz[4]);

#endif
