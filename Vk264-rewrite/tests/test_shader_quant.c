/* SPDX-License-Identifier: GPL-2.0-or-later
 * Equivalence: GPU quant4x4.comp == C oracle, bit-for-bit. */
#include "vk_host.h"
#include "slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCKS 4
#define PER_BLOCK 64
int main(int argc, char **argv)
{
    const char *spv = argc > 1 ? argv[1] : VK264_SHADER_DIR "quant4x4.spv";
    vk_host_t host;
    if (vk_host_init(&host) != 0) return 77;

    int32_t *buf = calloc(BLOCKS*PER_BLOCK, sizeof(int32_t));
    unsigned seed = 55555u;
    for (int b = 0; b < BLOCKS; ++b) {
        int32_t *blk = buf + b*PER_BLOCK;
        for (int i = 0; i < 16; ++i) {
            seed = seed*1103515245u + 12345u;
            blk[i] = (int32_t)(seed >> 13) - 32768;
        }
        for (int i = 0; i < 16; ++i) {
            seed = seed*1103515245u + 12345u;
            blk[16+i] = (int32_t)((seed >> 16) % 4000) + 100;
            blk[32+i] = (int32_t)((seed >> 8) % 40) + 1;
        }
    }
    int32_t before[BLOCKS][PER_BLOCK];
    memcpy(before, buf, sizeof(int32_t)*BLOCKS*PER_BLOCK);

    int rc = vk_host_dispatch(&host, spv, buf,
                              (size_t)BLOCKS*PER_BLOCK*sizeof(int32_t), BLOCKS);
    if (rc != 0) { fprintf(stderr, "dispatch failed %d\n", rc); return 1; }

    int bad = 0;
    for (int b = 0; b < BLOCKS; ++b) {
        int dct[16], mf[16], bias[16];
        for (int i = 0; i < 16; ++i) {
            dct[i] = before[b][i]; mf[i] = before[b][16+i]; bias[i] = before[b][32+i];
        }
        vk_ref_quant4x4(dct, mf, bias);
        for (int i = 0; i < 16; ++i)
            if (buf[b*PER_BLOCK+i] != dct[i]) {
                if (bad < 4)
                    fprintf(stderr, "block %d coef %d: gpu=%d oracle=%d\n",
                            b, i, buf[b*PER_BLOCK+i], dct[i]);
                ++bad;
            }
    }
    free(buf);
    vk_host_shutdown(&host);
    if (bad) return 1;
    printf("GPU quant4x4 == C oracle (%d blocks)\n", BLOCKS);
    return 0;
}
