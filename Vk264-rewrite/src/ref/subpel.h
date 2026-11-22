/* SPDX-License-Identifier: GPL-2.0-or-later
 * Sub-pel motion estimation (x264 me.c refine_subpel, subme=1 path). */
#ifndef VK264_SUBPEL_H
#define VK264_SUBPEL_H
#include <stdint.h>

/* Half-pel planes generated from a padded luma reference frame.
 * Each plane has the same layout as `ref` (stride rs, LPAD border). */
typedef struct {
    uint8_t *h;   /* horizontal half-pel */
    uint8_t *v;   /* vertical half-pel */
    uint8_t *c;   /* centre half-pel */
    int rs;       /* stride */
} vk_hpel_planes_t;

/* Generate h/v/c planes for a padded reference frame (w x h active area). */
void vk_hpel_planes_init(vk_hpel_planes_t *planes, const uint8_t *ref,
                         int rs, int w, int h, int pad);
void vk_hpel_planes_free(vk_hpel_planes_t *planes);

/* Refine a full-pel MV to quarter-pel using x264's subme=1 algorithm.
 * fenc is the current frame luma (stride cs); ref/hpel describe the padded
 * reference. x,y is the MB position. mvx/mvy are in/out quarter-pel MVs;
 * cost is in/out SAD + mv_cost. mvpx/mvpy are the predictor (qpel). */
void vk_me_refine_qpel(const uint8_t *fenc, int cs,
                       const uint8_t *ref, int rs, int pad,
                       const vk_hpel_planes_t *hpel,
                       int x, int y,
                       int *mvx, int *mvy, int *cost,
                       int mvpx, int mvpy, int lambda);

#endif
