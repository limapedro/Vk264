/* SPDX-License-Identifier: GPL-2.0-or-later
 * Equivalence: GPU dct4x4dc.comp == C oracle, bit-for-bit. */
#include "vk_host.h"
#include "slice.h"
#include <stdio.h>
#include <stdlib.h>

#define BLOCKS 4
int main(int argc, char **argv)
{
    const char *spv = argc > 1 ? argv[1] : VK264_SHADER_DIR "dct4x4dc.spv";
    vk_host_t host;
    if (vk_host_init(&host) != 0) return 77;

    int32_t *buf = calloc(BLOCKS*16, sizeof(int32_t));
    unsigned seed = 987654321u;
    for (int b = 0; b < BLOCKS; ++b)
        for (int i = 0; i < 16; ++i) {
            seed = seed*1103515245u + 12345u;
            buf[b*16+i] = (int32_t)(seed >> 12) - 65536;
        }
    int32_t before[BLOCKS][16];
    memcpy(before, buf, sizeof(int32_t)*BLOCKS*16);

    int rc = vk_host_dispatch(&host, spv, buf,
                              (size_t)BLOCKS*16*sizeof(int32_t), BLOCKS);
    if (rc != 0) { fprintf(stderr, "dispatch failed %d\n", rc); return 1; }

    int bad = 0;
    for (int b = 0; b < BLOCKS; ++b) {
        int oracle[16];
        memcpy(oracle, before[b], sizeof(oracle));
        vk_ref_dct4x4dc(oracle);
        for (int i = 0; i < 16; ++i)
            if (buf[b*16+i] != oracle[i]) {
                if (bad < 4)
                    fprintf(stderr, "block %d coef %d: gpu=%d oracle=%d\n",
                            b, i, buf[b*16+i], oracle[i]);
                ++bad;
            }
    }
    free(buf);
    vk_host_shutdown(&host);
    if (bad) return 1;
    printf("GPU dct4x4dc == C oracle (%d blocks)\n", BLOCKS);
    return 0;
}
