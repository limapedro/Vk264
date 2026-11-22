/* SPDX-License-Identifier: GPL-2.0-or-later
 * P-frame encoder for the C oracle (x264 ultrafast: CAVLC, 16x16 only,
 * integer-pel ME, probe-based P_SKIP). Test/reference implementation.
 */
#ifndef VK264_PFRAME_H
#define VK264_PFRAME_H

#include "bitstream.h"
#include "param.h"
#include <stdint.h>

/* Encode a P-slice. cur/ref are full YUV420 frames (w*h*3/2 bytes);
 * ref is the reconstructed previous frame. qp is the slice QP. */
void vk_encode_p_frame(vk_bs_t *bs, const vk_sps_t *sps, const vk_pps_t *pps,
                       const uint8_t *cur, const uint8_t *ref,
                       int w, int h, int qp, int frame_num);
/* As above, but also exports the reconstructed YUV420 frame (for chained
 * P-slices in a multi-frame video encode). recon_out may be NULL. */
void vk_encode_p_frame_recon(vk_bs_t *bs, const vk_sps_t *sps, const vk_pps_t *pps,
                             const uint8_t *cur, const uint8_t *ref,
                             int w, int h, int qp, int frame_num,
                             uint8_t *recon_out);

/* Decide P_SKIP for one MB: 1 iff the residual at the P_SKIP MV predictor
 * quantises to zero (x264 macroblock_probe_pskip). mvp is in quarter-pel. */
int vk_probe_pskip(const uint8_t *cur, int cw, const uint8_t *ref, int rw,
                   int pad, int x, int y, int mvp_x_qp, int mvp_y_qp, int qp);

#endif
