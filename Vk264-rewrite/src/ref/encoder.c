/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "encoder.h"

void vk_encoder_init(vk_encoder_t *e, int keyint, int bframes, int num_refs)
{
    e->keyint_max = keyint;
    e->keyint_min = keyint;
    e->bframes = bframes;
    e->num_ref_frames = num_refs;
    e->i_frame = 0;
    e->i_frame_num = 0;
    e->i_idr_pic_id = 0;
    e->i_last_idr = 0;
    e->i_last_keyframe = 0;
}

void vk_encoder_frame_begin(vk_encoder_t *e, int display_frame,
                            const vk_frame_t *st, vk_enc_frame_state_t *fs)
{
    (void)e;
    fs->i_frame = display_frame;
    fs->i_frame_num = st->frame_num;
    fs->i_poc = st->poc;
    fs->i_idr_pic_id = (st->type == VK_FRAME_IDR) ? (st->i_frame & 1) : 0;
    fs->nal_ref_idc = (st->type == VK_FRAME_IDR) ? 3 :
                      ((st->type == VK_FRAME_P || st->type == VK_FRAME_I ||
                        st->type == VK_FRAME_BREF) ? 2 : 0);
    fs->b_keyframe = (st->type == VK_FRAME_IDR);
}

void vk_encoder_frame_end(vk_encoder_t *e, const vk_enc_frame_state_t *fs)
{
    e->i_frame = fs->i_frame + 1;
    if (fs->b_keyframe) {
        e->i_frame_num = 0;
        e->i_last_idr = fs->i_frame;
        e->i_last_keyframe = fs->i_frame;
        e->i_idr_pic_id ^= 1;
    } else {
        e->i_frame_num = fs->i_frame_num + 1;
    }
}
