/* SPDX-License-Identifier: GPL-2.0-or-later
 * Encoder orchestration state for the C oracle (encoder/encoder.c).
 * Manages frame_num, POC, IDR counter and reference list across the encode
 * loop; the CLI currently tracks these with ad-hoc locals. */
#ifndef VK264_ENCODER_H
#define VK264_ENCODER_H
#include "slicetype.h"
#include <stdint.h>

typedef struct {
    int i_frame;            /* display frame index */
    int i_frame_num;        /* H.264 frame_num */
    int i_poc;              /* picture order count */
    int i_idr_pic_id;       /* IDR identifier (toggles) */
    int i_last_idr;         /* last IDR display index */
    int i_last_keyframe;
    int nal_ref_idc;
    int b_keyframe;
} vk_enc_frame_state_t;

typedef struct {
    int keyint_max, keyint_min, bframes;
    int i_frame;
    int i_frame_num;
    int i_idr_pic_id;
    int i_last_idr;
    int i_last_keyframe;
    int num_ref_frames;
} vk_encoder_t;

void vk_encoder_init(vk_encoder_t *e, int keyint, int bframes, int num_refs);
/* Begin a coded frame in the given display order; fills the frame state. */
void vk_encoder_frame_begin(vk_encoder_t *e, int display_frame,
                            const vk_frame_t *slicetype,
                            vk_enc_frame_state_t *fs);
/* End of frame: update reference/DPB state. */
void vk_encoder_frame_end(vk_encoder_t *e, const vk_enc_frame_state_t *fs);

#endif
