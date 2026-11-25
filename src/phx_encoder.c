/* Phoenix public encoder API implementation.
 *
 * Port map: encoder/api.c:76/130/159/166 (dispatch), encoder/encoder.c:1507
 * (open), :1579/:1581 (set.c SPS/PPS init), :1708 (cost init), :2096 (headers),
 * :3323 (encode), :4196/:4520 (close/cost free).
 * Ported stages are annotated with their reference lines inline; stages that
 * belong to modules still being written by other workers are left as clearly
 * marked TODO call sites with stable names.
 *
 * SHADER: C — serial host pipeline; every CABAC write happens in one thread.
 */
#include "phx_encoder.h"
#include <stdlib.h>
#include <string.h>

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
    free(h->bitstream);                                    /* encoder.c:4518 */
    free(h);                                               /* api.c:135 */
}

/* x264_encoder_open (api.c:76 -> encoder.c:1507).  The state fields below are
 * initialised in x264's order: private param copy (:1516), derived params
 * (:1552/:1579/:1581), frame geometry (:1590), frame pools (:1602),
 * cost tables (:1708), CABAC contexts (:1669), output bitstream (:1724) and
 * the frame counters (:1563-1564/:1630).
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
    h->i_input = 0;                                    /* encoder.c:1630 */

    h->fenc = phx_frame_new(h->i_width, h->i_height,
                            h->param.i_bframe);        /* encoder.c:3368 / frame.c:60 */
    h->mb_state = phx_mb_state_new(h->i_mb_width, h->i_mb_height);
                                                       /* encoder.c:1590-1592 */
    h->i_bitstream = h->i_width * h->i_height * 4;      /* encoder.c:1724-1728 */
    if (h->i_bitstream < 4096) h->i_bitstream = 4096;
    h->bitstream = (uint8_t *)malloc((size_t)h->i_bitstream);
                                                       /* encoder.c:1729 */

    if (!h->fenc || !h->mb_state || !h->bitstream ||
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
 * The two NAL descriptors are split back out of that block so callers get the
 * same {p_payload,i_payload,i_ref_idc,i_type} view as x264's pp_nal[].
 * SEI (encoder.c:2118-2122) is deliberately absent: phx_headers has no SEI
 * writer yet.
 * SHADER: C — one serial header stage. */
int phx_headers(phx_t *h, phx_nal_t **out, int *nnal)
{
    if (!h || !out || !nnal) return -1;
    *out = NULL;
    *nnal = 0;

    int n = phx_encoder_headers(h->header_bytes, (int)sizeof(h->header_bytes),
                                &h->param, h->i_width, h->i_height);
    if (n < 0) return -1;

    /* SPS and PPS both use a 4-byte start code (bitstream.c:63-67).  Emulation
     * prevention (bitstream.c:29) guarantees no 00 00 01 inside escaped RBSP. */
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

/* x264_encoder_encode (api.c:166 -> encoder.c:3323).  Only the envelope is
 * portable today: "no data out" (encoder.c:3354-3356) plus the stage order
 * documented below.  Full-frame encoding is intentionally NOT faked — the
 * pending modules are owned by other workers and will be wired by the
 * orchestrator in this exact order:
 *
 *   1. put_frame / copy picture      encoder.c:3368-3426
 *        TODO(worker-frame):      phx_frame_copy_picture(h->fenc, pic);
 *        (phx_frame_init_lowres already exists: mc.c:458, phx_frame.h:67)
 *   2. lookahead                     encoder.c:3445-3454, lookahead.c:192/223
 *        TODO(worker-slicetype):  phx_lookahead_put_frame(h, h->fenc);
 *                                 phx_lookahead_get_frames(h);
 *   3. rate control start            encoder.c:3857 (ratecontrol.c:1432)
 *        TODO(worker-ratecontrol): phx_ratecontrol_start(h, ...);
 *   4. slice / MB loop               encoder.c:2855-2892
 *        phx_macroblock_cache_load(...)     -- exists: mbcache.h:90
 *        TODO(worker-analysis):   phx_macroblock_analyse(h);   encoder.c:2861
 *        TODO(worker-mbencode):   phx_macroblock_encode(h);    encoder.c:2865
 *        TODO(worker-entropy):    phx_macroblock_write_cabac(h, &h->cabac);
 *                                  cabac.c:1088
 *                                 phx_macroblock_write_cavlc(h);
 *                                  cavlc.c:487
 *        phx_macroblock_cache_save(...)     -- exists: mbcache.h:98
 *   5. rate control end              encoder.c:3978 (ratecontrol.c:1829)
 *        TODO(worker-ratecontrol): phx_ratecontrol_end(h, frame_size*8, &filler);
 *   6. NAL assembly                  encoder.c:2124
 *        TODO(worker-slice):      phx_encoder_encapsulate_nals(h);
 *        then: *out = h->nal; *nnal = h->out_i_nal; return frame_size;
 *
 * SHADER: C — the completed pipeline must remain single-threaded/seek-safe.
 */
int phx_encode(phx_t *h, const phx_picture_t *pic,
               phx_nal_t **out, int *nnal, uint8_t **recon)
{
    if (!h || !out || !nnal) return -1;
    *out = NULL;                                       /* encoder.c:3355 */
    *nnal = 0;                                         /* encoder.c:3356 */
    if (recon) *recon = NULL;
    (void)pic;                                         /* pic / flush TODO above */
    return PHX_ERR_UNIMPLEMENTED;
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
