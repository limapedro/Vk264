/* Phase-B motion compensation primitives (8-bit 4:2:0, progressive frames).
 *
 * Function-for-function port of the scalar C paths in the vendored x264:
 *   reference/x264/common/mc.c:49-61    pixel_avg / pixel_avg_wxh
 *   reference/x264/common/mc.c:173-196  TAPFILTER + hpel_filter
 *   reference/x264/common/mc.c:162-171  mc_copy
 *   reference/x264/common/mc.c:198-219  mc_luma
 *   reference/x264/common/mc.c:252-283  mc_chroma (8th-pixel bilinear)
 *   reference/x264/common/mc.c:328-337  x264_plane_copy_deinterleave_c
 *   reference/x264/common/mc.c:407-410  load_deinterleave_chroma_fdec
 *   reference/x264/common/mc.c:704-726  x264_frame_filter (full-frame HPEL)
 *   reference/x264/common/tables.c:183-184  x264_hpel_ref0 / x264_hpel_ref1
 *   reference/x264/common/common.h:145-148 x264_clip_pixel (BIT_DEPTH == 8)
 *   reference/x264/encoder/macroblock.c:669-681  P_SKIP chroma MC + mv0 shortcut
 *   reference/x264/common/macroblock.c:61-64    inter 16x16 chroma MC call site
 *   reference/x264/common/macroblock.c:568-640  p_fref plane/offset construction
 *
 * All source planes are the padded x264 frame planes: plane[x + y*stride]
 * with (0,0) at the first coded luma (or chroma) sample.  The caller supplies
 * negative-x/negative-y padding of at least 8 samples; no clamping is done
 * inside the filters, exactly like x264 (common/mc.c:180/704-726).
 *
 * SHADER: B — one serial invocation per macroblock; every function here is
 * deterministic and seek-safe, with no hidden temporal state except the
 * optional read-only HPEL plane cache documented on phx_mc_luma_mb().
 */
#ifndef PHX_MC_H
#define PHX_MC_H

#include <stdint.h>
#include "phx_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* x264_hpel_ref0 / x264_hpel_ref1, tables.c:183-184.  Index is
 * ((mvy&3)<<2) + (mvx&3); the values select the fullpel/H/V/C plane. */
extern const uint8_t phx_mc_hpel_ref0[16];
extern const uint8_t phx_mc_hpel_ref1[16];

/* mc.c:173-196.  Fills the H (x/2), V (y/2) and C (x/2,y/2) halfpel planes
 * from src.  src, dsth, dstv and dstc are all at the coded-plane origin with
 * the same stride; the six-tap window reaches src[-4 .. width+5] and
 * src[-4*stride .. (height+4)*stride].  buf must hold width+5 int16_t entries.
 * SHADER: B — local 6-tap filtering, no global state. */
void phx_mc_hpel_filter(uint8_t *dsth, uint8_t *dstv, uint8_t *dstc,
                        const uint8_t *src, int stride,
                        int width, int height, int16_t *buf);

/* mc.c:704-726 (progressive, b_end == 1, no interlace): the -8 row/column
 * offset used by x264_frame_filter before calling hpel_filter with
 * width+16 / height+16.  dst* are at the coded-plane origin and need at least
 * stride*(height+16)+16 addressable bytes.  SHADER: B — full-frame prepass. */
void phx_mc_hpel_filter_frame(const uint8_t *plane, int stride,
                              int width, int height,
                              uint8_t *dsth, uint8_t *dstv, uint8_t *dstc,
                              int16_t *buf);

/* mc.c:63-75 (pixel_avg_wxh): (src1[x] + src2[x] + 1) >> 1 per sample.
 * SHADER: B — fractional-luma average. */
void phx_mc_pixel_avg(uint8_t *dst, int i_dst,
                      const uint8_t *src1, int i_src1,
                      const uint8_t *src2, int i_src2,
                      int width, int height);

/* mc.c:162-171: row-wise memcpy.  SHADER: B — fullpel luma copy. */
void phx_mc_copy(const uint8_t *src, int i_src_stride,
                 uint8_t *dst, int i_dst_stride, int width, int height);

/* mc.c:198-219 with x264_weight_none (no mc_weight branch: Phoenix does not
 * apply weighted prediction to MB reconstruction).  src[0..3] are the
 * fullpel/H/V/C planes at the block origin; mvx/mvy are in 1/4-pel units.
 * SHADER: B — one 16x16 luma MC block. */
void phx_mc_luma(uint8_t *dst, int i_dst_stride,
                 const uint8_t *src[4], int i_src_stride,
                 int mvx, int mvy, int width, int height);

/* mc.c:252-283: 8th-pel bilinear chroma MC.  src is the NV12 interleaved
 * plane (U at even bytes, V at odd bytes, frame.c:45-57); dstu/dstv are
 * planar U/V buffers.  For 4:2:0 pass mvx and (mvy >> 1) — the
 * 2*mvy>>CHROMA_V_SHIFT scaling of common/macroblock.c:64.
 * SHADER: B — one 8x8 chroma MC block. */
void phx_mc_chroma(uint8_t *dstu, int i_dstu, uint8_t *dstv, int i_dstv,
                   const uint8_t *src, int i_src_stride,
                   int mvx, int mvy, int width, int height);

/* mc.c:328-337 + :407-410 (load_deinterleave_chroma_fdec): the mv0 shortcut
 * of encoder/macroblock.c:674-681.  SHADER: B — planar U/V copy. */
void phx_mc_deinterleave_chroma(uint8_t *dstu, int i_dstu,
                                uint8_t *dstv, int i_dstv,
                                const uint8_t *src, int i_src_stride,
                                int width, int height);

/* Convenience wrapper used by phx_mbencode.c: prepares (and caches) the
 * full-frame H/V/C planes of fref, points src[0..3] at the current MB origin
 * (common/macroblock.c:568-640) and calls phx_mc_luma().  mvx/mvy must
 * already be frame-edge clipped by the caller (analyse.c:336-337).
 * Returns 0 on success, -1 for an invalid argument/allocation failure.
 *
 * The cache is a single entry keyed by (fref pointer, stride, width, height)
 * and is rebuilt on a key change; call phx_mc_hpel_invalidate() when the
 * contents of a frame are modified in place.  It is deliberately not
 * thread-safe — the B stage is serial per MB.
 * SHADER: B — cached read-only reference-frame preparation. */
int phx_mc_luma_mb(const phx_frame_t *fref, uint8_t *dst, int i_dst_stride,
                   int mb_x, int mb_y, int mvx, int mvy,
                   int width, int height);
void phx_mc_hpel_invalidate(void);

#ifdef __cplusplus
}
#endif
#endif
