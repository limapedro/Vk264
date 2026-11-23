/* SPDX-License-Identifier: GPL-2.0-or-later
 * x264 I4x4 intra mode decision (analyse.c mb_analyse_intra), subme<=1 path.
 * Ported from the byte-exact Python model in build/I4X4-ALGORITHM.md. */
#ifndef VK264_I4X4_H
#define VK264_I4X4_H
#include <stdint.h>

/* Decide the 16 raw intra4x4 prediction modes for one macroblock.
 * src_y: 16x16 source luma. recon_y: reconstructed luma plane with LPAD
 * padding (px/py = MB top-left in that plane). qp: slice QP.
 * modes_out[16] is filled in x264's decision order (block_idx_xy_fenc).
 * Returns the sum of best SAD costs (for I16-vs-I4 mode selection). */
int vk_i4x4_decide(const uint8_t *src_y, uint8_t *recon_y, int rs,
                   int px, int py, int qp, int have_l, int have_t,
                   int have_tl, int have_tr,
                   uint8_t modes_out[16]);

/* x264_mb_pred_mode4x4_fix: raw modes 9/10/11 map to DC(2) at write time. */
int vk_i4x4_fix(int mode);
void vk_predict4(uint8_t out[16],int mode,const uint8_t left[4],
                 const uint8_t top[4],uint8_t lt,const uint8_t tr[4]);

#endif
