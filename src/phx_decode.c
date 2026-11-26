/* Phoenix slice/MB decoder driver — serial inverse of the x264 write path.
 *
 * Source lines for every read are named inline.  The two entropy helpers are
 * W2/W3's stable API (src/phx_cabac_dec.h, src/phx_cavlc_dec.h); this file
 * never edits or re-declares them.  Reconstruction calls the existing class
 * A/B kernels (predict / dequant / IDCT / MC / deblock) directly.
 *
 * SHADER: C — Annex-B split, RBSP unescape, Exp-Golomb/slice-header reads and
 * the entropy stage are one ordered serial stream.
 * SHADER: A/B — each reconstruction kernel below is tagged at its call site.
 */
#include "phx_decode.h"
#include "phx_bitstream.h"
#include "phx_tables.h"
#include "phx_predict.h"
#include "phx_dct.h"
#include "phx_quant.h"
#include "phx_deblock.h"
#include "phx_mc.h"
#include "phx_chroma.h"
#include "phx_cabac_dec.h"
#include "phx_cavlc_dec.h"

#include <stdlib.h>
#include <string.h>

#define PHX_DEC_MIN(a,b) ((a)<(b)?(a):(b))
#define PHX_DEC_MAX(a,b) ((a)>(b)?(a):(b))
#define PHX_MAX(a,b) PHX_DEC_MAX(a,b)

/* common/set.h:32/34 — flat CQM indices used by the dequant kernels. */
#define PHX_CQM_4IY 0
#define PHX_CQM_4IC 2
#define PHX_LUMA_DC 48
#define PHX_CHROMA_DC 49

/* common/macroblock.h:181-187, zigzag_scan_4x4_frame — scan index -> natural
 * coefficient index (dct.c:817-821, :840).  SHADER: A — table constant. */
static const uint8_t phx_dec_zigzag4[16] = {
    (uint8_t)0,  (uint8_t)4,  (uint8_t)1,  (uint8_t)2,
    (uint8_t)5,  (uint8_t)8,  (uint8_t)12, (uint8_t)9,
    (uint8_t)6,  (uint8_t)3,  (uint8_t)7,  (uint8_t)10,
    (uint8_t)13, (uint8_t)14, (uint8_t)11, (uint8_t)15
};
/* common/macroblock.h:225-235, block_idx_xy_fdec (FDEC_STRIDE=32).  The
 * decoder stages a 32-stride window exactly like the encoder. */
static const uint16_t phx_dec_block_fdec[16] = {
      0,  4, 128, 132,   8,  12, 136, 140,
    256, 260, 384, 388, 264, 268, 392, 396
};

/* Defined below (macroblock.c:56-79); forward declarations keep the
 * reconstruction kernel readable top-to-bottom. */
static void phx_dec_idct_dequant_2x2_dc(phx_dctcoef dct[4],
                                        phx_dctcoef ac[4][16],
                                        const int dequant_mf[6][16], int qp);
static void phx_dec_idct_dequant_2x2_dconly(phx_dctcoef dct[4],
                                            const int dequant_mf[6][16],
                                            int qp);

/* ------------------------------------------------------------------ *
 * Serial bit reader (SHADER: C).  Inverse of common/bitstream.h:125,
 * :169, :221-264.  Unlike the writer this one has a hard end position:
 * a read past the RBSP returns -1/0 and sets error instead of inventing bits.
 * ------------------------------------------------------------------ */
typedef struct {
    const uint8_t *p;
    int bit;                      /* next bit, MSB first */
    int end_bit;                  /* exclusive RBSP bit limit */
    int error;
} phx_br_t;

static int phx_br_init(phx_br_t *b, const uint8_t *p, int nbytes)
{
    if (!b || !p || nbytes < 0) return -1;
    b->p = p;
    b->bit = 0;
    b->end_bit = nbytes * 8;
    b->error = 0;
    return 0;
}

static int phx_br_bit(phx_br_t *b)
{
    if (b->bit >= b->end_bit) { b->error = 1; return 0; }
    int v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1;
    b->bit++;
    return v;
}

static unsigned phx_br_bits(phx_br_t *b, int n)
{
    unsigned v = 0;
    if (n < 0 || n > 32) { b->error = 1; return 0; }
    while (n-- > 0) v = (v << 1) | (unsigned)phx_br_bit(b);
    return v;
}

static int phx_br_ue(phx_br_t *b)
{
    int zeros = 0;
    while (phx_br_bit(b) == 0) {
        if (++zeros > 31 || b->error) { b->error = 1; return -1; }
    }
    if (!zeros) return 0;
    unsigned suffix = phx_br_bits(b, zeros);
    if (b->error) return -1;
    return (int)((1u << zeros) - 1u + suffix);
}

/* bitstream.h:246-261 — se(v) codeNum -> signed value (H.264 table 9-1). */
static int phx_br_se(phx_br_t *b)
{
    int k = phx_br_ue(b);
    if (k < 0) return -1;
    return (k & 1) ? (k + 1) / 2 : -(k / 2);
}

/* bitstream.h:264 — te(v) = 1^bit for x==1, else ue(v). */
static int phx_br_te(phx_br_t *b, int x)
{
    if (x == 1) return phx_br_bit(b) ^ 1;
    return phx_br_ue(b);
}

static void phx_br_align(phx_br_t *b)
{
    int skip = (8 - (b->bit & 7)) & 7;
    b->bit += skip;
    if (b->bit > b->end_bit) { b->bit = b->end_bit; b->error = 1; }
}

/* The W3/W2 entropy readers advance a phx_bs_t directly.  These tiny local
 * mirrors read the same MSB-first bits so the driver can consume the syntax
 * elements that x264 writes outside phx_cavlc_dec_* (CBP, qp_delta, the
 * 8x8-transform flag).  SHADER: C — serial, same bit order as bitstream.h:125. */
static int phx_dec_bit_local(phx_bs_t *s)
{
    int v = (*s->p >> (7 - s->bit)) & 1;
    if (++s->bit == 8) { s->p++; s->bit = 0; }
    return v;
}

static unsigned phx_dec_bits_local(phx_bs_t *s, int n)
{
    unsigned v = 0;
    while (n-- > 0) v = (v << 1) | (unsigned)phx_dec_bit_local(s);
    return v;
}

static int phx_dec_ue_local(phx_bs_t *s)
{
    int z = 0;
    while (phx_dec_bit_local(s) == 0) {
        if (++z > 31) return -1;
    }
    return z ? (int)((1u << z) - 1u + phx_dec_bits_local(s, z)) : 0;
}

static int phx_dec_se_local(phx_bs_t *s)
{
    int k = phx_dec_ue_local(s);
    if (k < 0) return -1;
    return (k & 1) ? (k + 1) / 2 : -(k / 2);
}

/* ------------------------------------------------------------------ *
 * Annex-B / RBSP (SHADER: C, serial byte scan).
 * ------------------------------------------------------------------ */

/* bitstream.c:55-75 — a NAL begins after 00 00 01 (optionally 00 00 00 01). */
static int phx_nal_start(const uint8_t *nal, int size)
{
    if (!nal || size < 2) return -1;
    if (size >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1)
        return 4;
    if (size >= 3 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1)
        return 3;
    return 0;
}

int phx_decode_nal_info(const uint8_t *nal, int size, int *type, int *ref_idc)
{
    int k = phx_nal_start(nal, size);
    if (k < 0 || size <= k) return -1;
    uint8_t h = nal[k];
    if (h & 0x80) return -1;                          /* forbidden_zero_bit */
    int t = h & 0x1f;
    if (t == 0 || t > 12) return -1;                  /* 7.4.1: 1..12 only */
    if (type) *type = t;
    if (ref_idc) *ref_idc = (h >> 5) & 3;             /* bitstream.c:73 */
    return 0;
}

int phx_decode_annexb_nals(const uint8_t *data, int size,
                           int *types, int *offsets, int *lengths, int cap)
{
    if (!data || size <= 0 || !types || !offsets || !lengths || cap < 1)
        return -1;
    int n = 0, i = 0;
    while (i + 2 < size) {
        if (data[i] || data[i+1] || data[i+2] != 1) { i++; continue; }
        int s = i + 3;
        int e = size;
        for (int j = s; j + 2 < size; j++)
            if (!data[j] && !data[j+1] && data[j+2] == 1) { e = j; break; }
        /* 7.4.1 trailing_zero_8bits are not part of the NAL. */
        while (e > s && !data[e-1]) e--;
        if (e > s && n < cap) {
            types[n] = data[s] & 0x1f;
            offsets[n] = s;
            lengths[n] = e - s;
            n++;
        } else if (e > s) {
            return -1;
        }
        i = e;
    }
    return n;
}

/* H.264 7.3.1 / bitstream.c:29-39 — drop 0x03 after 00 00 {0,1,2,3}. */
int phx_decode_rbsp(const uint8_t *nal, int size, uint8_t *dst, int cap)
{
    int k = phx_nal_start(nal, size);
    if (k < 0 || size <= k || !dst || cap < size - k + 1) return -1;
    const uint8_t *src = nal + k + 1;                 /* skip NAL header */
    int len = size - k - 1, zeros = 0, o = 0;
    for (int i = 0; i < len; i++) {
        if (zeros >= 2 && src[i] == 3) { zeros = 0; continue; }
        dst[o++] = src[i];
        zeros = src[i] ? 0 : zeros + 1;
    }
    memset(dst + o, 0, (size_t)(cap - o));            /* CABAC needs lookahead */
    return o;
}

/* ------------------------------------------------------------------ *
 * SPS / PPS (SHADER: C).  Literal inverse of set.c:305-570, including the
 * duplicated chroma_qp_index_offset at set.c:567.
 * ------------------------------------------------------------------ */
int phx_decode_sps(const uint8_t *nal, int size, phx_sps_t *sps)
{
    int type = 0;
    if (phx_decode_nal_info(nal, size, &type, NULL) || type != 7 || !sps)
        return -1;
    uint8_t buf[512];
    int n = phx_decode_rbsp(nal, size, buf, (int)sizeof(buf));
    if (n < 4) return -1;
    phx_br_t b;
    phx_br_init(&b, buf, n);

    memset(sps, 0, sizeof(*sps));
    sps->i_profile_idc = (int)phx_br_bits(&b, 8);     /* set.c:308 */
    sps->b_constraint_set0 = phx_br_bit(&b);          /* :309 */
    sps->b_constraint_set1 = phx_br_bit(&b);          /* :310 */
    sps->b_constraint_set2 = phx_br_bit(&b);          /* :311 */
    sps->b_constraint_set3 = phx_br_bit(&b);          /* :312 */
    (void)phx_br_bits(&b, 4);                         /* :314 reserved_zero */
    sps->i_level_idc = (int)phx_br_bits(&b, 8);       /* :316 */
    sps->i_id = phx_br_ue(&b);                        /* :318 */
    if (sps->i_profile_idc >= 100) {                  /* :320-327 */
        sps->i_chroma_format_idc = phx_br_ue(&b);
        (void)phx_br_ue(&b);                          /* bit_depth_luma_minus8 */
        (void)phx_br_ue(&b);                          /* bit_depth_chroma_minus8 */
        sps->b_qpprime_y_zero_transform_bypass = phx_br_bit(&b);
        if (phx_br_bit(&b))                           /* seq_scaling_matrix_present */
            return -1;                                /* TODO: custom CQM */
    } else {
        sps->i_chroma_format_idc = 1;                 /* 7.4.2.1.1 (baseline/main) */
    }
    int lm4 = phx_br_ue(&b);                          /* :350 */
    sps->i_log2_max_frame_num = lm4 + 4;
    if (lm4 < 0 || sps->i_log2_max_frame_num > 16) return -1;
    sps->i_poc_type = phx_br_ue(&b);                  /* :351 */
    if (sps->i_poc_type == 0) {
        int lp4 = phx_br_ue(&b);                      /* :353 */
        sps->i_log2_max_poc_lsb = lp4 + 4;
        if (lp4 < 0 || sps->i_log2_max_poc_lsb > 16) return -1;
    } else if (sps->i_poc_type == 1) {
        return -1;                                    /* TODO: poc type 1 */
    }
    sps->i_num_ref_frames = phx_br_ue(&b);            /* :354 */
    sps->b_gaps_in_frame_num_value_allowed = phx_br_bit(&b); /* :355 */
    sps->i_mb_width = phx_br_ue(&b) + 1;              /* :356 */
    sps->i_mb_height = phx_br_ue(&b) + 1;             /* :357 */
    sps->b_frame_mbs_only = phx_br_bit(&b);           /* :358 */
    if (!sps->b_frame_mbs_only) {
        sps->b_mb_adaptive_frame_field = phx_br_bit(&b); /* :359-360 */
        if (sps->b_mb_adaptive_frame_field)
            return -1;                                /* TODO: MBAFF */
    }
    sps->b_direct8x8_inference = phx_br_bit(&b);      /* :361 */
    sps->b_crop = phx_br_bit(&b);                     /* :363 */
    if (sps->b_crop) {                                /* :365-369 */
        sps->crop_left  = phx_br_ue(&b) << 1;
        sps->crop_right = phx_br_ue(&b) << 1;
        sps->crop_top   = phx_br_ue(&b);
        sps->crop_bottom= phx_br_ue(&b);
    }
    sps->b_vui = phx_br_bit(&b);                      /* :374 */
    if (sps->b_vui) {                                 /* :377-471 */
        sps->vui_sar = phx_br_bit(&b) ? 1 : 0;
        if (sps->vui_sar) sps->vui_sar = phx_br_bits(&b, 8); /* :397 */
        (void)phx_br_bit(&b);                         /* overscan :405 */
        (void)phx_br_bit(&b);                         /* video_signal :409 */
        (void)phx_br_bit(&b);                         /* chroma_loc :423 */
        sps->vui_timing = phx_br_bit(&b);             /* :430 */
        if (sps->vui_timing) {
            sps->vui_num_units_in_tick = (int)phx_br_bits(&b, 32); /* :433 */
            sps->vui_time_scale = (int)phx_br_bits(&b, 32);        /* :434 */
            sps->vui_fixed_frame_rate = phx_br_bit(&b);            /* :435 */
        }
        (void)phx_br_bit(&b);                         /* nal_hrd :438 */
        (void)phx_br_bit(&b);                         /* vcl_hrd :456 */
        (void)phx_br_bit(&b);                         /* pic_struct :461 */
        sps->vui_bitstream_restriction = phx_br_bit(&b); /* :462 */
        if (sps->vui_bitstream_restriction) {
            (void)phx_br_bit(&b);                     /* mv_over_boundaries :465 */
            (void)phx_br_ue(&b);                      /* max_bytes_pic_denom */
            (void)phx_br_ue(&b);                      /* max_bits_mb_denom */
            sps->vui_log2_max_mv_length = phx_br_ue(&b);
            (void)phx_br_ue(&b);                      /* max_mv_v_length */
            sps->vui_num_reorder_frames = phx_br_ue(&b);
            sps->vui_max_dec_frame_buffering = phx_br_ue(&b);
        }
    }
    return b.error ? -1 : 0;
}

int phx_decode_pps(const uint8_t *nal, int size, phx_pps_t *pps)
{
    int type = 0;
    if (phx_decode_nal_info(nal, size, &type, NULL) || type != 8 || !pps)
        return -1;
    uint8_t buf[256];
    int n = phx_decode_rbsp(nal, size, buf, (int)sizeof(buf));
    if (n < 3) return -1;
    phx_br_t b;
    phx_br_init(&b, buf, n);

    memset(pps, 0, sizeof(*pps));
    pps->i_id = phx_br_ue(&b);                        /* set.c:508 */
    pps->i_sps_id = phx_br_ue(&b);                    /* :509 */
    pps->b_cabac = phx_br_bit(&b);                    /* :511 */
    pps->b_pic_order = phx_br_bit(&b);                /* :512 */
    pps->i_num_slice_groups = phx_br_ue(&b) + 1;     /* :513 */
    pps->i_num_ref_idx_l0 = phx_br_ue(&b) + 1;       /* :515 */
    pps->i_num_ref_idx_l1 = phx_br_ue(&b) + 1;       /* :516 */
    pps->b_weighted_pred = phx_br_bit(&b);           /* :517 */
    pps->b_weighted_bipred = (int)phx_br_bits(&b, 2);/* :518 */
    pps->i_pic_init_qp = phx_br_se(&b) + 26;         /* :520 */
    pps->i_pic_init_qs = phx_br_se(&b) + 26;         /* :521 */
    pps->i_chroma_qp_index_offset = phx_br_se(&b);   /* :522 */
    pps->b_deblocking_filter_control = phx_br_bit(&b); /* :524 */
    pps->b_constrained_intra_pred = phx_br_bit(&b);  /* :525 */
    pps->b_redundant_pic_cnt = phx_br_bit(&b);       /* :526 */
    if (b.error) return -1;
    /* set.c:529 writes the transform_8x8_mode_flag only when enabled; when it
     * is absent the very next bit is rbsp_stop_one_bit (:570).  Distinguish
     * the two by checking whether every bit after that 1 is zero. */
    int flag_pos = b.bit;
    int flag = phx_br_bit(&b);
    if (!flag) return -1;
    int only_stop = 1;
    for (int q = flag_pos + 1; q < b.end_bit; q++)
        if ((b.p[q >> 3] >> (7 - (q & 7))) & 1) { only_stop = 0; break; }
    if (!only_stop) {
        pps->b_transform_8x8_mode = 1;
        if (phx_br_bit(&b))                          /* pic_scaling_matrix_present */
            return -1;                               /* TODO: custom CQM */
        pps->i_chroma_qp_index_offset = phx_br_se(&b); /* set.c:567 */
    }
    if (pps->i_num_slice_groups > 1)
        return -1;                                    /* TODO: FMO (x264 pins 1) */
    return b.error ? -1 : 0;
}

/* ------------------------------------------------------------------ *
 * Slice header (SHADER: C) — exact inverse of encoder.c:213-364.
 * ------------------------------------------------------------------ */
int phx_decode_slice_header(const uint8_t *nal, int size,
                            const phx_sps_t *sps, const phx_pps_t *pps,
                            phx_slice_header_t *sh, int *end_bit)
{
    int type = 0, ref_idc = 0;
    if (phx_decode_nal_info(nal, size, &type, &ref_idc) ||
        (type != 1 && type != 5) || !sps || !pps || !sh) return -1;
    uint8_t buf[2048];
    int n = phx_decode_rbsp(nal, size, buf, (int)sizeof(buf));
    if (n < 2) return -1;
    phx_br_t b;
    phx_br_init(&b, buf, n);

    memset(sh, 0, sizeof(*sh));
    sh->sps = sps;
    sh->pps = pps;
    sh->i_nal_ref_idc = ref_idc;

    sh->i_first_mb = phx_br_ue(&b);                   /* encoder.c:215-223 */
    int st = phx_br_ue(&b);                           /* :225 (i_type+5) */
    if (st < 5 || st > 7) return -1;                  /* TODO: SP/SI slices */
    sh->i_type = st - 5;
    sh->i_pps_id = phx_br_ue(&b);                     /* :226 */
    if (sh->i_pps_id != pps->i_id) return -1;
    sh->i_frame_num = (int)phx_br_bits(&b, sps->i_log2_max_frame_num); /* :227 */
    if (sh->i_frame_num >= (1 << sps->i_log2_max_frame_num)) return -1;

    if (!sps->b_frame_mbs_only) {                     /* :229-234 */
        sh->b_field_pic = phx_br_bit(&b);             /* :231 */
        if (sh->b_field_pic) sh->b_bottom_field = phx_br_bit(&b); /* :233 */
    }
    int b_idr = type == 5;
    sh->i_idr_pic_id = b_idr ? phx_br_ue(&b) : -1;    /* :236-237 */
    if (sps->i_poc_type == 0) {                       /* :239 */
        sh->i_poc = (int)phx_br_bits(&b, sps->i_log2_max_poc_lsb); /* :241 */
        if (pps->b_pic_order && !sh->b_field_pic)
            sh->i_delta_poc_bottom = phx_br_se(&b);   /* :243 */
    } else if (sps->i_poc_type == 2) {
        sh->i_poc = 0;                                /* :239 false */
    }
    if (pps->b_redundant_pic_cnt)
        sh->i_redundant_pic_cnt = phx_br_ue(&b);      /* :247 */
    if (sh->i_type == PHX_SLICE_B)
        sh->b_direct_spatial_mv_pred = phx_br_bit(&b); /* :250 */

    sh->i_num_ref_idx_l0_active = pps->i_num_ref_idx_l0; /* set.c:488 */
    sh->i_num_ref_idx_l1_active = pps->i_num_ref_idx_l1; /* set.c:489 */
    if (sh->i_type == PHX_SLICE_P || sh->i_type == PHX_SLICE_B) {
        sh->b_num_ref_idx_override = phx_br_bit(&b);  /* :254 */
        if (sh->b_num_ref_idx_override) {
            sh->i_num_ref_idx_l0_active = phx_br_ue(&b) + 1; /* :257 */
            if (sh->i_type == PHX_SLICE_B)
                sh->i_num_ref_idx_l1_active = phx_br_ue(&b) + 1; /* :259 */
        }
    }
    for (int list = 0; list < 2; list++) {            /* :263-289 */
        if (list == 0 && sh->i_type == PHX_SLICE_I) continue;
        if (list == 1 && sh->i_type != PHX_SLICE_B) continue;
        sh->b_ref_pic_list_reordering[list] = phx_br_bit(&b); /* :266/:279 */
        if (sh->b_ref_pic_list_reordering[list]) {
            int count = list == 0 ? sh->i_num_ref_idx_l0_active
                                  : sh->i_num_ref_idx_l1_active;
            for (int i = 0; i < count && i < PHX_SLICE_REF_MAX; i++) {
                sh->ref_pic_list_order[list][i].idc = phx_br_ue(&b); /* :271/:284 */
                sh->ref_pic_list_order[list][i].arg = phx_br_ue(&b); /* :272/:285 */
            }
            if (phx_br_ue(&b) != 3) return -1;        /* :274/:288 end of list */
        }
    }
    if (pps->b_weighted_pred && sh->i_type == PHX_SLICE_P) {
        /* TODO: weighted prediction reconstruction.  The fields are read in
         * full so the header itself stays parseable, but the MB loop rejects
         * a slice that actually asserts a non-flat weight. */
        sh->weight[0][0].i_denom = phx_br_ue(&b);     /* :296 */
        if (sps->i_chroma_format_idc)
            sh->weight[0][1].i_denom = phx_br_ue(&b); /* :298 */
        int count = PHX_DEC_MIN(sh->i_num_ref_idx_l0_active,
                                PHX_SLICE_REF_MAX*2);  /* :295 */
        for (int i = 0; i < count; i++) {
            sh->weight[i][0].b_weighted = phx_br_bit(&b); /* :301-302 */
            if (sh->weight[i][0].b_weighted) {
                sh->weight[i][0].i_scale = phx_br_se(&b);  /* :305 */
                sh->weight[i][0].i_offset = phx_br_se(&b); /* :306 */
            }
            if (sps->i_chroma_format_idc) {
                int chroma = phx_br_bit(&b);          /* :311 */
                if (chroma) {
                    for (int j = 1; j < 3; j++) {     /* :314-318 */
                        sh->weight[i][j].i_scale = phx_br_se(&b);
                        sh->weight[i][j].i_offset = phx_br_se(&b);
                        sh->weight[i][j].b_weighted = chroma;
                    }
                }
            }
        }
    } else if (pps->b_weighted_pred && sh->i_type == PHX_SLICE_B) {
        return -1;                                    /* TODO: weighted B */
    }
    if (ref_idc != 0) {                               /* :328-348 */
        if (b_idr) {
            (void)phx_br_bit(&b);                     /* :332 no_output_of_prior_pics */
            (void)phx_br_bit(&b);                     /* :333 long_term_reference_flag */
        } else {
            int mmco_flag = phx_br_bit(&b);           /* :337 */
            if (mmco_flag) {
                for (int i = 0; i < PHX_SLICE_REF_MAX; i++) {
                    int op = phx_br_ue(&b);            /* :342 */
                    sh->mmco_diff[sh->i_mmco_command_count] = op;
                    sh->i_mmco_command_count++;
                    if (op == 0) break;                /* end of commands :345 */
                    if (op == 1 || op == 3)
                        sh->mmco_diff[sh->i_mmco_command_count - 1] =
                            phx_br_ue(&b) + 1;        /* :343 (writer stores minus1) */
                    if (i == PHX_SLICE_REF_MAX-1) return -1;
                }
            }
        }
    }
    if (pps->b_cabac && sh->i_type != PHX_SLICE_I)
        sh->i_cabac_init_idc = phx_br_ue(&b);         /* :350-351 */
    sh->i_qp_delta = phx_br_se(&b);                   /* :353 */
    sh->i_qp = pps->i_pic_init_qp + sh->i_qp_delta;   /* encoder.c:198-199 */
    if (sh->i_qp < 0 || sh->i_qp > PHX_QP_MAX_SPEC) return -1;
    if (pps->b_deblocking_filter_control) {           /* :355-361 */
        sh->i_disable_deblocking_filter_idc = phx_br_ue(&b); /* :357 */
        if (sh->i_disable_deblocking_filter_idc != 1) {
            sh->i_alpha_c0_offset = phx_br_se(&b) * 2; /* :360 (writer >>1) */
            sh->i_beta_offset = phx_br_se(&b) * 2;     /* :361 */
        }
    }
    if (pps->i_num_slice_groups > 1)                  /* H.264 7.3.3 */
        sh->i_slice_group_change_cycle =
            (unsigned)phx_br_bits(&b, phx_slice_group_change_bits(sps));
    if (b.error) return -1;
    if (end_bit) *end_bit = b.bit;
    return 0;
}

/* ------------------------------------------------------------------ *
 * CAVLC MB syntax (SHADER: C) — uses W3's stable phx_cavlc_dec_* API.
 * ------------------------------------------------------------------ */

/* cavlc.c:36-46, phx_cavlc_cbp_to_golomb[1][intra][cbp] inverse. */
static int phx_dec_cbp_code(int code, int intra)
{
    const uint8_t (*tab)[48] = phx_cavlc_cbp_to_golomb[1];
    for (int cbp = 0; cbp < 48; cbp++)
        if (tab[intra ? 1 : 0][cbp] == (uint8_t)code) return cbp;
    return -1;
}

/* common/macroblock.h:432-441 + cavlc.c:198-204 — nC for coeff_token.
 * SHADER: C — gather from the explicit neighbour cache. */
static int phx_dec_nc(const phx_mb_cache_t *c, int idx)
{
    int za = c->non_zero_count[phx_scan8[idx] - 1];
    int zb = c->non_zero_count[phx_scan8[idx] - 8];
    int sum = za + zb;
    if (sum < 0x80) sum = (sum + 1) >> 1;
    return phx_cavlc_ct_index[sum & 0x7f];
}

/* Count the coefficients a residual decoder actually emitted; x264 stores
 * that total in non_zero_count (cavlc.c:208). */
static int phx_dec_nnz(const int16_t *coef, int n)
{
    int nnz = 0;
    for (int i = 0; i < n; i++) nnz += coef[i] != 0;
    return nnz;
}

static void phx_dec_store_luma(phx_mb_decision_t *d, phx_mb_cache_t *c,
                               int idx, const int16_t *coef, int n, int off)
{
    int slot = phx_scan8[idx];
    memset(d->dct[slot], 0, sizeof(d->dct[slot]));
    for (int k = 0; k < n; k++) d->dct[slot][k+off] = coef[k];
    d->nnz[slot] = (uint8_t)phx_dec_nnz(coef, n);
    c->non_zero_count[slot] = d->nnz[slot];
}

static void phx_dec_store_chroma(phx_mb_decision_t *d, phx_mb_cache_t *c,
                                 int ch, int block, const int16_t *coef, int n,
                                 int off)
{
    memset(d->chroma_dct[ch][block], 0, sizeof(d->chroma_dct[ch][block]));
    for (int k = 0; k < n; k++) d->chroma_dct[ch][block][k+off] = coef[k];
    d->chroma_nnz[ch][block] = (uint8_t)phx_dec_nnz(coef, n);
    /* The neighbour cache uses the scan8 positions (phx_scan8[16..49] up to
     * 119).  phx_mb_decision_t::nnz has only 48 entries, so keep the explicit
     * chroma arrays authoritative there and store the 4:2:0 AC flags at the
     * compact 16+ch*16+i8 indices (phx_mb_decision.h:38-39). */
    int cache_slot = block == 0 ? PHX_CHROMA_DC + ch
                                : 16 + ch*16 + (block - 1);
    c->non_zero_count[phx_scan8[cache_slot]] = d->chroma_nnz[ch][block];
    if (block != 0)
        d->nnz[16 + ch*16 + (block - 1)] = d->chroma_nnz[ch][block];
}

/* x264_transform_allowed, common/macroblock.h:445-461.  The transform-size
 * bin is only present when this returns nonzero.  SHADER: C (syntax gate). */
static int phx_dec_transform_allowed(int mb_type)
{
    switch (mb_type) {
    case PHX_I_8x8: case PHX_P_L0: case PHX_P_8x8:
    case PHX_B_DIRECT: case PHX_B_L0_L0: case PHX_B_L0_L1: case PHX_B_L0_BI:
    case PHX_B_L1_L0: case PHX_B_L1_L1: case PHX_B_L1_BI:
    case PHX_B_BI_L0: case PHX_B_BI_L1: case PHX_B_BI_BI: case PHX_B_8x8:
        return (mb_type == PHX_I_8x8 || mb_type == PHX_P_8x8) ? 2 : 1;
    default:
        return 0;                              /* I_4x4 / I_16x16 / P_SKIP */
    }
}

/* Read one MB of CAVLC syntax into `d` and the borrowed neighbour cache.
 * The exact element order is cavlc.c:487-595.  `last_qp` is the previous
 * MB's QP (cavlc.c:214) and is updated on return.  SHADER: C. */
static int phx_dec_mb_cavlc_syntax(phx_bs_t *bs, phx_mb_cache_t *c,
                                   phx_mb_decision_t *d,
                                   const phx_slice_header_t *sh,
                                   const phx_pps_t *pps,
                                   const phx_sps_t *sps,
                                   int *last_qp)
{
    phx_cavlc_mb_t mb;
    phx_cavlc_mb_init(&mb, bs, c);              /* phx_cavlc_syntax.h:103 */
    mb.slice_type = sh->i_type;                 /* phx_cavlc_syntax.h:98 */
    mb.chroma = sps->i_chroma_format_idc != 0;  /* :99 (4:2:0/4:2:2) */
    mb.i_fref[0] = sh->i_num_ref_idx_l0_active; /* :94 */
    mb.i_fref[1] = sh->i_num_ref_idx_l1_active;
    mb.b_transform_8x8_mode = pps->b_transform_8x8_mode; /* :93 */
    mb.b_psub8x8 = 1;                           /* TODO: expose param.analyse.inter */
    if (sh->i_type == PHX_SLICE_B)
        return -1;                              /* TODO: B-direct reconstruction */
    if (pps->b_weighted_pred)
        return -1;                              /* TODO: weighted prediction */

    if (phx_cavlc_dec_mb_header(bs, &mb) < 0)   /* W3: cavlc.c:538-543 */
        return -1;

    memset(d, 0, sizeof(*d));
    d->mb_type = mb.i_type;
    d->i_partition = mb.i_partition;
    d->b_transform_8x8 = mb.b_transform_8x8;
    d->i_qp = *last_qp;
    d->intra16_pred_mode = mb.i_intra16x16_pred_mode;
    d->chroma_pred_mode = mb.i_chroma_pred_mode;
    for (int i = 0; i < 16; i++)
        d->intra4x4_pred_mode[i] =
            (int8_t)c->intra4x4_pred_mode[phx_scan8[i]];
    for (int i = 0; i < 16; i++) {
        int slot = phx_scan8[i];
        for (int l = 0; l < 2; l++) {
            d->mv[l][i][0] = c->mv[l][slot][0];
            d->mv[l][i][1] = c->mv[l][slot][1];
            d->ref[l][i] = (int8_t)c->ref[l][slot];
        }
    }

    d->cbp_luma = mb.i_cbp_luma;
    d->cbp_chroma = mb.i_cbp_chroma;
    if (mb.i_type != PHX_I_16x16) {
        int code = phx_dec_ue_local(bs);        /* cavlc.c:550-552 */
        if (code < 0) return -1;
        int cbp = phx_dec_cbp_code(code, mb.i_type == PHX_I_4x4 ||
                                        mb.i_type == PHX_I_8x8);
        if (cbp < 0) return -1;
        d->cbp_luma = cbp & 15;
        d->cbp_chroma = cbp >> 4;
    }
    mb.i_cbp_luma = d->cbp_luma;
    mb.i_cbp_chroma = d->cbp_chroma;
    c->i_cbp_luma = d->cbp_luma;
    c->i_cbp_chroma = d->cbp_chroma;

    if (pps->b_transform_8x8_mode && d->cbp_luma &&
        phx_dec_transform_allowed(mb.i_type)) {
        mb.b_transform_8x8 = phx_dec_bit_local(bs); /* cavlc.c:554-556 */
        d->b_transform_8x8 = mb.b_transform_8x8;
    }
    c->b_transform_8x8 = mb.b_transform_8x8;

    /* cavlc.c:211-239 — mb_qp_delta is a wrapped se(v). */
    if (mb.i_type == PHX_I_16x16 || (d->cbp_luma | d->cbp_chroma)) {
        int dqp = phx_dec_se_local(bs);
        if (dqp < 0) return -1;
        if (dqp < -(PHX_QP_MAX_SPEC+1)/2) dqp += PHX_QP_MAX_SPEC+1;
        else if (dqp > PHX_QP_MAX_SPEC/2) dqp -= PHX_QP_MAX_SPEC+1;
        int qp = *last_qp + dqp;
        if (qp < 0 || qp > PHX_QP_MAX_SPEC) return -1;
        *last_qp = qp;
        d->i_qp = qp;
    }
    d->i_cbp = (d->cbp_chroma << 4) | d->cbp_luma;

    int16_t coef[16];
    if (mb.i_type == PHX_I_16x16) {
        if (phx_cavlc_dec_residual(bs, coef, PHX_DCT_LUMA_DC,
                                   phx_dec_nc(c, 0), 0, 0) < 0) return -1;
        phx_dec_store_luma(d, c, PHX_LUMA_DC, coef, 16, 0);
        for (int i8 = 0; i8 < 4; i8++)
            if (d->cbp_luma & (1 << i8))
                for (int j = 0; j < 4; j++) {
                    int idx = i8*4 + j;
                    if (phx_cavlc_dec_residual(bs, coef, PHX_DCT_LUMA_AC,
                                               phx_dec_nc(c, idx), 0, 0) < 0)
                        return -1;
                    phx_dec_store_luma(d, c, idx, coef, 15, 1);
                }
    } else if (d->cbp_luma | d->cbp_chroma) {
        for (int i8 = 0; i8 < 4; i8++)
            if (d->cbp_luma & (1 << i8))
                for (int j = 0; j < 4; j++) {
                    int idx = i8*4 + j;
                    if (phx_cavlc_dec_residual(bs, coef, PHX_DCT_LUMA_4x4,
                                               phx_dec_nc(c, idx), 0, 0) < 0)
                        return -1;
                    phx_dec_store_luma(d, c, idx, coef, 16, 0);
                }
    }
    if (d->cbp_chroma) {
        for (int ch = 0; ch < 2; ch++) {
            if (phx_cavlc_dec_residual(bs, coef, PHX_DCT_CHROMA_DC,
                                       4, 1, 0) < 0) return -1;
            phx_dec_store_chroma(d, c, ch, 0, coef, 4, 0);
        }
        if (d->cbp_chroma == 2)
            /* cavlc.c:585-588 — U0..U3 then V0..V3 (step 8<<CHROMA_V_SHIFT). */
            for (int ch = 0; ch < 2; ch++)
                for (int i8 = 0; i8 < 4; i8++) {
                    int idx = 16 + ch*16 + i8;
                    if (phx_cavlc_dec_residual(bs, coef, PHX_DCT_CHROMA_AC,
                                               phx_dec_nc(c, idx), 0, 0) < 0)
                        return -1;
                    phx_dec_store_chroma(d, c, ch, 1+i8, coef, 15, 1);
                }
    }
    (void)sh;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Reconstruction (SHADER: A/B) — the existing predictor / dequant /
 * IDCT / MC / deblock kernels, driven by phx_mb_decision_t.
 * ------------------------------------------------------------------ */

static void phx_dec_dezigzag16(phx_dctcoef nat[16], const int16_t zz[16])
{
    memset(nat, 0, 16 * sizeof(phx_dctcoef));
    for (int k = 0; k < 16; k++) nat[phx_dec_zigzag4[k]] = zz[k];
}

/* x264's scalar predict_4x4_v replicates each column's own top sample;
 * phx_predict.c:426 broadcasts only SRC(0,-1).  phx_mbencode.c:263 installs
 * the same authoritative form over the table slot.  SHADER: A. */
static void phx_dec_pred4_v(uint8_t *dst, int stride)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            dst[x + y*stride] = dst[x - stride];
}

/* Clip an MV to the picture edges exactly like macroblock.c:659. */
static void phx_dec_clip_mv(const phx_frame_t *f, int px, int py,
                            int w, int h, int *mx, int *my)
{
    int xmin = 4 * (-px - 24), xmax = 4 * (f->i_width - px - w + 24);
    int ymin = 4 * (-py - 24), ymax = 4 * (f->i_height - py - h + 24);
    if (*mx < xmin) *mx = xmin; else if (*mx > xmax) *mx = xmax;
    if (*my < ymin) *my = ymin; else if (*my > ymax) *my = ymax;
}

/* Luma reconstruction for one MB.  `ref` is NULL for an intra MB or for an
 * IDR with no usable reference (which is rejected before this point). */
static void phx_dec_recon_luma(phx_frame_t *f, phx_mb_cache_t *c,
                               phx_mb_decision_t *d,
                               const phx_quant_tables_t *qt,
                               const phx_frame_t *ref, int qp)
{
    const int mb_x = c->i_mb_x, mb_y = c->i_mb_y;
    const int stride = f->i_stride;
    uint8_t *dst = f->plane + mb_y*16*stride + mb_x*16;
    int intra = d->mb_type == PHX_I_4x4 || d->mb_type == PHX_I_8x8 ||
                d->mb_type == PHX_I_16x16 || d->mb_type == PHX_I_PCM;
    phx_dctcoef nat[16];

    if (intra) {
        if (d->mb_type == PHX_I_16x16) {
            phx_predict_t pred16[7];
            phx_predict_16x16_init(pred16);             /* predict.c:889 */
            int mode = PHX_DEC_MIN(d->intra16_pred_mode, 6);
            if (mode < 0) mode = 2;
            pred16[mode](dst, stride);                  /* macroblock.c:142 */
        } else if (d->mb_type == PHX_I_4x4) {
            phx_predict_t pred4[12];
            phx_predict_4x4_init(pred4);                /* predict.c:1042 */
            pred4[0] = phx_dec_pred4_v;                 /* predict.c:508 */
            for (int i = 0; i < 16; i++) {
                uint8_t *p = dst + phx_dec_block_fdec[i];
                int mode = PHX_DEC_MIN(d->intra4x4_pred_mode[i], 11);
                if (mode < 0) mode = 2;
                if (!(c->i_neighbour_intra & PHX_MB_TOPRIGHT) &&
                     (c->i_neighbour_intra & PHX_MB_TOP))
                    for (int k = 0; k < 4; k++)
                        p[4+k-stride] = p[3-stride];     /* macroblock.c:761-763 */
                pred4[mode](p, stride);                  /* macroblock.h:144 */
            }
        } else {
            /* I_8x8 uses the filtered-edge 8x8 predictors; phx_mb_decision_t
             * cannot carry 64 coefficients per block.  Explicitly unsupported. */
            return;
        }
    } else if (ref) {
        /* Motion compensation, macroblock.c:61-64 / 568-640.  SHADER: B. */
        if (d->mb_type == PHX_P_SKIP || d->i_partition == PHX_D_16x16) {
            int mx = d->mv[0][0][0], my = d->mv[0][0][1];
            phx_dec_clip_mv(f, mb_x*16, mb_y*16, 16, 16, &mx, &my);
            phx_mc_luma_mb(ref, dst, stride, mb_x, mb_y, mx, my, 16, 16);
        } else if (d->i_partition == PHX_D_16x8 ||
                   d->i_partition == PHX_D_8x16) {
            int n = d->i_partition == PHX_D_16x8 ? 2 : 2;
            for (int k = 0; k < n; k++) {
                int bx = d->i_partition == PHX_D_8x16 ? 8*k : 0;
                int by = d->i_partition == PHX_D_16x8 ? 8*k : 0;
                int w = d->i_partition == PHX_D_8x16 ? 8 : 16;
                int h = d->i_partition == PHX_D_16x8 ? 8 : 16;
                int idx = d->i_partition == PHX_D_16x8 ? 8*k : 4*k;
                int mx = d->mv[0][idx][0], my = d->mv[0][idx][1];
                phx_dec_clip_mv(f, mb_x*16+bx, mb_y*16+by, w, h, &mx, &my);
                phx_mc_luma_mb(ref, dst + by*stride + bx, stride,
                               mb_x, mb_y, mx, my, w, h);
            }
        } else {
            /* P_8x8 / sub-8x8 partitions: per-4x4 MC.  SHADER: B. */
            for (int i = 0; i < 16; i++) {
                int bx = (i & 3) * 4, by = (i >> 2) * 4;
                int mx = d->mv[0][i][0], my = d->mv[0][i][1];
                phx_dec_clip_mv(f, mb_x*16+bx, mb_y*16+by, 4, 4, &mx, &my);
                phx_mc_luma_mb(ref, dst + by*stride + bx, stride,
                               mb_x, mb_y, mx, my, 4, 4);
            }
        }
    }

    /* Quantized coefficients -> dequant -> IDCT (dct.c:272/312/320,
     * quant.c:112/130 and encoder/macroblock.c:155-164).  SHADER: A. */
    if (d->mb_type == PHX_I_16x16) {
        phx_dctcoef dc[16], blocks[16][16];
        phx_dec_dezigzag16(dc, d->dct[phx_scan8[PHX_LUMA_DC]]);
        phx_idct4x4dc(dc);                             /* dct.c:78 */
        phx_dequant_4x4_dc(dc, qt->dequant4_mf[PHX_CQM_4IY], qp); /* quant.c:130 */
        int any_ac = 0;
        for (int i = 0; i < 16; i++) {
            phx_dec_dezigzag16(blocks[i], d->dct[phx_scan8[i]]);
            any_ac |= d->nnz[phx_scan8[i]] != 0;
            if (d->nnz[phx_scan8[i]])
                phx_dequant_4x4(blocks[i], qt->dequant4_mf[PHX_CQM_4IY], qp);
        }
        static const uint8_t idx1d[16] = {
            0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15
        };
        for (int i = 0; i < 16; i++) blocks[i][0] = dc[idx1d[i]];
        if (any_ac)
            phx_add16x16_idct(dst, stride, blocks);    /* dct.c:320 */
        else
            phx_add16x16_idct_dc(dst, stride, dc);     /* dct.c:462 */
    } else if (d->cbp_luma) {
        for (int i = 0; i < 16; i++) {
            int slot = phx_scan8[i];
            if (!d->nnz[slot]) continue;
            phx_dec_dezigzag16(nat, d->dct[slot]);
            phx_dequant_4x4(nat, qt->dequant4_mf[PHX_CQM_4IY], qp);
            phx_add4x4_idct(dst + phx_dec_block_fdec[i], stride, nat);
        }
    }
}

/* Chroma reconstruction into planar U/V scratch with a one-pixel border so
 * phx_predict_8x8c_* can read neighbours.  SHADER: A/B. */
static void phx_dec_recon_chroma(uint8_t *pu, uint8_t *pv, int cs,
                                 phx_mb_cache_t *c, phx_mb_decision_t *d,
                                 const phx_quant_tables_t *qt,
                                 int qp, int chroma_offset,
                                 const phx_frame_t *ref)
{
    const int mb_x = c->i_mb_x, mb_y = c->i_mb_y;
    uint8_t *u = pu + (mb_y*8+1)*cs + mb_x*8 + 1;
    uint8_t *v = pv + (mb_y*8+1)*cs + mb_x*8 + 1;
    int intra = d->mb_type == PHX_I_4x4 || d->mb_type == PHX_I_8x8 ||
                d->mb_type == PHX_I_16x16 || d->mb_type == PHX_I_PCM;

    if (intra) {
        phx_predict_t pred8[7];
        phx_predict_8x8c_init(pred8);                   /* predict.c:936 */
        int mode = PHX_DEC_MIN(d->chroma_pred_mode, 6);
        if (mode < 0) mode = 0;
        pred8[mode](u, cs);                             /* macroblock.c:934 */
        pred8[mode](v, cs);                             /* macroblock.c:935 */
    } else if (ref && ref->plane_chroma) {
        int mx = d->mv[0][0][0], my = d->mv[0][0][1];
        phx_dec_clip_mv(ref, mb_x*16, mb_y*16, 16, 16, &mx, &my);
        const uint8_t *src = ref->plane_chroma
                           + (mb_y*8)*ref->i_stride_planes[1] + mb_x*16;
        phx_mc_chroma(u, cs, v, cs, src, ref->i_stride_planes[1],
                      mx, my >> 1, 8, 8);              /* mc.c:252-283 */
    }

    if (!d->cbp_chroma) return;
    int qpi = PHX_DEC_MIN(PHX_CHROMA_QP_BASE_OFFSET + chroma_offset + qp, 75);
    int cqp = phx_chroma_qp_base[qpi] + PHX_MAX(qp - PHX_QP_MAX_SPEC, 0);
    phx_dctcoef dc[4], ac[4][16];
    for (int ch = 0; ch < 2; ch++) {
        uint8_t *p = ch ? v : u;
        for (int k = 0; k < 4; k++) dc[k] = d->chroma_dct[ch][0][k];
        int nz_ac = 0;
        for (int k = 0; k < 4; k++) {
            phx_dec_dezigzag16(ac[k], d->chroma_dct[ch][1+k]);
            nz_ac |= d->chroma_nnz[ch][1+k] != 0;
            if (d->chroma_nnz[ch][1+k])
                phx_dequant_4x4(ac[k], qt->dequant4_mf[PHX_CQM_4IC], cqp);
        }
        if (d->chroma_nnz[ch][0]) {
            if (nz_ac)
                phx_dec_idct_dequant_2x2_dc(dc, ac, qt->dequant4_mf[PHX_CQM_4IC],
                                            cqp);        /* macroblock.c:478 */
            else
                phx_dec_idct_dequant_2x2_dconly(dc, qt->dequant4_mf[PHX_CQM_4IC],
                                                cqp);    /* macroblock.c:458 */
        }
        if (nz_ac)
            phx_add8x8_idct(p, cs, ac);                 /* dct.c:312 */
        else if (d->chroma_nnz[ch][0])
            phx_add8x8_idct_dc(p, cs, dc);              /* dct.c:454 */
    }
}

/* chunk-local mirrors of macroblock.c:56-79 (the phx_quant.h declarations of
 * these two symbols have no definition in phx_quant.c).  SHADER: A — one
 * invocation per chroma DC block, pure arithmetic. */
static void phx_dec_idct_dequant_2x2_dc(phx_dctcoef dct[4],
                                        phx_dctcoef ac[4][16],
                                        const int dequant_mf[6][16], int qp)
{
    int d0 = dct[0] + dct[1], d1 = dct[2] + dct[3];      /* macroblock.c:57-58 */
    int d2 = dct[0] - dct[1], d3 = dct[2] - dct[3];      /* :59-60 */
    int dmf = dequant_mf[qp%6][0] << (qp/6);             /* :61 */
    ac[0][0] = (phx_dctcoef)((d0 + d1) * dmf >> 5);      /* :66 */
    ac[1][0] = (phx_dctcoef)((d0 - d1) * dmf >> 5);      /* :67 */
    ac[2][0] = (phx_dctcoef)((d2 + d3) * dmf >> 5);      /* :68 */
    ac[3][0] = (phx_dctcoef)((d2 - d3) * dmf >> 5);      /* :69 */
}

static void phx_dec_idct_dequant_2x2_dconly(phx_dctcoef dct[4],
                                            const int dequant_mf[6][16],
                                            int qp)
{
    int d0 = dct[0] + dct[1], d1 = dct[2] + dct[3];
    int d2 = dct[0] - dct[1], d3 = dct[2] - dct[3];
    int dmf = dequant_mf[qp%6][0] << (qp/6);
    dct[0] = (phx_dctcoef)((d0 + d1) * dmf >> 5);        /* :75 */
    dct[1] = (phx_dctcoef)((d0 - d1) * dmf >> 5);        /* :76 */
    dct[2] = (phx_dctcoef)((d2 + d3) * dmf >> 5);        /* :77 */
    dct[3] = (phx_dctcoef)((d2 - d3) * dmf >> 5);        /* :78 */
}

/* Copy the planar reconstruction into the NV12 chroma plane of `f`. */
static void phx_dec_store_chroma_planes(phx_frame_t *f, const uint8_t *pu,
                                        const uint8_t *pv, int cs)
{
    const int mb_w = f->i_width / 16, mb_h = f->i_height / 16;
    for (int y = 0; y < mb_h*8; y++)
        for (int x = 0; x < mb_w*8; x++) {
            uint8_t *dst = f->plane_chroma + y*f->i_stride_planes[1] + 2*x;
            dst[0] = pu[(y+1)*cs + x+1];
            dst[1] = pv[(y+1)*cs + x+1];
        }
}

/* ------------------------------------------------------------------ *
 * NAL / frame driver.
 * ------------------------------------------------------------------ */

/* Parameter context for the exact required phx_decode_nal() signature (which
 * carries no SPS/PPS argument).  phx_decode_annexb() installs it while walking
 * the parameter sets; standalone callers use phx_decode_set_params(). */
static phx_sps_t phx_dec_current_sps;
static phx_pps_t phx_dec_current_pps;
static int phx_dec_have_params = 0;

int phx_decode_set_params(const phx_sps_t *sps, const phx_pps_t *pps)
{
    if (!sps || !pps) return -1;
    phx_dec_current_sps = *sps;
    phx_dec_current_pps = *pps;
    phx_dec_have_params = 1;
    return 0;
}

/* Decode one slice with explicit parameter sets and an optional list-0
 * reference frame.  SHADER: C around the entropy stage; A/B for reconstruction. */
int phx_decode_nal_ex(const uint8_t *nal, int size,
                      const phx_sps_t *sps, const phx_pps_t *pps,
                      const phx_frame_t *ref,
                      phx_frame_t *frame, phx_mb_state_t *state,
                      phx_mb_decision_t *decisions, int *ndecisions)
{
    if (!nal || size < 2 || !sps || !pps || !frame || !state) return -1;
    if (!frame->plane || !frame->plane_chroma || !state->type) return -1;
    if (frame->i_width != sps->i_mb_width*16 ||
        frame->i_height != sps->i_mb_height*16) return -1;
    if (state->mb_width != sps->i_mb_width ||
        state->mb_height != sps->i_mb_height) return -1;

    phx_slice_header_t sh;
    int end_bit = -1;
    if (phx_decode_slice_header(nal, size, sps, pps, &sh, &end_bit))
        return -1;
    if (sh.b_mbaff || sh.b_field_pic) return -1;         /* TODO: MBAFF / field */
    if (sps->i_poc_type == 1) return -1;                 /* TODO: POC type 1 */
    if (sh.i_type == PHX_SLICE_B) return -1;             /* TODO: B-direct */
    if (sh.b_ref_pic_list_reordering[0] ||
        sh.i_mmco_command_count) return -1;              /* TODO: RPL/MMCO DPB */
    if (sh.i_first_mb != 0) return -1;                   /* TODO: multi-slice frames */
    if (sh.i_type == PHX_SLICE_P && !ref) return -1;     /* no reference available */

    int mb_count = sps->i_mb_width * sps->i_mb_height;
    state->slice_type = sh.i_type;
    state->b_cabac = 0;
    state->first_mb = sh.i_first_mb;
    state->b_constrained_intra = pps->b_constrained_intra_pred;
    for (int i = 0; i < mb_count; i++) {
        state->type[i] = 0;
        state->cbp[i] = 0;
        state->mb_transform_size[i] = 0;
        state->slice_table[i] = 0;
        state->field[i] = 0;
        memset(state->non_zero_count[i], 0, sizeof(state->non_zero_count[i]));
        memset(state->intra4x4_pred_mode[i], -1,
               sizeof(state->intra4x4_pred_mode[i]));
        for (int l = 0; l < 2; l++) {
            memset(state->ref[l] + 4*i, -1, 4);
            memset(state->mv[l][i], 0, sizeof(state->mv[l][i]));
        }
    }

    /* x264's FDEC border starts at 128 (common/predict.c:100/176 DC_128 and
     * common/macroblock.c:501-548 neighbour windows); unavailable neighbours
     * must read 128, not the zeroed calloc padding.  SHADER: A. */
    {
        uint8_t *p = frame->plane;
        int s = frame->i_stride, w = frame->i_width, h = frame->i_height;
        for (int y = -PHX_PADV; y < h + PHX_PADV; y++)
            memset(p + y*s - PHX_PADH, 128, (size_t)(w + 2*PHX_PADH));
    }

    int rbsp_cap = size + 32;
    uint8_t *rbsp = (uint8_t *)calloc((size_t)rbsp_cap, 1);
    if (!rbsp) return -1;
    int rbsp_n = phx_decode_rbsp(nal, size, rbsp, rbsp_cap);
    if (rbsp_n < 0) { free(rbsp); return -1; }

    /* CABAC: encoder.c:2795 byte-aligns the header, then W2's arithmetic
     * decoder is initialised with the real slice QP/type/init_idc.  The CABAC
     * *syntax* tree (cabac.c:917-1096) is still a TODO, so decoding stops
     * here rather than guessing bins. */
    if (pps->b_cabac) {
        int cabac_byte = (end_bit + 7) >> 3;
        phx_cabac_dec_t cb;
        if (cabac_byte >= rbsp_n || phx_cabac_dec_init(&cb, rbsp + cabac_byte,
                                                       rbsp_n - cabac_byte,
                                                       sh.i_qp, sh.i_type,
                                                       sh.i_cabac_init_idc))
            { free(rbsp); return -1; }
        free(rbsp);
        return -1;                         /* TODO: CABAC MB syntax bins */
    }

    phx_bs_t bs;
    bs.p_start = rbsp;
    bs.p = rbsp + (end_bit >> 3);
    bs.bit = end_bit & 7;

    /* Planar chroma reconstruction buffer (one-pixel border), interleaved
     * into the NV12 frame plane once all MBs have been decoded. */
    int cs = sps->i_mb_width*8 + 2;
    int ch_rows = sps->i_mb_height*8 + 2;
    size_t csz = (size_t)cs * ch_rows;
    uint8_t *pcu = (uint8_t *)calloc(csz, 1);
    uint8_t *pcv = (uint8_t *)calloc(csz, 1);
    if (!pcu || !pcv) { free(rbsp); free(pcu); free(pcv); return -1; }
    for (int y = 0; y < ch_rows; y++) {
        pcu[y*cs] = pcu[y*cs+1] = 128;
        pcv[y*cs] = pcv[y*cs+1] = 128;
        pcu[y*cs+cs-1] = 128; pcv[y*cs+cs-1] = 128;
    }
    for (int x = 0; x < cs; x++) {
        pcu[x] = pcu[cs+x] = 128;
        pcv[x] = pcv[cs+x] = 128;
        pcu[(ch_rows-1)*cs+x] = pcv[(ch_rows-1)*cs+x] = 128;
    }

    phx_quant_tables_t qt;
    phx_quant_tables_init(&qt);                         /* set.c:73-205 */
    phx_mb_cache_t cache;
    phx_mb_decision_t local;
    int last_qp = sh.i_qp;
    int skip_run = 0, written = 0, rc = -1;
    int chroma_off = pps->i_chroma_qp_index_offset;

    for (int mb_y = 0; mb_y < sps->i_mb_height; mb_y++) {
        for (int mb_x = 0; mb_x < sps->i_mb_width; mb_x++) {
            int xy = mb_y*sps->i_mb_width + mb_x;
            phx_mb_decision_t *d = decisions ? &decisions[xy] : &local;
            memset(d, 0, sizeof(*d));
            phx_macroblock_cache_load(state, &cache, mb_x, mb_y, 0);
            cache.p_fdec = frame->plane + mb_y*16*frame->i_stride + mb_x*16;
            cache.fdec_stride = frame->i_stride;
            cache.i_qp = last_qp;

            if (skip_run > 0) {                          /* encoder.c:2883-2890 */
                skip_run--;
                d->mb_type = PHX_P_SKIP;
                d->i_partition = PHX_D_16x16;
                d->i_qp = last_qp;
                d->chroma_pred_mode = 0;
                memset(d->mv, 0, sizeof(d->mv));
                memset(d->ref, 0, sizeof(d->ref));
                state->type[xy] = PHX_P_SKIP;
            } else {
                /* mb_skip_run only exists in P/SP slices (H.264 7.3.5,
                 * encoder.c:2883-2890); an I slice starts directly at mb_type. */
                skip_run = sh.i_type == PHX_SLICE_P ? phx_dec_ue_local(&bs) : 0;
                if (skip_run < 0 || skip_run > mb_count) goto done;
                if (skip_run > 0) {
                    skip_run--;
                    d->mb_type = PHX_P_SKIP;
                    d->i_partition = PHX_D_16x16;
                    d->i_qp = last_qp;
                    state->type[xy] = PHX_P_SKIP;
                    memset(d->mv, 0, sizeof(d->mv));
                    memset(d->ref, 0, sizeof(d->ref));
                } else {
                    if (phx_dec_mb_cavlc_syntax(&bs, &cache, d, &sh, pps, sps,
                                                &last_qp)) goto done;
                    state->type[xy] = d->mb_type;
                }
            }
            cache.i_cbp_luma = d->cbp_luma;
            cache.i_cbp_chroma = d->cbp_chroma;
            cache.b_transform_8x8 = d->b_transform_8x8;
            phx_dec_recon_luma(frame, &cache, d, &qt,
                               sh.i_type == PHX_SLICE_P ? ref : NULL,
                               d->i_qp);
            phx_dec_recon_chroma(pcu, pcv, cs, &cache, d, &qt, d->i_qp,
                                 chroma_off, sh.i_type == PHX_SLICE_P ? ref : NULL);
            /* phx_mb_state_t::intra_border_backup is only 16 bytes wide; the
             * ported backup_intra() writes at mb_x*16 and would overflow it.
             * The decoder predicts from the reconstructed plane directly, so
             * that MBAFF backup path is not needed here. */
            cache.p_fdec = NULL;
            phx_macroblock_cache_save(state, &cache, 0);  /* macroblock.c:1680 */
            written++;
        }
    }
    phx_dec_store_chroma_planes(frame, pcu, pcv, cs);

    /* In-loop deblocking, deblock.c:378-606.  SHADER: B — one MB workgroup;
     * the frame is fully reconstructed before filtering, so the top/left
     * neighbours are exactly x264's already-filtered row/pair. */
    if (sh.i_disable_deblocking_filter_idc != 1) {
        phx_deblock_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.alpha_c0_offset = sh.i_alpha_c0_offset;
        cfg.beta_offset = sh.i_beta_offset;
        cfg.chroma_qp_index_offset = chroma_off;
        cfg.disable_deblocking_filter_idc = sh.i_disable_deblocking_filter_idc;
        cfg.b_transform_8x8_mode = pps->b_transform_8x8_mode;
        for (int mb_y = 0; mb_y < sps->i_mb_height; mb_y++)
            for (int mb_x = 0; mb_x < sps->i_mb_width; mb_x++) {
                phx_macroblock_cache_load(state, &cache, mb_x, mb_y, 0);
                if (phx_frame_deblock_ex(frame, state, &cache, mb_x, mb_y,
                                         last_qp, 0, &cfg)) goto done;
            }
    }
    rc = 0;
    if (ndecisions) *ndecisions = written;
done:
    free(pcu); free(pcv); free(rbsp);
    return rc;
}

int phx_decode_nal(const uint8_t *nal, int size,
                   phx_frame_t *frame, phx_mb_state_t *state,
                   phx_mb_decision_t *decisions, int *ndecisions)
{
    if (!phx_dec_have_params) return -1;
    return phx_decode_nal_ex(nal, size, &phx_dec_current_sps,
                             &phx_dec_current_pps, NULL, frame, state,
                             decisions, ndecisions);
}

/* phx_frame_delete()/phx_mb_state_delete() free the containing struct; the
 * annexb API returns contiguous arrays of structs, so release the members
 * explicitly and free the arrays once. */
void phx_decode_free(phx_frame_t *frames, phx_mb_state_t *states, int nframes)
{
    if (frames) {
        for (int i = 0; i < nframes; i++) {
            phx_frame_t *f = &frames[i];
            for (int y = 0; y < PHX_BFRAME_MAX+2; y++)
                for (int x = 0; x < PHX_BFRAME_MAX+2; x++)
                    free(f->i_row_satds[y][x]);
            free(f->base);
        }
        free(frames);
    }
    if (states) {
        for (int i = 0; i < nframes; i++) {
            phx_mb_state_t *s = &states[i];
            free(s->type); free(s->intra4x4_pred_mode); free(s->non_zero_count);
            free(s->cbp); free(s->mb_transform_size);
            free(s->ref[0]); free(s->ref[1]);
            free(s->mv[0]); free(s->mv[1]);
            free(s->mvd[0]); free(s->mvd[1]);
            free(s->slice_table); free(s->field);
        }
        free(states);
    }
}

/* Decode a whole Annex-B stream.  Coding-order DPB is indexed by frame_num;
 * IDR flushes it (encoder.c:3504/3882).  SHADER: C — strictly serial NAL order. */
int phx_decode_annexb(const uint8_t *data, int size,
                      phx_frame_t **out, phx_mb_state_t **state, int *nframes)
{
    if (out) *out = NULL;
    if (state) *state = NULL;
    if (nframes) *nframes = 0;
    if (!data || size <= 0 || !out || !state || !nframes) return -1;

    int types[512], offs[512], lens[512];
    int nnal = phx_decode_annexb_nals(data, size, types, offs, lens, 512);
    if (nnal <= 0) return -1;

    phx_sps_t sps;
    phx_pps_t pps;
    int have_sps = 0, have_pps = 0;
    int nframes_est = 0;
    for (int i = 0; i < nnal; i++) {
        if (types[i] == 7) {
            if (phx_decode_sps(data + offs[i], lens[i], &sps)) return -1;
            have_sps = 1;
        } else if (types[i] == 8) {
            if (phx_decode_pps(data + offs[i], lens[i], &pps)) return -1;
            have_pps = 1;
        } else if (types[i] == 1 || types[i] == 5) {
            nframes_est++;
        }
    }
    if (!have_sps || !have_pps || !nframes_est) return -1;
    phx_decode_set_params(&sps, &pps);

    int w = sps.i_mb_width*16, h = sps.i_mb_height*16;
    /* The fixed API hands back contiguous arrays (*out)[i] / (*state)[i];
     * temporary pointers keep phx_frame_new()/phx_mb_state_new() ownership
     * simple until the whole stream is known to decode. */
    phx_frame_t *frames = (phx_frame_t *)calloc((size_t)nframes_est,
                                                sizeof(*frames));
    phx_mb_state_t *states = (phx_mb_state_t *)calloc((size_t)nframes_est,
                                                     sizeof(*states));
    phx_frame_t **tmpf = (phx_frame_t **)calloc((size_t)nframes_est,
                                               sizeof(*tmpf));
    phx_mb_state_t **tmps = (phx_mb_state_t **)calloc((size_t)nframes_est,
                                                     sizeof(*tmps));
    phx_frame_t **dpb = (phx_frame_t **)calloc((size_t)(1 << sps.i_log2_max_frame_num),
                                              sizeof(*dpb));
    if (!frames || !states || !tmpf || !tmps || !dpb) {
        free(frames); free(states); free(tmpf); free(tmps); free(dpb);
        return -1;
    }

    int nf = 0, rc = -1;
    for (int i = 0; i < nnal; i++) {
        if (types[i] != 1 && types[i] != 5) continue;      /* SPS/PPS/SEI skips */
        phx_slice_header_t sh;
        if (phx_decode_slice_header(data + offs[i], lens[i], &sps, &pps,
                                    &sh, NULL) || sh.i_first_mb != 0) goto done;
        if (types[i] == 5) memset(dpb, 0, (size_t)(1 << sps.i_log2_max_frame_num)*sizeof(*dpb));
        if (nf >= nframes_est) goto done;
        tmpf[nf] = phx_frame_new(w, h, sps.vui_num_reorder_frames);
        tmps[nf] = phx_mb_state_new(sps.i_mb_width, sps.i_mb_height);
        if (!tmpf[nf] || !tmps[nf]) goto done;
        const phx_frame_t *ref = NULL;
        if (sh.i_type == PHX_SLICE_P) {
            int prev = (sh.i_frame_num - 1) & ((1 << sps.i_log2_max_frame_num)-1);
            ref = dpb[prev];
            if (!ref) goto done;       /* TODO: DPB ref-list reconstruction */
        }
        if (phx_decode_nal_ex(data + offs[i], lens[i], &sps, &pps, ref,
                              tmpf[nf], tmps[nf], NULL, NULL)) goto done;
        if (sh.i_nal_ref_idc)
            dpb[sh.i_frame_num & ((1 << sps.i_log2_max_frame_num)-1)] = tmpf[nf];
        nf++;
    }
    if (!nf) goto done;
    for (int i = 0; i < nf; i++) {
        frames[i] = *tmpf[i];  states[i] = *tmps[i];
        free(tmpf[i]);         free(tmps[i]);             /* objects moved */
        tmpf[i] = NULL;        tmps[i] = NULL;
    }
    *out = frames; *state = states; *nframes = nf;
    frames = NULL; states = NULL;
    rc = 0;
done:
    phx_decode_free(frames, states, nf);
    for (int i = 0; i < nframes_est; i++) {
        if (tmpf && tmpf[i]) phx_frame_delete(tmpf[i]);
        if (tmps && tmps[i]) phx_mb_state_delete(tmps[i]);
    }
    free(tmpf); free(tmps); free(dpb);
    return rc;
}
