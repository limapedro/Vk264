/* SPDX-License-Identifier: GPL-2.0-or-later
 * CAVLC residual coding ported from x264 encoder/cavlc.c + common/vlc.c. */
#ifndef VK264_CAVLC_H
#define VK264_CAVLC_H

#include "bitstream.h"
#include <stdint.h>

enum {
    VK_DCT_LUMA_DC = 0,
    VK_DCT_LUMA_AC = 1,
    VK_DCT_LUMA_4x4 = 2,
    VK_DCT_CHROMA_DC = 3,
    VK_DCT_CHROMA_AC = 4,
    VK_LEVEL_TABLE_SIZE = 128
};

typedef struct {
    int32_t last;
    int32_t mask;
    int32_t level[18];
} vk_run_level_t;

void vk_cavlc_init(void);
int vk_cavlc_coeff0_bits(int nC);
void vk_cavlc_block_residual(vk_bs_t *s, int cat, const int *coef, int nC,
                             int chroma_v_shift, int count);

#endif /* VK264_CAVLC_H */
