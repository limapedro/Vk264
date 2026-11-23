/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VK264_LOWRES_H
#define VK264_LOWRES_H
#include <stdint.h>

#define VK_LOWRES_MAX_MBS (256)

/* x264 lookahead per-MB lowres MVs (frame_t.lowres_mvs). */
typedef struct {
    int mv[VK_LOWRES_MAX_MBS][2];   /* best fullpel MV per 8x8 lowres MB */
    int n_mbs;
} vk_lowres_mv_t;

void vk_lowres_mv_init(vk_lowres_mv_t *st,int dw,int dh);
/* Lowres luma buffer holds 4 contiguous dw*dh planes, as x264 frame_t.lowres:
 * [0]=base, [1]=h-shift (0.5px right), [2]=v-shift (0.5px down), [3]=both.
 * A prediction at fullpel (fx,fy) + halfpel (hx,hy) is read from
 * plane[hx+2*hy] at position (fx,fy). */
#define VK_LOWRES_PLANES 4
void vk_lowres_luma(const uint8_t *src,int sw,int sh,uint8_t *dst,int dw,int dh);
#define LR_PLANE(base,dw,dh,idx) ((base)+(size_t)(idx)*(dw)*(dh))
int vk_lowres_p_cost(const uint8_t *cur,const uint8_t *ref,int dw,int dh,
                     vk_lowres_mv_t *st);
/* Same P cost, also writes per-MB costs (with list0 bit) for mb-tree. */
int vk_lowres_p_cost_mb(const uint8_t *cur,const uint8_t *ref,int dw,int dh,
                        vk_lowres_mv_t *st,uint16_t *costs_out);
int vk_lowres_i_cost(const uint8_t *cur,int dw,int dh);
/* Per-MB intra costs (x264 frame_t.i_intra_cost) for mb-tree. */
void vk_lowres_intra_costs(const uint8_t *cur,int dw,int dh,uint16_t *out);
int vk_lowres_b_cost(const uint8_t *cur,const uint8_t *r0,const uint8_t *r1,
                     int dw,int dh,const vk_lowres_mv_t *st_p0,
                     const vk_lowres_mv_t *st_p1,int p0,int p1,int b);
uint64_t vk_slicetype_path_cost(uint8_t **frames,int dw,int dh,
                                const char *path,int nframes,uint64_t threshold,
                                vk_lowres_mv_t *st);

#endif
