/* SPDX-License-Identifier: GPL-2.0-or-later
 * Equivalence test: GPU dct4x4.comp output must equal the C oracle exactly. */
#include "vk_host.h"
#include "slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCKS 4
#define INTS_PER_BLOCK 48  /* src[16] + pred[16] + dct[16] */

int main(int argc, char **argv)
{
    const char *spv = argc > 1 ? argv[1] : VK264_SHADER_DIR "dct4x4.spv";
    vk_host_t host;
    if (vk_host_init(&host) != 0) {
        fprintf(stderr, "Vulkan init failed\n");
        return 77; /* skip if no device */
    }

    int32_t *buf = calloc(BLOCKS * INTS_PER_BLOCK, sizeof(int32_t));
    uint8_t src[16], pred[16];
    unsigned seed = 12345;
    for (int b = 0; b < BLOCKS; ++b) {
        for (int i = 0; i < 16; ++i) {
            seed = seed * 1103515245u + 12345u;
            src[i] = (uint8_t)(seed >> 16);
            pred[i] = (uint8_t)(seed >> 8);
        }
        int oracle[16];
        vk_ref_dct4x4(oracle, src, pred);
        int32_t *block = buf + b*INTS_PER_BLOCK;
        for (int i = 0; i < 16; ++i) {
            block[i] = src[i];
            block[16+i] = pred[i];
            block[32+i] = -999999; /* poison: must be overwritten by shader */
        }
    }

    int rc = vk_host_dispatch(&host, spv, buf,
                              (size_t)BLOCKS*INTS_PER_BLOCK*sizeof(int32_t), BLOCKS);
    if (rc != 0) { fprintf(stderr, "dispatch failed: %d\n", rc); return 1; }

    int failures = 0;
    for (int b = 0; b < BLOCKS; ++b) {
        int oracle[16];
        /* regenerate the same src/pred for this block */
        unsigned s2 = 12345;
        for (int j = 0; j < b; ++j)
            for (int i = 0; i < 16; ++i) s2 = s2*1103515245u + 12345u;
        uint8_t s[16], p[16];
        for (int i = 0; i < 16; ++i) {
            s2 = s2*1103515245u + 12345u;
            s[i] = (uint8_t)(s2 >> 16);
            p[i] = (uint8_t)(s2 >> 8);
        }
        vk_ref_dct4x4(oracle, s, p);
        const int32_t *got = buf + b*INTS_PER_BLOCK + 32;
        for (int i = 0; i < 16; ++i)
            if (got[i] != oracle[i]) {
                if (failures < 4)
                    fprintf(stderr, "block %d coef %d: gpu=%d oracle=%d\n",
                            b, i, got[i], oracle[i]);
                ++failures;
            }
    }
    free(buf);
    vk_host_shutdown(&host);
    if (failures) { fprintf(stderr, "%d coefficient mismatches\n", failures); return 1; }
    printf("GPU dct4x4 == C oracle (%d blocks)\n", BLOCKS);
    return 0;
}
