/* Slice/MB decoder driver smoke test.
 *
 * Part 1 parses the saved x264 reference stream
 * build/parity/synth-ultrafast-cavlc.ref.h264 (8-bit 4:2:0, 64x64, CAVLC):
 * NAL types [7,8,6,5,1x7], SPS/PPS fields, and the first IDR slice header
 * (encoder.c:213-364).  Part 2 checks byte-for-byte deterministic
 * reconstruction with phx_decode_nal() on a small, self-generated CAVLC IDR
 * (one MB), which isolates the reconstruction kernels from the reference
 * stream's still-unported tail syntax (see the limitations printed below).
 *
 * Sources mirrored by src/phx_decode.c:
 *   reference/x264/encoder/set.c:305-570 (SPS/PPS),
 *   reference/x264/encoder/encoder.c:213-364, :2791-3093 (slice/MB),
 *   reference/x264/encoder/cavlc.c:241-595 (CAVLC header/residual),
 *   reference/x264/common/bitstream.c:55-75 (Annex-B NAL framing).
 *
 * No parity claim is made.  SHADER: C — serial host-side test.
 */
#include "phx_decode.h"
#include "phx_encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond,msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

#define MAX_NALS 64

static uint8_t *read_stream(const char *path, int *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END)) { fclose(fp); return NULL; }
    long n = ftell(fp);
    if (n <= 0 || fseek(fp, 0, SEEK_SET)) { fclose(fp); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, fp);
    fclose(fp);
    if ((long)got != n) { free(buf); return NULL; }
    *len = (int)n;
    return buf;
}

static uint32_t hash_bytes(const uint8_t *p, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t frame_hash(const phx_frame_t *f)
{
    uint32_t h = hash_bytes(f->plane, f->i_stride * f->i_height);
    return h * 16777619u +
           hash_bytes(f->plane_chroma, f->i_stride_planes[1] * (f->i_height/2));
}

static uint32_t decision_hash(const phx_mb_decision_t *d, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        h = h * 16777619u + (uint32_t)d[i].mb_type;
        h = h * 16777619u + (uint32_t)d[i].i_partition;
        h = h * 16777619u + (uint32_t)d[i].i_qp;
        h = h * 16777619u + (uint32_t)d[i].i_cbp;
        h = h * 16777619u + (uint32_t)d[i].nnz[0];
        h = h * 16777619u + (uint32_t)d[i].dct[0][0];
    }
    return h;
}

/* Reference-stream structure and the first IDR header (SHADER: C). */
static void check_reference(const char *path)
{
    int size = 0;
    uint8_t *data = read_stream(path, &size);
    if (!data) { printf("FAIL cannot read %s\n", path); ++fails; return; }
    printf("reference: %s (%d bytes)\n", path, size);

    int types[MAX_NALS], offs[MAX_NALS], lens[MAX_NALS];
    int nn = phx_decode_annexb_nals(data, size, types, offs, lens, MAX_NALS);
    static const int want[] = { 7, 8, 6, 5, 1, 1, 1, 1, 1, 1, 1 };
    CHECK(nn == (int)(sizeof(want)/sizeof(want[0])), "NAL count (want 11)");
    for (int i = 0; i < nn && i < (int)(sizeof(want)/sizeof(want[0])); i++)
        CHECK(types[i] == want[i], "NAL types [7,8,6,5,1x7]");

    phx_sps_t sps;
    phx_pps_t pps;
    CHECK(!phx_decode_sps(data + offs[0], lens[0], &sps), "SPS parse");
    CHECK(!phx_decode_pps(data + offs[1], lens[1], &pps), "PPS parse");
    CHECK(sps.i_mb_width == 4 && sps.i_mb_height == 4, "SPS 64x64 / 4x4 MBs");
    CHECK(sps.i_chroma_format_idc == 1 && sps.b_frame_mbs_only == 1,
          "SPS 4:2:0 progressive");
    CHECK(sps.i_log2_max_frame_num == 4 && sps.i_poc_type == 2,
          "SPS log2fn=4, poc_type=2");
    CHECK(!pps.b_cabac && pps.i_num_ref_idx_l0 == 1 &&
          pps.i_num_ref_idx_l1 == 1 && pps.i_pic_init_qp == 28,
          "PPS CAVLC/1 ref/QP28");

    phx_slice_header_t sh;
    int end_bit = -1;
    CHECK(!phx_decode_slice_header(data + offs[3], lens[3], &sps, &pps,
                                   &sh, &end_bit), "IDR slice header");
    CHECK(sh.i_first_mb == 0 && sh.i_type == PHX_SLICE_I, "IDR first_mb/type");
    CHECK(sh.i_frame_num == 0 && sh.i_idr_pic_id == 0 && sh.i_poc == 0,
          "IDR frame_num/idr_pic_id/POC");
    CHECK(sh.i_qp_delta == -3 && sh.i_qp == 25, "IDR qp_delta=-3, slice_qp=25");
    CHECK(sh.i_disable_deblocking_filter_idc == 1, "IDR deblocking disabled");
    CHECK(end_bit == 24, "IDR header end bit (encoder.c:2791)");

    /* Re-parsing is deterministic too, even though the full MB loop of this
     * long reference stream is not yet ported (reported at the end). */
    phx_slice_header_t sh2;
    int end2 = -1;
    CHECK(!phx_decode_slice_header(data + offs[3], lens[3], &sps, &pps,
                                   &sh2, &end2), "IDR header reparse");
    CHECK(end2 == end_bit && sh2.i_qp == sh.i_qp &&
          sh2.i_frame_num == sh.i_frame_num && sh2.i_poc == sh.i_poc,
          "deterministic slice header");
    printf("  IDR: type=%d fn=%d idr=%d qp=%d(%+d) deblock=%d end=%d\n",
           sh.i_type, sh.i_frame_num, sh.i_idr_pic_id, sh.i_qp,
           sh.i_qp_delta, sh.i_disable_deblocking_filter_idc, end_bit);
    free(data);
}

/* Deterministic reconstruction on a one-MB CAVLC IDR made by phx_encoder.
 * SHADER: C — serial decode of two identical NALs. */
static void check_single_mb_reconstruction(void)
{
    phx_param_t p;
    phx_param_default(&p);                             /* base.c:344 */
    phx_param_apply_preset(&p, "ultrafast");           /* base.c:489 */
    p.i_width = 32; p.i_height = 16;
    p.i_qp_constant = 28;
    p.b_cabac = 0;
    p.i_bframe = 0;
    p.i_keyint_max = p.i_keyint_min = 30;
    p.i_scenecut_threshold = 0;
    p.i_aq_mode = 0;
    p.b_mb_tree = 0;
    phx_param_finalize(&p);                            /* encoder.c:1218 */

    phx_sps_t sps;
    phx_pps_t pps;
    phx_sps_init(&sps, &p, 32, 16);                    /* set.c:101 */
    phx_pps_init(&pps, &sps, &p);                      /* set.c:479 */

    phx_t *h = phx_encoder_open(&p);                   /* encoder.c:1507 */
    if (!h) { printf("FAIL single-MB: encoder open\n"); ++fails; return; }
    static uint8_t plane[32*16 + 2*16*8];
    for (size_t k = 0; k < sizeof(plane); k++)
        plane[k] = (uint8_t)((k*11 + 3) & 255);
    phx_picture_t pic;
    memset(&pic, 0, sizeof(pic));
    pic.plane[0] = plane;
    pic.plane[1] = plane + 32*16;
    pic.plane[2] = plane + 32*16 + 16*8;
    pic.i_stride[0] = 32; pic.i_stride[1] = pic.i_stride[2] = 16;
    pic.i_width = 32; pic.i_height = 16;
    uint8_t stream[8192];
    int used = 0;
    for (int call = 0; call < 8; call++) {
        phx_nal_t *nas = NULL;
        int nn = 0;
        uint8_t *rr = NULL;
        int rc = call ? phx_encoder_encode(h, NULL, &nas, &nn, &rr)
                      : phx_encode(h, &pic, &nas, &nn, &rr);
        if (rc < 0) { printf("FAIL single-MB: encode\n"); ++fails; break; }
        for (int i = 0; i < nn; i++)
            if (used + nas[i].i_payload <= (int)sizeof(stream)) {
                memcpy(stream + used, nas[i].p_payload, (size_t)nas[i].i_payload);
                used += nas[i].i_payload;
            }
        if (!call && rc == 0 && nn == 0) break;
        if (rc == 0 && call > 0) break;
    }
    phx_encoder_close(h);                              /* api.c:130 */
    int types2[MAX_NALS], offs2[MAX_NALS], lens2[MAX_NALS];
    int nn2 = phx_decode_annexb_nals(stream, used, types2, offs2, lens2, MAX_NALS);
    int idr = -1;
    for (int i = 0; i < nn2; i++)
        if (types2[i] == 5) { idr = i; break; }
    printf("  encoder emitted %d NALs:", nn2);
    for (int i = 0; i < nn2; i++) printf(" %d", types2[i]);
    printf("\n");
    if (idr < 0) { printf("FAIL single-MB: no IDR NAL\n"); ++fails; return; }
    uint8_t nal_copy[2048];
    if (lens2[idr] > (int)sizeof(nal_copy)) {
        printf("FAIL single-MB: NAL too large\n"); ++fails; return;
    }
    memcpy(nal_copy, stream + offs2[idr], (size_t)lens2[idr]);
    int nal_size = lens2[idr];

    CHECK(!phx_decode_set_params(&sps, &pps), "install SPS/PPS");
    phx_frame_t *f[2] = { phx_frame_new(32, 16, 0), phx_frame_new(32, 16, 0) };
    phx_mb_state_t *s[2] = { phx_mb_state_new(2, 1), phx_mb_state_new(2, 1) };
    phx_mb_decision_t d[2][2];
    int nd[2] = { -1, -1 };
    CHECK(f[0] && f[1] && s[0] && s[1], "single-MB allocation");
    for (int k = 0; k < 2 && f[k] && s[k]; k++) {
        int rc = phx_decode_nal(nal_copy, nal_size, f[k], s[k], d[k], &nd[k]);
        CHECK(!rc, "phx_decode_nal single MB");
        CHECK(nd[k] == 2, "two MB decisions");
    }
    if (f[0] && f[1] && s[0] && s[1] && nd[0] == 2 && nd[1] == 2) {
        uint32_t h0 = frame_hash(f[0]), h1 = frame_hash(f[1]);
        uint32_t q0 = decision_hash(d[0], 2), q1 = decision_hash(d[1], 2);
        CHECK(h0 == h1, "deterministic reconstruction");
        CHECK(q0 == q1, "deterministic MB decisions");
        printf("  single MB: frame %08X/%08X, decisions %08X/%08X\n",
               h0, h1, q0, q1);
    }
    for (int k = 0; k < 2; k++) {
        if (f[k]) phx_frame_delete(f[k]);
        if (s[k]) phx_mb_state_delete(s[k]);
    }
}

int main(int argc, char **argv)
{
    check_reference(argc > 1 ? argv[1]
                             : "build/parity/synth-ultrafast-cavlc.ref.h264");
    check_single_mb_reconstruction();
    printf("decoder: known limitations — CABAC MB syntax, B-direct/weighted "
           "prediction, MBAFF, ref-list modification and the reference "
           "stream's tail MBs are explicit TODOs (no approximation used).\n");
    printf("%s: phx_decode Annex-B/header/MB, %d failures\n",
           fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
