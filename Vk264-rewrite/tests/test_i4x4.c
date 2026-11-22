/* Byte-exact test for vk_i4x4_decide against x264's raw I4x4 mode dump
 * (build/I4X4-ALGORITHM.md: flat fenc=0 / fdec=128 fixture). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "i4x4.h"

#define RS 64
#define ORIGIN 32

int main(void)
{
    uint8_t *src = calloc(256, 1);          /* flat black source */
    uint8_t *recon = malloc((size_t)RS * (16 + 2*ORIGIN));
    if (!src || !recon) return 2;
    memset(recon, 128, (size_t)RS * (16 + 2*ORIGIN));

    uint8_t modes[16];
    vk_i4x4_decide(src, recon + ORIGIN*RS + ORIGIN, RS, 0, 0, 25,
                   0, 0, 0, 0, modes);

    /* x264 raw modes in decision order for this fixture */
    static const uint8_t expect[16] = {
        11,9,3,2, 9,9,2,2, 10,2,10,2, 2,2,2,2
    };
    int bad=0;
    for (int i=0;i<16;++i)
        if (modes[i]!=expect[i]) {
            fprintf(stderr,"i4x4 idx%d got %d expected %d\n",i,modes[i],expect[i]);
            bad++;
        }
    free(src); free(recon);
    if (bad) { fprintf(stderr,"i4x4: %d/16 mode mismatches\n",bad); return 1; }
    printf("i4x4: byte-exact (16/16 modes)\n");
    return 0;
}
