#include <stdio.h>
#include <stdint.h>
#include <math.h>
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
    /* macroblock_tree_finish (x264 slicetype.c:1029-1049):
     * ic=(intra*invq+128)>>8; pc=(prop*fps+128)>>8;
     * off = aq - 5*(1-qcompress)*(log2(ic+pc)-log2(ic)). */
    uint16_t ic[2]={1000,2000}, iq[2]={256,300}, pc[2]={500,800};
    float aq[2]={0.0f,0.0f}, off[2]={99.0f,99.0f};
    vk_mbtree_finish(off,ic,iq,pc,aq,256,0.6f,2);
    float strength=5.0f*(1.0f-0.6f);
    for(int i=0;i<2;++i){
        int ci=(ic[i]*iq[i]+128)>>8;
        int cp=(pc[i]*256+128)>>8;
        float want=aq[i]-strength*(log2f((float)(ci+cp))-log2f((float)ci));
        if(fabsf(off[i]-want)>1e-4f){
            fprintf(stderr,"mbtree finish FAIL i=%d got=%.6f want=%.6f\n",i,off[i],want);
            return 1;
        }
    }
    /* propagate_frame: two MBs, zero MVs -> each ref MB gets the clamped
     * propagated amount. intra=1000, invq=256(Q8.8), inter=0, fps=256,
     * pin=0 -> amount=1000*256=256000, num=den=1000 -> 32767 after clamp. */
    uint16_t ref0[2]={0,0}, ref1[2]={0,0};
    int16_t mvz[2][2]={{0,0},{0,0}};
    uint16_t pin2[2]={0,0}, ia2[2]={1000,1000}, iq2[2]={256,256};
    uint16_t lc2[2]={1<<VK_LOWRES_COST_SHIFT,1<<VK_LOWRES_COST_SHIFT};
    vk_mbtree_propagate_frame(ref0,ref1,mvz,NULL,pin2,ia2,iq2,lc2,
                              0,1,256,256.0f,2,1,2);
    for(int i=0;i<2;++i){
        if(ref0[i]!=32767){fprintf(stderr,"propagate_frame FAIL ref0[%d]=%d\n",i,ref0[i]);return 1;}
        if(ref1[i]!=0){fprintf(stderr,"propagate_frame FAIL ref1[%d]=%d\n",i,ref1[i]);return 1;}
    }
    printf("mbtree fixture PASS (cost + list + finish + propagate_frame)\n");
    return 0;
}
