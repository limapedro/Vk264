/* SPDX-License-Identifier: GPL-2.0-or-later
 * x264 lookahead / slice-type decision port for the C oracle.
 * Mirrors encoder/slicetype.c: scenecut detection (scenecut_internal),
 * frame-type assignment (x264_slicetype_decide) and the B-frame/GOP
 * reorder with POC computation from encoder.c. */
#ifndef VK264_SLICETYPE_H
#define VK264_SLICETYPE_H
#include <stdint.h>

typedef enum {
    VK_FRAME_AUTO = -1,
    VK_FRAME_IDR  = 0,
    VK_FRAME_I    = 1,
    VK_FRAME_P    = 2,
    VK_FRAME_BREF = 3,
    VK_FRAME_B    = 4
} vk_frame_type_t;

typedef struct {
    vk_frame_type_t type;   /* decided type in display order */
    int i_frame;            /* display frame index */
    int frame_num;          /* H.264 frame_num (encoding order) */
    int poc;                /* picture order count */
    int is_scenecut;        /* scenecut flag */
    int num_bframes;        /* B frames preceding this frame in coding order */
} vk_frame_t;

/* Decide frame types, reorder into coding order, assign frame_num/POC.
 * frames: array of n raw YUV420 frames (w*h*3/2 bytes each), display order.
 * Returns coding order as an array of display indices in `coding_order`. */
void vk_slicetype_decide(const uint8_t *frames, int n, int w, int h,
                         int keyint_max, int keyint_min, int bframes,
                         int b_pyramid, int scenecut_threshold,
                         vk_frame_t *out, int *coding_order, int *n_coded);

/* SATD of a w*h luma plane against another (used by scenecut costs). */
uint64_t vk_slicetype_satd(const uint8_t *a, const uint8_t *b, int w, int h);

#endif
