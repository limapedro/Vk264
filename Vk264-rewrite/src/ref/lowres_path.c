/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lowres.h"

static uint64_t frame_cost(uint8_t **frames,int dw,int dh,int p0,int p1,int b,
                           vk_lowres_mv_t *st)
{
    uint64_t c;
    if(p0==p1) return vk_lowres_i_cost(frames[b],dw,dh);
    if(b==p0 || b==p1) return vk_lowres_p_cost(frames[b],frames[p0],dw,dh,&st[b]);
    c=vk_lowres_b_cost(frames[b],frames[p0],frames[p1],dw,dh,
                       &st[p0],&st[p1],p0,p1,b);
    return c*100/120;  /* x264 slicetype.c:985 bframe_bias=0 */
}

uint64_t vk_slicetype_path_cost(uint8_t **frames,int dw,int dh,
                                const char *path,int nframes,uint64_t threshold,
                                vk_lowres_mv_t *st)
{
    uint64_t cost=0;
    int loc=1, cur_nonb=0;
    while(loc<nframes && path[loc]){
        int next_nonb=loc;
        while(next_nonb<nframes-1 && path[next_nonb]=='B') next_nonb++;
        if(next_nonb>=nframes) next_nonb=nframes-1;
        if(path[next_nonb]=='I')
            cost+=vk_lowres_i_cost(frames[next_nonb],dw,dh);
        else
            cost+=frame_cost(frames,dw,dh,cur_nonb,next_nonb,next_nonb,st);
        if(cost>threshold) break;
        for(int nb=loc;nb<next_nonb && cost<threshold;++nb)
            cost+=frame_cost(frames,dw,dh,cur_nonb,next_nonb,nb,st);
        loc=next_nonb+1; cur_nonb=next_nonb;
    }
    return cost;
}
