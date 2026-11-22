/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VK264_BSLICE_H
#define VK264_BSLICE_H
#include "bitstream.h"
#include "param.h"

void vk_encode_b_slice_allskip(vk_bs_t *bs, const vk_sps_t *sps,
                               const vk_pps_t *pps, int frame_num, int poc,
                               int slice_qp, int num_mbs, int nal_ref_idc);

void vk_encode_p_slice_allskip(vk_bs_t *bs, const vk_sps_t *sps,
                               const vk_pps_t *pps, int frame_num, int poc,
                               int slice_qp, int num_mbs);

#endif
