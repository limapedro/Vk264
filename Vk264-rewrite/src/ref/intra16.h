/* SPDX-License-Identifier: GPL-2.0-or-later
 * 16x16 intra prediction + mode decision (x264 common/predict.c,
 * encoder/analyse.c). Test-only oracle support. */
#ifndef VK264_INTRA16_H
#define VK264_INTRA16_H

#include <stdint.h>

/* Predict a 16x16 block from reconstructed neighbours.
 * top[16] = row above, left[16] = column left, tl = top-left corner.
 * Modes: 0=V, 1=H, 2=DC, 3=P, 4=DC_LEFT, 5=DC_TOP, 6=DC_128. */
void vk_predict16(uint8_t out[256], const uint8_t top[16],
                  const uint8_t left[16], uint8_t tl, int mode);

/* x264 lambda_tab[qp] */
int vk_lambda(int qp);
/* bs_size_ue(fix[mode]) */
int vk_mode_bits(int mode);

/* Pick the best I16 mode by SAD + lambda*bits. AVAIL flags: bit0=left,
 * bit1=top, bit3=top-left (x264 convention). Returns the mode index. */
/* 8x8 chroma prediction; modes 0=DC,1=H,2=V,3=P,4=DC_LEFT,5=DC_TOP,6=DC_128. */
void vk_predict_chroma8(uint8_t out[64], const uint8_t top[8],
                        const uint8_t left[8], uint8_t tl, int mode);
/* Chroma mode cost: SAD over both planes + lambda*bs_size_ue(fix[mode]). */
int vk_select_chroma(const uint8_t srcu[64], const uint8_t srcv[64],
                     const uint8_t topu[8], const uint8_t leftu[8],
                     const uint8_t topv[8], const uint8_t leftv[8],
                     uint8_t tlu, uint8_t tlv, int avail, int qp);

int vk_select16(const uint8_t src[256], const uint8_t top[16],
                const uint8_t left[16], uint8_t tl, int avail, int qp);

#endif
