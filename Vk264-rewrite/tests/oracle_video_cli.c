/* SPDX-License-Identifier: GPL-2.0-or-later
 * Oracle video CLI: multi-frame Annex-B H.264 encoder for byte-exact
 * comparison against libx264 on real YUV420 clips.
 *
 * Usage: oracle_video_cli input.yuv w h qp_i qp_p entropy keyint frames out.h264
 *   qp_i/qp_p: IDR slice QP and P-slice QP (x264 derives qp_i from ipratio)
 *   entropy: 0=CAVLC, 1=CABAC
 *
 * Non-reference B-frames, weighted prediction, 8x8 transform, trellis, AQ
 * and lookahead are not modelled: the oracle reproduces x264's ultrafast
 * analysis path. Other presets are rejected by the verification harness.
 */
#include "bitstream.h"
#include "param.h"
#include "slice.h"
#include "pframe.h"
#include "cabac.h"
#include "cabac_enc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void encode_idr_cavlc(vk_bs_t *bs, const vk_sps_t *sps, const vk_pps_t *pps,
                             const uint8_t *frame, int w, int h, int qp,
                             int idr_pic_id,
                             uint8_t (*nnz_c)[16], uint8_t (*cnnz_c)[2][4],
                             uint8_t *dcnz_c,
                             uint8_t *ry, int rs, int ro,
                             uint8_t *ru, uint8_t *rv, int cs, int co,
                             uint8_t *recon)
{
    int mw = w/16, mh = h/16;
    vk_slice_header_t sh;
    vk_slice_header_init(&sh, sps, pps, qp);
    sh.i_idr_pic_id = idr_pic_id;
    vk_slice_header_write(bs, &sh, sps, pps, 3);

    for (int my = 0; my < mh; ++my)
        for (int mx = 0; mx < mw; ++mx) {
            int px = mx*16, py = my*16;
            int avail = (mx ? 1 : 0) | (my ? 2 : 0) | (my && mx ? 8 : 0);
            vk_neighbours_t nb; memset(&nb, 0, sizeof(nb));
            nb.avail = avail; nb.qp = qp;
            uint8_t ll[4], tl[4], lc[2][2], tc[2][2];
            for (int i = 0; i < 4; ++i) {
                ll[i] = mx ? nnz_c[my*mw+mx-1][i*4+3] : 0x80;
                tl[i] = my ? nnz_c[(my-1)*mw+mx][12+i] : 0x80;
            }
            for (int q = 0; q < 2; ++q)
                for (int i = 0; i < 2; ++i) {
                    lc[q][i] = mx ? cnnz_c[my*mw+mx-1][q][i*2+1] : 0x80;
                    tc[q][i] = my ? cnnz_c[(my-1)*mw+mx][q][2+i] : 0x80;
                }
            nb.left_luma = ll; nb.top_luma = tl;
            nb.left_chroma[0] = (const uint8_t *)lc[0];
            nb.left_chroma[1] = (const uint8_t *)lc[1];
            nb.top_chroma[0] = (const uint8_t *)tc[0];
            nb.top_chroma[1] = (const uint8_t *)tc[1];
            uint8_t top[16], left[16], tup[8], lup[8], tvp[8], lvp[8];
            for (int i = 0; i < 16; ++i) {
                top[i] = my ? ry[ro+(py-1)*rs+px+i] : 128;
                left[i] = mx ? ry[ro+(py+i)*rs+px-1] : 128;
            }
            nb.top_y = top; nb.left_y = left;
            nb.tl_y = (my && mx) ? ry[ro+(py-1)*rs+px-1] : 128;
            for (int i = 0; i < 8; ++i) {
                tup[i] = my ? ru[co+(py/2-1)*cs+px/2+i] : 128;
                tvp[i] = my ? rv[co+(py/2-1)*cs+px/2+i] : 128;
                lup[i] = mx ? ru[co+(py/2+i)*cs+px/2-1] : 128;
                lvp[i] = mx ? rv[co+(py/2+i)*cs+px/2-1] : 128;
            }
            nb.top_u = (const uint8_t *)tup; nb.left_u = (const uint8_t *)lup;
            nb.top_v = (const uint8_t *)tvp; nb.left_v = (const uint8_t *)lvp;
            nb.tl_u = (my && mx) ? ru[co+(py/2-1)*cs+px/2-1] : 128;
            nb.tl_v = (my && mx) ? rv[co+(py/2-1)*cs+px/2-1] : 128;
            const uint8_t *sy = frame + py*w + px;
            const uint8_t *su = frame + (size_t)w*h + (py/2)*(w/2) + px/2;
            const uint8_t *sv = su + (w/2)*(h/2);
            uint8_t oy[256], ou[64], ov[64], on[16], oc[2][4], od;
            vk_encode_i16_mb(bs, sy, su, sv, oy, ou, ov, qp, mx, my, &nb,
                             w, w/2, on, oc, &od);
            memcpy(nnz_c[my*mw+mx], on, 16);
            memcpy(cnnz_c[my*mw+mx], oc, 8);
            dcnz_c[my*mw+mx] = od;
            for (int y = 0; y < 16; ++y)
                memcpy(ry + ro + (py+y)*rs + px, oy + y*16, 16);
            for (int y = 0; y < 8; ++y) {
                memcpy(ru + co + (py/2+y)*cs + px/2, ou + y*8, 8);
                memcpy(rv + co + (py/2+y)*cs + px/2, ov + y*8, 8);
            }
            if (recon) {
                for (int y = 0; y < 16; ++y)
                    memcpy(recon + (py+y)*w + px, oy + y*16, 16);
                for (int y = 0; y < 8; ++y) {
                    memcpy(recon + (size_t)w*h + (py/2+y)*(w/2) + px/2, ou + y*8, 8);
                    memcpy(recon + (size_t)w*h + (size_t)(w/2)*(h/2)
                           + (py/2+y)*(w/2) + px/2, ov + y*8, 8);
                }
            }
        }
    vk_bs_rbsp_trailing(bs);
}

int main(int argc, char **argv)
{
    if (argc != 10 && argc != 11) {
        fprintf(stderr, "usage: oracle_video_cli in.yuv w h qp_i qp_p entropy(0/1) keyint frames out.h264 [preset]\n");
        return 2;
    }
    const char *preset = (argc == 11) ? argv[10] : "ultrafast";
    int w = atoi(argv[2]), h = atoi(argv[3]);
    int qp_i = atoi(argv[4]), qp_p = atoi(argv[5]);
    int entropy = atoi(argv[6]), keyint = atoi(argv[7]), nframes = atoi(argv[8]);
    if (w % 16 || h % 16 || nframes < 1 || keyint < 1) return 2;
    int mw = w/16, mh = h/16;
    size_t fs = (size_t)w*h*3/2;
    uint8_t *frames = malloc(fs);
    FILE *fi = fopen(argv[1], "rb");
    if (!fi) return 1;
    FILE *fo = fopen(argv[9], "wb");
    if (!fo) { fclose(fi); return 1; }

    vk_param_t p; vk_sps_t sps; vk_pps_t pps; vk_bs_t bs;
    vk_cabac_t cb;
    static uint8_t buf[1 << 24];
    vk_cavlc_init();
    vk_param_default(&p);
    if (vk_param_apply_preset(&p, preset) < 0) {
        fprintf(stderr, "unknown preset: %s\n", preset);
        return 2;
    }
    p.i_width = w; p.i_height = h; p.i_qp_constant = qp_p;
    /* --cabac is an explicit override; without it the preset default stands. */
    if (entropy) p.b_cabac = 1;
    p.i_keyint_max = keyint;
    vk_sps_init(&sps, 0, &p);
    vk_pps_init(&pps, 0, &p, &sps);

    memset(buf, 0, sizeof(buf)); vk_bs_init(&bs, buf, sizeof(buf));
    vk_sps_write(&bs, &sps);
    vk_nal_write(fo, 0x67, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 1);
    memset(buf, 0, sizeof(buf)); vk_bs_init(&bs, buf, sizeof(buf));
    vk_pps_write(&bs, &sps, &pps);
    vk_nal_write(fo, 0x68, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 1);

    /* match x264 PADH/PADV=32 (luma) and the 4:2:0 chroma equivalent */
    int rs = w + 64, rh = h + 64, ro = 32*rs + 32;
    int cs = w/2 + 32, co = 16*cs + 16;
    uint8_t (*nnz_c)[16] = calloc((size_t)mw*mh, 16);
    uint8_t (*cnnz_c)[2][4] = calloc((size_t)mw*mh, 8);
    uint8_t *dcnz_c = calloc((size_t)mw*mh, 1);
    uint8_t *ry = malloc((size_t)rs*rh), *ru = malloc((size_t)cs*(h/2+32));
    uint8_t *rv = malloc((size_t)cs*(h/2+32));
    memset(ry, 128, (size_t)rs*rh);
    memset(ru, 128, (size_t)cs*(h/2+32));
    memset(rv, 128, (size_t)cs*(h/2+32));
    uint8_t *ref = calloc(1, fs), *recon = calloc(1, fs);
    /* x264's fdec->mv16x16: per-MB analysis MVs of the reference frame,
     * used for temporal MV predictors in chained P-slices. */
    int (*refmv)[2] = calloc((size_t)mw*mh, 2*sizeof(int));
    int (*curmv)[2] = calloc((size_t)mw*mh, 2*sizeof(int));
    /* x264 fdec->mb_type intra flags for collocated b_fast_intra decisions.
     * Frame 0 is an IDR (all intra), so seed the reference map with ones. */
    uint8_t *refintra = malloc((size_t)mw*mh), *curintra = malloc((size_t)mw*mh);
    memset(refintra, 1, (size_t)mw*mh);
    memset(curintra, 0, (size_t)mw*mh);
    int idr_count = 0;

    for (int f = 0; f < nframes; ++f) {
        if (fread(frames, 1, fs, fi) != fs) { fprintf(stderr, "short input at frame %d\n", f); break; }
        int is_idr = (f % keyint == 0);
        memset(buf, 0, sizeof(buf)); vk_bs_init(&bs, buf, sizeof(buf));
        if (is_idr) {
            int idr_pic_id = idr_count & 1;
            ++idr_count;
            if (entropy) {
                vk_slice_header_t sh;
                vk_slice_header_init(&sh, &sps, &pps, qp_i);
                sh.i_idr_pic_id = idr_pic_id;
                vk_slice_header_write(&bs, &sh, &sps, &pps, 3);
                vk_bs_align_1(&bs);
                vk_cabac_init_ctx(&cb, 2, qp_i, 0);
                vk_cabac_encode_idr_recon_frame(&bs, &cb, frames, w, h, qp_i, recon, f);
                memset(curintra, 1, (size_t)mw*mh);  /* IDR: all MBs intra */
            } else {
                memset(nnz_c, 0, (size_t)mw*mh*16);
                memset(cnnz_c, 0, (size_t)mw*mh*8);
                memset(dcnz_c, 0, (size_t)mw*mh);
                memset(ry, 128, (size_t)rs*rh);
                memset(ru, 128, (size_t)cs*(h/2+32));
                memset(rv, 128, (size_t)cs*(h/2+32));
                encode_idr_cavlc(&bs, &sps, &pps, frames, w, h, qp_i,
                                 idr_pic_id, nnz_c, cnnz_c, dcnz_c,
                                 ry, rs, ro, ru, rv, cs, co, recon);
                memset(curintra, 1, (size_t)mw*mh);  /* IDR: all MBs intra */
            }
            vk_nal_write(fo, 0x65, buf,
                         (size_t)(entropy ? bs.p - buf : (vk_bs_pos(&bs)+7)/8), 3);
        } else {
            int frame_num = f % keyint;
            if (entropy) {
                vk_slice_header_t sh; memset(&sh, 0, sizeof(sh));
                sh.i_type = 0; sh.i_pps_id = 0; sh.i_frame_num = frame_num;
                sh.i_idr_pic_id = -1; sh.i_qp_delta = qp_p - pps.i_pic_init_qp;
                sh.i_disable_deblocking_filter_idc = 1;
                vk_slice_header_write(&bs, &sh, &sps, &pps, 2);
                vk_bs_align_1(&bs);
                vk_cabac_init_ctx(&cb, 0, qp_p, 0);
                vk_cabac_encode_p_recon_frame(&bs, &cb, frames, ref, w, h, qp_p, recon, f,
                                              refmv, curmv, refintra, curintra);
                vk_nal_write(fo, 0x41, buf, (size_t)(bs.p - buf), 2);
            } else {
                vk_encode_p_frame_recon_mv(&bs, &sps, &pps, frames, ref,
                                           w, h, qp_p, frame_num, recon,
                                           refmv, curmv, refintra, curintra, 0);
                vk_nal_write(fo, 0x41, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 2);
            }
        }
        const char *rpath = getenv("ORACLE_DUMP_RECON");
        if (rpath) {
            char rp[1024]; snprintf(rp, sizeof(rp), "%s.%d", rpath, f);
            FILE *rf = fopen(rp, "wb");
            if (rf) { fwrite(recon, 1, fs, rf); fclose(rf); }
        }
        memcpy(ref, recon, fs);
        int (*tmp)[2] = refmv; refmv = curmv; curmv = tmp;
        memset(curmv, 0, (size_t)mw*mh*2*sizeof(int));
        uint8_t *tmpi = refintra; refintra = curintra; curintra = tmpi;
        memset(curintra, 0, (size_t)mw*mh);
    }
    fclose(fi); fclose(fo);
    free(frames); free(ref); free(recon); free(refmv); free(curmv);
    free(refintra); free(curintra); free(ry); free(ru); free(rv);
    free(nnz_c); free(cnnz_c); free(dcnz_c);
    return 0;
}
