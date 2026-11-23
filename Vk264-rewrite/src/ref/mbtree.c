/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mbtree.h"

void vk_mbtree_propagate_cost(int16_t *dst,const uint16_t *propagate_in,
                              const uint16_t *intra_costs,
                              const uint16_t *inter_costs,
                              const uint16_t *inv_qscales,float fps,int len)
{
    /* x264 common/mc.c:511-525. inv_qscales[i] is Q8.8 (256 = 1.0). */
    for(int i=0;i<len;++i){
        int intra_cost=intra_costs[i];
        int inter_cost=intra_cost < (inter_costs[i]&VK_LOWRES_COST_MASK)
                      ? intra_cost : (inter_costs[i]&VK_LOWRES_COST_MASK);
        /* x264 passes Q8.8 inv_qscales directly; fps_factor carries the
         * compensating 1/256 scaling (slicetype.c:1063). */
        float propagate_intra=(float)intra_cost*inv_qscales[i];
        float propagate_amount=propagate_in[i]+propagate_intra*fps;
        float propagate_num=(float)(intra_cost-inter_cost);
        float propagate_denom=(float)intra_cost;
        int v=propagate_denom>0 ? (int)(propagate_amount*propagate_num/propagate_denom+0.5f) : 0;
        dst[i]=(int16_t)(v<32767?v:32767);
    }
}

static void clip_add(uint16_t *s,int x)
{
    int v=(int)*s+x;
    *s=(uint16_t)(v<32767?v:32767);
}

void vk_mbtree_propagate_list(uint16_t *ref_costs,const int16_t (*mvs)[2],
                              const int16_t *propagate_amount,
                              const uint16_t *lowres_costs,int bipred_weight,
                              int mb_y,int len,int list,int width,int height,
                              int stride)
{
    /* x264 common/mc.c:527-598. mvs are spel (<<5) relative to the MB. */
    for(int i=0;i<len;++i){
        int lists_used=lowres_costs[i]>>VK_LOWRES_COST_SHIFT;
        if(!(lists_used&(1<<list))) continue;
        int listamount=propagate_amount[i];
        if(lists_used==3) listamount=(listamount*bipred_weight+32)>>6;
        if(mvs[i][0]==0 && mvs[i][1]==0){
            clip_add(&ref_costs[mb_y*stride+i],listamount);
            continue;
        }
        int x=mvs[i][0], y=mvs[i][1];
        unsigned mbx=(unsigned)((x>>5)+i);
        unsigned mby=(unsigned)((y>>5)+mb_y);
        unsigned idx0=mbx+mby*stride;
        unsigned idx2=idx0+stride;
        x&=31; y&=31;
        int w0=(32-y)*(32-x), w1=(32-y)*x, w2=y*(32-x), w3=y*x;
        w0=(w0*listamount+512)>>10;
        w1=(w1*listamount+512)>>10;
        w2=(w2*listamount+512)>>10;
        w3=(w3*listamount+512)>>10;
        if(mbx<(unsigned)width-1 && mby<(unsigned)height-1){
            clip_add(&ref_costs[idx0],w0);
            clip_add(&ref_costs[idx0+1],w1);
            clip_add(&ref_costs[idx2],w2);
            clip_add(&ref_costs[idx2+1],w3);
        } else {
            if(mby<(unsigned)height){
                if(mbx<(unsigned)width) clip_add(&ref_costs[idx0],w0);
                if(mbx+1<(unsigned)width) clip_add(&ref_costs[idx0+1],w1);
            }
            if(mby+1<(unsigned)height){
                if(mbx<(unsigned)width) clip_add(&ref_costs[idx2],w2);
                if(mbx+1<(unsigned)width) clip_add(&ref_costs[idx2+1],w3);
            }
        }
    }
}
