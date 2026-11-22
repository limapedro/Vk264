/* SPDX-License-Identifier: GPL-2.0-or-later
 * Equivalence: GPU predict16x16.comp == C oracle, byte-for-byte. */
#include "vk_host.h"
#include "slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCKS 4
#define PER_BLOCK 289
int main(int argc, char **argv)
{
    const char *spv = argc > 1 ? argv[1] : VK264_SHADER_DIR "predict16x16.spv";
    vk_host_t host;
    if (vk_host_init(&host) != 0) return 77;

    int32_t *buf = calloc(BLOCKS*PER_BLOCK, sizeof(int32_t));
    unsigned seed = 424242u;
    for (int b = 0; b < BLOCKS; ++b) {
        int32_t *blk = buf + b*PER_BLOCK;
        for (int i = 0; i < 16; ++i) {
            seed = seed*1103515245u + 12345u;
            blk[i] = (int32_t)((seed >> 16) & 255);
            blk[16+i] = (int32_t)((seed >> 8) & 255);
        }
        blk[32] = b % 4; /* V, H, DC, DC_128 */
    }
    int32_t before[BLOCKS][PER_BLOCK];
    memcpy(before, buf, sizeof(int32_t)*BLOCKS*PER_BLOCK);

    int rc = vk_host_dispatch(&host, spv, buf,
                              (size_t)BLOCKS*PER_BLOCK*sizeof(int32_t), BLOCKS);
    if (rc != 0) { fprintf(stderr, "dispatch failed %d\n", rc); return 1; }

    int bad = 0;
    for (int b = 0; b < BLOCKS; ++b) {
        uint8_t top[16], left[16], oracle[256];
        for (int i = 0; i < 16; ++i) {
            top[i] = (uint8_t)before[b][i];
            left[i] = (uint8_t)before[b][16+i];
        }
        vk_ref_predict16x16(oracle, top, left, before[b][32]);
        for (int i = 0; i < 256; ++i)
            if (buf[b*PER_BLOCK+33+i] != oracle[i]) {
                if (bad < 4)
                    fprintf(stderr, "block %d pixel %d: gpu=%d oracle=%d\n",
                            b, i, buf[b*PER_BLOCK+33+i], oracle[i]);
                ++bad;
            }
    }
    free(buf);
    vk_host_shutdown(&host);
    if (bad) return 1;
    printf("GPU predict16x16 == C oracle (%d blocks)\n", BLOCKS);
    return 0;
}
