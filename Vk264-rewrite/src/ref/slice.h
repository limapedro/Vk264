/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VK264_SLICE_H
#define VK264_SLICE_H

#include "bitstream.h"
#include "param.h"
#include <stdint.h>

typedef struct {
    int i_first_mb, i_type, i_pps_id, i_frame_num;
    int i_idr_pic_id;
    int i_qp_delta;
    int i_disable_deblocking_filter_idc;
    int i_alpha_c0_offset, i_beta_offset;
} vk_slice_header_t;

void vk_slice_header_init(vk_slice_header_t *sh, const vk_sps_t *sps,
                          const vk_pps_t *pps, int slice_qp);
void vk_slice_header_write(vk_bs_t *s, const vk_slice_header_t *sh,
                           const vk_sps_t *sps, const vk_pps_t *pps, int nal_ref_idc);

/* Encode one I16x16 macroblock. source points at 256 Y + 64 Cb + 64 Cr bytes.
 * Entropy is written to s; decoded samples are written back to recon_y/u/v. */
/* Neighbour samples for multi-MB frames. NULL => uniform 128 (frame corner). */
typedef struct {
    const uint8_t *top_y, *left_y, *top_u, *left_u, *top_v, *left_v;
    uint8_t tl_y, tl_u, tl_v;
    int avail;   /* bit0=left, bit1=top, bit3=top-left (x264) */
    int luma_dc_nc;  /* CAVLC nC for the luma DC block */
    int qp;
} vk_neighbours_t;

/* Oracle entry points for GPU-vs-C equivalence tests. */
void vk_ref_dct4x4(int dct[16], const uint8_t src[16], const uint8_t pred[16]);
void vk_ref_dct4x4dc(int d[16]);
int vk_ref_quant4x4(int dct[16], const int mf[16], const int bias[16]);
void vk_ref_predict16x16(uint8_t out[256], const uint8_t top[16],
                         const uint8_t left[16], int mode);

void vk_encode_i16_mb(vk_bs_t *s, const uint8_t *src_y, const uint8_t *src_u,
                      const uint8_t *src_v, uint8_t *recon_y, uint8_t *recon_u,
                      uint8_t *recon_v, int qp, int mb_x, int mb_y,
                      const vk_neighbours_t *nb, int stride_y, int stride_c);

#endif /* VK264_SLICE_H */
