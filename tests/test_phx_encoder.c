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
