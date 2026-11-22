/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VK264_LOWRES_H
#define VK264_LOWRES_H
#include <stdint.h>

/* Lookahead lowres state: x264 frame_t.lowres_mvs / lowres_mv_costs. */
typedef struct {
    int mv[2][2];    /* [list][xy] best fullpel MV for this frame */
    int cost[2];
} vk_lowres_mv_t;

void vk_lowres_luma(const uint8_t *src,int sw,int sh,uint8_t *dst,int dw,int dh);
/* P cost; also stores the best MV into state[b].mv[0] when state != NULL. */
int vk_lowres_p_cost(const uint8_t *cur,const uint8_t *ref,int dw,int dh,
                     vk_lowres_mv_t *st);
int vk_lowres_i_cost(const uint8_t *cur,int dw,int dh);
/* B cost with dmv prediction from the future reference's stored MV and
 * dist_scale_factor = ((b-p0)<<8 + (p1-p0)/2) / (p1-p0). */
int vk_lowres_b_cost(const uint8_t *cur,const uint8_t *r0,const uint8_t *r1,
                     int dw,int dh,const vk_lowres_mv_t *st_p0,
                     const vk_lowres_mv_t *st_p1,int p0,int p1,int b);

uint64_t vk_slicetype_path_cost(uint8_t **frames,int dw,int dh,
                                const char *path,int nframes,uint64_t threshold,
                                vk_lowres_mv_t *st);

#endif
