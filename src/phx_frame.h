/* Phoenix frame object — mirrors the fields of x264_frame_t that the
 * lookahead/lowres path uses.
 *
 * Source: reference/x264/common/frame.h:37-140 (struct layout),
 *         reference/x264/common/frame.c:30/38/60/535/627 (allocation + border),
 *         reference/x264/common/mc.c:458/484 (lowres planes).
 *
 * SHADER: A — allocation and plane preparation are host-side; the lowres
 * planes are the read-only input to lookahead shaders.
 */
#ifndef PHX_FRAME_H
#define PHX_FRAME_H

#include <stdint.h>

#define PHX_PADH 32                 /* frame.h:32 */
#define PHX_PADV 32                 /* frame.h:33 */
#define PHX_PADH_ALIGN 32           /* frame.h:34 (scalar build) */
#define PHX_PADH2 (PHX_PADH_ALIGN + PHX_PADH) /* frame.h:35 */
#define PHX_BFRAME_MAX 16           /* base.h:136 */
#define PHX_LOWRES_PLANES 4

/* The subset of x264_frame_t (frame.h:37-140) needed by the port. */
typedef struct {
    int i_width, i_height;          /* coded luma size (16*mb_width/height) */
    int i_stride;                   /* luma stride, frame.c:87 */
    int i_width_lowres, i_lines_lowres, i_stride_lowres; /* frame.h:77-79 */

    uint8_t *base;                  /* single allocation (frame.c:235) */
    uint8_t *plane;                 /* luma plane, frame.c:261 */
    uint8_t *lowres[4];             /* Orig/H/V/HV, frame.c:281 */

    int i_cost_est[PHX_BFRAME_MAX+2][PHX_BFRAME_MAX+2]; /* frame.h:123 */
    int *i_row_satds[PHX_BFRAME_MAX+2][PHX_BFRAME_MAX+2]; /* frame.h:127 */
    int16_t lowres_mvs[2][PHX_BFRAME_MAX+1][2]; /* frame.h:103 */

    int bframes;                    /* x264 param.i_bframe, used by init */
} phx_frame_t;

/* frame.c:30 — align_stride() with the scalar-build align/disalign values. */
int phx_align_stride(int x, int align, int disalign);

/* frame.c:38 — align_plane_size(). */
int phx_align_plane_size(int x, int disalign);

/* frame.c:60 — allocate a frame; width/height must be 16-pixel multiples. */
phx_frame_t *phx_frame_new(int width, int height, int bframes);

/* frame.c:312 — release the frame and all its sub-allocations. */
void phx_frame_delete(phx_frame_t *f);

/* frame.c:535 — replicate edge samples into the left/right/top/bottom pads. */
void phx_plane_expand_border(uint8_t *pix, int stride, int width, int height,
                             int padh, int padv, int pad_top, int pad_bottom,
                             int chroma);

/* frame.c:627 — expand all four lowres planes. */
void phx_frame_expand_border_lowres(phx_frame_t *f);

/* mc.c:484 — the C reference implementation of frame_init_lowres_core. */
void phx_frame_init_lowres_core(uint8_t *src0, uint8_t *dst0, uint8_t *dsth,
                                uint8_t *dstv, uint8_t *dstc,
                                int src_stride, int dst_stride,
                                int width, int height);

/* mc.c:458 — prepare the lowres planes and lookahead caches of a frame. */
void phx_frame_init_lowres(phx_frame_t *f);

#endif
