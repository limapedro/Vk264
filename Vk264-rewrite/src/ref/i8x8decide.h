/* SPDX-License-Identifier: GPL-2.0-or-later
 * I8x8 intra prediction mode decision (encoder/analyse.c mb_analyse_intra).
 * Uses SATD cost + x264's mode-bit penalty to choose among the 9 directions
 * and the DC_LEFT/DC_TOP/DC_128 edge variants. */
#ifndef VK264_I8X8DECIDE_H
#define VK264_I8X8DECIDE_H
#include <stdint.h>

/* Predict an 8x8 block for mode m (0..11) from left[8], top[8] and
 * top-right tr[8]. Edge samples unavailable should be passed as 128. */
void vk_predict8(uint8_t out[64], int m, const uint8_t left[8],
                 const uint8_t top[8], const uint8_t tr[8]);

/* Choose the best mode for an 8x8 source block; have_l/have_t indicate
 * neighbour availability, lambda is x264's MB lambda, qp the slice QP. */
int vk_i8x8_decide(const uint8_t src[64], const uint8_t left[8],
                   const uint8_t top[8], const uint8_t tr[8],
                   int have_l, int have_t, int lambda);

/* SATD of two 8x8 blocks (Hadamard, x264 pixel_satd_8x8). */
int vk_satd8(const uint8_t a[64], const uint8_t b[64]);

#endif
