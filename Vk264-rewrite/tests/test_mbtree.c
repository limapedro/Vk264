#include <stdio.h>
#include <stdint.h>
#include "mbtree.h"

int main(void)
{
    /* Fixtures from x264 common/mc.c:511-525 (Q8.8 inv_qscales, fps carries
     * the compensating 1/256 scaling as in slicetype.c:1063). */
    const uint16_t pin[3]    = {0, 100, 50};
    const uint16_t intra[3]  = {1000, 2000, 500};
    const uint16_t inter[3]  = {400, 2000, 32768}; /* 32768 = list bits, cost 0 */
    const uint16_t invq[3]   = {256, 256, 300};
    const float fps[3]       = {1.0f, 1.0f, 0.5f};
    const int16_t want[3]    = {32767, 0, 32767}; /* x264 clamps to (1<<15)-1 */
    int16_t dst[3]={0,0,0};
    for(int mode=0;mode<1;++mode)
        vk_mbtree_propagate_cost(dst,pin,intra,inter,invq,1.0f,3);
    for(int i=0;i<3;++i){
        /* recompute the x264 formula with each frame's own fps */
        float prop_intra=(float)intra[i]*invq[i];
        float amount=pin[i]+prop_intra*fps[i];
        int ic=inter[i]&VK_LOWRES_COST_MASK;
        if(ic>intra[i])ic=intra[i];
        int expect=(int)(amount*(intra[i]-ic)/(float)intra[i]+0.5f);
        if(expect>32767)expect=32767;
        if(expect!=want[i]){ fprintf(stderr,"fixture self-check FAIL %d expect=%d want=%d\n",i,expect,want[i]); return 1; }
    }
    /* Direct per-element checks with the matching fps factor. */
    for(int i=0;i<3;++i){
        int16_t one[1]; uint16_t pi[1]={pin[i]},ia[1]={intra[i]},
                ie[1]={inter[i]},iq[1]={invq[i]};
        vk_mbtree_propagate_cost(one,pi,ia,ie,iq,fps[i],1);
        if(one[0]!=want[i]){
            fprintf(stderr,"mbtree cost FAIL i=%d got=%d want=%d\n",i,one[0],want[i]);
            return 1;
        }
    }
    /* propagate_list: zero MV adds listamount to the target MB. */
    uint16_t refs[16]={0}; int16_t mvs[1][2]={{0,0}};
    int16_t amt[1]={100}; uint16_t lc[1]={1<<VK_LOWRES_COST_SHIFT};
    vk_mbtree_propagate_list(refs,mvs,amt,lc,32,0,1,0,4,4,4);
    if(refs[0]!=100){ fprintf(stderr,"mbtree list mv0 FAIL %d\n",refs[0]); return 1; }
    printf("mbtree fixture PASS (propagate_cost + zero-MV list)\n");
    return 0;
}
