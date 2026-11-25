/* Smoke test for the Phoenix public encoder API: open/close and SPS+PPS
 * emission for a tiny I420 fixture.  Standalone main, 0 on pass.
 *
 * Source: reference/x264/encoder/api.c:76/159/166/130 (public entry points),
 *         reference/x264/encoder/encoder.c:1507 (open), :2096 (headers),
 *         :3323 (encode), :4196 (close),
 *         reference/x264/encoder/set.c:305/:505 (SPS/PPS write).
 *
 * SHADER: C — serial host-side smoke test; no rendering or pixel shaders.
 */
#include "phx_encoder.h"
#include <stdio.h>
#include <string.h>

#define TW 16
#define TH 16

/* Tiny deterministic I420 fixture: one 16x16 Y plane plus two 8x8 chroma
 * planes.  Mirrors the raw plane layout of input/raw.c:113-124.
 * SHADER: C — host-side fixture buffer. */
static const uint8_t fixture[TW*TH + 2*(TW*TH)/4] = {
    /* Y: a diagonal ramp filled at runtime below (static storage is zeroed). */
};

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

/* Verify the SPS/PPS pair a handle emits: x264_encoder_headers
 * (encoder.c:2096-2133) returns NAL_SPS + NAL_PPS with NAL_PRIORITY_HIGHEST.
 * Also check it is byte-identical to the low-level phx_encoder_headers()
 * (phx_headers.h:53), so the wrapper is not inventing a second bitstream.
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

/* Standalone main; x264_encoder_open/headers/encode/close (api.c:76/159/166/
 * 130, encoder.c:1507/2096/3323/4196).  SHADER: C — serial smoke test. */
int main(void)
{
    int fails = 0;

    const char *cases[][2] = { {"medium", "cabac"}, {"ultrafast", "cavlc"} };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        phx_param_t p;
        make_param(&p, cases[i][0], 28, !strcmp(cases[i][1], "cabac"));

        phx_t *h = phx_encoder_open(&p);
        if (!h) { printf("FAIL %s/%s: open\n", cases[i][0], cases[i][1]); ++fails; continue; }
        fails += check_headers(h, &p);

        /* The encode stub must not fabricate a stream: outputs are cleared and
         * the "not implemented" sentinel is returned (encoder.c:3354-3356). */
        uint8_t frame[sizeof(fixture)];
        for (size_t k = 0; k < sizeof(frame); k++)
            frame[k] = (uint8_t)((k*7 + 13) & 255);
        phx_picture_t pic;
        memset(&pic, 0, sizeof(pic));
        pic.plane[0] = frame;
        pic.plane[1] = frame + TW*TH;
        pic.plane[2] = frame + TW*TH + (TW*TH)/4;
        pic.i_stride[0] = TW; pic.i_stride[1] = TW/2; pic.i_stride[2] = TW/2;
        pic.i_width = TW; pic.i_height = TH;

        phx_nal_t *out = (phx_nal_t *)&pic;
        int nnal = 99;
        uint8_t *recon = frame;
        int rc = phx_encode(h, &pic, &out, &nnal, &recon);
        if (rc != PHX_ERR_UNIMPLEMENTED || out || nnal || recon) {
            printf("FAIL %s/%s: encode stub rc=%d out=%p nnal=%d recon=%p\n",
                   cases[i][0], cases[i][1], rc, (void *)out, nnal, (void *)recon);
            ++fails;
        }
        rc = phx_encoder_encode(h, &pic, &out, &nnal, &recon);
        if (rc != PHX_ERR_UNIMPLEMENTED) { printf("FAIL encode alias\n"); ++fails; }

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

    printf("%s: phx_encoder open/close/headers, %d failures\n",
           fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
