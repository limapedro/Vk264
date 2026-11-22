/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VK264_LOWRES_H
#define VK264_LOWRES_H
#include <stdint.h>

void vk_lowres_luma(const uint8_t *src,int sw,int sh,uint8_t *dst,int dw,int dh);
int vk_lowres_p_cost(const uint8_t *cur,const uint8_t *ref,int dw,int dh);
int vk_lowres_i_cost(const uint8_t *cur,int dw,int dh);

/* b-adapt=fast path cost: given lowres frames in display order starting at
 * last_nonb, a path string of 'B'/'P' and the number of frames, compute the
 * total frame cost (x264 slicetype_path_cost). threshold allows early exit. */
uint64_t vk_slicetype_path_cost(uint8_t **frames,int dw,int dh,
                                const char *path,int nframes,uint64_t threshold);

#endif
