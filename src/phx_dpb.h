/* Phoenix decoded-picture buffer (DPB) — reconstructed, in-loop-deblocked
 * reference pictures for list-0/later list-1 inter prediction.
 *
 * Authoritative source (vendored x264), followed line by line:
 *   reference/x264/encoder/macroblock.c:1423-1450  deblock strength is produced
 *       from the live MB cache/state after reconstruction;
 *   reference/x264/encoder/encoder.c:2432-2470  x264_frame_deblock_row() runs
 *       per MB row, then x264_frame_expand_border() fills the reference pads;
 *   reference/x264/encoder/encoder.c:3057  the h->fref[0][0] picture handed to
 *       motion estimation is the *reconstructed + deblocked* fdec, not fenc;
 *   reference/x264/common/deblock.c:378-606  the per-MB edge loop ported as
 *       phx_frame_deblock_ex() (phx_deblock.h:159-164);
 *   reference/x264/common/frame.c:535-553/633-638  border replication of the
 *       luma/NV12 chroma planes used by a stored reference;
 *   reference/x264/common/frame.h:37-140 / frame.c:87-99/235-261  plane
 *       geometry exposed by phx_frame_t (phx_frame.h:23-49);
 *   reference/x264/common/common.h:571 (PHX_FDEC_STRIDE) and
 *       phx_encoder.c:1381-1383 (h->recon + 64 + mb_y*16*32 + mb_x*16).
 *
 * Evidence: x264's ME reference p_fref[0] starts [135,131,125,119] (post
 * reconstruction + in-loop deblock) where the raw source starts
 * [132,131,127,117]; Phoenix's phx_encoder.c:1335-1339 currently passes the
 * raw source frame instead.  This DPB owns the reconstructed picture so the
 * orchestrator can pass a byte-exact fref.
 *
 * SHADER: C — one serial push/filter of a completed picture.  The deblock
 * inner MB work is SHADER B (phx_frame_deblock_ex); the copied planes are the
 * explicit SSBO-style edge handed to that work.  No threads are introduced.
 *
 * Boundaries: progressive 4:2:0 NV12 only.  No MBAFF, no MMCO commands and no
 * long-term references yet; IDR reset and short-term overflow eviction are the
 * only DPB management operations implemented here.
 */
#ifndef PHX_DPB_H
#define PHX_DPB_H

#include <stdint.h>
#include "phx_frame.h"
#include "phx_mbcache.h"
#include "phx_deblock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PHX_DPB_MAX_PICTURES 16   /* base.h:137 X264_REF_MAX */

/* One decoded picture.  `frame` owns its planes (phx_frame_new/delete); the
 * metadata mirrors the x264_frame_t fields read by reference_build_list()
 * (encoder.c:2295-2411). */
typedef struct {
    int i_frame_num;             /* x264_frame_t.i_frame_num */
    int i_poc;                   /* x264_frame_t.i_poc */
    int i_frame;                 /* x264_frame_t.i_frame (display order) */
    int i_type;                  /* PHX_TYPE_* decided type */
    int b_kept_as_ref;           /* fdec->b_kept_as_ref (encoder.c:2470) */
    phx_frame_t *frame;          /* reconstructed + deblocked planes */
} phx_dpb_entry_t;

/* The DPB handle.  Created once per encoder, reset on IDR.  SHADER: C. */
typedef struct {
    int i_width, i_height;       /* coded (16-aligned) luma size */
    int i_mb_width, i_mb_height;
    int bframes;                 /* passed to phx_frame_new() */
    phx_dpb_entry_t pictures[PHX_DPB_MAX_PICTURES];
    int n_pictures;              /* number of live entries, oldest first */
} phx_dpb_t;

/* frame.c:60/85-87 — allocate an empty DPB for coded width/height (both must
 * be 16-pixel multiples).  `bframes` only sizes the frame's lookahead caches;
 * pass h->param.i_bframe.  Returns NULL on invalid geometry/OOM. */
phx_dpb_t *phx_dpb_create(int width, int height, int bframes);

/* frame.c:312 / encoder.c:4196 — delete every owned picture and the handle.
 * NULL-safe. */
void phx_dpb_free(phx_dpb_t *dpb);

/* Parameters for one completed coded picture.  The two reconstruction planes
 * are the encoder's live buffers: fdec_y = h->recon + 64 (FDEC_STRIDE layout,
 * phx_encoder.c:1381-1383) and fdec_uv = h->fenc->plane_chroma (NV12,
 * phx_chroma.c:193-204).  mb_state = h->mb_state after the last MB's
 * phx_macroblock_cache_save(); it supplies type/nnz/ref/mv/cbp/slice_table/
 * mb_transform_size needed to rebuild the deblock boundary cache.  qp_array
 * (size mb_width*mb_height, one byte of range per MB) is optional: NULL uses
 * the scalar `qp` for every MB.  cfg may be NULL (x264 8-bit defaults: slice
 * alpha/beta offsets and chroma QP offset all zero). */
typedef struct {
    const uint8_t *fdec_y;
    int fdec_y_stride;           /* PHX_FDEC_STRIDE (32) in normal use */
    const uint8_t *fdec_uv;      /* interleaved NV12 U/V */
    int fdec_uv_stride;
    const phx_mb_state_t *mb_state;
    const int *qp_array;
    int qp;                      /* used when qp_array == NULL */
    int b_cabac;
    const phx_deblock_cfg_t *cfg;/* NULL => zeroed x264 defaults */

    int frame_num, poc, display_frame;
    int i_type;                  /* PHX_TYPE_* */
    int b_kept_as_ref;           /* non-disposable pictures only enter list 0 */
} phx_dpb_store_params_t;

/* encoder.c:2432-2470 / deblock.c:378-606 — copy the reconstructed FDEC
 * picture into a phx_frame_t, apply x264's in-loop deblock to every MB in
 * raster (coding) order, replicate the luma/chroma border samples exactly as
 * x264_frame_expand_border() does, then push the result into the DPB.  The
 * caller's buffers are never modified, so the existing single-frame recon
 * path in phx_encoder.c stays byte-identical until the orchestrator wires
 * phx_dpb_get_ref() into phx_me16().
 *
 * Call this once per completed picture with b_kept_as_ref =
 * (fc.i_nal_ref_idc != 0); only such pictures are returned by the get_ref()
 * calls.  A picture stored with b_kept_as_ref == 0 still occupies a slot, so
 * for B-heavy streams the orchestrator should skip the call for disposable
 * pictures (x264 never puts them in the DPB).  Since deblocking here is a
 * post-frame pass, it cannot supply within-frame deblocked neighbours to
 * later MB rows — that remains a documented limitation.
 * Returns 0 on success, -1 for invalid arguments/geometry. */
int phx_dpb_store_frame(phx_dpb_t *dpb, const phx_dpb_store_params_t *p);

/* encoder.c:2295-2411 — newest kept short-term reference with the given
 * frame_num.  Returns NULL when absent.  Duplicates (weightp) carry the
 * original frame_num, so the first match is x264's fref[0][i] picture. */
const phx_frame_t *phx_dpb_get_ref(const phx_dpb_t *dpb, int frame_num);

/* Convenience list-0 accessor: ref_idx 0 is the most recently stored kept
 * reference, 1 the one before it, etc.  Mirrors h->fref[0][ref_idx] for the
 * common IPPP case (and is what phx_encoder.c:1335-1339 needs today). */
const phx_frame_t *phx_dpb_get_ref_l0(const phx_dpb_t *dpb, int ref_idx);

/* encoder.c:2563 — clear all short-term references on an IDR. */
void phx_dpb_reset(phx_dpb_t *dpb);

/* encoder.c:2548-2551 — drop the short-term reference with this POC (MMCO
 * "mark unused" primitive).  Returns 1 if one was removed, 0 otherwise. */
int phx_dpb_remove_poc(phx_dpb_t *dpb, int poc);

/* Number of live pictures (including non-reference ones). */
int phx_dpb_count(const phx_dpb_t *dpb);

#ifdef __cplusplus
}
#endif
#endif /* PHX_DPB_H */
