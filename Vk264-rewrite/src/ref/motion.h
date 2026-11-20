/* SPDX-License-Identifier: GPL-2.0-or-later
 * Integer-pel motion estimation for the C oracle.
 * x264 ultrafast uses me=dia, subme=0 (integer-pel only), so this module
 * implements the integer-pel search and the H.264 16x16 MV predictor.
 * This is a reference/test implementation; it intentionally does not attempt
 * sub-pel refinement or RD mode decision.
 */
#ifndef VK264_MOTION_H
#define VK264_MOTION_H

#include <stdint.h>

/* Median MV predictor for a 16x16 P macroblock (H.264 8.4.1.3):
 * median of left (A), top (B) and top-right (C). The caller supplies C as
 * the top-right neighbour MV, falling back to top-left at the right edge. */
void vk_mvp16(int left_x, int left_y, int top_x, int top_y,
              int topright_x, int topright_y, int *mvpx, int *mvpy);

/* Find the best integer MV for a 16x16 block by full SAD search over
 * [mvp-range, mvp+range]. cur: current plane (stride cur_stride);
 * ref: reference plane padded by `pad` pixels on every side (ref_stride,
 * top-left of the padded buffer). x,y: block position in unpadded pixels.
 * On search completion the best MV (pixel units) is written to mvx/mvy and
 * the minimum SAD is returned. The MVP candidate is tested first, so ties
 * resolve to the predictor (matching x264's behaviour). */
int vk_me16(const uint8_t *cur, int cur_stride,
            const uint8_t *ref, int ref_stride, int pad,
            int x, int y, int mvpx, int mvpy, int range,
            int *mvx, int *mvy);

/* Pad a plane by replicating edge pixels into `dst` (stride ds, pad border). */
void vk_pad_plane(uint8_t *dst, int ds, const uint8_t *src, int ss,
                  int w, int h, int pad);

#endif
