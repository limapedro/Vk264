/* SPDX-License-Identifier: GPL-2.0-or-later
 * CAVLC I4x4 macroblock emitter (x264 encoder/macroblock.c + cavlc.c). */
#ifndef VK264_I4X4ENC_H
#define VK264_I4X4ENC_H
#include "bitstream.h"
#include "slice.h"
#include <stdint.h>

void vk_encode_i4x4_mb(vk_bs_t *bs, const uint8_t *src_y,
                       const uint8_t *src_u, const uint8_t *src_v,
                       uint8_t *recon_y, uint8_t *recon_u, uint8_t *recon_v,
                       int qp, const vk_neighbours_t *nb,
                       int stride_y, int stride_c,
                       const uint8_t modes[16],
                       uint8_t out_nnz[16], uint8_t out_cnnz[2][4],
                       uint8_t *out_dcnz, int mb_type_offset);

#endif
