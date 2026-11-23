/* SPDX-License-Identifier: GPL-2.0-or-later
 * x264 mb-tree primitives (common/mc.c:511-600). */
#ifndef VK264_MBTREE_H
#define VK264_MBTREE_H
#include <stdint.h>

#define VK_LOWRES_COST_MASK 0x3fff
#define VK_LOWRES_COST_SHIFT 14

/* x264 common/mc.c:511-525. */
void vk_mbtree_propagate_cost(int16_t *dst,const uint16_t *propagate_in,
                              const uint16_t *intra_costs,
                              const uint16_t *inter_costs,
                              const uint16_t *inv_qscales,float fps,int len);

/* x264 common/mc.c:527-600. MVx/MVy are spel (<<5) relative to the MB. */
void vk_mbtree_propagate_list(uint16_t *ref_costs,const int16_t (*mvs)[2],
                              const int16_t *propagate_amount,
                              const uint16_t *lowres_costs,int bipred_weight,
                              int mb_y,int len,int list,int width,int height,
                              int stride);

/* x264 encoder/slicetype.c:1029-1049 macroblock_tree_finish: derive the
 * per-MB QP offset from intra cost, propagated cost and Q8.8 inv_qscale.
 * fps_factor is the integer factor from line 1031. */
void vk_mbtree_finish(float *qp_offset,const uint16_t *intra_cost,
                      const uint16_t *inv_qscale,const uint16_t *propagate_cost,
                      const float *aq_offset,int fps_factor,float qcompress,
                      int len);

#endif
