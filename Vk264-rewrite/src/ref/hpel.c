/* SPDX-License-Identifier: GPL-2.0-or-later
 * Byte-exact port of x264 common/mc.c hpel_filter() (8-bit 4:2:0 path).
 * x264's 6-tap filter is [1,-5,20,20,-5,1] with >>5 for h/v and >>10 for c. */
#include "hpel.h"

static inline int clip_pixel(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

#define TAP(pix, d, x) \
    ((int)(pix)[(x)-2*(d)] + (int)(pix)[(x)+3*(d)] \
     - 5*((int)(pix)[(x)-(d)] + (int)(pix)[(x)+2*(d)]) \
     + 20*((int)(pix)[(x)] + (int)(pix)[(x)+(d)]))

void vk_hpel_filter(uint8_t *dsth, uint8_t *dstv, uint8_t *dstc,
                    const uint8_t *src, int stride, int width, int height,
                    int16_t *buf)
{
    for (int y = 0; y < height; ++y) {
        for (int x = -2; x < width+3; ++x) {
            int v = TAP(src, stride, x);
            dstv[x] = (uint8_t)clip_pixel((v + 16) >> 5);
            buf[x+2] = (int16_t)v;  /* bit depth 8 => pad=0 */
        }
        for (int x = 0; x < width; ++x)
            dstc[x] = (uint8_t)clip_pixel((TAP(buf+2, 1, x) + 512) >> 10);
        for (int x = 0; x < width; ++x)
            dsth[x] = (uint8_t)clip_pixel((TAP(src, 1, x) + 16) >> 5);
        dsth += stride; dstv += stride; dstc += stride; src += stride;
    }
}
