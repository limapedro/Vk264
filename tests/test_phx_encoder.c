/* Smoke test for the Phoenix public encoder API: open/close, SPS+PPS+SEI
  * emission, a one-frame encode and a four-frame lookahead/P-B encode.
  * Standalone main, 0 on pass.
  *
  * Source: reference/x264/encoder/api.c:76/159/166/130 (public entry points),
  *         reference/x264/encoder/encoder.c:1507 (open), :2096 (headers),
  *         :213-364 (slice header), :2791-3093 (slice header + MB loop),
  *         :3323 (encode), :3368-3454 (put_frame/lookahead/shift),
  *         :3504/:3513/:3882 (frame_num/POC), :4196 (close),
  *         reference/x264/encoder/lookahead.c:192/223 (put/get frames),
  *         reference/x264/encoder/slicetype.c:1950-1964 (coded-order reorder),
  *         reference/x264/encoder/ratecontrol.c:821-825 (CQP QP),
  *         reference/x264/encoder/set.c:305/:505 (SPS/PPS write),
  *         reference/x264/common/bitstream.c:55 (Annex-B assembly).
  *
  * No byte-exact x264 parity is claimed: the MB stage still falls back to
  * I_16x16 for P/B frames (inter ME is not ported).  This test checks the
  * frame-type wiring, slice NAL count/types and monotonic frame_num/POC.
  *
  * SHADER: C — serial host-side smoke test; no rendering or pixel shaders.
  */
#include "phx_encoder.h"
#include "phx_tables.h"
#include <stdio.h>
#include <string.h>

#define TW 16
#define TH 16
#define NFRAMES 4
#define STREAM_CAP 65536
#define MAX_NALS 64

/* Tiny deterministic I420 fixture (input/raw.c:113-124).  A different value
  * per frame keeps the lookahead's zero-MV cost path deterministic.
  * SHADER: C — host-side fixture buffer. */
static uint8_t fixture[NFRAMES][TW*TH + 2*(TW*TH)/4];

/* x264 base.c:344 defaults, base.c:489 preset, encoder.c:1218-1231 finalize,
  * and set.c:494 fixed QP.  SHADER: C — host-side parameter setup. */
static void make_param(phx_param_t *p, const char *preset, int qp, int cabac)
{
    phx_param_default(p);
    phx_param_apply_preset(p, preset);
    p->i_width = TW;
    p->i_height = TH;
    p->i_qp_constant = qp;
    p->b_cabac = cabac;
    /* Keep POC type 0 (sps.c:173) and force the lookahead into B frames.
      * scenecut/keyint are pinned so the 4-frame fixture has one IDR only. */
    p->i_bframe = 3;                                   /* base.c:387 */
    p->i_bframe_adaptive = PHX_B_ADAPT_FAST;           /* base.c:389 */
    p->i_scenecut_threshold = 0;
    p->i_keyint_max = 30;
    p->i_keyint_min = 30;
    p->b_mb_tree = 0;
    phx_param_finalize(p);
}

/* Set up the I420 plane view over fixture[frame] (input/raw.c:113-124).
  * SHADER: C — host-side picture view. */
static void make_picture(phx_picture_t *pic, int frame)
{
    memset(pic, 0, sizeof(*pic));
    pic->plane[0] = fixture[frame];
    pic->plane[1] = fixture[frame] + TW*TH;
    pic->plane[2] = fixture[frame] + TW*TH + (TW*TH)/4;
    pic->i_stride[0] = TW; pic->i_stride[1] = TW/2; pic->i_stride[2] = TW/2;
    pic->i_width = TW; pic->i_height = TH;
}

/* Verify the SPS/PPS/SEI triple a handle emits: x264_encoder_headers
  * (encoder.c:2096-2133) returns NAL_SPS + NAL_PPS + version SEI.
  * SHADER: C — serial header comparison. */
static int check_headers(phx_t *h, const phx_param_t *p)
{
    phx_nal_t *nals = NULL;
    int nnal = 0;
    int n = phx_headers(h, &nals, &nnal);
    if (n <= 0 || nnal != 3 || !nals) {
        printf("FAIL headers: n=%d nnal=%d\n", n, nnal);
        return 1;
    }
    if (nals[0].i_type != 7 || nals[1].i_type != 8 || nals[2].i_type != 6 ||
        nals[0].i_ref_idc != 3 || nals[1].i_ref_idc != 3 ||
        nals[2].i_ref_idc != 0) {
        printf("FAIL header types/ref_idc: %d/%d/%d %d/%d/%d\n",
               nals[0].i_type, nals[1].i_type, nals[2].i_type,
               nals[0].i_ref_idc, nals[1].i_ref_idc, nals[2].i_ref_idc);
        return 1;
    }
    if (nals[0].i_payload <= 4 || nals[1].i_payload <= 4 ||
        nals[0].i_payload + nals[1].i_payload + nals[2].i_payload != n ||
        nals[0].p_payload[0] || nals[0].p_payload[1] ||
        nals[0].p_payload[2] || nals[0].p_payload[3] != 1 ||
        nals[1].p_payload[0] || nals[1].p_payload[1] ||
        nals[1].p_payload[2] || nals[1].p_payload[3] != 1 ||
        nals[2].p_payload[0] || nals[2].p_payload[1] ||
        nals[2].p_payload[2] || nals[2].p_payload[3] != 1) {
        printf("FAIL header Annex-B framing: sizes %d+%d+%d != %d\n",
               nals[0].i_payload, nals[1].i_payload, nals[2].i_payload, n);
        return 1;
    }

    uint8_t ref[512];
    int rn = phx_encoder_headers(ref, (int)sizeof(ref), p, TW, TH);
    int rn2 = phx_encoder_header_bytes(h, ref, (int)sizeof(ref));
    if (rn2 != rn || n != rn + nals[2].i_payload ||
        memcmp(ref, nals[0].p_payload, (size_t)rn)) {
        printf("FAIL header bytes: high-level %d, low-level %d/%d\n", n, rn, rn2);
        return 1;
    }
    return 0;
}

/* Append a NAL's Annex-B bytes to a test stream buffer and record its type.
  * SHADER: C — serial byte copy. */
static int append_nals(uint8_t *dst, int cap, int *used, phx_nal_t *nal,
                       int *types, int *offs, int *lens, int *nn)
{
    if (!nal->p_payload || nal->i_payload <= 0 ||
        *used + nal->i_payload > cap || *nn >= MAX_NALS) return 1;
    offs[*nn] = *used;
    lens[*nn] = nal->i_payload;
    types[*nn] = nal->p_payload[4] & 0x1f;
    memcpy(dst + *used, nal->p_payload, (size_t)nal->i_payload);
    *used += nal->i_payload;
    (*nn)++;
    return 0;
}

/* One-frame encode: SPS/PPS/SEI then a slice NAL.  The stream must start with
  * 00 00 00 01 67 (4-byte start code + NAL_SPS), as written by phx_nal_encode
  * (bitstream.c:55-75).  SHADER: C — serial stream assembly. */
static int check_encode(const char *label, phx_t *h, phx_picture_t *pic)
{
    uint8_t stream[STREAM_CAP];
    int used = 0, nn = 0;
    int types[MAX_NALS], offs[MAX_NALS], lens[MAX_NALS];
    phx_nal_t *nals = NULL;
    int nnal = 0;
    int fails = 0;

    int hdr_n = phx_headers(h, &nals, &nnal);
    if (hdr_n <= 0 || nnal != 3 || !nals ||
        append_nals(stream, STREAM_CAP, &used, &nals[0], types, offs, lens, &nn) ||
        append_nals(stream, STREAM_CAP, &used, &nals[1], types, offs, lens, &nn) ||
        append_nals(stream, STREAM_CAP, &used, &nals[2], types, offs, lens, &nn) ||
        used != hdr_n) {
        printf("FAIL %s: header append (n=%d used=%d)\n", label, hdr_n, used);
        return 1;
    }

    phx_nal_t *out = NULL;
    uint8_t *recon = NULL;
    int rc = phx_encode(h, pic, &out, &nnal, &recon);
    if (rc < 0) {
        printf("FAIL %s: encode rc=%d\n", label, rc);
        return 1;
    }
    /* encoder.c:3428-3433 — with B frames the first picture is delayed
     * until the lookahead gate opens; the flush call releases it. */
    if (nnal == 0)
        rc = phx_encoder_encode(h, NULL, &out, &nnal, &recon);
    if (rc <= 0 || nnal != 1 || !out || !recon ||
        append_nals(stream, STREAM_CAP, &used, &out[0], types, offs, lens, &nn)) {
        printf("FAIL %s: encode rc=%d nnal=%d out=%p recon=%p\n",
               label, rc, nnal, (void *)out, (void *)recon);
        return 1;
    }

    if (used <= hdr_n || stream[0] != 0 || stream[1] != 0 ||
        stream[2] != 0 || stream[3] != 1 || stream[4] != 0x67) {
        printf("FAIL %s: Annex-B start: %02X %02X %02X %02X %02X (len %d)\n",
               label, stream[0], stream[1], stream[2], stream[3], stream[4], used);
        ++fails;
    }
    if (used > hdr_n + 5 && !(stream[hdr_n] == 0 && stream[hdr_n+1] == 0 &&
                              stream[hdr_n+2] == 0 && stream[hdr_n+3] == 1)) {
        printf("FAIL %s: slice start code at %d\n", label, hdr_n);
        ++fails;
    }
    if (used > hdr_n + 5 && types[3] != 5) {
        printf("FAIL %s: first slice must be IDR (type %d)\n", label, types[3]);
        ++fails;
    }
    if (!fails)
        printf("  %s: %d-byte Annex-B stream, first NAL %02X %02X %02X %02X %02X\n",
               label, used, stream[0], stream[1], stream[2], stream[3], stream[4]);

    /* x264.c:2049 flush: the one-frame path has no delayed frames. */
    out = (phx_nal_t *)&pic; nnal = 99;
    rc = phx_encoder_encode(h, NULL, &out, &nnal, &recon);
    if (rc != 0 || out || nnal) {
        printf("FAIL %s: flush rc=%d out=%p nnal=%d\n",
               label, rc, (void *)out, nnal);
        ++fails;
    }
    return fails;
}

/* ---- minimal RBSP bit reader (SHADER: C, serial scalar) ---------------- */

typedef struct { const uint8_t *p; int bit; } phx_test_br_t;

static int br_bit(phx_test_br_t *b)
{
    int v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1;
    b->bit++;
    return v;
}

static unsigned br_bits(phx_test_br_t *b, int n)
{
    unsigned v = 0;
    while (n-- > 0) v = (v << 1) | (unsigned)br_bit(b);
    return v;
}

static int br_ue(phx_test_br_t *b)
{
    int z = 0;
    while (br_bit(b) == 0) { if (++z > 16) return -1; }
    return z ? (int)((1u << z) - 1u + br_bits(b, z)) : 0;
}

/* Remove Annex-B emulation prevention bytes (H.264 7.3.1, bitstream.c:29-39).
  * SHADER: C — serial byte scan. */
static int deescape(const uint8_t *src, int n, uint8_t *dst)
{
    int zeros = 0, o = 0;
    for (int i = 0; i < n; i++) {
        if (zeros >= 2 && src[i] == 3) { zeros = 0; continue; }
        dst[o++] = src[i];
        zeros = src[i] ? 0 : zeros + 1;
    }
    return o;
}

/* Read first_mb/slice_type/frame_num/POC from a slice header RBSP.
  * Source: encoder.c:215-243 (first_mb, slice_type, pps_id, frame_num, POC).
  * SHADER: C — serial bit parse. */
static int parse_slice(const uint8_t *payload, int len, const phx_sps_t *sps,
                       int nal_type, int *slice_type, int *frame_num, int *poc)
{
    uint8_t rbsp[1024];
    int n = deescape(payload + 5, len - 5, rbsp);       /* skip start code + NAL */
    if (n < 8) return -1;
    phx_test_br_t b = { rbsp, 0 };
    if (br_ue(&b) < 0) return -1;                       /* first_mb_in_slice */
    int st = br_ue(&b);
    if (st < 0 || br_ue(&b) < 0) return -1;             /* pps_id */
    /* encoder.c:225 writes i_type+5: 5=P, 6=B, 7=I (H.264 7.3.3 "all" forms). */
    if (st >= 5) st -= 5;
    int fn = (int)br_bits(&b, sps->i_log2_max_frame_num);
    if (!sps->b_frame_mbs_only) br_bit(&b);             /* field_pic_flag */
    if (nal_type == 5 && br_ue(&b) < 0) return -1;      /* idr_pic_id */
    int p = 0;
    if (sps->i_poc_type == 0)
        p = (int)br_bits(&b, sps->i_log2_max_poc_lsb);  /* encoder.c:241 */
    *slice_type = st;
    *frame_num = fn;
    *poc = p;
    return 0;
}

/* Four frames through the lookahead: at least four slice NALs, first IDR and
  * the rest non-IDR, at least one P/B slice, and monotonically increasing
  * frame_num/POC in coding order (encoder.c:3504/3513/3882).
  * SHADER: C — serial stream collection and header parse. */
static int check_multiframe(const char *label, phx_param_t *p)
{
    phx_t *h = phx_encoder_open(p);
    if (!h) { printf("FAIL %s: open\n", label); return 1; }
    phx_sps_t sps;
    phx_sps_init(&sps, p, TW, TH);

    uint8_t stream[STREAM_CAP];
    int used = 0, nn = 0;
    int types[MAX_NALS], offs[MAX_NALS], lens[MAX_NALS];
    int fails = 0;

    phx_picture_t pic;
    for (int i = 0; i < NFRAMES; i++) {
        make_picture(&pic, i);
        phx_nal_t *out = NULL;
        int nnal = 0;
        uint8_t *recon = NULL;
        int rc = phx_encode(h, &pic, &out, &nnal, &recon); /* encoder.c:3323 */
        if (rc < 0) { printf("FAIL %s: encode %d rc=%d\n", label, i, rc); ++fails; break; }
        for (int j = 0; j < nnal; j++)
            if (append_nals(stream, STREAM_CAP, &used, &out[j],
                            types, offs, lens, &nn)) {
                printf("FAIL %s: append frame %d\n", label, i);
                ++fails;
            }
    }
    /* x264.c:2049 — drain the lookahead remainder. */
    for (;;) {
        phx_nal_t *out = NULL;
        int nnal = 0;
        uint8_t *recon = NULL;
        int rc = phx_encoder_encode(h, NULL, &out, &nnal, &recon);
        if (rc < 0) { printf("FAIL %s: flush rc=%d\n", label, rc); ++fails; break; }
        for (int j = 0; j < nnal; j++)
            if (append_nals(stream, STREAM_CAP, &used, &out[j],
                            types, offs, lens, &nn)) {
                printf("FAIL %s: append flush\n", label);
                ++fails;
            }
        if (rc == 0) break;
    }

    if (nn < NFRAMES) {
        printf("FAIL %s: %d slice NALs, want >= %d\n", label, nn, NFRAMES);
        ++fails;
    }
    int nslice = 0;
    for (int i = 0; i < nn; i++) {
        if (types[i] == 5 || types[i] == 1) nslice++;
        else { printf("FAIL %s: unexpected NAL type %d\n", label, types[i]); ++fails; }
    }
    if (nslice < NFRAMES) {
        printf("FAIL %s: only %d slice NALs\n", label, nslice);
        ++fails;
    }

    int prev_fn = -1, prev_poc = -1, saw_pb = 0;
    for (int i = 0; i < nslice && i < MAX_NALS; i++) {
        if (types[i] != 5 && i == 0) {
            printf("FAIL %s: first slice is not IDR\n", label);
            ++fails;
        }
        int st = -1, fn = -1, poc = -1;
        if (parse_slice(stream + offs[i], lens[i], &sps, types[i],
                        &st, &fn, &poc)) {
            printf("FAIL %s: parse slice %d\n", label, i);
            ++fails;
            continue;
        }
        if (st == 0 || st == 1) saw_pb = 1;             /* x264 base.h:111-112 */
        if (fn <= prev_fn || poc <= prev_poc) {
            printf("FAIL %s: slice %d non-monotonic fn/poc %d/%d (prev %d/%d)\n",
                   label, i, fn, poc, prev_fn, prev_poc);
            ++fails;
        }
        prev_fn = fn; prev_poc = poc;
        printf("  %s: slice %d NAL=%d type=%d fn=%d poc=%d\n",
               label, i, types[i], st, fn, poc);
    }
    if (!saw_pb) {
        printf("FAIL %s: no P/B slice from the lookahead\n", label);
        ++fails;
    }

    phx_encoder_close(h);                              /* api.c:130 */
    return fails;
}

/* ---- deterministic MB0 CABAC decision-sequence golden ------------------ */
/* The expected values below were decoded from the x264 reference IDR NAL of
 * tools/parity_harness.py's 64x64 "synth" fixture (--preset medium --cabac
 * --qp 28).  They are decision values, not stream bytes: mb_type=I_4x4, the
 * 16 signalled (FIX4) intra4x4 modes, chroma mode, cbp_luma/chroma, slice QP,
 * qp_delta=0 and the ordered first luma-residual cbf.  Source of the write
 * order: reference/x264/encoder/cabac.c:917-1096 and
 * encoder/macroblock.h:132-166.  SHADER: C — serial decode of one MB header. */

typedef struct {
    const uint8_t *rbsp;
    int bit;
    int low, range;
    uint8_t state[PHX_CABAC_CTX_COUNT];
} phx_test_cabac_t;

static int tc_bit(phx_test_cabac_t *d)
{
    int v = (d->rbsp[d->bit >> 3] >> (7 - (d->bit & 7))) & 1;
    d->bit++;
    return v;
}

static void tc_norm(phx_test_cabac_t *d)
{
    while (d->range < 256) {
        d->range <<= 1;
        d->low = (d->low << 1) | tc_bit(d);
    }
}

static int tc_dec(phx_test_cabac_t *d, int ctx)
{
    int st = d->state[ctx];
    int lps = phx_cabac_range_lps[st >> 1][(d->range >> 6) - 4];
    int b;
    d->range -= lps;
    if (d->low < d->range) {
        b = st & 1;
    } else {
        b = 1 - (st & 1);
        d->low -= d->range;
        d->range = lps;
    }
    d->state[ctx] = phx_cabac_transition[st][b];     /* common/cabac.c:113 */
    tc_norm(d);
    return b;
}

static int tc_init(phx_test_cabac_t *d, const uint8_t *rbsp, int start, int qp)
{
    phx_cabac_t cb;
    memset(&cb, 0, sizeof(cb));
    phx_cabac_context_init(&cb, PHX_SLICE_I, qp, 0);  /* cabac.c:32-43 */
    memset(d, 0, sizeof(*d));
    d->rbsp = rbsp;
    d->bit = (start + 1) * 8;   /* 9-bit offset: 8th bit is from the next byte */
    memcpy(d->state, cb.state, sizeof(d->state));
    d->range = 0x01FE;
    d->low = (rbsp[start] << 1) | tc_bit(d);
    return 0;
}

/* Parity fixture from tools/parity_harness.py::make_synthetic (64x64, 8 frames). */
static void make_mb0_fixture(uint8_t *buf)
{
    int p = 0;
    for (int f = 0; f < 8; f++) {
        for (int j = 0; j < 64; j++)
            for (int i = 0; i < 64; i++)
                buf[p++] = (uint8_t)((i*7 + j*13 + f*29) & 255);
        for (int j = 0; j < 32; j++)
            for (int i = 0; i < 32; i++)
                buf[p++] = (uint8_t)((128 + i*5 + f*11) & 255);
        for (int j = 0; j < 32; j++)
            for (int i = 0; i < 32; i++)
                buf[p++] = (uint8_t)((128 + j*5 + f*7) & 255);
    }
}

static int check_mb0_sequence(void)
{
    static const int want_mode[16] = {
        2,1,2,8,1,1,3,1,2,2,2,1,2,1,4,8
    };
    phx_param_t p;
    phx_param_default(&p);                          /* base.c:344 */
    phx_param_apply_preset(&p, "medium");           /* base.c:553 */
    p.i_width = 64; p.i_height = 64; p.i_qp_constant = 28;
    p.b_cabac = 1; p.i_keyint_max = 30; p.i_keyint_min = 30;
    p.i_scenecut_threshold = 0; p.rc_method = PHX_RC_CQP;
    p.i_aq_mode = 0; p.b_mb_tree = 0;
    phx_param_finalize(&p);                         /* encoder.c:1218-1231 */

    phx_t *h = phx_encoder_open(&p);
    if (!h) { printf("FAIL mb0: open\n"); return 1; }
    static uint8_t raw[8 * (64*64 + 2*32*32)];
    make_mb0_fixture(raw);

    uint8_t stream[STREAM_CAP];
    int used = 0, nn = 0, types[MAX_NALS], offs[MAX_NALS], lens[MAX_NALS];
    for (int f = 0; f < 8; f++) {
        phx_picture_t pic;
        memset(&pic, 0, sizeof(pic));
        size_t ysz = 64*64, csz = 32*32;
        pic.plane[0] = raw + f*(ysz + 2*csz);
        pic.plane[1] = pic.plane[0] + ysz;
        pic.plane[2] = pic.plane[1] + csz;
        pic.i_stride[0] = 64; pic.i_stride[1] = pic.i_stride[2] = 32;
        pic.i_width = 64; pic.i_height = 64;
        phx_nal_t *out = NULL;
        int nnal = 0;
        uint8_t *recon = NULL;
        if (phx_encode(h, &pic, &out, &nnal, &recon) < 0) {
            printf("FAIL mb0: encode %d\n", f);
            phx_encoder_close(h);
            return 1;
        }
        for (int j = 0; j < nnal; j++)
            append_nals(stream, STREAM_CAP, &used, &out[j],
                        types, offs, lens, &nn);
    }
    for (;;) {
        phx_nal_t *out = NULL;
        int nnal = 0;
        uint8_t *recon = NULL;
        int rc = phx_encoder_encode(h, NULL, &out, &nnal, &recon);
        for (int j = 0; j < nnal; j++)
            append_nals(stream, STREAM_CAP, &used, &out[j],
                        types, offs, lens, &nn);
        if (rc == 0) break;
    }
    phx_encoder_close(h);

    int idr = -1;
    for (int i = 0; i < nn; i++)
        if (types[i] == 5) { idr = i; break; }
    if (idr < 0) { printf("FAIL mb0: no IDR slice\n"); return 1; }

    uint8_t rbsp[2048];
    int n = deescape(stream + offs[idr] + 5, lens[idr] - 5, rbsp);
    /* Slice header of this fixture is 30 bits, then bs_align_1 (encoder.c:2795)
     * leaves the first CABAC byte at rbsp[4]. */
    int cabac_start = 4;
    if (n < 16) { printf("FAIL mb0: short RBSP\n"); return 1; }

    phx_test_cabac_t d;
    tc_init(&d, rbsp, cabac_start, 25);            /* slice_qp = 25, ratecontrol.c:821 */
    int fails = 0;
    int mb_type_flag = tc_dec(&d, 3);              /* cabac.c:391, ctx = 3+0 */
    if (mb_type_flag != 0) { printf("FAIL mb0: mb_type not I_4x4\n"); fails++; }
    /* cabac.c:419-420 — header transform-size bin (PPS 8x8dct=1), value 0. */
    if (tc_dec(&d, 399) != 0) {
        printf("FAIL mb0: header transform_size flag\n"); fails++;
    }
    phx_test_cabac_t m;
    tc_init(&m, rbsp, cabac_start, 25);
    tc_dec(&m, 3);
    tc_dec(&m, 399);
    int cache[80];
    for (int i = 0; i < 80; i++) cache[i] = -1;
    static const int8_t fix4[13] = { -1,0,1,2,3,4,5,6,7,8,2,2,2 };
    static const int sc[16] = {
        12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39
    };
    for (int i = 0; i < 16; i++) {
        int ma = fix4[cache[sc[i]-1] + 1];
        int mb = fix4[cache[sc[i]-8] + 1];
        int pred = ma < mb ? ma : mb;
        if (pred < 0) pred = 2;
        int mode;
        if (tc_dec(&m, 68)) mode = pred;             /* cabac.c:85-86 */
        else {
            int b0 = tc_dec(&m, 69);                 /* cabac.c:92, LSB first */
            int b1 = tc_dec(&m, 69);                 /* :93 */
            int b2 = tc_dec(&m, 69);                 /* :94 */
            mode = b0 | (b1 << 1) | (b2 << 2);
            if (mode > pred) mode++;                 /* :90-91 */
        }
        cache[sc[i]] = mode;
        if (mode != want_mode[i]) {
            printf("FAIL mb0: intra4 mode %d = %d, want %d\n", i, mode, want_mode[i]);
            fails++;
        }
    }
    if (tc_dec(&m, 64) != 0) {                      /* cabac.c:431, chroma mode 0 */
        printf("FAIL mb0: chroma_pred_mode != 0\n"); fails++;
    }
    for (int i = 0; i < 4; i++) {
        if (tc_dec(&m, 73) != 1) {                   /* cabac.c:123-126, cbp_luma=15 */
            printf("FAIL mb0: cbp_luma bit %d\n", i); fails++;
        }
    }
    if (tc_dec(&m, 77) != 1 || tc_dec(&m, 81) != 1) {
        printf("FAIL mb0: cbp_chroma != 2\n"); fails++;
    }
    /* No post-cbp transform_size bin for I_4x4: x264_transform_allowed[I_4x4]
     * is 0 (common/macroblock.h:445-461), so qp_delta follows immediately.
     * A stray bin here was the original first divergence at payload byte 11. */
    if (tc_dec(&m, 60) != 0) {                      /* cabac.c:981, dqp = 0 */
        printf("FAIL mb0: qp_delta != 0\n"); fails++;
    }
    /* First luma residual cbf (cabac.c:989/1057): ctx 93 + 3 (unavailable
     * left/top nnz = 0x80, b_intra=1 -> mask 0xff, phx_cabac_syntax.c:750-757). */
    if (tc_dec(&m, 96) != 1) {
        printf("FAIL mb0: first luma cbf / residual order\n"); fails++;
    }
    if (!fails)
        printf("  mb0: I_4x4, 16 modes, chroma 0, cbp 15/2, qp 25, dqp 0, "
               "cbp->qp_delta->luma order OK\n");
    return fails;
}

/* Standalone main; x264_encoder_open/headers/encode/close (api.c:76/159/166/
  * 130, encoder.c:1507/2096/3323/4196).  SHADER: C — serial smoke test. */
int main(void)
{
    int fails = 0;

    for (int f = 0; f < NFRAMES; f++)
        for (size_t k = 0; k < sizeof(fixture[f]); k++)
            fixture[f][k] = (uint8_t)((k*7 + f*37 + 13) & 255);

    const char *cases[][2] = { {"medium", "cabac"}, {"ultrafast", "cavlc"} };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        phx_param_t p;
        make_param(&p, cases[i][0], 28, !strcmp(cases[i][1], "cavlc") ? 0 : 1);

        phx_t *h = phx_encoder_open(&p);
        if (!h) {
            printf("FAIL %s/%s: open\n", cases[i][0], cases[i][1]);
            ++fails;
            continue;
        }
        fails += check_headers(h, &p);

        phx_picture_t pic;
        make_picture(&pic, 0);
        char label[64];
        snprintf(label, sizeof(label), "%s/%s", cases[i][0], cases[i][1]);
        fails += check_encode(label, h, &pic);
        phx_encoder_close(h);

        /* Fresh handle so the one-frame smoke test does not offset the
         * four-frame lookahead counters. */
        make_param(&p, cases[i][0], 28, !strcmp(cases[i][1], "cavlc") ? 0 : 1);
        fails += check_multiframe(label, &p);
    }

    fails += check_mb0_sequence();

    /* Aliases from docs/ORACLE_DESIGN.md:40-45 and invalid-parameter path
      * (validate_parameters, encoder.c:423/1552). */
    phx_param_t p;
    make_param(&p, "medium", 28, 1);
    phx_t *alias = phx_open(&p);
    if (!alias) { printf("FAIL phx_open alias\n"); ++fails; }
    else phx_close(alias);
    p.i_width = 17;                        /* frame.c:85-86 requires %16 == 0 */
    if (phx_encoder_open(&p)) { printf("FAIL open accepted width 17\n"); ++fails; }

    printf("%s: phx_encoder open/close/headers/encode, %d failures\n",
           fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
