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
#include "i4x4.h"
#include "i4x4enc.h"
#include "i8x8enc.h"
#include "bslice.h"
#include "slicetype.h"
#include "lowres.h"
#include "ratecontrol.h"
#include "encoder.h"
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
                             uint8_t *recon, int use_i4x4)
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
            if (use_i4x4 && mx==0 && my==0 && getenv("ORACLE_I4X4")) {
                uint8_t modes[16];
                uint8_t *tmp = malloc((size_t)rs*(h+64));
                memset(tmp, 128, (size_t)rs*(h+64));
                vk_i4x4_decide(sy, tmp+32*rs+32, rs, px, py, qp, 0,0,0,0, modes);
                free(tmp);
                vk_encode_i4x4_mb(bs, sy, su, sv, oy, ou, ov, qp, &nb, w, w/2,
                                  modes, on, oc, &od, 0);
            } else if (getenv("ORACLE_I8X8") &&
                       ((mx==1 && my==0) || (mx==0 && my==1))) {
                /* Test path for x264's I8x8 MBs on the 64x64 clip. */
                /* x264 internal pred modes: MB1 (top row) uses DC_LEFT(9)
                 * for its top 8x8 blocks; MB4 (left col) uses DC_TOP(10). */
                uint8_t modes[4];
                if (mx==1 && my==0) { modes[0]=9; modes[1]=9; modes[2]=2; modes[3]=2; }
                else { modes[0]=10; modes[1]=2; modes[2]=10; modes[3]=2; }
                int cm = 0;  /* x264 writes DC (0) for these I8x8 MBs */
                uint8_t sy16[256];
                for (int yy=0; yy<16; ++yy)
                    memcpy(sy16+yy*16, sy+yy*w, 16);
                vk_encode_i8x8_mb(bs, sy16, su, sv, oy, ou, ov, qp, &nb,
                                  16, 8, modes, cm, on, oc, &od, 0);
            } else
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
    /* Optional CRF: x264 derives each frame's complexity from its lookahead
     * cost (x264_rc_analyse_slice -> i_cost_est) and feeds it through the
     * ratecontrol state to pick a QP. Off by default: fixed-QP paths stay
     * byte-exact. */
    const char *crf_env = getenv("VK264_CRF");
    double crf = crf_env ? atof(crf_env) : -1.0;
    vk_rc_state_t rc;
    int dw = w/2, dh = h/2;
    uint8_t *lo_cur = NULL, *lo_ref = NULL;
    if (crf >= 0) {
        vk_rc_state_init(&rc, mw*mh, strstr(preset,"ultrafast") ? 0 : 1, 0.6, 0, crf);
        lo_cur = malloc((size_t)dw*dh*VK_LOWRES_PLANES);
        lo_ref = malloc((size_t)dw*dh*VK_LOWRES_PLANES);
    }
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
    /* The entropy argument is an explicit CAVLC/CABAC override. */
    p.b_cabac = entropy ? 1 : 0;
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

    /* Optional general slice-type + encoder-state path (VK_SLICETYPE_CLI). */
    vk_frame_t *stypes = NULL; int *corder = NULL, ncoded = 0;
    uint8_t *allframes = NULL;
    if (getenv("VK_SLICETYPE_CLI")) {
        allframes = malloc(fs*nframes);
        rewind(fi);
        if (fread(allframes, fs, nframes, fi) != (size_t)nframes) {
            fprintf(stderr,"short input for slicetype\n"); return 2;
        }
        stypes = calloc(nframes, sizeof(*stypes));
        corder = calloc(nframes, sizeof(*corder));
        vk_slicetype_decide(allframes, nframes, w, h, keyint, keyint,
                            p.i_bframe, 2, 40, stypes, corder, &ncoded);
        rewind(fi);
    }
    for (int f = 0; f < nframes; ++f) {
        if (allframes) memcpy(frames, allframes+(size_t)f*fs, fs);
        else if (fread(frames, 1, fs, fi) != fs) { fprintf(stderr, "short input at frame %d\n", f); break; }
        int is_idr = stypes ? (stypes[f].type == VK_FRAME_IDR) : (f % keyint == 0);
        if (crf >= 0) {
            vk_lowres_luma(frames, w, h, lo_cur, dw, dh);
            double cost = is_idr ? vk_lowres_i_cost(lo_cur, dw, dh)
                                 : vk_lowres_p_cost(lo_cur, lo_ref, dw, dh, NULL);
            double q = vk_rc_frame_qscale(&rc, cost);
            int crf_qp = (int)(vk_qscale2qp((float)q) + 0.5f);
            if (crf_qp < 0) crf_qp = 0; if (crf_qp > 51) crf_qp = 51;
            qp_i = qp_p = crf_qp;
            memcpy(lo_ref, lo_cur, (size_t)dw*dh*VK_LOWRES_PLANES);
        }
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
                                 ry, rs, ro, ru, rv, cs, co, recon, p.i_subme > 0);
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
                sh.i_disable_deblocking_filter_idc = pps.b_deblocking_filter ? 0 : 1;
                vk_slice_header_write(&bs, &sh, &sps, &pps, 2);
                vk_bs_align_1(&bs);
                vk_cabac_init_ctx(&cb, 0, qp_p, 0);
                vk_cabac_encode_p_recon_frame(&bs, &cb, frames, ref, w, h, qp_p, recon, f,
                                              refmv, curmv, refintra, curintra);
                vk_nal_write(fo, 0x41, buf, (size_t)(bs.p - buf), 2);
            } else if (getenv("ORACLE_BSLICE_TEST") && f == 5) {
                /* x264 frame 5: P-slice, all MBs skipped, POC=12, fn=3 */
                vk_encode_p_slice_allskip(&bs, &sps, &pps, 3, 12,
                                          qp_p, mw*mh);
                vk_bs_rbsp_trailing(&bs);
                vk_nal_write(fo, 0x41, buf,
                             (size_t)((vk_bs_pos(&bs)+7)/8), 2);
                memcpy(recon, ref, (size_t)w*h*3/2);
            } else if (getenv("ORACLE_BSLICE_TEST") &&
                       (f==2 || f==3 || f==4 || f==6)) {
                /* x264 superfast bframes=3 frame map (frame_num, POC) for
                 * the 8-frame test clip, in coding order. */
                static const int bfn[8]  = {0,1,2,3,3,3,4,4};
                static const int bpoc[8] = {0,8,4,2,6,12,10,14};
                static const int bref[8] = {3,2,2,0,0,2,0,2};
                int bn = (f < 8) ? bfn[f] : frame_num;
                int bp = (f < 8) ? bpoc[f] : 2*frame_num;
                int br = (f < 8) ? bref[f] : 0;
                /* x264: reference B uses qp offset 1, non-reference B uses 2 */
                vk_encode_b_slice_allskip(&bs, &sps, &pps, bn, bp,
                                          qp_p + (br ? 1 : 2), mw*mh, br);
                vk_bs_rbsp_trailing(&bs);
                vk_nal_write(fo, br ? 0x21 : 0x01, buf,
                             (size_t)((vk_bs_pos(&bs)+7)/8), 2);
                memcpy(recon, ref, (size_t)w*h*3/2);
            } else {
                int pfn = (getenv("ORACLE_BSLICE_TEST") && f==7) ? 4 : frame_num;
                int ppoc = (getenv("ORACLE_BSLICE_TEST") && f==7) ? 14 : 8*frame_num;
                vk_encode_p_frame_recon_mv(&bs, &sps, &pps, frames, ref,
                                           w, h, qp_p, pfn, recon,
                                           refmv, curmv, refintra, curintra,
                                           p.i_subme, ppoc);
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
