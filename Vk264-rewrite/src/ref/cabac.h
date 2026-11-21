/* SPDX-License-Identifier: GPL-2.0-or-later
 * CABAC arithmetic coder ported from x264 common/cabac.c (H.264 standard).
 */
#ifndef VK264_CABAC_H
#define VK264_CABAC_H

#include <stdint.h>

typedef struct {
    int i_low, i_range, i_queue, i_bytes_outstanding;
    uint8_t *p_start, *p, *p_end;
    uint8_t state[1024];
} vk_cabac_t;

/* Initialise contexts for a slice type (0=I, 1/2=P/B) and QP. */
void vk_cabac_init_ctx(vk_cabac_t *cb, int slice_type, int qp, int model);
/* Start a CABAC slice; first bit is byte-aligned. */
void vk_cabac_start(vk_cabac_t *cb, uint8_t *data, uint8_t *end);
/* Binary arithmetic coding primitives. */
void vk_cabac_decision(vk_cabac_t *cb, int ctx, int b);
void vk_cabac_decision_noup(vk_cabac_t *cb, int ctx, int b);
void vk_cabac_bypass(vk_cabac_t *cb, int b);
void vk_cabac_terminal(vk_cabac_t *cb);
void vk_cabac_ue_bypass(vk_cabac_t *cb, int exp_bits, int val);
/* Finish the slice: returns the number of bytes written. */
int vk_cabac_flush(vk_cabac_t *cb, int frame);

#endif
