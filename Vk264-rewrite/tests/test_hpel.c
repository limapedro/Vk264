/* Byte-exact test for vk_hpel_filter vs x264 common/mc.c:174 algorithm.
 * The expected planes are computed independently from x264's 6-tap
 * [1,-5,20,20,-5,1] filter with >>5 (h/v) and >>10 (c) rounding. */
#include <stdio.h>
#include <stdlib.h>
#include "hpel.h"

#define PAD 16
#define W 64
#define H 48
#define STRIDE (W + 2*PAD)

static int clip(int v){ return v<0?0:(v>255?255:v); }
static int tap(const uint8_t *p, int d, int x){
    return (int)p[x-2*d] + (int)p[x+3*d]
         - 5*((int)p[x-d] + (int)p[x+2*d])
         + 20*((int)p[x] + (int)p[x+d]);
}
static int tap16(const int16_t *p, int x){
    return (int)p[x-2] + (int)p[x+3]
         - 5*((int)p[x-1] + (int)p[x+2])
         + 20*((int)p[x] + (int)p[x+1]);
}

int main(void)
{
    size_t sz = (size_t)STRIDE*(H+2*PAD);
    uint8_t *src = malloc(sz);
    uint8_t *dh = malloc(sz), *dv = malloc(sz), *dc = malloc(sz);
    if (!src||!dh||!dv||!dc) return 2;
    unsigned s = 12345u;
    for (size_t i = 0; i < sz; ++i) { s=s*1103515245u+12345u; src[i]=(uint8_t)(s>>16); }
    const uint8_t *srow = src + PAD*STRIDE + PAD;
    uint8_t *oh = dh + PAD*STRIDE + PAD, *ov = dv + PAD*STRIDE + PAD, *oc = dc + PAD*STRIDE + PAD;

    int16_t buf[W+8];
    vk_hpel_filter(oh, ov, oc, srow, STRIDE, W, H, buf);

    int bad = 0;
    for (int y = 0; y < H; ++y) {
        const uint8_t *row = srow + y*STRIDE;
        int16_t b[W+8];
        for (int k = -2; k < W+3; ++k) b[k+2] = (int16_t)tap(row, STRIDE, k);
        for (int x = 0; x < W; ++x) {
            int expv = clip((b[x+2]+16)>>5);
            int expc = clip((tap16(b, x+2)+512)>>10);
            int exph = clip((tap(row,1,x)+16)>>5);
            if (oh[y*STRIDE+x]!=(uint8_t)exph ||
                ov[y*STRIDE+x]!=(uint8_t)expv ||
                oc[y*STRIDE+x]!=(uint8_t)expc) {
                if (++bad <= 5) fprintf(stderr,"y=%d x=%d h %d/%d v %d/%d c %d/%d\n",
                    y,x,oh[y*STRIDE+x],exph,ov[y*STRIDE+x],expv,oc[y*STRIDE+x],expc);
            }
        }
    }
    free(src);free(dh);free(dv);free(dc);
    if (bad) { fprintf(stderr,"hpel: %d mismatches\n",bad); return 1; }
    printf("hpel: byte-exact (%d pixels)\n",W*H);
    return 0;
}
