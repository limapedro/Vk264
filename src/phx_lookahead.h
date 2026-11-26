/* Phoenix lookahead / slice-type state machine.
 *
 * Function-for-function port of the *serial* (non-threaded) x264 path:
 *   reference/x264/encoder/lookahead.c:42   lookahead_shift()
 *   reference/x264/encoder/lookahead.c:59   lookahead_update_last_nonb()
 *   reference/x264/encoder/lookahead.c:116  x264_lookahead_init()  (i_last_keyframe)
 *   reference/x264/encoder/lookahead.c:192  x264_lookahead_put_frame()
 *   reference/x264/encoder/lookahead.c:210  lookahead_encoder_shift()
 *   reference/x264/encoder/lookahead.c:223  x264_lookahead_get_frames()
 *   reference/x264/encoder/slicetype.c:1384 scenecut_internal()
 *   reference/x264/encoder/slicetype.c:1430 scenecut()
 *   reference/x264/encoder/slicetype.c:1473 x264_slicetype_analyse()
 *   reference/x264/encoder/slicetype.c:1745 x264_slicetype_decide()
 *   reference/x264/x264.h:272-280           frame type constants
 *
 * phx_frame_t deliberately has no i_type / i_frame / i_bframes fields
 * (src/phx_frame.h:17-39), so this module keeps a parallel, private metadata
 * record per queued frame.  The lowres[] planes and the i_cost_est /
 * i_row_satds / lowres_mvs caches are read from phx_frame_t itself
 * (src/phx_frame.h:28-35), exactly as x264 reads them from x264_frame_t.
 *
 * Ownership: frames are borrowed, never copied or freed.  A frame must stay
 * alive until phx_lookahead_delete() because the most recent non-B frame is
 * retained as `last_nonb` and is used as a motion-compensation reference for
 * later decisions.  This mirrors x264's frame pool lifetime.
 *
 * Not ported (explicit TODOs in the .c file): MB-tree propagation,
 * trellis B-adapt, VBV lookahead, intra refresh, forced frame types,
 * B-pyramid parameter (normal-mode BREF insertion at slicetype.c:1892-1895
 * IS ported), weighted prediction, full lowres motion search.
 * No claim of bitstream or decision parity with x264 is made.
 *
 * SHADER: C -- serial lookahead; one ordered queue, one decision per
 *              minigop, no workgroups and no hidden threading state.
 */
#ifndef PHX_LOOKAHEAD_H
#define PHX_LOOKAHEAD_H

#include "phx_param.h"
#include "phx_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* x264.h:272-278.  Numeric values are kept identical so callers can map them
 * to PHX_SLICE_* (src/phx_slice.h:27-31): IDR/I -> PHX_SLICE_I,
 * P -> PHX_SLICE_P, B/BREF -> PHX_SLICE_B. */
#define PHX_TYPE_AUTO     0x0000 /* x264.h:272 */
#define PHX_TYPE_IDR      0x0001 /* x264.h:273 */
#define PHX_TYPE_I        0x0002 /* x264.h:274 */
#define PHX_TYPE_P        0x0003 /* x264.h:275 */
#define PHX_TYPE_BREF     0x0004 /* x264.h:276 */
#define PHX_TYPE_B        0x0005 /* x264.h:277 */
#define PHX_TYPE_KEYFRAME 0x0006 /* x264.h:278 */

#define PHX_IS_I(x) ((x) == PHX_TYPE_I || (x) == PHX_TYPE_IDR || \
                     (x) == PHX_TYPE_KEYFRAME)           /* x264.h:279 */
#define PHX_IS_B(x) ((x) == PHX_TYPE_B || (x) == PHX_TYPE_BREF) /* x264.h:280 */
#define PHX_IS_AUTO_OR_I(x) ((x) == PHX_TYPE_AUTO || PHX_IS_I(x)) /* slicetype.c:1470 */
#define PHX_IS_AUTO_OR_B(x) ((x) == PHX_TYPE_AUTO || PHX_IS_B(x)) /* slicetype.c:1471 */

typedef struct phx_lookahead phx_lookahead_t;

/* lookahead.c:110-150 (x264_lookahead_init).  Copies the parameters it needs
 * and initialises i_last_keyframe = -i_keyint_max.  mb_width/mb_height are the
 * macroblock dimensions used to walk the lowres 8x8 blocks.  Returns NULL on
 * invalid arguments or allocation failure.
 * SHADER: C -- serial lookahead, host-side construction only. */
phx_lookahead_t *phx_lookahead_new(const phx_param_t *p, int mb_width, int mb_height);

/* lookahead.c:192-197 (x264_lookahead_put_frame).  Assigns a monotonically
 * increasing presentation number, resets the per-frame decision metadata and
 * calls phx_frame_init_lowres() (common/mc.c:458) so the lowres planes and
 * cost caches are ready.  The frame is borrowed.  Silently drops frames past
 * the internal queue capacity (PHX_LOOKAHEAD_MAX+3).
 * SHADER: C -- serial lookahead queue append. */
void phx_lookahead_put_frame(phx_lookahead_t *l, phx_frame_t *f);

/* lookahead.c:223-254 (x264_lookahead_get_frames) plus
 * lookahead.c:210-221 (lookahead_encoder_shift).  When no minigop is pending,
 * runs x264_slicetype_decide() on the queued frames, updates last_nonb
 * (lookahead.c:59), shifts bframes+1 frames into the output queue and returns
 * their decided types in *coded* order (after slicetype.c:1898-1910 reorder).
 * Writes at most n types and returns how many were written; 0 means no output
 * is available yet.  Call with a NULL frame_types to query without consuming.
 * SHADER: C -- serial lookahead, one minigop at a time. */
int phx_lookahead_get_frames(phx_lookahead_t *l, int *frame_types, int n);

/* lookahead.c:223-254 / slicetype.c:1892-1895 — same coded-order output as
 * phx_lookahead_get_frames(), but preserves the BREF tag on the reference B
 * frame of a pyramid minigop.  The plain API keeps its documented B/BREF -> B
 * mapping (phx_lookahead.h:46-47); the encoder needs the raw type so that
 * encoder.c:3546 can give BREF nal_ref_idc=2 and encoder.c:3882 can advance
 * frame_num.  SHADER: C — serial lookahead, one minigop at a time. */
int phx_lookahead_get_frames_ex(phx_lookahead_t *l, int *frame_types, int n);

/* lookahead.c:223-254 with frame.h:56 i_frame numbers alongside the raw
 * types.  The reorder at slicetype.c:1950-1964 moves frames out of display
 * order (the BREF goes to slot 1 while the first B goes to slot 2), so a
 * caller must not infer display order from the output slot.  frame_numbers
 * may be NULL.  SHADER: C — serial lookahead, one minigop at a time. */
int phx_lookahead_get_frames_full(phx_lookahead_t *l, int *frame_types,
                                  int *frame_numbers, int n);

/* lookahead.c:177-187 (x264_lookahead_delete).  Frees only the lookahead;
 * borrowed frames remain the caller's responsibility.
 * SHADER: C -- serial lookahead teardown. */
void phx_lookahead_delete(phx_lookahead_t *l);

/* x264 common/mc.h:235-245 -- the subset of x264_weight_t that
 * pred_weight_table() (encoder.c:295-321) writes.  i_scale/i_offset are the
 * se(v) values; i_denom is luma_log2_weight_denom (or chroma for planes 1/2).
 * b_weighted == (weightfn != NULL), i.e. whether the corresponding
 * luma/chroma_weight_l0_flag is written as 1. */
typedef struct {
    int b_weighted;
    int i_scale;
    int i_denom;
    int i_offset;
} phx_weight_t;

typedef struct {
    phx_weight_t plane[3];       /* [0]=Y, [1]=U, [2]=V */
} phx_weights_t;

/* x264 encoder/slicetype.c:284-501 -- x264_weights_analyse().  `fenc_frame`
 * and `ref_frame` are the display-order i_frame numbers (frame.h:56) used for
 * ref0_distance = fenc - ref - 1 (:79) and f_weighted_cost_delta (:463).
 * b_lookahead=1 is the lowres-only single-candidate path called from
 * slicetype_frame_cost() (:859-864); b_lookahead=0 is the encoder path called
 * from x264_slicetype_decide() (:1937-1943) and feeds weighted_pred_init()
 * (encoder.c:2202-2284) -> pred_weight_table (encoder.c:295-321).
 *
 * Only the luma plane is analysed: phx_frame_t has no per-MB i_intra_cost or
 * chroma MC state, so chroma is left unweighted (encoder.c:2281-2283 writes
 * chroma_log2_weight_denom=0), and the chroma search of slicetype.c:373-389,
 * :467-485 is an explicit TODO.  No weights are invented to force a decision.
 * Returns 0 on success, -1 on invalid arguments.
 * SHADER: C -- serial host-side analysis of one fixed frame pair. */
int phx_lookahead_weights_analyse(const phx_lookahead_t *l,
                                  phx_frame_t *fenc, int fenc_frame,
                                  phx_frame_t *ref, int ref_frame,
                                  int b_lookahead, phx_weights_t *out);

/* x264 lookahead.c:223-254 + slicetype.c:1950-1964 + encoder.c:3881-3882:
 * the decided types in *coded* order together with the exact slice-header
 * frame_num and pic_order_cnt_lsb of each frame.  frame_num starts at 0 on an
 * IDR and increments only after a reference (I/P/BREF) frame (:3882);
 * POC = 2*(i_frame - last IDR i_frame) (encoder.c:3513).  All three arrays may
 * be NULL; frame_types is required when n > 0.  Returns the number of frames
 * written, 0 when none are available.
 * SHADER: C -- serial coded-order output, one minigop at a time. */
int phx_lookahead_get_frames_coded(phx_lookahead_t *l, int *frame_types,
                                   int *frame_nums, int *pocs, int n);

#ifdef __cplusplus
}
#endif

#endif /* PHX_LOOKAHEAD_H */
