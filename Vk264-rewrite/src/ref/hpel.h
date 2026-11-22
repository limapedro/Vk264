/* SPDX-License-Identifier: GPL-2.0-or-later
 * Half-pel interpolation filter ported from x264 common/mc.c hpel_filter(). */
#ifndef VK264_HPEL_H
#define VK264_HPEL_H
#include <stdint.h>

/* Produce horizontal (dsth), vertical (dstv) and centre (dstc) half-pel
 * planes from a source plane with `pad` pixels of border.  All destinations
 * are strides of `stride` bytes; x runs -2..width+2, y runs 0..height-1.
 * `buf` is a scratch row of int16_t[width+5]. */
void vk_hpel_filter(uint8_t *dsth, uint8_t *dstv, uint8_t *dstc,
                    const uint8_t *src, int stride, int width, int height,
                    int16_t *buf);

#endif
