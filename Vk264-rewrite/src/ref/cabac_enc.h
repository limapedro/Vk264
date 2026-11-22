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

/* Write a P-slice where every MB is P_SKIP (static-frame milestone).
 * bs has the P slice header written and 1-aligned; cb starts at bs->p. */
int vk_cabac_encode_pskip(vk_bs_t *bs, vk_cabac_t *cb, int w, int h);

/* Write a motion-compensated P-slice (16x16 partitions, integer-pel ME,
 * probe-based P_SKIP, zero residual). `cur` is frame 1, `ref` is the
 * reconstructed frame 0 (raw YUV420). */
int vk_cabac_encode_p(vk_bs_t *bs, vk_cabac_t *cb,
                      const uint8_t *cur, const uint8_t *ref,
                      int w, int h, int qp);
/* As above, with optional reconstructed-frame output for multi-frame encodes. */
int vk_cabac_encode_p_recon(vk_bs_t *bs, vk_cabac_t *cb,
                            const uint8_t *cur, const uint8_t *ref,
                            int w, int h, int qp, uint8_t *recon_out);
int vk_cabac_encode_p_recon_frame(vk_bs_t *bs, vk_cabac_t *cb,
                                  const uint8_t *cur, const uint8_t *ref,
                                  int w, int h, int qp, uint8_t *recon_out,
                                  int frame_idx);

/* Write an IDR slice and optionally copy out the reconstructed YUV420 frame. */
int vk_cabac_encode_idr_recon(vk_bs_t *bs, vk_cabac_t *cb,
                              const uint8_t *frame, int w, int h, int qp,
                              uint8_t *recon_out);
/* As above with the real frame index (x264's CABAC flush terminator bit
 * depends on h->i_frame & 31). */
int vk_cabac_encode_idr_recon_frame(vk_bs_t *bs, vk_cabac_t *cb,
                                    const uint8_t *frame, int w, int h, int qp,
                                    uint8_t *recon_out, int frame_idx);

#endif
