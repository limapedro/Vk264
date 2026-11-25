/* Phoenix thin CLI: read one raw I420 frame, emit SPS+PPS, then call the
 * encoder API.  No encoding logic lives here (ORACLE_DESIGN.md:56-57).
 *
 * Source: reference/x264/x264.c:358 (main), :1977 (x264_encoder_headers),
 *         :1851-1868 (encode_frame / write_frame), :2024 (frame loop),
 *         :2049 (flush with pic == NULL);
 *         reference/x264/input/raw.c:113-124 (I420 plane read).
 *
 * SHADER: C — host-side file IO and API calls only; the serial encoder stage
 * owns every bit of stream state.
 *
 * Usage: phx_cli <yuv> <width> <height> <qp> <preset> <cabac|cavlc> <outfile>
 * Exit:  0 success, 1 IO/API error, 2 bad arguments, 3 encoding not yet
 *        implemented (headers are still written in this case).
 */
#include "phx_encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* x264 raw input read_frame_internal (input/raw.c:113-124): plane 0 is
 * w*h bytes, planes 1 and 2 are w*h/4 each for I420 (x264.h:324).
 * SHADER: C — blocking host-side fread. */
static int cli_read_i420(FILE *f, int w, int h, uint8_t *buf)
{
    size_t y = (size_t)w * (size_t)h;
    size_t c = y / 4;
    if (fread(buf, 1, y, f) != y) return -1;
    if (fread(buf + y, 1, c, f) != c) return -1;
    if (fread(buf + y + c, 1, c, f) != c) return -1;
    return 0;
}

/* x264_picture_t I420 plane/stride setup (x264.h:862-904, input/raw.c:113-124).
 * SHADER: C — builds a host-side view over the caller's frame buffer. */
static void cli_make_picture(phx_picture_t *pic, uint8_t *buf, int w, int h)
{
    memset(pic, 0, sizeof(*pic));
    size_t y = (size_t)w * (size_t)h;
    pic->plane[0] = buf;
    pic->plane[1] = buf + y;
    pic->plane[2] = buf + y + y / 4;
    pic->i_stride[0] = w;
    pic->i_stride[1] = w / 2;
    pic->i_stride[2] = w / 2;
    pic->i_width = w;
    pic->i_height = h;
}

/* x264.c:1862-1864 — write_frame() emits nal[0].p_payload, i_frame_size bytes.
 * Here each phx_nal_t already carries its Annex-B start code (phx_bitstream.h:53).
 * SHADER: C — ordered host-side write of the serial NAL array. */
static int cli_write_nals(FILE *f, phx_nal_t *nals, int nnal)
{
    for (int i = 0; i < nnal; i++) {
        if (!nals[i].p_payload || nals[i].i_payload <= 0) return -1;
        if (fwrite(nals[i].p_payload, 1, (size_t)nals[i].i_payload, f) !=
            (size_t)nals[i].i_payload)
            return -1;
    }
    return 0;
}

/* Entry point; mirrors x264.c:358 (main), :1977 (headers) and :2024/:2049
 * (encode loop / flush).  Full-frame encoding is not available yet, so after
 * writing the SPS+PPS the tool exits nonzero with an explanatory message.
 * SHADER: C — thin host-side wrapper around the serial encoder API. */
int main(int argc, char **argv)
{
    if (argc != 8) {
        fprintf(stderr,
                "usage: %s <yuv> <width> <height> <qp> <preset> "
                "<cabac|cavlc> <outfile>\n", argv[0]);
        return 2;
    }

    const char *yuv_path = argv[1];
    int w = atoi(argv[2]), h = atoi(argv[3]);
    int qp = atoi(argv[4]);
    const char *preset = argv[5];
    const char *entropy = argv[6];
    const char *out_path = argv[7];

    phx_param_t p;
    phx_param_default(&p);                             /* base.c:344 */
    if (phx_param_apply_preset(&p, preset)) {           /* base.c:489 */
        fprintf(stderr, "phx_cli: unknown preset '%s'\n", preset);
        return 2;
    }
    if (!strcmp(entropy, "cabac")) p.b_cabac = 1;      /* x264.c: eflag */
    else if (!strcmp(entropy, "cavlc")) p.b_cabac = 0;
    else {
        fprintf(stderr, "phx_cli: entropy must be 'cabac' or 'cavlc'\n");
        return 2;
    }
    p.i_width = w; p.i_height = h;
    p.i_qp_constant = qp;
    phx_param_finalize(&p);                            /* encoder.c:1218-1231 */

    phx_t *enc = phx_encoder_open(&p);                 /* api.c:76 */
    if (!enc) {
        fprintf(stderr, "phx_cli: encoder open failed (check dimensions)\n");
        return 1;
    }

    size_t frame_bytes = (size_t)w * (size_t)h * 3 / 2;
    uint8_t *frame = (uint8_t *)malloc(frame_bytes ? frame_bytes : 1);
    FILE *fin = fopen(yuv_path, "rb");
    FILE *fout = fopen(out_path, "wb");
    if (!frame || !fin || !fout ||
        cli_read_i420(fin, w, h, frame)) {             /* input/raw.c:113 */
        fprintf(stderr, "phx_cli: cannot read I420 frame from '%s'\n", yuv_path);
        if (fin) fclose(fin);
        if (fout) fclose(fout);
        free(frame);
        phx_encoder_close(enc);
        return 1;
    }
    fclose(fin);

    phx_picture_t pic;
    cli_make_picture(&pic, frame, w, h);

    /* x264.c:1977 -> encoder.c:2096: emit SPS+PPS.  The byte assembly is the
     * ported phx_encoder_headers() (phx_headers.h:53); phx_headers() wraps it
     * into the public phx_nal_t array. */
    uint8_t hdr[512];
    int hdr_n = phx_encoder_headers(hdr, (int)sizeof(hdr), &p, w, h);
    phx_nal_t *hnals = NULL;
    int hnnal = 0;
    int n = phx_headers(enc, &hnals, &hnnal);
    if (hdr_n <= 0 || n != hdr_n || hnnal != 2 ||
        cli_write_nals(fout, hnals, hnnal)) {          /* x264.c:1864 */
        fprintf(stderr, "phx_cli: header emission failed\n");
        fclose(fout);
        free(frame);
        phx_encoder_close(enc);
        return 1;
    }

    phx_nal_t *out = NULL;
    int nnal = 0;
    uint8_t *recon = NULL;
    int rc = phx_encode(enc, &pic, &out, &nnal, &recon); /* x264.c:1858 */
    if (rc == PHX_ERR_UNIMPLEMENTED) {
        fprintf(stderr, "phx_cli: full-frame encoding is not implemented yet "
                        "(SPS+PPS written; TODO: slice/MB pipeline)\n");
        fclose(fout);
        free(frame);
        phx_encoder_close(enc);
        return 3;
    }
    if (rc < 0 || cli_write_nals(fout, out, nnal)) {
        fprintf(stderr, "phx_cli: encode failed (%d)\n", rc);
        fclose(fout);
        free(frame);
        phx_encoder_close(enc);
        return 1;
    }

    /* x264.c:2049 — flush with pic == NULL once the pipeline is wired. */
    do {
        out = NULL; nnal = 0; recon = NULL;
        rc = phx_encoder_encode(enc, NULL, &out, &nnal, &recon);
        if (rc < 0) break;
        if (nnal && cli_write_nals(fout, out, nnal)) { rc = -1; break; }
    } while (rc > 0 && nnal > 0);

    fclose(fout);
    free(frame);
    phx_encoder_close(enc);                            /* api.c:130 */
    return rc < 0 ? 1 : 0;
}
