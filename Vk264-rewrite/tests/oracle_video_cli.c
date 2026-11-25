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
#include "sei_table.h"
#include "pframe.h"
#include "i4x4.h"
#include "i4x4enc.h"
#include "i8x8enc.h"
#include "bslice.h"
#include "slicetype.h"
#include "lowres.h"
#include "ratecontrol.h"
#include "mbtree.h"
#include "encoder.h"
#include "cabac.h"
#include "cabac_enc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void pad420(uint8_t *dst,int cw,int ch,const uint8_t *src,int w,int h){
    for(int y=0;y<ch;++y){ int sy=y<h?y:h-1;
        memcpy(dst+y*cw, src+sy*w, w);
        memset(dst+y*cw+w, src[sy*w+w-1], cw-w);
    }
    uint8_t *du=dst+(size_t)cw*ch, *dv=du+(size_t)(cw/2)*(ch/2);
    const uint8_t *su=src+(size_t)w*h, *sv=su+(size_t)(w/2)*(h/2);
    int sw=w/2,sh=h/2,scw=cw/2,sch=ch/2;
    for(int y=0;y<sch;++y){ int sy=y<sh?y:sh-1;
        memcpy(du+y*scw, su+sy*sw, sw); memset(du+y*scw+sw, su[sy*sw+sw-1], scw-sw);
        memcpy(dv+y*scw, sv+sy*sw, sw); memset(dv+y*scw+sw, sv[sy*sw+sw-1], scw-sw);
    }
}

static void encode_idr_cavlc(vk_bs_t *bs, const vk_sps_t *sps, const vk_pps_t *pps,
                             const uint8_t *frame, int w, int h, int qp,
                             int idr_pic_id,
                             uint8_t (*nnz_c)[16], uint8_t (*cnnz_c)[2][4],
                             uint8_t *dcnz_c,
                             uint8_t *ry, int rs, int ro,
                             uint8_t *ru, uint8_t *rv, int cs, int co,
                              uint8_t *recon, int use_i4x4,
                              const vk_idr_decisions_t *dec)
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
            int mbidx = my*mw+mx;
            if (dec && dec->mbts8[mbidx]) {
                uint8_t sy16[256];
                for (int yy=0; yy<16; ++yy)
                    memcpy(sy16+yy*16, sy+yy*w, 16);
                vk_encode_i8x8_mb(bs, sy16, su, sv, oy, ou, ov, qp, &nb,
                                  16, 8, dec->modes8[mbidx],
                                  dec->cmode_fix[mbidx], on, oc, &od, 0);
            } else if (dec && dec->mbtype_i4[mbidx]) {
                vk_encode_i4x4_mb(bs, sy, su, sv, oy, ou, ov, qp, &nb, w, w/2,
                                  dec->modes4[mbidx], on, oc, &od, 0);
            } else if (use_i4x4 && mx==0 && my==0 && getenv("ORACLE_I4X4")) {
                uint8_t modes[16];
                uint8_t *tmp = malloc((size_t)rs*(h+64));
                memset(tmp, 128, (size_t)rs*(h+64));
                vk_i4x4_decide(sy, tmp+32*rs+32, rs, px, py, qp, 0,0,0,0, modes,VK_I4_COST_MAX);
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
    if (w % 2 || h % 2 || nframes < 1 || keyint < 1) return 2;
    int cw = ((w+15)/16)*16, ch = ((h+15)/16)*16;
    int mw = cw/16, mh = ch/16;
    /* Optional CRF: x264 derives each frame's complexity from its lookahead
     * cost (x264_rc_analyse_slice -> i_cost_est) and feeds it through the
     * ratecontrol state to pick a QP. Off by default: fixed-QP paths stay
     * byte-exact. */
    const char *crf_env = getenv("VK264_CRF");
    double crf = crf_env ? atof(crf_env) : -1.0;
    vk_rc_state_t rc;
    int dw = cw/2, dh = ch/2;
    uint8_t *lo_cur = NULL, *lo_ref = NULL;
    /* mb-tree per-frame state (x264 frame_t.i_intra_cost / i_propagate_cost). */
    int n_mbs = (dw/8)*(dh/8);
    uint16_t *mb_intra = NULL, *mb_prop = NULL, *mb_cost = NULL, *mb_invq = NULL;
    int16_t (*mb_mv[2])[2] = {NULL,NULL};
    vk_lowres_mv_t *mvst = NULL;
    if (crf >= 0) {
        vk_rc_state_init(&rc, mw*mh, strstr(preset,"ultrafast") ? 0 : 1, 0.6, 0, crf);
        lo_cur = malloc((size_t)dw*dh*VK_LOWRES_PLANES);
        lo_ref = malloc((size_t)dw*dh*VK_LOWRES_PLANES);
        mb_intra = calloc((size_t)nframes*n_mbs,sizeof(uint16_t));
        mb_prop  = calloc((size_t)nframes*n_mbs,sizeof(uint16_t));
        mb_cost  = calloc((size_t)nframes*n_mbs,sizeof(uint16_t));
        mb_invq  = calloc((size_t)nframes*n_mbs,sizeof(uint16_t));
        for(int q=0;q<nframes*n_mbs;++q) mb_invq[q]=256;  /* Q8.8 1.0 */
        mb_mv[0] = calloc((size_t)nframes*n_mbs,sizeof(int16_t)*2);
        mb_mv[1] = calloc((size_t)nframes*n_mbs,sizeof(int16_t)*2);
        mvst = calloc(nframes,sizeof(*mvst));
        for(int q=0;q<nframes;++q) vk_lowres_mv_init(&mvst[q],dw,dh);
    }
    size_t fs = (size_t)w*h*3/2;
    size_t cfs = (size_t)cw*ch*3/2;
    uint8_t *srcf = malloc(fs);
    uint8_t *frames = malloc(cfs);
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
    /* x264 emits a version-SEI NAL (type 6) after SPS/PPS. */
    static const char *const SEI_PRESETS[10] = {
        "ultrafast","superfast","veryfast","faster","fast",
        "medium","slow","slower","veryslow","placebo"
    };
    int sei_preset=5;
    for(int i=0;i<10;i++) if(strcmp(preset,SEI_PRESETS[i])==0){sei_preset=i;break;}
    int sei_idx=sei_preset*2+(entropy?1:0);
    vk_nal_write(fo, 0x06, vk_sei_table[sei_idx], vk_sei_sizes[sei_idx], 1);

    /* match x264 PADH/PADV=32 (luma) and the 4:2:0 chroma equivalent */
    int rs = cw + 64, rh = ch + 64, ro = 32*rs + 32;
    int cs = cw/2 + 32, co = 16*cs + 16;
    uint8_t (*nnz_c)[16] = calloc((size_t)mw*mh, 16);
    uint8_t (*cnnz_c)[2][4] = calloc((size_t)mw*mh, 8);
    uint8_t *dcnz_c = calloc((size_t)mw*mh, 1);
    uint8_t *ry = malloc((size_t)rs*rh), *ru = malloc((size_t)cs*(ch/2+32));
    uint8_t *rv = malloc((size_t)cs*(ch/2+32));
    memset(ry, 128, (size_t)rs*rh);
    memset(ru, 128, (size_t)cs*(ch/2+32));
    memset(rv, 128, (size_t)cs*(ch/2+32));
    uint8_t *ref = calloc(1, cfs), *recon = calloc(1, cfs);
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
    uint8_t *idr_flags = malloc((size_t)nframes);
    /* scenecut needs all display frames contiguous, not the single padded
     * frame buffer reused by the encoder. */
    uint8_t *sc_frames = malloc(fs*(size_t)nframes);
    rewind(fi);
    size_t sc_read = fread(sc_frames, fs, (size_t)nframes, fi);
    if ((int)sc_read != nframes) memset(sc_frames, 0, fs*(size_t)nframes);
    rewind(fi);
    /* x264 b-adapt TRELLIS presets use wide flash-detection windows that
     * suppress most scenecuts; approximate as keyint-only. */
    int scenecut_on = !(strcmp(preset,"slower")==0 || strcmp(preset,"veryslow")==0
                        || strcmp(preset,"placebo")==0);
    vk_slicetype_idr_flags(sc_frames, nframes, w, h, keyint,
                           scenecut_on ? p.i_bframe : 0, 40, idr_flags);

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
        if (allframes) pad420(frames,cw,ch,allframes+(size_t)f*fs,w,h);
        else { if (fread(srcf, 1, fs, fi) != fs) { fprintf(stderr, "short input at frame %d\n", f); break; } pad420(frames,cw,ch,srcf,w,h); }
        int is_idr = stypes ? (stypes[f].type == VK_FRAME_IDR) : idr_flags[f];
        if (crf >= 0) {
            vk_lowres_luma(frames, cw, ch, lo_cur, dw, dh);
            uint16_t *ic = mb_intra + (size_t)f*n_mbs;
            uint16_t *lc = mb_cost  + (size_t)f*n_mbs;
            double cost;
            if (is_idr) {
                cost = vk_lowres_i_cost(lo_cur, dw, dh);
                for(int q=0;q<n_mbs;++q) lc[q]=(uint16_t)(1<<VK_LOWRES_COST_SHIFT);
            } else {
                cost = vk_lowres_p_cost_mb(lo_cur, lo_ref, dw, dh, &mvst[f], lc);
                /* my halfpel units -> x264 spel (<<5 relative to MB). */
                for(int q=0;q<n_mbs;++q){
                    mb_mv[0][(size_t)f*n_mbs+q][0]=(int16_t)(mvst[f].mv[q][0]*2);
                    mb_mv[0][(size_t)f*n_mbs+q][1]=(int16_t)(mvst[f].mv[q][1]*2);
                }
            }
            vk_lowres_intra_costs(lo_cur,dw,dh,ic);
            /* Propagate this frame's cost back onto the previous frame when a
             * reference exists (x264 macroblock_tree_propagate). */
            if (f>0 && !is_idr) {
                uint16_t *prev = mb_prop + (size_t)(f-1)*n_mbs;
                vk_mbtree_propagate_frame(prev,NULL,mb_mv[0]+(size_t)f*n_mbs,NULL,
                    NULL,ic,mb_invq+(size_t)f*n_mbs,lc,
                    0,1,128,256.0f,dw/8,dh/8,dw/8);
                /* Derive per-MB QP offsets and use their mean to scale cost. */
                float aqzero[VK_LOWRES_MAX_MBS], off[VK_LOWRES_MAX_MBS];
                for(int q=0;q<n_mbs;++q) aqzero[q]=0.0f;
                vk_mbtree_finish(off,ic,mb_invq+(size_t)f*n_mbs,prev,aqzero,
                                 256,0.6f,n_mbs);
                float mean=0.0f; for(int q=0;q<n_mbs;++q) mean+=off[q];
                cost *= pow(2.0, -mean/n_mbs/6.0);
            }
            double qp = vk_rc_frame_qp(&rc, cost, is_idr, f, 1.4f);
            int crf_qp = (int)(qp + 0.5f);
            if (crf_qp < 0) crf_qp = 0; if (crf_qp > 51) crf_qp = 51;
            qp_i = qp_p = crf_qp;
            vk_rc_accum_update(&rc, qp, is_idr, 1.4f);
            memcpy(lo_ref, lo_cur, (size_t)dw*dh*VK_LOWRES_PLANES);
        }
        memset(buf, 0, sizeof(buf)); vk_bs_init(&bs, buf, sizeof(buf));
        if (is_idr) {
            int idr_pic_id = idr_count & 1;
            /* x264 repeats SPS and PPS before every IDR slice. */
            if (idr_count > 0) {
                memset(buf, 0, sizeof(buf)); vk_bs_init(&bs, buf, sizeof(buf));
                vk_sps_write(&bs, &sps);
                vk_nal_write(fo, 0x67, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 1);
                memset(buf, 0, sizeof(buf)); vk_bs_init(&bs, buf, sizeof(buf));
                vk_pps_write(&bs, &sps, &pps);
                vk_nal_write(fo, 0x68, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 1);
            }
            ++idr_count;
            if (entropy) {
                vk_slice_header_t sh;
                vk_slice_header_init(&sh, &sps, &pps, qp_i);
                sh.i_idr_pic_id = idr_pic_id;
                vk_slice_header_write(&bs, &sh, &sps, &pps, 3);
                vk_bs_align_1(&bs);
                vk_cabac_init_ctx(&cb, 2, qp_i, 0);
                vk_cabac_encode_idr_recon_frame(&bs, &cb, frames, cw, ch, qp_i, recon, f, NULL);
                memset(curintra, 1, (size_t)mw*mh);  /* IDR: all MBs intra */
            } else {
                /* Reuse the byte-exact CABAC IDR analysis to decide which
                 * MBs are I4/I8; the CABAC bitstream is discarded. */
                uint8_t (*m4)[16] = malloc((size_t)mw*mh*16);
                uint8_t (*m8)[4]  = malloc((size_t)mw*mh*4);
                uint8_t *t4 = malloc((size_t)mw*mh);
                uint8_t *t8 = malloc((size_t)mw*mh);
                uint8_t *cf = malloc((size_t)mw*mh);
                vk_idr_decisions_t dec = {t4, t8, cf, m4, m8};
                size_t dbsz = (size_t)mw*mh*128 + 4096;
                uint8_t *dbuf = malloc(dbsz);
                vk_bs_t dbs; vk_bs_init(&dbs, dbuf, dbsz);
                vk_cabac_init_ctx(&cb, 2, qp_i, 0);
                vk_cabac_encode_idr_recon_frame(&dbs, &cb, frames, cw, ch,
                                                qp_i, NULL, f, &dec);
                free(dbuf);
                memset(nnz_c, 0, (size_t)mw*mh*16);
                memset(cnnz_c, 0, (size_t)mw*mh*8);
                memset(dcnz_c, 0, (size_t)mw*mh);
                memset(ry, 128, (size_t)rs*rh);
                memset(ru, 128, (size_t)cs*(ch/2+32));
                memset(rv, 128, (size_t)cs*(ch/2+32));
                encode_idr_cavlc(&bs, &sps, &pps, frames, cw, ch, qp_i,
                                 idr_pic_id, nnz_c, cnnz_c, dcnz_c,
                                 ry, rs, ro, ru, rv, cs, co, recon,
                                 p.i_subme > 0, &dec);
                free(m4); free(m8); free(t4); free(t8); free(cf);
                memset(curintra, 1, (size_t)mw*mh);  /* IDR: all MBs intra */
            }
            vk_nal_write(fo, 0x65, buf,
                         (size_t)(entropy ? bs.p - buf : (vk_bs_pos(&bs)+7)/8), 3);
        } else {
            int frame_num = f % keyint;
            if (entropy) {
                vk_slice_header_t sh; memset(&sh, 0, sizeof(sh));
                sh.i_type = 0; sh.i_pps_id = 0; sh.i_frame_num = frame_num;
                /* x264 writes POC = 2*frame_num for non-B P slices. */
                sh.i_poc = 2*frame_num;
                sh.i_idr_pic_id = -1; sh.i_qp_delta = qp_p - pps.i_pic_init_qp;
                sh.i_disable_deblocking_filter_idc = pps.b_deblocking_filter ? 0 : 1;
                vk_slice_header_write(&bs, &sh, &sps, &pps, 2);
                vk_bs_align_1(&bs);
                vk_cabac_init_ctx(&cb, 0, qp_p, 0);
                vk_cabac_encode_p_recon_frame(&bs, &cb, frames, ref, cw, ch, qp_p, recon, f,
                                              refmv, curmv, refintra, curintra);
                vk_nal_write(fo, 0x41, buf, (size_t)(bs.p - buf), 2);
            } else if (getenv("ORACLE_BSLICE_TEST") && f == 5) {
                /* x264 frame 5: P-slice, all MBs skipped, POC=12, fn=3 */
                vk_encode_p_slice_allskip(&bs, &sps, &pps, 3, 12,
                                          qp_p, mw*mh);
                vk_bs_rbsp_trailing(&bs);
                vk_nal_write(fo, 0x41, buf,
                             (size_t)((vk_bs_pos(&bs)+7)/8), 2);
                memcpy(recon, ref, cfs);
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
                memcpy(recon, ref, cfs);
            } else {
                int pfn = (getenv("ORACLE_BSLICE_TEST") && f==7) ? 4 : frame_num;
                int ppoc = (getenv("ORACLE_BSLICE_TEST") && f==7) ? 14 : 8*frame_num;
                vk_encode_p_frame_recon_mv(&bs, &sps, &pps, frames, ref,
                                           cw, ch, qp_p, pfn, recon,
                                           refmv, curmv, refintra, curintra,
                                           p.i_subme, ppoc);
                vk_nal_write(fo, 0x41, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 2);
            }
        }
        const char *rpath = getenv("ORACLE_DUMP_RECON");
        if (rpath) {
            char rp[1024]; snprintf(rp, sizeof(rp), "%s.%d", rpath, f);
            FILE *rf = fopen(rp, "wb");
            if (rf) { fwrite(recon, 1, cfs, rf); fclose(rf); }
        }
        memcpy(ref, recon, cfs);
        int (*tmp)[2] = refmv; refmv = curmv; curmv = tmp;
        memset(curmv, 0, (size_t)mw*mh*2*sizeof(int));
        uint8_t *tmpi = refintra; refintra = curintra; curintra = tmpi;
        memset(curintra, 0, (size_t)mw*mh);
    }
    fclose(fi); fclose(fo);
    free(srcf); free(frames); free(ref); free(recon); free(refmv); free(curmv);
    free(refintra); free(curintra); free(ry); free(ru); free(rv);
    free(nnz_c); free(cnnz_c); free(dcnz_c);
    return 0;
}
