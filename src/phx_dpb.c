/* Decoded-picture buffer for Phoenix.  See phx_dpb.h for the x264 source
 * line citations and the SHADER ownership note.
 *
 * The picture copy / deblock / border sequence below is the host-side mirror
 * of x264's encoder.c:2432-2470: reconstruct into fdec (macroblock.c:632),
 * x264_frame_deblock_row() per MB row (deblock.c:378-606), then
 * x264_frame_expand_border() (frame.c:560-600).  Only the resulting
 * phx_frame_t planes are exposed to ME.
 */
#include "phx_dpb.h"
#include "phx_tables.h"          /* PHX_QP_MAX (deblock.c:615) */

#include <stdlib.h>
#include <string.h>

/* base.h:180 x264_scan8 block starts whose four nnz bytes are stored in
 * h->mb.non_zero_count[mb] at macroblock.c:1761-1768 (nz_idx/nz_dst). */
static const int phx_dpb_nz_idx[8]  = { 0, 2, 8, 10, 16, 18, 32, 34 };
static const int phx_dpb_nz_dst[8]  = { 0, 4, 8, 12, 16, 20, 32, 36 };

phx_dpb_t *phx_dpb_create(int width, int height, int bframes)
{
    if (width <= 0 || height <= 0 || width % 16 || height % 16) return NULL;
    phx_dpb_t *dpb = (phx_dpb_t *)calloc(1, sizeof(*dpb));
    if (!dpb) return NULL;
    dpb->i_width = width;
    dpb->i_height = height;
    dpb->i_mb_width = width / 16;
    dpb->i_mb_height = height / 16;
    dpb->bframes = bframes;
    return dpb;
}

void phx_dpb_free(phx_dpb_t *dpb)
{
    if (!dpb) return;
    for (int i = 0; i < dpb->n_pictures; i++)
        phx_frame_delete(dpb->pictures[i].frame);    /* frame.c:312 */
    memset(dpb->pictures, 0, sizeof(dpb->pictures));
    dpb->n_pictures = 0;
    free(dpb);
}

void phx_dpb_reset(phx_dpb_t *dpb)
{
    if (!dpb) return;
    for (int i = 0; i < dpb->n_pictures; i++)
        phx_frame_delete(dpb->pictures[i].frame);
    memset(dpb->pictures, 0, sizeof(dpb->pictures));
    dpb->n_pictures = 0;
}

int phx_dpb_remove_poc(phx_dpb_t *dpb, int poc)
{
    if (!dpb) return 0;
    for (int i = 0; i < dpb->n_pictures; i++)
        if (dpb->pictures[i].i_poc == poc) {
            phx_frame_delete(dpb->pictures[i].frame);
            memmove(&dpb->pictures[i], &dpb->pictures[i+1],
                    (size_t)(dpb->n_pictures - i - 1) * sizeof(dpb->pictures[0]));
            memset(&dpb->pictures[--dpb->n_pictures], 0, sizeof(dpb->pictures[0]));
            return 1;
        }
    return 0;
}

int phx_dpb_count(const phx_dpb_t *dpb)
{
    return dpb ? dpb->n_pictures : 0;
}

/* macroblock.c:1761-1768 + :1785-1806 in reverse.  phx_macroblock_cache_load()
 * fills the neighbour slots of c from s; the current MB's nnz/ref/mv were
 * written to the per-MB arrays by phx_macroblock_cache_save(), so rebuild the
 * current MB's scan8 slots from those arrays before deriving bs.  This is the
 * exact data x264's x264_frame_deblock_row() reads through
 * macroblock_cache_load_neighbours_deblock() (deblock.c:340-376).
 * SHADER: C — one MB's shared-cache rebuild; no hidden state. */
static void phx_dpb_rebuild_mb_cache(const phx_mb_state_t *s, phx_mb_cache_t *c,
                                     int mb_x, int mb_y)
{
    const int xy = mb_y*s->mb_stride + mb_x;
    memset(c, 0, sizeof(*c));
    phx_macroblock_cache_load((phx_mb_state_t *)s, c, mb_x, mb_y, 0);
    c->i_mb_xy = xy;                                  /* macroblock.c:681 */
    c->b_transform_8x8 = (int)s->mb_transform_size[xy]; /* :1779 */
    c->i_cbp_luma = (int)(s->cbp[xy] & 0xf);          /* :1782 */

    for (int k = 0; k < 8; k++) {
        const uint8_t *src = &s->non_zero_count[xy][phx_dpb_nz_dst[k]];
        uint8_t *dst = &c->non_zero_count[phx_scan8[phx_dpb_nz_idx[k]]];
        memcpy(dst, src, 4);                          /* :1761-1768 */
    }
    for (int l = 0; l < 2; l++) {
        for (int i = 0; i < 16; i++) {
            /* Four 4x4 blocks share one 8x8 ref (macroblock.c:1785-1790). */
            c->ref[l][phx_scan8[i]] = s->ref[l][xy*4 + (i >> 2)];
            c->mv[l][phx_scan8[i]][0] = s->mv[l][xy*16 + i][0];
            c->mv[l][phx_scan8[i]][1] = s->mv[l][xy*16 + i][1]; /* :1791-1794 */
        }
    }
}

/* frame.c:535-553 for the full-frame (b_start/b_end) case.  phx_frame_new()
 * already reserves PHX_PADH/PADV on every plane. */
static void phx_dpb_expand_border(phx_frame_t *f)
{
    phx_plane_expand_border(f->plane, f->i_stride,
                            f->i_width, f->i_height,
                            PHX_PADH, PHX_PADV, 1, 1, 0);   /* frame.c:630 */
    phx_frame_expand_border_chroma(f);                      /* frame.c:637 */
}

int phx_dpb_store_frame(phx_dpb_t *dpb, const phx_dpb_store_params_t *p)
{
    if (!dpb || !p || !p->mb_state || !p->fdec_y || !p->fdec_uv) return -1;
    if (p->fdec_y_stride < dpb->i_width || p->fdec_uv_stride < dpb->i_width)
        return -1;
    const phx_mb_state_t *s = p->mb_state;
    if (s->mb_width != dpb->i_mb_width || s->mb_height != dpb->i_mb_height ||
        s->mb_stride != dpb->i_mb_width || !s->type || !s->non_zero_count ||
        !s->cbp || !s->mb_transform_size || !s->slice_table ||
        !s->ref[0] || !s->mv[0] || !s->field)
        return -1;
    if (p->qp < 0 || p->qp > PHX_QP_MAX) return -1;
    if (p->qp_array)
        for (int i = 0; i < s->mb_count; i++)
            if (p->qp_array[i] < 0 || p->qp_array[i] > PHX_QP_MAX) return -1;

    phx_frame_t *f = phx_frame_new(dpb->i_width, dpb->i_height, dpb->bframes);
    if (!f) return -1;

    /* macroblock.c:632/636-637 — copy the completed FDEC reconstruction into
     * the owned frame.  The source buffers are read-only; the single-frame
     * recon path in phx_encoder.c is left untouched. */
    for (int y = 0; y < dpb->i_height; y++)
        memcpy(f->plane + (size_t)y*f->i_stride,
               p->fdec_y + (size_t)y*p->fdec_y_stride,
               (size_t)dpb->i_width);
    for (int y = 0; y < dpb->i_height/2; y++)
        memcpy(f->plane_chroma + (size_t)y*f->i_stride_planes[1],
               p->fdec_uv + (size_t)y*p->fdec_uv_stride,
               (size_t)dpb->i_width);                   /* NV12 row bytes */

    phx_deblock_cfg_t cfg;
    if (p->cfg) cfg = *p->cfg;
    else memset(&cfg, 0, sizeof(cfg));                 /* 8-bit slice defaults */

    phx_mb_cache_t c;
    for (int mb_y = 0; mb_y < dpb->i_mb_height; mb_y++) {
        /* encoder.c:2456-2458 — deblock each MB row, left-to-right, exactly
         * where x264 filters it.  All source MBs are already reconstructed,
         * so the top-edge neighbours are the already-filtered row above. */
        for (int mb_x = 0; mb_x < dpb->i_mb_width; mb_x++) {
            int xy = mb_y*dpb->i_mb_width + mb_x;
            int qp = p->qp_array ? p->qp_array[xy] : p->qp;
            phx_dpb_rebuild_mb_cache(s, &c, mb_x, mb_y);
            int rc = phx_frame_deblock_ex(f, (phx_mb_state_t *)s, &c,
                                          mb_x, mb_y, qp, p->b_cabac, &cfg);
            if (rc < 0) { phx_frame_delete(f); return -1; }
        }
    }
    phx_dpb_expand_border(f);                           /* encoder.c:2470 */

    if (dpb->n_pictures == PHX_DPB_MAX_PICTURES) {
        /* encoder.c:2555-2556 — oldest short-term picture is evicted when the
         * DPB is full (MMCO/overflow handling remains a documented TODO). */
        phx_frame_delete(dpb->pictures[0].frame);
        memmove(&dpb->pictures[0], &dpb->pictures[1],
                (size_t)(PHX_DPB_MAX_PICTURES - 1) * sizeof(dpb->pictures[0]));
        dpb->n_pictures--;
    }
    int slot = dpb->n_pictures++;
    dpb->pictures[slot].i_frame_num = p->frame_num;
    dpb->pictures[slot].i_poc = p->poc;
    dpb->pictures[slot].i_frame = p->display_frame;
    dpb->pictures[slot].i_type = p->i_type;
    dpb->pictures[slot].b_kept_as_ref = p->b_kept_as_ref;
    dpb->pictures[slot].frame = f;
    return 0;
}

const phx_frame_t *phx_dpb_get_ref(const phx_dpb_t *dpb, int frame_num)
{
    if (!dpb) return NULL;
    for (int i = 0; i < dpb->n_pictures; i++)            /* encoder.c:2372 */
        if (dpb->pictures[i].b_kept_as_ref &&
            dpb->pictures[i].i_frame_num == frame_num)
            return dpb->pictures[i].frame;
    return NULL;
}

const phx_frame_t *phx_dpb_get_ref_l0(const phx_dpb_t *dpb, int ref_idx)
{
    if (!dpb || ref_idx < 0) return NULL;
    for (int i = dpb->n_pictures - 1, n = 0; i >= 0; i--)
        if (dpb->pictures[i].b_kept_as_ref && n++ == ref_idx)
            return dpb->pictures[i].frame;
    return NULL;
}

const phx_frame_t *phx_dpb_get_ref_l1(const phx_dpb_t *dpb, int frame_num)
{
    return phx_dpb_get_ref(dpb, frame_num);  /* same short-term lookup, :124 */
}

/* encoder.c:2282-2288 — reference_distance(): display-order distance from
 * the current picture (field-packing case 5 is out of scope, no MBAFF). */
static int phx_dpb_distance(int current_frame, const phx_dpb_entry_t *e)
{
    int d = current_frame - e->i_frame;
    return d < 0 ? -d : d;
}

/* encoder.c:2343-2358 — the stable bubble sort by reference_distance. */
static void phx_dpb_sort_list(phx_dpb_ref_lists_t *out, int list,
                              int current_frame)
{
    int n = out->i_ref[list];
    int b_ok;
    do {
        b_ok = 1;
        for (int i = 0; i < n - 1; i++) {
            int da = phx_dpb_distance(current_frame,
                                      &out->frame_entry[list][i]);
            int db = phx_dpb_distance(current_frame,
                                      &out->frame_entry[list][i+1]);
            if (da > db) {
                phx_dpb_entry_t t = out->frame_entry[list][i];
                out->frame_entry[list][i] = out->frame_entry[list][i+1];
                out->frame_entry[list][i+1] = t;
                b_ok = 0;
            }
        }
    } while (!b_ok);
}

/* encoder.c:2348-2351 — nearest past/future picture.  The source starts with
 * fref[list][0] and considers indices 1..n-1, so an empty list leaves NULL. */
static void phx_dpb_set_nearest(phx_dpb_ref_lists_t *out, int list)
{
    int n = out->i_ref[list];
    out->nearest_index[list] = 0;
    for (int i = 0; i < n - 1; i++)
        if (list ? out->frame_entry[list][i+1].i_poc
                   < out->frame_entry[list][out->nearest_index[list]].i_poc
                 : out->frame_entry[list][i+1].i_poc
                   > out->frame_entry[list][out->nearest_index[list]].i_poc)
            out->nearest_index[list] = i + 1;
}

/* encoder.c:2137-2154 — standard default-order check (P: descending
 * frame_num; B L0: descending POC; B L1: ascending POC). */
static void phx_dpb_check_reorder(phx_dpb_ref_lists_t *out)
{
    for (int list = 0; list <= (out->slice_type == PHX_SLICE_B); list++)
        for (int i = 0; i < out->i_ref[list] - 1; i++) {
            const phx_dpb_entry_t *a = &out->frame_entry[list][i];
            const phx_dpb_entry_t *b = &out->frame_entry[list][i+1];
            int dfn = b->i_frame_num - a->i_frame_num;
            int dpoc = b->i_poc - a->i_poc;
            if (out->slice_type == PHX_SLICE_P ? dfn > 0
                : list == 1 ? dpoc < 0 : dpoc > 0) {
                out->b_ref_reorder[list] = 1;
                return;
            }
        }
}

int phx_dpb_init_ref_lists(const phx_dpb_t *dpb, phx_dpb_ref_lists_t *out,
                           int current_poc, int current_frame, int slice_type,
                           const phx_dpb_list_cfg_t *cfg)
{
    if (!dpb || !out) return -1;
    if (slice_type != PHX_SLICE_I && slice_type != PHX_SLICE_P &&
        slice_type != PHX_SLICE_B) return -1;
    memset(out, 0, sizeof(*out));
    out->i_current_poc = current_poc;
    out->i_current_frame = current_frame;
    out->slice_type = slice_type;
    if (slice_type == PHX_SLICE_I) return 0;      /* encoder.c:2297-2298 */

    for (int i = 0; i < dpb->n_pictures; i++) {
        const phx_dpb_entry_t *e = &dpb->pictures[i];
        if (!e->b_kept_as_ref) continue;         /* encoder.c:2470 */
        if (e->i_poc < current_poc) {
            if (out->i_ref[0] < PHX_DPB_REF_MAX)
                out->frame_entry[0][out->i_ref[0]++] = *e;
        } else if (e->i_poc > current_poc) {
            if (out->i_ref[1] < PHX_DPB_REF_MAX)
                out->frame_entry[1][out->i_ref[1]++] = *e;
        }                                        /* encoder.c:2308-2313 */
    }

    for (int list = 0; list < 2; list++) {
        phx_dpb_sort_list(out, list, current_frame);
        phx_dpb_set_nearest(out, list);
    }
    phx_dpb_check_reorder(out);                  /* encoder.c:2360 */

    if (cfg) {
        if (cfg->max_ref1 > 0 && out->i_ref[1] > cfg->max_ref1)
            out->i_ref[1] = cfg->max_ref1;       /* encoder.c:2365 */
        if (cfg->max_ref0 > 0 && out->i_ref[0] > cfg->max_ref0)
            out->i_ref[0] = cfg->max_ref0;
        if (cfg->frame_reference > 0 &&
            out->i_ref[0] > cfg->frame_reference) /* encoder.c:2366 */
            out->i_ref[0] = cfg->frame_reference;
    }

    for (int list = 0; list < 2; list++)
        for (int i = 0; i < out->i_ref[list]; i++) {
            const phx_dpb_entry_t *e = &out->frame_entry[list][i];
            out->ref_frame_num[list][i] = e->i_frame_num;
            out->ref_poc[list][i] = e->i_poc;
            out->ref_frame[list][i] = e->i_frame;
            out->ref_type[list][i] = e->i_type;
            out->frame[list][i] = e->frame;
        }
    for (int list = 0; list < 2; list++)
        out->fref_nearest[list] = out->i_ref[list] > 0
            ? &out->frame_entry[list][out->nearest_index[list]] : NULL;
    return 0;
}

const phx_frame_t *phx_dpb_ref_frame(const phx_dpb_ref_lists_t *lists,
                                     int list, int ref_idx)
{
    if (!lists || list < 0 || list > 1 || ref_idx < 0 ||
        ref_idx >= lists->i_ref[list]) return NULL;
    return lists->frame[list][ref_idx];
}

int phx_dpb_ref_frame_num(const phx_dpb_ref_lists_t *lists, int list, int ref_idx)
{
    if (!lists || list < 0 || list > 1 || ref_idx < 0 ||
        ref_idx >= lists->i_ref[list]) return -1;
    return lists->ref_frame_num[list][ref_idx];
}

int phx_dpb_ref_poc(const phx_dpb_ref_lists_t *lists, int list, int ref_idx)
{
    if (!lists || list < 0 || list > 1 || ref_idx < 0 ||
        ref_idx >= lists->i_ref[list]) return -1;
    return lists->ref_poc[list][ref_idx];
}

const phx_frame_t *phx_dpb_get_ref_l0_at(const phx_dpb_t *dpb, int current_poc,
                                         int current_frame, int ref_idx)
{
    phx_dpb_ref_lists_t lists;
    if (phx_dpb_init_ref_lists(dpb, &lists, current_poc, current_frame,
                               PHX_SLICE_B, NULL) < 0) return NULL;
    return phx_dpb_ref_frame(&lists, 0, ref_idx);
}

const phx_frame_t *phx_dpb_get_ref_l1_at(const phx_dpb_t *dpb, int current_poc,
                                         int current_frame, int ref_idx)
{
    phx_dpb_ref_lists_t lists;
    if (phx_dpb_init_ref_lists(dpb, &lists, current_poc, current_frame,
                               PHX_SLICE_B, NULL) < 0) return NULL;
    return phx_dpb_ref_frame(&lists, 1, ref_idx);
}
