/* Phoenix thin CLI: read every raw I420 frame, emit SPS+PPS+SEI once, then
  * drain the encoder's lookahead into one slice NAL per input frame.
  * No encoding logic lives here (ORACLE_DESIGN.md:56-57).
  *
  * Source: reference/x264/x264.c:358 (main), :1977 (x264_encoder_headers),
  *         :1851-1868 (encode_frame / write_frame), :2024 (frame loop),
  *         :2049 (flush with pic == NULL);
  *         reference/x264/encoder/encoder.c:3368/3426/3447/3454 (put/lookahead/
  *         shift), reference/x264/encoder/lookahead.c:192/223,
  *         reference/x264/encoder/slicetype.c:1950-1964 (coded-order reorder);
  *         reference/x264/input/raw.c:113-124 (I420 plane read).
  *
  * SHADER: C — host-side file IO and API calls only; the serial encoder stage
  * owns every bit of stream state.
  *
  * Usage: phx_cli <yuv> <width> <height> <qp> <preset> <cabac|cavlc> <outfile>
  *        phx_cli <yuv> <width> <height> <qp> <preset> <cabac|cavlc> <outfile>
  *                --bitrate <kbps>
  * The 7-argument form is the original fixed-QP (CQP) form and is unchanged;
  * --bitrate switches to one-pass ABR via phx_ratecontrol (ratecontrol.c:
  * :1431 start / :1828 end).  qp stays the positional fallback constant.
  * Source: reference/x264/encoder/ratecontrol.c:750-825 (ABR config).
  * Exit:  0 success, 1 IO/API error, 2 bad arguments.
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

/* Number of complete I420 pictures in the file (input/raw.c:113-124).
 * SHADER: C — host-side file-size query; no encoder state is read. */
static int cli_frame_count(FILE *f, size_t frame_bytes)
{
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) return -1;
    if (frame_bytes == 0) return 0;
    return (int)((size_t)size / frame_bytes);
}

/* Entry point; mirrors x264.c:358 (main), :1977 (headers), :2024 (frame loop)
 * and :2049 (flush).  One phx_encode() call consumes one picture and emits at
 * most one decided slice; the flush loop drains the lookahead remainder
 * (encoder.c:3447/3454, lookahead.c:223).  SHADER: C — thin host-side wrapper
 * around the serial encoder API. */
int main(int argc, char **argv)
{
    /* x264.c:358 — 7 positional args are the original --qp form.  The optional
     * --bitrate tail switches to one-pass ABR without changing that form. */
    int bitrate_mode = 0;
    int bitrate_kbps = 0;
    if (argc == 8) {
        /* fixed-QP (CQP), backwards compatible byte-for-byte */
    } else if (argc == 10 && !strcmp(argv[8], "--bitrate")) {
        bitrate_mode = 1;
        bitrate_kbps = atoi(argv[9]);
        if (bitrate_kbps <= 0) {
            fprintf(stderr, "phx_cli: --bitrate must be a positive kbps value\n");
            return 2;
        }
    } else {
        fprintf(stderr,
                "usage: %s <yuv> <width> <height> <qp> <preset> "
                "<cabac|cavlc> <outfile> [--bitrate <kbps>]\n", argv[0]);
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
    /* Match the flags tools/parity_harness.py passes to the x264 reference. */
    p.i_keyint_max = 30;                               /* x264.c --keyint 30 */
    p.i_keyint_min = 30;                               /* x264.c --min-keyint 30 */
    p.i_scenecut_threshold = 0;                        /* x264.c --scenecut 0 */
    p.rc_method = bitrate_mode ? PHX_RC_ABR : PHX_RC_CQP; /* x264 RC_ABR/RC_CQP */
    if (!bitrate_mode) {
        /* CQP disables AQ/MB-tree (x264 --qp).  ABR keeps the x264 defaults
         * from base.c:415/427: variance AQ (strength 1.0) and MB-tree, with
         * qcompress forced to 1.0 by the port when MB-tree is on
         * (ratecontrol.c:762-768).  Presets that disable them stay disabled. */
        p.i_aq_mode = 0;                               /* CQP disables AQ */
        p.b_mb_tree = 0;                               /* CQP disables MB-tree */
    }
    phx_param_finalize(&p);                            /* encoder.c:1218-1231 */

    phx_t *enc = phx_encoder_open(&p);                 /* api.c:76 */
    if (!enc) {
        fprintf(stderr, "phx_cli: encoder open failed (check dimensions)\n");
        return 1;
    }
    if (bitrate_mode) {
        /* x264 param.rc.i_bitrate/fps (base.c:403, ratecontrol.c:757) live in
         * the ratecontrol config, not phx_param_t.  fps is 25 here: the CLI
         * has no fps argument and the port's VUI timebase is 1/25. */
        phx_ratecontrol_config_t rc_cfg;
        memset(&rc_cfg, 0, sizeof(rc_cfg));
        rc_cfg.bitrate = (double)bitrate_kbps * 1000.0;
        rc_cfg.fps = 25.0;
        rc_cfg.duration = 0.0;                         /* 1/fps, ratecontrol.h:66 */
        rc_cfg.qcompress = 0.6;                        /* base.c:423 */
        rc_cfg.rate_tolerance = 1.0;                   /* base.c:404 */
        rc_cfg.qp_min = PHX_RC_QP_MIN;                 /* base.c:410 */
        rc_cfg.qp_max = PHX_RC_QP_MAX;                 /* base.c:411 */
        rc_cfg.qp_step = 4;                            /* base.c:412 */
        if (phx_encoder_configure_ratecontrol(enc, &rc_cfg)) {
            fprintf(stderr, "phx_cli: rate control configuration failed\n");
            phx_encoder_close(enc);
            return 1;
        }
    }

    size_t frame_bytes = (size_t)w * (size_t)h * 3 / 2;
    uint8_t *frame = (uint8_t *)malloc(frame_bytes ? frame_bytes : 1);
    FILE *fin = fopen(yuv_path, "rb");
    FILE *fout = fopen(out_path, "wb");
    if (!frame || !fin || !fout) {
        fprintf(stderr, "phx_cli: cannot open input/output\n");
        if (fin) fclose(fin);
        if (fout) fclose(fout);
        free(frame);
        phx_encoder_close(enc);
        return 1;
    }

    int nframes = cli_frame_count(fin, frame_bytes);   /* input/raw.c:113 */
    if (nframes <= 0) {
        fprintf(stderr, "phx_cli: no complete I420 frames in '%s'\n", yuv_path);
        fclose(fin);
        fclose(fout);
        free(frame);
        phx_encoder_close(enc);
        return 1;
    }

    /* x264.c:1977 -> encoder.c:2096: emit SPS+PPS+SEI once.  phx_headers()
     * wraps the ported phx_encoder_headers()/phx_sei_version_write(). */
    phx_nal_t *hnals = NULL;
    int hnnal = 0;
    int n = phx_headers(enc, &hnals, &hnnal);
    if (n <= 0 || hnnal < 3 ||
        cli_write_nals(fout, hnals, hnnal)) {          /* x264.c:1864 */
        fprintf(stderr, "phx_cli: header emission failed\n");
        fclose(fin);
        fclose(fout);
        free(frame);
        phx_encoder_close(enc);
        return 1;
    }

    phx_picture_t pic;
    cli_make_picture(&pic, frame, w, h);
    int rc = 0;

    /* x264.c:2024 — one picture per loop iteration.  phx_encode() may return
     * zero NALs while the lookahead window fills (encoder.c:3428-3433). */
    for (int i = 0; i < nframes; i++) {
        if (cli_read_i420(fin, w, h, frame)) {         /* input/raw.c:113 */
            fprintf(stderr, "phx_cli: short I420 frame %d\n", i);
            rc = 1;
            break;
        }
        phx_nal_t *out = NULL;
        int nnal = 0;
        uint8_t *recon = NULL;
        rc = phx_encode(enc, &pic, &out, &nnal, &recon); /* x264.c:1858 */
        if (rc < 0 || (nnal && cli_write_nals(fout, out, nnal))) {
            fprintf(stderr, "phx_cli: encode failed on frame %d (%d)\n", i, rc);
            rc = 1;
            break;
        }
        rc = 0;
    }

    /* x264.c:2049 — flush: phx_encoder_encode(NULL) drains one decided frame
     * per call and returns 0/NULL when the lookahead is empty. */
    for (;;) {
        phx_nal_t *out = NULL;
        int nnal = 0;
        uint8_t *recon = NULL;
        rc = phx_encoder_encode(enc, NULL, &out, &nnal, &recon);
        if (rc < 0) {
            fprintf(stderr, "phx_cli: flush failed (%d)\n", rc);
            break;
        }
        if (nnal && cli_write_nals(fout, out, nnal)) {
            rc = 1;
            break;
        }
        if (rc == 0) break;                            /* lookahead drained */
        rc = 0;                                        /* keep draining */
    }

    fclose(fin);
    fclose(fout);
    free(frame);
    phx_encoder_close(enc);                            /* api.c:130 */
    return rc < 0 ? 1 : rc;
}
