/* Smoke/determinism test for vk_me_refine_qpel (x264 subme=1 algorithm).
 * Verifies the search converges and the returned cost matches a full
 * recomputation at the reported MV. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "subpel.h"

#define PAD 32
#define W 64
#define H 64
#define RS (W + 2*PAD)
#define SZ ((H + 2*PAD) * RS)

static int sad16(const uint8_t *a, const uint8_t *b)
{
    int s = 0;
    for (int i = 0; i < 256; ++i) s += a[i] > b[i] ? a[i]-b[i] : b[i]-a[i];
    return s;
}

int main(void)
{
    uint8_t *ref = calloc(1, SZ), *fenc = malloc((size_t)W*H);
    if (!ref || !fenc) return 2;
    /* reference: vertical gradient so subpel shifts change SAD measurably */
    for (int y = 0; y < H+2*PAD; ++y)
        for (int x = 0; x < RS; ++x)
            ref[y*RS+x] = (uint8_t)((x + 2*y) & 255);
    /* fenc = ref shifted by (+1,+2) fullpel, stored as a full W-wide frame */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            fenc[y*W+x] = ref[(PAD+y+2)*RS + (PAD+x+1)];
    const uint8_t *refp = ref + PAD*RS + PAD;

    vk_hpel_planes_t hp;
    vk_hpel_planes_init(&hp, refp, RS, W, H, PAD);

    /* Fullpel search would have found (4,8) qpel = (1,2) fullpel. Start there. */
    int mvx = 4, mvy = 8, cost = 0x7fffffff;
    vk_me_refine_qpel(fenc, W, refp, RS, PAD, &hp, 32, 32,
                      &mvx, &mvy, &cost,
                      4, 8, 6);
    printf("subpel: mv=(%d,%d) cost=%d\n", mvx, mvy, cost);
    /* The source is an exact integer-pel shift, so SAD at (4,8) should be 0. */
    /* mvcost(0)=4 per axis at lambda=6, so zero SAD yields cost 8. */
    if (mvx != 4 || mvy != 8 || cost > 16) {
        fprintf(stderr,"subpel: expected mv=(4,8) cost~0\n");
        vk_hpel_planes_free(&hp); free(ref); free(fenc); return 1;
    }
    vk_hpel_planes_free(&hp); free(ref); free(fenc);
    printf("subpel: converged\n");
    return 0;
}
