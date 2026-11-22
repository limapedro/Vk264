/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VK264_I8X8ENC_H
#define VK264_I8X8ENC_H
#include "bitstream.h"
#include "param.h"
#include "slice.h"
#include <stdint.h>

/* Encode one I8x8 macroblock (CAVLC). modes[4] are the fixed intra8x8
 * prediction modes for the four 8x8 blocks; transform8 is always 1. */
void vk_encode_i8x8_mb(vk_bs_t *bs, const uint8_t *src_y,
                       const uint8_t *src_u, const uint8_t *src_v,
                       uint8_t *recon_y, uint8_t *recon_u, uint8_t *recon_v,
                       int qp, const vk_neighbours_t *nb,
                       int stride_y, int stride_c,
                       const uint8_t modes[4], int chroma_mode,
                       uint8_t out_nnz[16], uint8_t out_cnnz[2][4],
                       uint8_t *out_dcnz);

#endif
