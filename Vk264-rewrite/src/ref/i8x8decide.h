/* SPDX-License-Identifier: GPL-2.0-or-later
 * I8x8 intra prediction mode decision (encoder/analyse.c mb_analyse_intra).
 * Uses SATD cost + x264's mode-bit penalty to choose among the 9 directions
 * and the DC_LEFT/DC_TOP/DC_128 edge variants. */
#ifndef VK264_I8X8DECIDE_H
#define VK264_I8X8DECIDE_H
#include <stdint.h>

/* Predict an 8x8 block for mode m (0..11) from left[8], top[8] and
 * top-right tr[8]. Edge samples unavailable should be passed as 128. */
void vk_predict8_filtered(uint8_t out[64], int m, const uint8_t left[8],
                          const uint8_t top[16], int lt);
void vk_predict8_filtered_ex(uint8_t out[64], int m, const uint8_t left[8],
                             const uint8_t top[16], int lt, int have_tr);
void vk_predict8(uint8_t out[64], int m, const uint8_t left[8],
                 const uint8_t top[8], const uint8_t tr[8]);

/* Choose the best mode for an 8x8 source block; have_l/have_t indicate
 * neighbour availability, lambda is x264's MB lambda, qp the slice QP. */
int vk_i8x8_decide(const uint8_t src[64], const uint8_t left[8],
                   const uint8_t top[8], const uint8_t tr[8], int lt,
                   int have_l, int have_t, int lambda, int *cost_out,
                   int use_satd, int mb_gate);
/* As above with an explicit top-right availability flag: x264's
 * i_neighbour8[idx] includes MB_TOPRIGHT only for idx0 (when TOP is
 * available), idx1 (when the MB's top-right neighbour exists) and
 * idx2; idx3 never has it. */
int vk_i8x8_decide_ex(const uint8_t src[64], const uint8_t left[8],
                   const uint8_t top[8], const uint8_t tr[8], int lt,
                   int have_l, int have_t, int lambda, int *cost_out,
                   int use_satd, int mb_gate, int have_tr);

/* SATD of two 8x8 blocks (Hadamard, x264 pixel_satd_8x8). */
int vk_satd8(const uint8_t a[64], const uint8_t b[64]);

/* I16x16 intra decision (x264 analyse.c:690-734). Modes V=0,H=1,DC=2,P=3.
 * Returns the best mode; *cost_out gets its SA8D + lambda*mode_bits. */
int vk_i16x16_decide(const uint8_t src[256],const uint8_t top[16],
                     const uint8_t left[16],int have_t,int have_l,int lambda,
                     int *cost_out);

#endif
