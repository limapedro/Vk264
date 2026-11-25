/* Phoenix public encoder API implementation.
  *
  * Port map: encoder/api.c:76/130/159/166 (dispatch), encoder/encoder.c:1507
  * (open), :1579/:1581 (SPS/PPS init), :1669 (cabac init), :1671 (cavlc init),
  * :1708 (cost init), :2096 (headers), :2791-3093 (slice header + MB loop),
  * :3323 (encode), :3368-3426 (put_frame / lowres), :4196/:4520 (close).
  *
  * The frame loop is the serial class-C stage: cache_load -> analyse ->
  * encode -> entropy write -> cache_save (encoder.c:2855-2972).  Analysis and
  * MB encode are supplied by src/phx_analysis.h / src/phx_mbencode.h; the
  * explicit phx_mb_decision_t boundary is documented in docs/SHADER_NOTES.md.
  *
  * SHADER: C — serial host pipeline; every CABAC/CAVLC write happens in one
  * thread, with no globals and no environment-dependent state.
  */
#include "phx_encoder.h"
#include "phx_slice.h"
#include "phx_cabac_syntax.h"
#include "phx_cavlc.h"
#include "phx_tables.h"
#include "phx_mb_decision.h"
#include "phx_analysis.h"
#include "phx_mbencode.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* x264 validate_parameters (encoder.c:423, called from encoder.c:1552).
  * Phoenix only accepts the configurations its ported frame/header path owns:
  * positive 16-pixel-multiple I420 dimensions (frame.c:85-86, set.c:106-107).
  * SHADER: C — one-time host-side validation. */
static int phx_encoder_validate(const phx_param_t *p)
{
    if (!p) return -1;
    if (p->i_csp != 0) return -1;                       /* only I420: x264.h:324 */
    if (p->i_width <= 0 || p->i_height <= 0) return -1;
    if (p->i_width % 16 || p->i_height % 16) return -1; /* frame.c:85-86 */
    if (p->i_width > 4096 || p->i_height > 4096) return -1; /* bitstream cap */
    return 0;
}

/* x264_encoder_close helpers (encoder.c:4196, :4520/:4521, :4527-4530).
  * Shared by close and the open-failure path so every allocation is released
  * exactly once.
  * SHADER: C — serial teardown. */
static void phx_encoder_release(phx_t *h)
{
    if (!h) return;
    phx_analyse_free_costs(&h->cost, h->param.i_me_range); /* encoder.c:4520 */
    phx_mb_state_delete(h->mb_state);                      /* encoder.c:4527 */
    phx_frame_delete(h->fenc);                             /* encoder.c:4528 */
    free(h->rbsp);                                         /* encoder.c:4518 */
    free(h->recon);                                        /* encoder.c:4527 */
    free(h->bitstream);                                    /* encoder.c:4518 */
    free(h);                                               /* api.c:135 */
}

/* x264_encoder_open (api.c:76 -> encoder.c:1507).  The state fields below are
  * initialised in x264's order: private param copy (:1516), derived params
  * (:1552/:1579/:1581), frame geometry (:1590), frame pools (:1602),
  * cost tables (:1708), CABAC/CAVLC init (:1668-1671), output bitstream
  * (:1724) and the frame counters (:1563-1564/:1630).
  * SHADER: C — serial host-side initialisation. */
phx_t *phx_encoder_open(const phx_param_t *p)
{
    if (phx_encoder_validate(p) < 0) return NULL;

    phx_t *h = (phx_t *)calloc(1, sizeof(*h));         /* encoder.c:1513 */
    if (!h) return NULL;

    h->param = *p;                                     /* encoder.c:1516 */
    phx_param_finalize(&h->param);                     /* encoder.c:1218-1231 */
    h->i_width = h->param.i_width;
    h->i_height = h->param.i_height;
    h->i_mb_width = (h->i_width + 15) / 16;            /* set.c:106 / encoder.c:1590 */
    h->i_mb_height = (h->i_height + 15) / 16;          /* set.c:107 / encoder.c:1591 */

    phx_sps_init(&h->sps, &h->param, h->i_width, h->i_height); /* encoder.c:1579 */
    phx_pps_init(&h->pps, &h->sps, &h->param);                 /* encoder.c:1581 */

    h->i_frame = -1;                                   /* encoder.c:1563 */
    h->i_frame_num = 0;                                /* encoder.c:1564 */
    h->i_poc = 0;                                      /* encoder.c:1629 */
    h->i_idr_pic_id = 0;                               /* encoder.c:142 */
    h->i_input = 0;                                    /* encoder.c:1630 */

    h->fenc = phx_frame_new(h->i_width, h->i_height,
                            h->param.i_bframe);        /* encoder.c:3368 / frame.c:60 */
    h->mb_state = phx_mb_state_new(h->i_mb_width, h->i_mb_height);
                                                       /* encoder.c:1590-1592 */
    /* encoder.c:1724-1729 — one RBSP scratch plus the Annex-B destination.
     * The +4096 margin covers NAL emulation-prevention expansion
     * (bitstream.c:29-39) for small frames. */
    h->i_bitstream = h->i_width * h->i_height * 4 + 4096;
    if (h->i_bitstream < 8192) h->i_bitstream = 8192;
    h->rbsp = (uint8_t *)malloc((size_t)h->i_bitstream);
    h->bitstream = (uint8_t *)malloc((size_t)h->i_bitstream);

    /* Reconstructed plane in FDEC_STRIDE layout (common/common.h:571,
     * encoder.c:2783 -> macroblock.c:501-548).  The first 64 bytes and 2
     * leading rows are padding for the x=-1 / y=-1 neighbours. */
    {
        int rows = h->i_mb_height * 16 + 40;
        h->recon = (uint8_t *)calloc((size_t)rows * PHX_FDEC_STRIDE + 128, 1);
    }

    if (!h->fenc || !h->mb_state || !h->rbsp || !h->bitstream || !h->recon ||
        phx_analyse_init_costs(&h->cost, h->param.i_me_range,
                               (int)h->param.i_me_method, 0, PHX_QP_MAX)) {
        phx_encoder_release(h);                        /* encoder.c:goto fail */
        return NULL;
    }

    if (h->param.b_cabac) {                            /* encoder.c:1668-1669 */
        int qp = h->param.i_qp_constant >= 0
               ? h->param.i_qp_constant : 26;          /* set.c:494 */
        if (qp < 0) qp = 0;
        if (qp > PHX_QP_MAX_SPEC) qp = PHX_QP_MAX_SPEC;
        phx_cabac_context_init(&h->cabac, PHX_SLICE_I, qp, 0); /* cabac.c:32 */
        phx_cabac_encode_init_core(&h->cabac);                 /* cabac.c:53 */
    } else {
        phx_cavlc_init();                              /* encoder.c:1671 */
    }
    return h;
}

/* Short alias (ORACLE_DESIGN.md:40); x264_encoder_open, api.c:76.
  * SHADER: C — forwards to the serial open path. */
phx_t *phx_open(const phx_param_t *p)
{
    return phx_encoder_open(p);
}

/* x264_encoder_headers (api.c:159 -> encoder.c:2096-2133).  The Annex-B byte
  * assembly is delegated to phx_encoder_headers() (phx_headers.h:53), which is
  * the port of nal_start/sps_write/pps_write/nal_end (encoder.c:2106-2115).
  * SHADER: C — one serial header stage. */
int phx_headers(phx_t *h, phx_nal_t **out, int *nnal)
{
    if (!h || !out || !nnal) return -1;
    *out = NULL;
    *nnal = 0;

    int n = phx_encoder_headers(h->header_bytes, (int)sizeof(h->header_bytes),
                                 &h->param, h->i_width, h->i_height);
    if (n < 0) return -1;

    int split = -1;
    for (int i = 4; i + 2 < n; i++) {
        if (h->header_bytes[i] == 0 && h->header_bytes[i+1] == 0 &&
            h->header_bytes[i+2] == 1) {
            split = (i > 0 && h->header_bytes[i-1] == 0) ? i - 1 : i;
            break;
        }
    }
    if (split <= 0 || split >= n) return -1;

    h->header_nal[0].p_payload = h->header_bytes;
    h->header_nal[0].i_payload = split;
    h->header_nal[0].i_ref_idc = 3;                     /* NAL_PRIORITY_HIGHEST */
    h->header_nal[0].i_type = 7;                       /* NAL_SPS, encoder.c:2106 */
    h->header_nal[0].b_long_startcode = 1;

    h->header_nal[1].p_payload = h->header_bytes + split;
    h->header_nal[1].i_payload = n - split;
    h->header_nal[1].i_ref_idc = 3;                     /* encoder.c:2112 */
    h->header_nal[1].i_type = 8;                       /* NAL_PPS, encoder.c:2112 */
    h->header_nal[1].b_long_startcode = 1;

    *out = h->header_nal;                              /* encoder.c:2130 */
    *nnal = 2;                                         /* encoder.c:2129 */
    return n;                                          /* encoder.c:2133 */
}

/* Byte-form header emission; phx_encoder_headers is phx_headers.h:53,
  * assembling set.c:305 (SPS write) and set.c:505 (PPS write).
  * SHADER: C — serial header stage. */
int phx_encoder_header_bytes(phx_t *h, uint8_t *dst, int capacity)
{
    if (!h || !dst || capacity < 0) return -1;
    return phx_encoder_headers(dst, capacity, &h->param,
                               h->i_width, h->i_height);
}

/* -------------------------------------------------------------------------
  * Frame-loop helpers.  Every one of them is class C (serial host work).
  * ------------------------------------------------------------------------- */

/* x264_frame_copy_picture (encoder.c:3372, common/frame.c:...):
  * copy the caller's I420 luma rows into the padded frame plane.  Chroma is
  * not consumed by the ported 4:2:0 analysis/encode path yet (TODO: the
  * phx_frame_t has no chroma planes); only luma is copied here.
  * SHADER: C — one ordered memcpy per row. */
static int phx_encoder_copy_picture(phx_t *h, const phx_picture_t *pic)
{
    phx_frame_t *f = h->fenc;
    if (!f || !pic || !pic->plane[0]) return -1;
    if (pic->i_width != h->i_width || pic->i_height != h->i_height) return -1;

    int src_stride = pic->i_stride[0] > 0 ? pic->i_stride[0] : h->i_width;
    for (int y = 0; y < f->i_height; y++)                /* frame.c:535 */
        memcpy(f->plane + (size_t)y * f->i_stride,
               pic->plane[0] + (size_t)y * src_stride,
               (size_t)f->i_width);
    /* frame.c:627 / encoder.c:3377 (the picture is already mod16). */
    phx_plane_expand_border(f->plane, f->i_stride, f->i_width, f->i_height,
                            PHX_PADH, PHX_PADV, 1, 1, 0);
    return 0;
}

/* bs_align_1 (common/bitstream.h:187-191): x264 pads the slice header with
  * one bits before handing the buffer to CABAC.  phx_bitstream exposes
  * align_0/align_10 only, so the one-bit variant is spelled out here.
  * SHADER: C — serial bitstream write. */
static void phx_encoder_bs_align_1(phx_bs_t *bs)
{
    int n = 8 - bs->bit;
    if (n != 8)
        phx_bs_write(bs, n, (uint32_t)((1u << n) - 1));  /* bitstream.h:189 */
}

/* encoder.c:2783 (x264_macroblock_thread_init) + encoder.c:2808-2809:
  * reset the per-frame MB state that belongs to the entropy stage.
  * SHADER: C — serial state reset, no cross-frame globals. */
static void phx_encoder_reset_mb_state(phx_t *h)
{
    phx_mb_state_t *s = h->mb_state;
    if (!s) return;
    s->b_cabac = h->param.b_cabac;                      /* encoder.c:2855 */
    s->slice_type = PHX_SLICE_I;                        /* encoder.c:225 */
    s->first_mb = 0; s->threadslice_start = 0;          /* encoder.c:206 */
    for (int i = 0; i < s->mb_count; i++) {
        s->type[i] = 0;
        s->cbp[i] = 0;
        s->mb_transform_size[i] = 0;
        s->slice_table[i] = 0;
        s->field[i] = 0;
        memset(s->non_zero_count[i], 0, sizeof(s->non_zero_count[i]));
        memset(s->intra4x4_pred_mode[i], 2, sizeof(s->intra4x4_pred_mode[i]));
        for (int l = 0; l < 2; l++) {
            memset(s->ref[l] + 4*i, -1, 4);             /* macroblock.c:1071 */
            memset(s->mv[l] + 16*i, 0, 16*sizeof(int16_t[2]));
            memset(s->mvd[l][i], 0, sizeof(s->mvd[l][i]));
        }
    }
    memset(h->recon, 0, (size_t)((h->i_mb_height*16 + 40) * PHX_FDEC_STRIDE + 128));
}

/* docs/SHADER_NOTES.md:126-144 — publish the class-B decision into the class-C
  * neighbour cache.  phx_macroblock_encode() already writes the authoritative
  * nnz / intra-pred-mode arrays into the cache (phx_mbencode.c:69-76, :245,
  * :356); this function only installs the values the entropy stage reads from
  * the decision rather than from the cache, so nothing written by encode is
  * clobbered.  SHADER: C — serial copy, one MB at a time. */
static void phx_encoder_apply_decision(phx_t *h, const phx_mb_decision_t *d)
{
    phx_mb_cache_t *c = &h->mb_cache;
    c->i_cbp_luma   = d->cbp_luma & 0x0f;               /* encoder.c:2856 */
    c->i_cbp_chroma = d->cbp_chroma & 0x03;             /* macroblock.c:1761 */
    c->b_transform_8x8 = h->pps.b_transform_8x8_mode
                       ? (d->b_transform_8x8 & 1) : 0; /* cabac.c:973 */
    c->i_qp = d->i_qp;                                 /* macroblock.c:620 */
    for (int i = 0; i < 16; i++) {                     /* decision mv/ref are block-indexed */
        c->ref[0][phx_scan8[i]] = d->ref[0][i];        /* SHADER_NOTES.md:137 */
        c->ref[1][phx_scan8[i]] = d->ref[1][i];
        c->mv[0][phx_scan8[i]][0] = d->mv[0][i][0];
        c->mv[0][phx_scan8[i]][1] = d->mv[0][i][1];
        c->mv[1][phx_scan8[i]][0] = d->mv[1][i][0];
        c->mv[1][phx_scan8[i]][1] = d->mv[1][i][1];
    }
}

/* Mapping from phx_mb_decision_t's 48x16 coefficient array to x264's
  * dct.luma4x4[]/luma16x16_dc[] arrays.  phx_macroblock_encode() writes luma
  * block i to dct[phx_scan8[i]] and the I16x16 Hadamard DC block to
  * dct[phx_scan8[48]] == dct[0] (phx_mbencode.h:19-26, phx_mbencode.c:69-76,
  * :199).  Chroma coefficients are not representable in the shared struct yet
  * (phx_mbencode.h:25-26): those blocks return NULL and code as zero.
  * SHADER: C — read-only view over the decision. */
enum {
    PHX_BLK_LUMA16_DC = 0, PHX_BLK_LUMA_AC, PHX_BLK_LUMA4,
    PHX_BLK_CHROMA_DC, PHX_BLK_CHROMA_AC
};
static const int16_t *phx_encoder_block(const phx_mb_decision_t *d, int kind,
                                        int idx, int *ncoeff)
{
    switch (kind) {
    case PHX_BLK_LUMA16_DC: *ncoeff = 4;  return d->dct[0]; /* scan8[48] == 0 */
    case PHX_BLK_LUMA_AC:   *ncoeff = 15; return d->dct[phx_scan8[idx]] + 1; /* cavlc.c:570 */
    case PHX_BLK_LUMA4:     *ncoeff = 16; return d->dct[phx_scan8[idx]];     /* phx_mbencode.c:73 */
    case PHX_BLK_CHROMA_DC: *ncoeff = 4;  return NULL; /* TODO(chroma): no chroma planes */
    case PHX_BLK_CHROMA_AC: *ncoeff = 15; return NULL; /* TODO(chroma): no chroma planes */
    default: *ncoeff = 0; return NULL;
    }
}

/* Derive the coded-block flag from the actual quantised coefficients.  The
  * decision's nnz[] bytes are published for neighbour contexts, but deriving
  * the flag keeps CBF and the written residual self-consistent even if the
  * analysis worker has not filled nnz[] yet.
  * SHADER: C — serial coefficient scan. */
static int phx_encoder_block_nz(const int16_t *coef, int ncoeff)
{
    if (!coef) return 0;
    for (int i = 0; i < ncoeff; i++)
        if (coef[i]) return 1;
    return 0;
}

/* cabac.c:892-916 (cabac_block_residual_cbf) wrapper.
  * SHADER: C — serial bin writer. */
static void phx_encoder_cabac_block(phx_t *h, phx_cabac_mb_t *mb, int cat,
                                    int idx, const int16_t *coef, int ncoeff,
                                    int b_intra, int b_dc)
{
    h->mb_cache.non_zero_count[phx_scan8[idx]] =
        (uint8_t)phx_encoder_block_nz(coef, ncoeff);
    phx_cabac_block_residual_cbf(&h->cabac, mb, cat, idx, coef,
                                  b_intra, b_dc, 0);
}

/* common/macroblock.h:432-442 + cavlc.c:198 — nC prediction and its VLC index.
  * SHADER: C — serial coefficient-context lookup. */
static int phx_encoder_cavlc_nc(const phx_mb_cache_t *c, int idx)
{
    static const uint8_t ct_index[17] = {
        0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3
    };                                   /* cavlc.c:198 */
    int za = c->non_zero_count[phx_scan8[idx] - 1];
    int zb = c->non_zero_count[phx_scan8[idx] - 8];
    int ret = za + zb;
    if (ret < 0x80) ret = (ret + 1) >> 1;
    return ct_index[ret & 0x0f];
}

/* cavlc.c:200-209 (x264_cavlc_block_residual macro).
  * SHADER: C — serial bin writer. */
static void phx_encoder_cavlc_block(phx_t *h, phx_cavlc_t *cav, int cat,
                                    int idx, const int16_t *coef, int ncoeff)
{
    uint8_t *nnz = &h->mb_cache.non_zero_count[phx_scan8[idx]];
    *nnz = (uint8_t)phx_encoder_block_nz(coef, ncoeff);
    int nC;
    if (cat == PHX_DCT_CHROMA_DC) {
        nC = 4;                                        /* cavlc.c:202 */
    } else if (cat == PHX_DCT_LUMA_DC) {
        /* cavlc.c:203 maps LUMA_DC (48) back to block 0 for nC prediction. */
        nC = phx_encoder_cavlc_nc(&h->mb_cache, 0);
    } else {
        nC = phx_encoder_cavlc_nc(&h->mb_cache, idx);
    }
    phx_cavlc_block_residual(cav, cat, coef, nnz, nC, ncoeff);
}

/* predict.h:45-49,62-66,84-92 (mirrors phx_cabac_syntax.c:170-177).
  * SHADER: C — serial intra prediction-mode lookup. */
static int phx_encoder_predict_i4(const phx_mb_cache_t *c, int idx)
{
    static const int8_t fix[13] = { -1,0,1,2,3,4,5,6,7,8,2,2,2 };
    int a = c->intra4x4_pred_mode[phx_scan8[idx] - 1];
    int b = c->intra4x4_pred_mode[phx_scan8[idx] - 8];
    int ma = fix[a+1], mb = fix[b+1];
    int m = ma < mb ? ma : mb;
    return m < 0 ? 2 : m;
}

/* cavlc.c:305-335 (cavlc_mb_header_i), I-slice offset 0 (cavlc.c:543).
  * SHADER: C — serial Exp-Golomb writer. */
static void phx_encoder_cavlc_header_i(phx_t *h, phx_bs_t *bs,
                                       const phx_mb_decision_t *d)
{
    static const uint8_t fix16[7] = { 0,1,2,3,2,2,2 };
    static const uint8_t fixc[7]  = { 0,1,2,3,0,0,0 };
    static const int8_t fix4[13]  = { -1,0,1,2,3,4,5,6,7,8,2,2,2 };

    if (d->mb_type == PHX_I_16x16) {
        int pred = d->intra16_pred_mode;
        if (pred < 0) pred = 0;
        if (pred > 6) pred = 6;
        phx_bs_write_ue(bs, (unsigned)(1 + fix16[pred]
                        + 4*d->cbp_chroma + (d->cbp_luma ? 12 : 0))); /* :310 */
    } else {
        phx_bs_write_ue(bs, 0);                          /* :316 */
        if (h->pps.b_transform_8x8_mode)
            phx_bs_write1(bs, (unsigned)(d->b_transform_8x8 & 1)); /* :318 */
        int di = (h->pps.b_transform_8x8_mode && d->b_transform_8x8 &&
                  d->mb_type == PHX_I_8x8) ? 4 : 1;     /* :315/:322 */
        for (int i = 0; i < 16; i += di) {               /* :321 */
            int pred = phx_encoder_predict_i4(&h->mb_cache, i); /* :323 */
            int mode = d->intra4x4_pred_mode[i];
            if (mode < -1) mode = -1;
            if (mode > 11) mode = 11;
            mode = fix4[mode+1];                         /* :324 */
            if (pred == mode)
                phx_bs_write1(bs, 1);                    /* :327 */
            else
                phx_bs_write(bs, 4,
                    (uint32_t)(mode - (mode > pred)));   /* :329 */
        }
    }
    int cm = d->chroma_pred_mode;
    if (cm < 0) cm = 0;
    if (cm > 6) cm = 6;
    phx_bs_write_ue(bs, (unsigned)fixc[cm]);            /* :334 */
    (void)fix4;
}

/* cavlc.c:211-239 (cavlc_qp_delta).  SHADER: C — serial Exp-Golomb writer. */
static void phx_encoder_cavlc_qp_delta(phx_bs_t *bs, int qp, int *last_qp)
{
    int dqp = qp - *last_qp;                            /* :214 */
    if (dqp < -(PHX_QP_MAX_SPEC+1)/2) dqp += PHX_QP_MAX_SPEC+1; /* :233 */
    else if (dqp > PHX_QP_MAX_SPEC/2) dqp -= PHX_QP_MAX_SPEC+1; /* :235 */
    phx_bs_write_se(bs, dqp);                           /* :238 */
    *last_qp = qp;
}

/* cavlc.c:487-595 (x264_macroblock_write_cavlc).  Only the I-slice header is
  * complete today; P_L0 16x16 is a best-effort path and P_8x8/B remain TODO
  * (all frames are coded as I by phx_encode() for now).
  * SHADER: C — strictly serial CAVLC MB syntax. */
static void phx_encoder_write_cavlc_mb(phx_t *h, const phx_mb_decision_t *d,
                                       phx_cavlc_t *cav, int *last_qp)
{
    phx_bs_t *bs = cav->bs;
    int mt = d->mb_type;
    int intra = (mt == PHX_I_4x4 || mt == PHX_I_8x8 ||
                 mt == PHX_I_16x16 || mt == PHX_I_PCM);

    if (mt == PHX_I_PCM) {
        static const int offsets[3] = { 5, 23, 0 };     /* cavlc.c:513 */
        phx_bs_write_ue(bs, (unsigned)(offsets[PHX_SLICE_I] + 25)); /* :515 */
        phx_bs_align_0(bs);                             /* :519 */
        /* x264 reinitialises bs at s->p (cavlc.c:530-531) after the raw
         * samples.  The decision type carries no fenc samples, so zero
         * samples are emitted; I_PCM is not produced by the current
         * analysis path. */
        for (int i = 0; i < 256 + 32; i++)
            phx_bs_write(bs, 8, 0);                     /* TODO(cavlc-pcm) */
        return;
    }

    if (intra) {
        phx_encoder_cavlc_header_i(h, bs, d);           /* cavlc.c:543 */
    } else if (mt == PHX_P_L0 && d->i_partition == PHX_D_16x16) {
        phx_bs_write1(bs, 1);                           /* cavlc.c:344 */
        /* i_fref[0] == 1: no ref_idx syntax.  MVD TODO(for P frames). */
        phx_bs_write_se(bs, 0);                         /* cavlc.c:246 */
        phx_bs_write_se(bs, 0);
    } else {
        /* TODO(P/B): P_8x8 / B headers need sub-partitions and MVDs from the
         * decision; I-slice frames never take this branch. */
        phx_encoder_cavlc_header_i(h, bs, d);           /* cavlc.c:408/481 */
    }

    if (mt != PHX_I_16x16) {
        static const uint8_t cbp_golomb[2][2][48] = {
            {{ 0, 1, 2, 5, 3, 6, 14, 10, 4, 15, 7, 11, 8, 12, 13, 9 },
             { 1, 10, 11, 6, 12, 7, 14, 2, 13, 15, 8, 3, 9, 4, 5, 0 }},
            {{ 0, 2, 3, 7, 4, 8, 17, 13, 5, 18, 9, 14, 10, 15, 16, 11,
               1, 32, 33, 36, 34, 37, 44, 40, 35, 45, 38, 41, 39, 42, 43, 19,
               6, 24, 25, 20, 26, 21, 46, 28, 27, 47, 22, 29, 23, 30, 31, 12 },
             { 3, 29, 30, 17, 31, 18, 37, 8, 32, 38, 19, 9, 20, 10, 11, 2,
               16, 33, 34, 21, 35, 22, 39, 4, 36, 40, 23, 5, 24, 6, 7, 1,
               41, 42, 43, 25, 44, 26, 46, 12, 45, 47, 27, 13, 28, 14, 15, 0 }}
        };                                             /* cavlc.c:36-46 */
        unsigned cbp = (unsigned)((d->cbp_chroma << 4) | d->cbp_luma);
        phx_bs_write_ue(bs, (unsigned)cbp_golomb[1][intra][cbp]); /* :552 */
    }
    if (h->pps.b_transform_8x8_mode && d->cbp_luma)
        phx_bs_write1(bs, (unsigned)(d->b_transform_8x8 & 1)); /* cavlc.c:556 */

    if (mt == PHX_I_16x16) {
        phx_encoder_cavlc_qp_delta(bs, d->i_qp, last_qp); /* cavlc.c:560 */
        int n;
        const int16_t *l = phx_encoder_block(d, PHX_BLK_LUMA16_DC, 0, &n);
        phx_encoder_cavlc_block(h, cav, PHX_DCT_LUMA_DC, 48, l, n); /* :565 */
        if (d->cbp_luma) {
            for (int i = 0; i < 16; i++) {              /* :569 */
                l = phx_encoder_block(d, PHX_BLK_LUMA_AC, i, &n);
                phx_encoder_cavlc_block(h, cav, PHX_DCT_LUMA_AC, i, l, n);
            }
        }
    } else if (d->cbp_luma || d->cbp_chroma) {
        phx_encoder_cavlc_qp_delta(bs, d->i_qp, last_qp); /* cavlc.c:575 */
        for (int i8 = 0; i8 < 4; i8++) {
            if (!(d->cbp_luma & (1 << i8))) continue;   /* :287 */
            for (int j = 0; j < 4; j++) {
                int idx = i8*4 + j;
                int n;
                const int16_t *l = phx_encoder_block(d, PHX_BLK_LUMA4, idx, &n);
                phx_encoder_cavlc_block(h, cav, PHX_DCT_LUMA_4x4, idx, l, n);
            }
        }
    }
    if (d->cbp_chroma) {                               /* cavlc.c:578 */
        int n;
        for (int ch = 0; ch < 2; ch++) {
            const int16_t *l = phx_encoder_block(d, PHX_BLK_CHROMA_DC, ch, &n);
            phx_encoder_cavlc_block(h, cav, PHX_DCT_CHROMA_DC, 48+ch, l, n);
        }
        if (d->cbp_chroma == 2) {
            for (int j = 16; j < 48; j += 16)          /* :586, 4:2:0 step */
                for (int k = j; k < j+4; k++) {
                    const int16_t *l = phx_encoder_block(d, PHX_BLK_CHROMA_AC,
                                                         k, &n);
                    phx_encoder_cavlc_block(h, cav, PHX_DCT_CHROMA_AC, k, l, n);
                }
        }
    }
}

/* cabac.c:917-1096 (macroblock_write_cabac_internal), 4:2:0 path.
  * SHADER: C — strictly serial CABAC bin writer. */
static void phx_encoder_write_cabac_mb(phx_t *h, phx_cabac_mb_t *mb,
                                       const phx_mb_decision_t *d,
                                       int mb_xy)
{
    phx_cabac_t *cb = &h->cabac;
    int mt = d->mb_type;

    if (mb->slice_type == PHX_SLICE_P)
        phx_cabac_mb_header_p(cb, mb, mt, 1);           /* cabac.c:933 */
    else if (mb->slice_type == PHX_SLICE_B)
        phx_cabac_mb_header_b(cb, mb, mt, 1);           /* cabac.c:935 */
    else
        phx_cabac_mb_header_i(cb, mb, mt, PHX_SLICE_I, 1); /* cabac.c:937 */

    if (mt == PHX_I_PCM) {
        /* cabac.c:943-963: raw samples follow the aligned cabac byte pointer.
         * The explicit decision has no fenc sample buffer, so zero samples
         * are emitted; I_PCM is not produced by the current analysis path. */
        phx_bs_t pcm;
        phx_bs_init(&pcm, cb->p, (int)(cb->p_end - cb->p)); /* :945-946 */
        for (int i = 0; i < 256 + 32; i++)
            phx_bs_write(&pcm, 8, 0);                   /* TODO(cabac-pcm) */
        cb->p = pcm.p;                                  /* :958 */
        phx_cabac_encode_init_core(cb);                 /* :959 */
        return;
    }

    if (mt != PHX_I_16x16) {                           /* cabac.c:966 */
        phx_cabac_cbp_luma(cb, mb);                     /* :968 */
        phx_cabac_cbp_chroma(cb, mb);                   /* :970 */
    }
    if (h->pps.b_transform_8x8_mode && d->cbp_luma)
        phx_cabac_transform_size(cb, mb);               /* :975 */

    if (d->cbp_luma || d->cbp_chroma || mt == PHX_I_16x16) { /* :978 */
        int b_intra = (mt == PHX_I_4x4 || mt == PHX_I_8x8 ||
                       mt == PHX_I_16x16 || mt == PHX_I_PCM); /* :980 */
        phx_cabac_qp_delta(cb, mb);                     /* :981 */
        (void)mb_xy;

        if (mt == PHX_I_16x16) {
            int n;
            const int16_t *l = phx_encoder_block(d, PHX_BLK_LUMA16_DC, 0, &n);
            phx_encoder_cabac_block(h, mb, PHX_DCT_LUMA_DC, 48, l, n, 1, 1); /* :989 */
            if (d->cbp_luma)
                for (int i = 0; i < 16; i++) {          /* :993 */
                    l = phx_encoder_block(d, PHX_BLK_LUMA_AC, i, &n);
                    phx_encoder_cabac_block(h, mb, PHX_DCT_LUMA_AC, i, l, n, 1, 0);
                }
        } else {
            for (int i8 = 0; i8 < 4; i8++) {
                if (!(d->cbp_luma & (1 << i8))) continue; /* :1055 */
                for (int j = 0; j < 4; j++) {            /* :1056 */
                    int idx = i8*4 + j;
                    int n;
                    const int16_t *l = phx_encoder_block(d, PHX_BLK_LUMA4, idx, &n);
                    phx_encoder_cabac_block(h, mb, PHX_DCT_LUMA_4x4,
                                            idx, l, n, b_intra, 0); /* :1057 */
                }
            }
        }
        if (d->cbp_chroma) {                            /* :1060 */
            for (int ch = 0; ch < 2; ch++) {
                int n;
                const int16_t *l = phx_encoder_block(d, PHX_BLK_CHROMA_DC, ch, &n);
                phx_encoder_cabac_block(h, mb, PHX_DCT_CHROMA_DC,
                                        48+ch, l, n, b_intra, 1); /* :1069-1070 */
            }
            if (d->cbp_chroma == 2) {                   /* :1073 */
                for (int j = 16; j < 48; j += 16)     /* :1075, 4:2:0 step */
                    for (int k = j; k < j+4; k++) {
                        int n;
                        const int16_t *l = phx_encoder_block(d,
                            PHX_BLK_CHROMA_AC, k, &n);
                        phx_encoder_cabac_block(h, mb, PHX_DCT_CHROMA_AC,
                                                k, l, n, b_intra, 0); /* :1078 */
                    }
            }
        }
    }
}

/* x264_encoder_encode (api.c:166 -> encoder.c:3323).  Stage order:
  *   put_frame/copy picture (encoder.c:3368-3426) -> lowres (mc.c:458) ->
  *   [TODO: lookahead, rate control] ->
  *   slice header (encoder.c:2791) -> MB loop (encoder.c:2812-3074) ->
  *   CABAC/CAVLC trailing bits (encoder.c:3080-3092) ->
  *   NAL assembly (encoder.c:3093 -> encoder.c:2124).
  * Every frame is coded as an all-intra slice: frame 0 is IDR, later frames
  * are non-IDR I frames.  P/B lookahead, slice-type decision and rate control
  * are explicit TODOs below, and no global or environment state is used.
  * SHADER: C — the whole staged pipeline is serial and seek-safe. */
int phx_encode(phx_t *h, const phx_picture_t *pic,
               phx_nal_t **out, int *nnal, uint8_t **recon)
{
    if (!h || !out || !nnal) return -1;
    *out = NULL;                                       /* encoder.c:3355 */
    *nnal = 0;                                         /* encoder.c:3356 */
    if (recon) *recon = NULL;

    /* x264.c:2049 — flush has no delayed frames in this path. */
    if (!pic) return 0;
    if (phx_encoder_copy_picture(h, pic) < 0) return -1;
    phx_frame_init_lowres(h->fenc);                    /* encoder.c:3423 / mc.c:458 */
    /* TODO(worker-slicetype): phx_lookahead_put_frame/get_frames, P/B decision.
     * TODO(worker-ratecontrol): phx_ratecontrol_start/end (encoder.c:3857/3978). */

    h->i_frame++;                                      /* encoder.c:3444 */
    h->i_input++;                                      /* encoder.c:3379 */

    /* Explicit per-frame context: x264 derives these from h->sh / fenc
      * (encoder.c:132-211).  Here they come from the handle counters only. */
    phx_slice_frame_t fc;
    memset(&fc, 0, sizeof(fc));
    int mask = (1 << h->sps.i_log2_max_frame_num) - 1;
    fc.i_frame_num = h->i_frame_num & mask;            /* encoder.c:136 */
    fc.i_poc = h->i_poc;                               /* encoder.c:145 */
    fc.b_idr = (h->i_frame == 0);                      /* encoder.c:236 */
    fc.i_idr_pic_id = h->i_idr_pic_id;                  /* encoder.c:142 */
    fc.i_nal_ref_idc = 3;                              /* encoder.c:328 */
    fc.i_type = PHX_SLICE_I;                           /* encoder.c:225 */
    fc.i_first_mb = 0;                                 /* encoder.c:132 */
    fc.i_last_mb = h->mb_state->mb_count - 1;           /* encoder.c:133 */
    /* ratecontrol.c:823-825 — CQP maps the constant QP through the IP factor:
     * I frames use qp_constant - 6*log2(f_ip_factor) + 0.5, P frames the
     * constant itself. With --qp 28 and the default ip_factor 1.4 this is 25. */
    {
        double ip_offset = 6.0 * log2(h->param.f_ip_factor); /* ratecontrol.c:821 */
        int qp_i = (int)(h->pps.i_pic_init_qp - ip_offset + 0.5); /* :824 */
        if (qp_i < 0) qp_i = 0;
        if (qp_i > PHX_QP_MAX_SPEC) qp_i = PHX_QP_MAX_SPEC;
        fc.i_qp = qp_i;
    }
    fc.b_variable_qp = 0;                              /* encoder.c:205 */
    fc.b_sliced_threads = 0;                           /* encoder.c:206 */
    fc.b_mbaff = 0;                                    /* encoder.c:138 */
    fc.b_direct_spatial_mv_pred = 0;                   /* encoder.c:156 */
    fc.i_cabac_init_idc = 0;                           /* encoder.c:196 */

    phx_slice_header_t sh;
    phx_slice_header_init(&sh, &h->param, &h->sps, &h->pps, &fc); /* :2791 */

    phx_bs_t bs;
    phx_bs_init(&bs, h->rbsp, h->i_bitstream);         /* encoder.c:2776 bs_realign */
    phx_slice_header_write(&bs, &h->param, &sh);       /* encoder.c:2791 */
    if (h->param.b_cabac) {
        phx_encoder_bs_align_1(&bs);                   /* encoder.c:2795 */
        phx_cabac_context_init(&h->cabac, PHX_SLICE_I,
                               sh.i_qp, sh.i_cabac_init_idc); /* :2798 */
        phx_cabac_encode_init(&h->cabac, bs.p,
                              h->rbsp + h->i_bitstream); /* :2799 */
    }

    phx_encoder_reset_mb_state(h);                     /* encoder.c:2783 */
    int last_qp = sh.i_qp;                             /* encoder.c:2804 */
    int last_dqp = 0;                                  /* encoder.c:2805 */
    phx_cavlc_t cav;
    cav.bs = &bs;
    cav.profile_idc = h->sps.i_profile_idc;            /* cavlc.c:92 */
    cav.b_overflow = 0;                                /* cavlc.c:107 */
    cav.chroma_2x4_dc = 0;                             /* cavlc.c:184 */

    phx_mb_decision_t d;
    memset(&d, 0, sizeof(d));
    d.mb_type = PHX_I_16x16;
    d.i_partition = PHX_D_16x16;
    d.intra16_pred_mode = 2;
    d.chroma_pred_mode = 0;
    d.i_qp = sh.i_qp;

    for (int mb_y = 0; mb_y < h->i_mb_height; mb_y++) { /* encoder.c:2808-2810 */
        for (int mb_x = 0; mb_x < h->i_mb_width; mb_x++) {
            int mb_xy = mb_y * h->i_mb_width + mb_x;   /* encoder.c:2814 */
            phx_mb_cache_t *c = &h->mb_cache;
            c->p_fdec = h->recon + 64 + (size_t)(mb_y*16)*PHX_FDEC_STRIDE
                        + mb_x*16;                     /* macroblock.c:533 */
            c->fdec_stride = PHX_FDEC_STRIDE;
            phx_macroblock_cache_load(h->mb_state, c, mb_x, mb_y, 0); /* :2859 */

            memset(&d, 0, sizeof(d));                  /* fresh explicit decision */
            d.i_qp = sh.i_qp;
            d.mb_type = PHX_I_16x16;
            d.i_partition = PHX_D_16x16;
            d.intra16_pred_mode = 2;
            d.chroma_pred_mode = 0;
            int rc = phx_macroblock_analyse(&h->param, h->fenc, c,
                                            mb_x, mb_y, sh.i_qp, &d); /* :2861 */
            /* phx_mbencode.c rejects I_8x8 (no 64-coefficient slot in the
             * shared decision struct) and B/P modes (no reference list).
             * Fall back to I_16x16, which is always representable, instead
             * of aborting the whole frame.  TODO(worker-mbencode): remove
             * this guard once those modes are supported. */
            if (rc < 0 || (d.mb_type != PHX_I_16x16 && d.mb_type != PHX_I_4x4 &&
                           d.mb_type != PHX_I_PCM)) {
                d.mb_type = PHX_I_16x16;
                d.i_partition = PHX_D_16x16;
                d.b_transform_8x8 = 0;
                if (d.intra16_pred_mode < 0 || d.intra16_pred_mode > 6)
                    d.intra16_pred_mode = 2;
            }
            /* phx_analysis now publishes trial coefficients at dct[phx_scan8[k]]
             * (the same scan8 cache layout phx_mbencode writes), so no stale
             * overlap slots remain.  Intra predictor fields carry x264's raw
             * mode indices (analyse.c:3739-3751); the FIX4/FIX16 mapping is
             * applied only by the CABAC/CAVLC syntax writers. */
            d.b_transform_8x8 = 0;                     /* pps transform flag is 0 below */
            rc = phx_macroblock_encode(&h->param, h->fenc, c,
                                       mb_x, mb_y, &d); /* :2865 */
            if (rc < 0) {
                /* Last resort: an empty I_16x16 MB keeps the slice parseable
                 * if the B-stage still refuses the decision. */
                memset(d.dct, 0, sizeof(d.dct));
                memset(d.nnz, 0, sizeof(d.nnz));
                d.mb_type = PHX_I_16x16;
                d.i_partition = PHX_D_16x16;
                d.intra16_pred_mode = 2;
                d.cbp_luma = d.cbp_chroma = d.i_cbp = 0;
                d.b_transform_8x8 = 0;
                d.skip = 0;
                for (int i = 0; i < 16; i++) {
                    c->non_zero_count[phx_scan8[i]] = 0;
                    c->intra4x4_pred_mode[phx_scan8[i]] = PHX_I_PRED_4x4_DC;
                }
                c->i_cbp_luma = c->i_cbp_chroma = 0;
            }
            phx_encoder_apply_decision(h, &d);         /* docs/SHADER_NOTES.md:126 */

            if (h->param.b_cabac) {
                phx_cabac_mb_t mb;
                phx_cabac_mb_init(&mb, c);             /* encoder.c:1669 */
                mb.i_type = d.mb_type;
                mb.i_partition = d.i_partition;
                for (int i = 0; i < 4; i++) mb.i_sub_partition[i] = PHX_D_L0_8x8;
                mb.i_intra16x16_pred_mode = d.intra16_pred_mode;
                mb.i_chroma_pred_mode = d.chroma_pred_mode;
                mb.i_qp = d.i_qp;
                mb.i_last_qp = last_qp;
                mb.i_last_dqp = last_dqp;
                mb.prev_type = mb_xy > 0 ? (int)h->mb_state->type[mb_xy-1] : -1;
                mb.prev_cbp = mb_xy > 0 ? (int)h->mb_state->cbp[mb_xy-1] : 0;
                mb.curr_cbp = d.i_cbp;
                mb.i_fref[0] = 1; mb.i_fref[1] = 0;
                mb.b_transform_8x8_mode = h->pps.b_transform_8x8_mode;
                mb.slice_type = PHX_SLICE_I;
                mb.i_frame = h->i_frame;
                mb.plane_count = 1; mb.chroma = 1;
                if (mb_xy > fc.i_first_mb)             /* encoder.c:2869-2870 */
                    phx_cabac_encode_terminal(&h->cabac);
                if (d.skip) {
                    phx_cabac_mb_skip(&h->cabac, &mb, 1); /* :2873 */
                } else {
                    phx_encoder_write_cabac_mb(h, &mb, &d, mb_xy); /* :2878 */
                    last_dqp = mb.i_qp - last_qp;      /* macroblock.c:1756 */
                    last_qp = mb.i_qp;                 /* macroblock.c:1757 */
                }
            } else {
                phx_encoder_write_cavlc_mb(h, &d, &cav, &last_qp); /* :2892 */
            }

            h->mb_state->type[mb_xy] = (uint8_t)d.mb_type; /* cache_save contract */
            phx_macroblock_cache_save(h->mb_state, c, 0); /* :2972 */
        }
    }

    phx_nal_t nal;
    if (h->param.b_cabac) {
        phx_cabac_encode_flush(&h->cabac, h->i_frame); /* encoder.c:3082 */
        bs.p = h->cabac.p;                             /* encoder.c:3083 */
    } else {
        phx_bs_rbsp_trailing(&bs);                     /* encoder.c:3090 */
    }
    nal.p_payload = h->rbsp;                           /* encoder.c:2124 */
    nal.i_payload = (int)(bs.p - h->rbsp);
    nal.i_ref_idc = fc.i_nal_ref_idc;                  /* encoder.c:328 */
    nal.i_type = fc.b_idr ? 5 : 1;                     /* encoder.c:236 */
    nal.b_long_startcode = 1;                          /* bitstream.c:63 */
    phx_nal_encode(h->bitstream, &nal, 1);             /* encoder.c:2124 */
    h->nal[0] = nal;                                   /* encoder.c:2130 */
    *out = h->nal;                                     /* api.c:169 */
    *nnal = 1;                                         /* api.c:170 */
    if (recon) *recon = h->recon;

    /* Explicit per-frame context advance (encoder.c:1564, :1629). */
    h->i_frame_num = (h->i_frame_num + 1) & mask;
    h->i_poc = fc.b_idr ? 2 : h->i_poc + 2;
    return nal.i_payload;                              /* encoder.c:2133 */
}

/* Mirror name (api.c:166 / encoder.c:3323).
  * SHADER: C — forwards to the serial phx_encode pipeline. */
int phx_encoder_encode(phx_t *h, const phx_picture_t *pic,
                       phx_nal_t **out, int *nnal, uint8_t **recon)
{
    return phx_encode(h, pic, out, nnal, recon);
}

/* x264_encoder_close (api.c:130 -> encoder.c:4196): cost tables (:4520),
  * frames (:4527), bitstream (:4518), handle (api.c:135).
  * SHADER: C — serial teardown. */
void phx_encoder_close(phx_t *h)
{
    phx_encoder_release(h);
}

/* Short alias (ORACLE_DESIGN.md:45); x264_encoder_close, api.c:130.
  * SHADER: C — forwards to the serial close path. */
void phx_close(phx_t *h)
{
    phx_encoder_close(h);
}
