/* Scalar in-loop deblocking filter — 8-bit 4:2:0 progressive path.
 *
 * Authoritative source (vendored x264), followed line by line:
 *   reference/x264/common/deblock.c:32-76    alpha/beta/tc0 tables and indices
 *   reference/x264/common/deblock.c:79-135   luma inter edge filters (-C variants)
 *   reference/x264/common/deblock.c:137-181  chroma inter edge filters
 *   reference/x264/common/deblock.c:183-275  luma/chroma intra edge filters
 *   reference/x264/common/deblock.c:277-304  deblock_strength_c
 *   reference/x264/common/deblock.c:306-338  deblock_edge / deblock_edge_intra
 *   reference/x264/common/deblock.c:340-376  macroblock_cache_load_neighbours_deblock
 *   reference/x264/common/deblock.c:378-606  x264_frame_deblock_row (the per-MB
 *                                            edge loop ported here)
 *   reference/x264/common/macroblock.c:1438-1622  x264_macroblock_deblock_strength,
 *                                            which is the bs producer actually
 *                                            used by the encoder (encoder.c:3057)
 *   reference/x264/common/common.h:93-148     x264_clip3 / x264_clip_pixel
 *   reference/x264/common/macroblock.h:249-270 chroma QP table
 *   reference/x264/common/base.h:180         x264_scan8 layout (phx_scan8)
 *
 * SHADER: B — one workgroup invocation per MB, edge-parallel within the MB.
 * The alpha/beta/tc0 tables are immutable Class A data; the boundary cache
 * (`phx_mb_cache_t`) and the reconstructed planes are the explicit SSBO edge.
 * Every edge filter is seek-safe from (pix, strides, alpha, beta, tc).
 *
 * Known boundaries of this port (see also docs/SHADER_NOTES.md:209-227):
 *   - progressive frames only; MBAFF neighbour pairing (deblock.c:351-368,
 *     404-409, 449-511, 545-567) is not wired into phx_frame_deblock();
 *   - 4:2:0 NV12 only (no 4:4:4 / 4:2:2 planes, deblock.c:425-446);
 *   - no cross-slice reload of neighbour nnz/ref/mv beyond the flags computed
 *     from slice_table at deblock.c:342/370-375;
 *   - phx_mb_state_t has no per-MB qp[] array, so the plain API treats qp as
 *     constant across the MB pair.  phx_frame_deblock_ex() accepts an optional
 *     qp[] for bit-exact per-MB QP reconstruction.
 */
#ifndef PHX_DEBLOCK_H
#define PHX_DEBLOCK_H

#include <stdint.h>
#include "phx_frame.h"
#include "phx_mbcache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Immutable table accessors — deblock.c:74-76 (the `(x)+24` index).
 * SHADER: A — pure lookup into constant memory.
 */
int phx_deblock_alpha(int qp_plus_offset);   /* alpha_table(x) */
int phx_deblock_beta(int qp_plus_offset);    /* beta_table(x)  */
int phx_deblock_tc0(int qp_plus_offset, int bs); /* tc0_table(x)[bs] */

/* deblock_strength_c, deblock.c:277-304.
 * Reads the current MB's scan8 cache (nnz/ref/mv) and writes bs[2][8][4].
 * dir 0 = vertical edges (bs[0][edge][i], edge = 4x4 block row),
 * dir 1 = horizontal edges (bs[1][edge][i], edge = 4x4 block column).
 * mvy_limit is `4 >> MB_INTERLACED` (deblock.c:636); bframe is
 * `h->sh.i_type == SLICE_TYPE_B`.
 * SHADER: B — edge-parallel gather from the shared per-MB cache.
 */
void phx_deblock_strength_c(const phx_mb_cache_t *c, uint8_t bs[2][8][4],
                            int mvy_limit, int bframe);

/* x264_macroblock_deblock_strength, macroblock.c:1438-1622 (progressive
 * subset: the MBAFF helper at :1364-1436 and the slice-edge neighbour reload
 * at :1466-1538 are not needed when cache_load already saw the neighbours).
 * Includes intra bs seeding (:1441-1447, M32 at edge 1 + M64 at edge 2 which
 * also seeds edge 3), the 8x8dct early termination
 * (:1451-1464) and the CAVLC+8x8dct nnz munging (:1568-1615).
 * `b_transform_8x8_mode` is pps.b_transform_8x8_mode (macroblock.c:1569).
 * SHADER: B — derives the bs edge array from the shared MB cache/state.
 */
void phx_macroblock_deblock_strength(const phx_mb_state_t *s,
                                     const phx_mb_cache_t *c,
                                     uint8_t bs[2][8][4], int b_cabac,
                                     int disable_deblocking_filter_idc,
                                     int b_transform_8x8_mode);

/* Inter edge filters, deblock.c:79-181.  `pix` points at q0; xstride is the
 * q0/p0 separation, ystride the separation of the four samples along the
 * edge.  deblock_v_luma_c passes (stride, 1), deblock_h_luma_c (1, stride),
 * deblock_v_chroma_c (stride, 2), deblock_h_chroma_c (2, stride)
 * (deblock.c:128-135/170-177).
 * SHADER: B — edge-parallel within the MB; per-edge computation over the
 * reconstructed plane.
 */
void phx_deblock_v_luma_c(uint8_t *pix, intptr_t stride,
                          int alpha, int beta, const int8_t tc0[4]);
void phx_deblock_h_luma_c(uint8_t *pix, intptr_t stride,
                          int alpha, int beta, const int8_t tc0[4]);
void phx_deblock_v_chroma_c(uint8_t *pix, intptr_t stride,
                            int alpha, int beta, const int8_t tc0[4]);
void phx_deblock_h_chroma_c(uint8_t *pix, intptr_t stride,
                            int alpha, int beta, const int8_t tc0[4]);

/* Intra edge filters, deblock.c:183-275; same stride conventions.
 * SHADER: B — edge-parallel within the MB; per-edge computation over the
 * reconstructed plane.
 */
void phx_deblock_v_luma_intra_c(uint8_t *pix, intptr_t stride,
                                int alpha, int beta);
void phx_deblock_h_luma_intra_c(uint8_t *pix, intptr_t stride,
                                int alpha, int beta);
void phx_deblock_v_chroma_intra_c(uint8_t *pix, intptr_t stride,
                                  int alpha, int beta);
void phx_deblock_h_chroma_intra_c(uint8_t *pix, intptr_t stride,
                                  int alpha, int beta);

/* MBAFF edge entry points, deblock.c:123-127/166-169/227-231/260-263.
 * Provided for formula completeness; phx_frame_deblock() is progressive only.
 * SHADER: B — edge-parallel within the MB; per-edge computation.
 */
void phx_deblock_h_luma_mbaff_c(uint8_t *pix, intptr_t stride,
                                int alpha, int beta, const int8_t tc0[4]);
void phx_deblock_h_chroma_mbaff_c(uint8_t *pix, intptr_t stride,
                                  int alpha, int beta, const int8_t tc0[4]);
void phx_deblock_h_luma_intra_mbaff_c(uint8_t *pix, intptr_t ystride,
                                      int alpha, int beta);
void phx_deblock_h_chroma_intra_mbaff_c(uint8_t *pix, intptr_t stride,
                                        int alpha, int beta);

/* One filtered edge, deblock.c:306-338 (inter and intra variants combined by
 * the `intra` flag).  `dir` selects the x264 filter function: 0 = h_luma /
 * h_chroma (vertical boundary), 1 = v_luma / v_chroma (horizontal boundary),
 * as in h->loopf.deblock_luma[dir] (deblock.c:424/559).  b_chroma is the 1
 * added to tc for 4:2:0 chroma (deblock.c:318-321/437-438).  a/b are
 * sh.i_alpha_c0_offset / beta_offset minus QP_BD_OFFSET (zero for 8-bit,
 * deblock.c:381-382).
 * SHADER: B — one edge of the workgroup, edge-parallel within the MB.
 */
void phx_deblock_edge(uint8_t *pix, intptr_t stride, const uint8_t bs[4],
                      int qp, int a, int b, int b_chroma, int intra, int dir);

/* Optional full-state configuration.  All zeroes reproduce the plain API and
 * x264's default slice-header offsets.
 */
typedef struct {
    int alpha_c0_offset;              /* sh.i_alpha_c0_offset, deblock.c:381 */
    int beta_offset;                  /* sh.i_beta_offset, deblock.c:382 */
    int chroma_qp_index_offset;       /* pps offset, deblock.c:383 */
    int disable_deblocking_filter_idc;/* sh flag, deblock.c:342/383 */
    int b_transform_8x8_mode;         /* pps.b_transform_8x8_mode; 0 => derive
                                       * from any set mb_transform_size[] */
    const int *qp_array;              /* per-MB QP; NULL => uniform `qp` */
} phx_deblock_cfg_t;

/* Stable API — the per-MB body of x264_frame_deblock_row, deblock.c:378-606.
 * Filters the left edge, the three internal vertical edges, the top edge and
 * the three internal horizontal edges of MB (mb_x, mb_y) in x264's order.
 * `c` is the current MB cache (already loaded by phx_macroblock_cache_load);
 * `s` carries type/nnz/cbp/ref/mv/slice_table/field neighbours.
 * Returns 0 on success (including "no filtering below qp threshold" and
 * "filter disabled"), -1 for invalid arguments/geometry.
 * SHADER: B — one MB workgroup, edges inside it are independent.
 */
int phx_frame_deblock(phx_frame_t *f, phx_mb_state_t *s, phx_mb_cache_t *c,
                      int mb_x, int mb_y, int qp, int b_cabac);

/* Same, with explicit slice-header offsets and optional per-MB QP array.
 * SHADER: B — one MB workgroup with explicit parameters.
 */
int phx_frame_deblock_ex(phx_frame_t *f, phx_mb_state_t *s, phx_mb_cache_t *c,
                         int mb_x, int mb_y, int qp, int b_cabac,
                         const phx_deblock_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
#endif /* PHX_DEBLOCK_H */
