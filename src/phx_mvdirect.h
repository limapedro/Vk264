/* Byte-exact B-frame direct-mode MV prediction and B_SKIP probe.
 *
 * This is a function-for-function port of the progressive (non-MBAFF) paths of
 *   reference/x264/common/mvpred.c:183-287   temporal direct,
 *   reference/x264/common/mvpred.c:289-452   spatial direct,
 *   reference/x264/common/mvpred.c:454-516   x264_mb_predict_mv_direct16x16,
 *   reference/x264/common/macroblock.c:1883-1925 x264_macroblock_bipred_init,
 *   reference/x264/common/macroblock.c:443-455  map_col_to_list0,
 *   reference/x264/encoder/analyse.c:3334-3359 B_SKIP gate,
 *   reference/x264/encoder/macroblock.h:38-41 x264_macroblock_probe_bskip,
 *   reference/x264/encoder/macroblock.c:988-970 probe_skip + B_SKIP promotion,
 * with the scan8 cache layout of reference/x264/common/base.h:180 and
 * phx_scan8 (src/phx_mbcache.c:12).  Intermediate values, shifts, rounding and
 * early-out conditions are copied verbatim — no approximation.
 *
 * The two entry points are pure: every input is passed explicitly and every
 * output is written to a caller-owned struct, so the analysis/encoder stages
 * can call them without any x264_t state (SHADER: B — one workgroup per MB).
 */
#ifndef PHX_MVDIR_H
#define PHX_MVDIR_H

#include <stdint.h>
#include "phx_mbcache.h"
#include "phx_frame.h"

/* x264 param.analyse.i_direct_mv_pred (x264.h:197-200, base.c:438/561). */
#define PHX_MVDIR_NONE     0
#define PHX_MVDIR_SPATIAL  1
#define PHX_MVDIR_TEMPORAL 2
#define PHX_MVDIR_AUTO     3

/* x264 partition enum (common/macroblock.h:136-139). */
#define PHX_MVDIR_D_8x8    13
#define PHX_MVDIR_D_16x8   14
#define PHX_MVDIR_D_8x16   15
#define PHX_MVDIR_D_16x16  16

#ifdef __cplusplus
extern "C" {
#endif

/* The fields of x264_frame_t fref[1][0] (the colocated frame) read by
 * mvpred.c:188-441.  Arrays use x264's frame-level strides so the indices
 * i_mb_8x8 / i_mb_4x4 of mvpred.c:230-231/:258/:265/:373-379 are reproduced
 * exactly.  `field` may be NULL for progressive frames (treated as all 0).
 * SHADER: B — read-only frame data. */
typedef struct {
    const uint8_t *mb_type;        /* frame [mb_count]      :188/:340 */
    const uint8_t *mb_partition;   /* frame [mb_count]      :189/:341 */
    const int8_t  *ref[2];         /* frame [b8_stride*rows]:259,376-377 */
    const int16_t (*mv[2])[2];     /* frame [b4_stride*rows]:265,378-379 */
    const uint8_t *field;          /* frame [mb_count]      :195/:343 (MBAFF) */
    int i_poc;                     /* frame.h:i_poc         :218/:364 */
    int i_delta_poc[2];            /* frame.h:i_delta_poc   :218-219/:364-365 */
    int i_mb_stride;               /* h->mb.i_mb_stride     :230-231 */
    int i_b8_stride;               /* h->mb.i_b8_stride     :419 */
    int i_b4_stride;               /* h->mb.i_b4_stride     :420 */
} phx_mvdirect_col_t;

typedef struct phx_mvdirect_result_s phx_mvdirect_result_t;

/* All encoder state the direct predictor reads (x264_t subset).
 * SHADER: B — explicit inputs, no hidden state. */
typedef struct {
    const phx_mb_cache_t *cache;   /* h->mb.cache ref/mv (scan8 layout) */
    int mb_x, mb_y, mb_xy;         /* h->mb.i_mb_x/y/xy */
    int i_b8_xy, i_b4_xy;          /* h->mb.cache.i_b8_xy/i_b4_xy (:374-375) */

    int direct_mode;               /* h->param.analyse.i_direct_mv_pred */
    int b_direct_spatial;          /* h->sh.b_direct_spatial_mv_pred (:459) */
    int b_interlaced;              /* SLICE_MBAFF / MB_INTERLACED.  Only 0 is
                                    * supported; see limitation in .c. */
    int i_threads;                 /* h->param.i_threads (:269/:388) */
    int mv_max_spel_y;             /* h->mb.mv_max_spel[1] (only used when
                                    * i_threads > 1, :269/:388) */

    const phx_mvdirect_col_t *col; /* h->fref[1][0] */
    int cur_poc;                   /* h->fdec->i_poc (:217/:363) */

    /* x264 h->mb.dist_scale_factor[i_ref][0] from macroblock.c:1896-1908.
     * Offset to list-0 ref i_ref; NULL means use a flat table of 256. */
    const int16_t (*dist_scale_factor)[4];
    /* x264 h->mb.map_col_to_list0 with the +2 macro offset already applied
     * (common.h:674), i.e. indexed as map[col].  NULL treats col 0 -> ref 0,
     * negative col -> -1/-2. */
    const int8_t *map_col_to_list0;

    /* x264's cache.direct_* before this call.  Only read for the b_changed
     * computation of mvpred.c:469-498 (direct=auto); NULL means all zero. */
    const phx_mvdirect_result_t *prev;
} phx_mvdirect_input_t;

/* Result of one direct prediction.  ref/mv are stored in raster 4x4-block
 * order k=0..15, matching phx_mb_decision_t (cache position = phx_scan8[k]).
 * direct_ref/direct_mv mirror x264's cache.direct_* (mvpred.c:500-513).
 * SHADER: B — caller-owned output. */
typedef struct phx_mvdirect_result_s {
    int available;                 /* return value of x264_mb_predict_mv_direct16x16 */
    int i_partition;               /* h->mb.i_partition after prediction */
    int8_t  ref[2][16];            /* per 4x4 block, x264 ref index (=-1 unused) */
    int16_t mv[2][16][2];          /* per 4x4 block, qpel MVs */
    int8_t  direct_ref[2][4];      /* cache.direct_ref[l][i8 0..3] */
    int16_t direct_mv[2][4][2];    /* cache.direct_mv[l][i8 0..3] */
    int changed;                   /* b_changed semantics (mvpred.c:469-498) */
} phx_mvdirect_result_t;

/* x264 common/mvpred.c:183-516 — predict direct MVs and refs for the current
 * MB.  Returns 0 on success, -1 on invalid arguments / unsupported MBAFF.
 * `out->available` is x264's return value (1 = prediction usable).
 * SHADER: B — reads neighbour cache + colocated frame, writes one MB result. */
int phx_mvdirect_predict(const phx_mvdirect_input_t *in,
                         phx_mvdirect_result_t *out);

/* x264 encoder/macroblock.c:988-1127 (b_bidir=1, 8-bit 4:2:0) plus the B_SKIP
 * gate at encoder/analyse.c:3334-3359 / encoder/macroblock.c:966-970.  Performs
 * bidirectional MC with `direct`'s MVs (default weight 32 -> (a+b+1)>>1,
 * mc.c:49-75), then the exact DCT/quant/decimate and chroma-SSD early-outs.
 * Returns 1 when x264 emits B_SKIP, 0 otherwise; -1 on invalid arguments.
 * `bipred_weight` is x264 h->bipred_weight[ref0][ref1] (32 when no weighted
 * bipred); `qp`/`chroma_qp` are h->mb.i_qp / i_chroma_qp.
 * SHADER: B — deterministic workgroup probe, no entropy state. */
int phx_mvdirect_probe_bskip(const phx_frame_t *fenc,
                             const phx_frame_t *fref0,
                             const phx_frame_t *fref1,
                             const phx_mvdirect_result_t *direct,
                             int mb_x, int mb_y, int qp, int chroma_qp,
                             int bipred_weight);

#ifdef __cplusplus
}
#endif
#endif
