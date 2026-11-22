/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lowres.h"

/* x264 slicetype_frame_cost: a B frame at b between p0 and p1 is coded
 * bidirectionally — take the cheaper of list0 (from p0) and list1 (from p1). */
static uint64_t frame_cost(uint8_t **frames,int dw,int dh,int p0,int p1,int b)
{
    uint64_t cp = vk_lowres_p_cost(frames[b],frames[p0],dw,dh);
    if (b!=p1 && p1<=b+2 && p1>p0) {
        uint64_t c1 = vk_lowres_p_cost(frames[b],frames[p1],dw,dh);
        if (c1<cp) cp=c1;
    }
    return cp;
}

uint64_t vk_slicetype_path_cost(uint8_t **frames,int dw,int dh,
                                const char *path,int nframes,uint64_t threshold)
{
    uint64_t cost=0;
    int loc=1, cur_nonb=0;
    while(loc<nframes && path[loc-1]){
        int next_nonb=loc;
        while(next_nonb<nframes-1 && path[next_nonb-1]=='B') next_nonb++;
        if(next_nonb>=nframes) next_nonb=nframes-1;
        /* the non-B frame */
        cost+=frame_cost(frames,dw,dh,cur_nonb,next_nonb,next_nonb);
        if(cost>threshold) break;
        /* the B frames before it, bidirectionally predicted */
        for(int nb=loc;nb<next_nonb && cost<threshold;++nb)
            cost+=frame_cost(frames,dw,dh,cur_nonb,next_nonb,nb);
        loc=next_nonb+1; cur_nonb=next_nonb;
    }
    return cost;
}
