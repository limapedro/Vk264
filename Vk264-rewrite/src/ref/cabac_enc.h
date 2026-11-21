/* SPDX-License-Identifier: GPL-2.0-or-later
 * CABAC slice/MB writer for the C oracle (I-slice I16 path). */
#ifndef VK264_CABAC_ENC_H
#define VK264_CABAC_ENC_H

#include "bitstream.h"
#include "cabac.h"
#include "param.h"
#include <stdint.h>

/* Write an IDR slice with CABAC entropy coding. The bitstream writer `bs`
 * must already have the slice header written and 1-aligned; `cb` is the
 * CABAC coder positioned at bs->p. Returns total slice length in bytes. */
int vk_cabac_encode_idr(vk_bs_t *bs, vk_cabac_t *cb,
                        const uint8_t *frame, int w, int h, int qp,
                        int frame_num);

#endif
