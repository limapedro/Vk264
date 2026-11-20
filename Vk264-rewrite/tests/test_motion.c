/* SPDX-License-Identifier: GPL-2.0-or-later
 * Unit test for the integer-pel motion-search module: verify that vk_me16
 * recovers a known translation and that vk_mvp16 computes the H.264 median.
 */
#include "motion.h"
#include <stdio.h>
#include <string.h>

#define W 64
#define H 64
#define PAD 16
#define RS (W+2*PAD)

static uint8_t pix(int x, int y)
{
    /* Non-periodic pseudo-random pattern so a translation has a unique SAD=0. */
    uint32_t h = (uint32_t)(x*374761393 + y*668265263 + 13);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (uint8_t)((h ^ (h >> 16)) & 0xff);
}

int main(void)
{
    static uint8_t src[W*H], ref[RS*(H+2*PAD)], cur[W*H];
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            src[y*W+x] = pix(x, y);
    memset(ref, 0, sizeof(ref));
    vk_pad_plane(ref, RS, src, W, W, H, PAD);

    /* Current block is the reference translated by MV = (5, -3):
     * cur[y][x] = ref[y + (-3)][x + 5] = pix(x+5, y-3). */
    const int TX = 5, TY = -3;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            cur[y*W+x] = pix(x + TX, y + TY);

    int mx = 99, my = 99;
    int sad = vk_me16(cur, W, ref, RS, PAD, 16, 16, 0, 0, 12, &mx, &my);
    printf("  ME recovered mv=(%d,%d) sad=%d (expected 5,-3 / sad 0)\n", mx, my, sad);
    if (mx != TX || my != TY || sad != 0) return 1;

    /* MVP median: component-wise median of left/top/top-right. */
    int px, py;
    vk_mvp16(5, -3, 2, 4, 9, -8, &px, &py);
    printf("  MVP median((5,-3),(2,4),(9,-8)) = (%d,%d) (expected 5,-3)\n", px, py);
    if (px != 5 || py != -3) return 1;

    vk_mvp16(0, 0, 0, 0, 8, 2, &px, &py);
    printf("  MVP median((0,0),(0,0),(8,2)) = (%d,%d) (expected 0,0)\n", px, py);
    if (px != 0 || py != 0) return 1;

    printf("motion search: integer ME + 16x16 MV predictor OK\n");
    return 0;
}
