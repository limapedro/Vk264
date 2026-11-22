/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lowres.h"

/* x264 slicetype_frame_cost: P frame from p0, B frame bidirectionally from
 * p0/p1 (or I when p0==p1). */
static uint64_t frame_cost(uint8_t **frames,int dw,int dh,int p0,int p1,int b)
{
    uint64_t c;
    if(p0==p1) return vk_lowres_i_cost(frames[b],dw,dh);
    /* A non-B frame at b==p1 is predicted from p0 (P cost), not bidir. */
    if(b==p0 || b==p1) return vk_lowres_p_cost(frames[b],frames[p0],dw,dh);
    c=vk_lowres_b_cost(frames[b],frames[p0],frames[p1],dw,dh);
    /* x264 slicetype.c:985: B cost scaled by 100/(120+bframe_bias).
     * Default bframe_bias=0 => *100/120, making B comparatively cheaper. */
    return c*100/120;
}

/* Mirrors x264 slicetype_path_cost (encoder/slicetype.c:1288).
 * x264 does `path--` so path[1] is the type of frames[1].
 * Our caller passes a path with path[0] as a dummy prefix, so path[1]
 * likewise represents frames[1]. */
uint64_t vk_slicetype_path_cost(uint8_t **frames,int dw,int dh,
                                const char *path,int nframes,uint64_t threshold)
{
    uint64_t cost=0;
    int loc=1, cur_nonb=0;
    while(loc<nframes && path[loc]){
        int next_nonb=loc;
        while(next_nonb<nframes-1 && path[next_nonb]=='B') next_nonb++;
        if(next_nonb>=nframes) next_nonb=nframes-1;
        /* P (or I) frame at the next non-B position */
        if(path[next_nonb]=='I')
            cost+=frame_cost(frames,dw,dh,next_nonb,next_nonb,next_nonb);
        else
            cost+=frame_cost(frames,dw,dh,cur_nonb,next_nonb,next_nonb);
        if(cost>threshold) break;
        /* B frames between cur_nonb and next_nonb, predicted from both */
        for(int nb=loc;nb<next_nonb && cost<threshold;++nb)
            cost+=frame_cost(frames,dw,dh,cur_nonb,next_nonb,nb);
        loc=next_nonb+1; cur_nonb=next_nonb;
    }
    return cost;
}
