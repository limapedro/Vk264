/* Phoenix lowres slice-type frame/path costs.
 *
 * Function-for-function port of the serial, C-only x264 lowres path:
 *   reference/x264/encoder/slicetype.c:45-61    lowres_context_init()
 *   reference/x264/encoder/slicetype.c:514-791  slicetype_mb_cost()
 *   reference/x264/encoder/slicetype.c:814-834  slicetype_slice_cost()
 *   reference/x264/encoder/slicetype.c:836-995  slicetype_frame_cost()
 *   reference/x264/encoder/slicetype.c:983-987  B discount 100/(120+bias)
 *   reference/x264/encoder/slicetype.c:1288-1327 slicetype_path_cost()
 *   reference/x264/encoder/me.c:38-56           subpel_iterations / hex2 / square1
 *   reference/x264/encoder/me.c:60-107          BITS_MVD / COST_MV[_X3/_X4_DIR]
 *   reference/x264/encoder/me.c:178-180         FPEL / SPEL
 *   reference/x264/encoder/me.c:182-419         x264_me_search_ref() fullpel
 *   reference/x264/encoder/me.c:774-796         qpel MV / refine_subpel dispatch
 *   reference/x264/encoder/me.c:865-963         refine_subpel() hpel/qpel
 *   reference/x264/common/mc.c:221-249          get_ref() (hpel + qpel avg)
 *   reference/x264/common/mc.c:63-75            pixel_avg_wxh / weighted avg
 *   reference/x264/common/tables.c:183-184      x264_hpel_ref0 / ref1
 *   reference/x264/common/pixel.c:49-71         SAD_8x8_C
 *   reference/x264/common/pixel.c:265-332       SATD_8x8_C
 *   reference/x264/common/frame.h:103-127       MV / cost / intra caches
 *
 * phx_frame_t only exposes ONE lowres_mvs[2][bframes+1][2] slot per list and
 * distance (phx_frame.h:35), whereas x264 caches one MV per 8x8 block
 * (frame.h:103).  The context allocated here owns the per-MB mirrors of those
 * caches and the `b_intra_calculated` flag (frame.h:126); the three values in
 * phx_frame_t are kept in sync as a public, read-only view.  Nothing is copied
 * from a reference stream and no cost is faked to force B frames.
 *
 * SHADER: A -- every per-8x8-block SAD/SATD/predictor/MC invocation is pure,
 *              deterministic and seek-safe.  One work item per lowres block.
 *          C -- cost accumulation, MV cache reuse and the path prefix sum are
 *              serial host-side state (one minigop at a time).
 */
#ifndef PHX_LOWRES_H
#define PHX_LOWRES_H

#include <stdint.h>
#include "phx_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct phx_lowres phx_lowres_t;

/* slicetype.c:45-61 + analyse.c:179/248.  `mv_range` is x264's
 * analyse.i_mv_range (full-pel lowres limit, base.h default 16); `bframes` is
 * param.i_bframe; `b_pyramid` follows base.c:391 (normal == 2, non-zero here);
 * `bframe_bias` is base.c:390 (0).  The lookahead QP cost table is built here.
 * Returns NULL on invalid arguments or OOM.  SHADER: C -- construction. */
phx_lowres_t *phx_lowres_new(int mb_width, int mb_height, int mv_range,
                             int bframes, int b_pyramid, int bframe_bias);
void phx_lowres_delete(phx_lowres_t *l);             /* SHADER: C -- teardown */

/* mc.c:473-481 — clear the derived caches when a frame is re-queued.  Called
 * by phx_lookahead_put_frame() right after phx_frame_init_lowres().
 * SHADER: C -- serial cache invalidation. */
void phx_lowres_frame_reset(phx_lowres_t *l, phx_frame_t *f);

/* slicetype.c:823 -- x264 scores the edge MBs when MB-tree or VBV is active.
 * phx_param_t has b_mb_tree but no VBV buffer size; call this with
 * p->b_mb_tree (default 1).  SHADER: C -- serial flag, no hidden state. */
void phx_lowres_set_mbtree(phx_lowres_t *l, int b_mb_tree);

/* slicetype.c:836-995.  `frames` is a pointer array indexed exactly like
 * x264's: p0 <= b <= p1, p0 == p1 is the intra pass.  Returns i_score after
 * the B discount (slicetype.c:983-987), also cached in f->i_cost_est[][].
 * SHADER: A/C -- pure per-block kernels with serial accumulation. */
int phx_lowres_frame_cost(phx_lowres_t *l, phx_frame_t **frames,
                          int p0, int p1, int b);

/* slicetype.c:1288-1327.  `path` is 1-indexed from frames+1; the B-pyramid
 * branch (:1310-1318) is taken when the context was created with b_pyramid.
 * SHADER: C -- serial prefix sum with early threshold termination. */
uint64_t phx_lowres_path_cost(phx_lowres_t *l, phx_frame_t **frames,
                              const char *path, uint64_t threshold);

/* Convenience entry point with the standalone signature: derives mb_width /
 * mb_height from frames[b], uses the medium defaults (mv_range 16, b_pyramid
 * normal, bias 0) and caches in a lazily created process-wide context.
 * Prefer phx_lowres_new() + phx_lowres_frame_cost() for multiple lookahead
 * instances.  SHADER: A/C — as phx_lowres_frame_cost(). */
int phx_slicetype_frame_cost(phx_frame_t **frames, int p0, int p1, int b,
                             int b_pyramid);

#ifdef __cplusplus
}
#endif

#endif /* PHX_LOWRES_H */
