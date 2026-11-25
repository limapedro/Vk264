/* Smoke test for the Phoenix public encoder API: open/close, SPS+PPS
  * emission and a real one-frame encode producing a non-empty Annex-B stream.
  * Standalone main, 0 on pass.
  *
  * Source: reference/x264/encoder/api.c:76/159/166/130 (public entry points),
  *         reference/x264/encoder/encoder.c:1507 (open), :2096 (headers),
  *         :2791-3093 (slice header + MB loop), :3323 (encode), :4196 (close),
  *         reference/x264/encoder/set.c:305/:505 (SPS/PPS write),
  *         reference/x264/common/bitstream.c:55 (Annex-B assembly).
  *
  * SHADER: C — serial host-side smoke test; no rendering or pixel shaders.
  */
#include "phx_encoder.h"
#include <stdio.h>
#include <string.h>

#define TW 16
#define TH 16
#define STREAM_CAP 16384

/* Tiny deterministic I420 fixture: one 16x16 Y plane plus two 8x8 chroma
  * planes.  Mirrors the raw plane layout of input/raw.c:113-124.
  * SHADER: C — host-side fixture buffer. */
static uint8_t fixture[TW*TH + 2*(TW*TH)/4];

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
    phx_param_finalize(p);
}

/* Set up the I420 plane view over the fixture (input/raw.c:113-124).
  * SHADER: C — host-side picture view. */
static void make_picture(phx_picture_t *pic)
{
    memset(pic, 0, sizeof(*pic));
    pic->plane[0] = fixture;
    pic->plane[1] = fixture + TW*TH;
    pic->plane[2] = fixture + TW*TH + (TW*TH)/4;
    pic->i_stride[0] = TW; pic->i_stride[1] = TW/2; pic->i_stride[2] = TW/2;
    pic->i_width = TW; pic->i_height = TH;
}

/* Verify the SPS/PPS pair a handle emits: x264_encoder_headers
  * (encoder.c:2096-2133) returns NAL_SPS + NAL_PPS with NAL_PRIORITY_HIGHEST.
  * SHADER: C — serial header comparison. */
static int check_headers(phx_t *h, const phx_param_t *p)
{
    phx_nal_t *nals = NULL;
    int nnal = 0;
    int n = phx_headers(h, &nals, &nnal);
    if (n <= 0 || nnal != 2 || !nals) {
        printf("FAIL headers: n=%d nnal=%d\n", n, nnal);
        return 1;
    }
    if (nals[0].i_type != 7 || nals[1].i_type != 8 ||
        nals[0].i_ref_idc != 3 || nals[1].i_ref_idc != 3) {
        printf("FAIL header types/ref_idc: %d/%d %d/%d\n",
               nals[0].i_type, nals[1].i_type,
               nals[0].i_ref_idc, nals[1].i_ref_idc);
        return 1;
    }
    if (nals[0].i_payload <= 4 || nals[1].i_payload <= 4 ||
        nals[0].i_payload + nals[1].i_payload != n ||
        nals[0].p_payload[0] || nals[0].p_payload[1] ||
        nals[0].p_payload[2] || nals[0].p_payload[3] != 1 ||
        nals[1].p_payload[0] || nals[1].p_payload[1] ||
        nals[1].p_payload[2] || nals[1].p_payload[3] != 1) {
        printf("FAIL header Annex-B framing: sizes %d+%d != %d\n",
               nals[0].i_payload, nals[1].i_payload, n);
        return 1;
    }

    uint8_t ref[512];
    int rn = phx_encoder_headers(ref, (int)sizeof(ref), p, TW, TH);
    int rn2 = phx_encoder_header_bytes(h, ref, (int)sizeof(ref));
    if (rn != n || rn2 != n || memcmp(ref, nals[0].p_payload, (size_t)n)) {
        printf("FAIL header bytes: high-level %d, low-level %d/%d\n", n, rn, rn2);
        return 1;
    }
    return 0;
}

/* Append a NAL's Annex-B bytes to a test stream buffer.
  * SHADER: C — serial byte copy. */
static int append_nals(uint8_t *dst, int cap, int *used, phx_nal_t *nal)
{
    if (!nal->p_payload || nal->i_payload <= 0 ||
        *used + nal->i_payload > cap) return 1;
    memcpy(dst + *used, nal->p_payload, (size_t)nal->i_payload);
    *used += nal->i_payload;
    return 0;
}

/* One-frame encode: SPS/PPS then a slice NAL.  The stream must be non-empty
  * and start with 00 00 00 01 67 (4-byte start code + NAL_SPS), as written by
  * phx_nal_encode (bitstream.c:55-75).  SHADER: C — serial stream assembly. */
static int check_encode(const char *label, phx_t *h, phx_picture_t *pic)
{
    uint8_t stream[STREAM_CAP];
    int used = 0;
    phx_nal_t *nals = NULL;
    int nnal = 0;
    int fails = 0;

    int hdr_n = phx_headers(h, &nals, &nnal);
    if (hdr_n <= 0 || nnal != 2 || !nals ||
        append_nals(stream, STREAM_CAP, &used, &nals[0]) ||
        append_nals(stream, STREAM_CAP, &used, &nals[1]) ||
        used != hdr_n) {
        printf("FAIL %s: header append (n=%d used=%d)\n", label, hdr_n, used);
        return 1;
    }

    phx_nal_t *out = NULL;
    uint8_t *recon = NULL;
    int rc = phx_encode(h, pic, &out, &nnal, &recon);
    if (rc <= 0 || nnal != 1 || !out || !recon ||
        append_nals(stream, STREAM_CAP, &used, &out[0])) {
        printf("FAIL %s: encode rc=%d nnal=%d out=%p recon=%p\n",
               label, rc, nnal, (void *)out, (void *)recon);
        return 1;
    }

    /* The first four bytes are the SPS start code; byte 4 is NAL_SPS (7). */
    if (used <= hdr_n || stream[0] != 0 || stream[1] != 0 ||
        stream[2] != 0 || stream[3] != 1 || stream[4] != 0x67) {
        printf("FAIL %s: Annex-B start: %02X %02X %02X %02X %02X (len %d)\n",
               label, stream[0], stream[1], stream[2], stream[3], stream[4], used);
        ++fails;
    }
    /* The slice NAL follows the two headers with its own Annex-B prefix. */
    if (used > hdr_n + 5 && !(stream[hdr_n] == 0 && stream[hdr_n+1] == 0 &&
                              stream[hdr_n+2] == 0 && stream[hdr_n+3] == 1)) {
        printf("FAIL %s: slice start code at %d\n", label, hdr_n);
        ++fails;
    }
    if (used > hdr_n + 5 && (stream[hdr_n+4] & 0x1f) != 5) {
        printf("FAIL %s: first slice must be IDR (type %d)\n",
               label, stream[hdr_n+4] & 0x1f);
        ++fails;
    }
    if (!fails)
        printf("  %s: %d-byte Annex-B stream, first NAL %02X %02X %02X %02X %02X\n",
               label, used, stream[0], stream[1], stream[2], stream[3], stream[4]);

    /* A second frame goes through the mirror alias and a non-IDR slice. */
    out = NULL; nnal = 0; recon = NULL;
    rc = phx_encoder_encode(h, pic, &out, &nnal, &recon);
    if (rc <= 0 || nnal != 1 || !out || (out[0].p_payload[4] & 0x1f) != 1) {
        printf("FAIL %s: second frame rc=%d nnal=%d\n", label, rc, nnal);
        ++fails;
    }
    /* x264.c:2049 flush: no delayed frames in this path. */
    out = (phx_nal_t *)&pic; nnal = 99;
    rc = phx_encoder_encode(h, NULL, &out, &nnal, &recon);
    if (rc != 0 || out || nnal) {
        printf("FAIL %s: flush rc=%d out=%p nnal=%d\n",
               label, rc, (void *)out, nnal);
        ++fails;
    }
    return fails;
}

/* Standalone main; x264_encoder_open/headers/encode/close (api.c:76/159/166/
  * 130, encoder.c:1507/2096/3323/4196).  SHADER: C — serial smoke test. */
int main(void)
{
    int fails = 0;

    for (size_t k = 0; k < sizeof(fixture); k++)
        fixture[k] = (uint8_t)((k*7 + 13) & 255);

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
        make_picture(&pic);
        char label[64];
        snprintf(label, sizeof(label), "%s/%s", cases[i][0], cases[i][1]);
        fails += check_encode(label, h, &pic);
        phx_encoder_close(h);
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
