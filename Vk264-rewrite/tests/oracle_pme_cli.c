/* SPDX-License-Identifier: GPL-2.0-or-later
 * Oracle CLI: SPS + PPS + IDR (frame 0) + motion-compensated P-slice (frame 1).
 * Usage: oracle_pme_cli input.yuv w h qp slice_qp output.h264
 */
#include "bitstream.h"
#include "param.h"
#include "slice.h"
#include "pframe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: oracle_pme_cli in.yuv w h qp slice_qp out.h264\n");
        return 2;
    }
    int w = atoi(argv[2]), h = atoi(argv[3]);
    int qp = atoi(argv[4]), slice_qp = atoi(argv[5]);
    if (w % 16 || h % 16) return 2;
    int mw = w/16, mh = h/16;
    size_t fs = (size_t)w*h*3/2;
    uint8_t *frames = malloc(2*fs);
    FILE *fi = fopen(argv[1], "rb");
    if (!fi || fread(frames, 1, 2*fs, fi) != 2*fs) return 1;
    fclose(fi);

    int rs = w + 32, rh = h + 32, ro = 16*rs + 16;
    int cs = w/2 + 32, co = 8*cs + 8;
    uint8_t (*nnz_c)[16] = calloc((size_t)mw*mh, 16);
    uint8_t (*cnnz_c)[2][4] = calloc((size_t)mw*mh, 8);
    uint8_t *dcnz_c = calloc((size_t)mw*mh, 1);
    uint8_t *ry = malloc((size_t)rs*rh), *ru = malloc((size_t)cs*(h/2+32));
    uint8_t *rv = malloc((size_t)cs*(h/2+32));
    memset(ry, 128, (size_t)rs*rh);
    memset(ru, 128, (size_t)cs*(h/2+32));
    memset(rv, 128, (size_t)cs*(h/2+32));

    vk_param_t p; vk_sps_t sps; vk_pps_t pps; vk_slice_header_t sh; vk_bs_t bs;
    static uint8_t buf[1 << 20];
    FILE *fo = fopen(argv[6], "wb");
    if (!fo) return 1;
    vk_cavlc_init();
    vk_param_default(&p);
    p.i_width = w; p.i_height = h; p.i_qp_constant = qp;
    p.i_keyint_max = 30;   /* P-frame sequence: match x264 --keyint 30 */
    vk_sps_init(&sps, 0, &p);
    vk_pps_init(&pps, 0, &p, &sps);

    memset(buf, 0, sizeof(buf)); vk_bs_init(&bs, buf, sizeof(buf));
    vk_sps_write(&bs, &sps);
    vk_nal_write(fo, 0x67, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 1);
    memset(buf, 0, sizeof(buf)); vk_bs_init(&bs, buf, sizeof(buf));
    vk_pps_write(&bs, &sps, &pps);
    vk_nal_write(fo, 0x68, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 1);

    /* ---- frame 0: IDR (identical to oracle_cli.c) ---- */
    memset(buf, 0, sizeof(buf)); vk_bs_init(&bs, buf, sizeof(buf));
    vk_slice_header_init(&sh, &sps, &pps, slice_qp);
    vk_slice_header_write(&bs, &sh, &sps, &pps, 3);
    for (int my = 0; my < mh; ++my)
        for (int mx = 0; mx < mw; ++mx) {
            int px = mx*16, py = my*16;
            int avail = (mx ? 1 : 0) | (my ? 2 : 0) | (my && mx ? 8 : 0);
            vk_neighbours_t nb; memset(&nb, 0, sizeof(nb));
            nb.avail = avail; nb.qp = slice_qp;
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
            nb.left_chroma[0] = lc[0]; nb.left_chroma[1] = lc[1];
            nb.top_chroma[0] = tc[0]; nb.top_chroma[1] = tc[1];
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
            nb.top_u = tup; nb.left_u = lup; nb.top_v = tvp; nb.left_v = lvp;
            nb.tl_u = (my && mx) ? ru[co+(py/2-1)*cs+px/2-1] : 128;
            nb.tl_v = (my && mx) ? rv[co+(py/2-1)*cs+px/2-1] : 128;
            uint8_t *sy = frames + py*w + px;
            uint8_t *su = frames + (size_t)w*h + (py/2)*(w/2) + px/2;
            uint8_t *sv = su + (w/2)*(h/2);
            uint8_t oy[256], ou[64], ov[64], on[16], oc[2][4], od;
            vk_encode_i16_mb(&bs, sy, su, sv, oy, ou, ov, slice_qp, mx, my,
                             &nb, w, w/2, on, oc, &od);
            memcpy(nnz_c[my*mw+mx], on, 16);
            memcpy(cnnz_c[my*mw+mx], oc, 8);
            dcnz_c[my*mw+mx] = od;
            for (int y = 0; y < 16; ++y)
                memcpy(ry + ro + (py+y)*rs + px, oy + y*16, 16);
            for (int y = 0; y < 8; ++y) {
                memcpy(ru + co + (py/2+y)*cs + px/2, ou + y*8, 8);
                memcpy(rv + co + (py/2+y)*cs + px/2, ov + y*8, 8);
            }
        }
    vk_bs_rbsp_trailing(&bs);
    vk_nal_write(fo, 0x65, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 0);

    /* ---- frame 1: motion-compensated P-slice ---- */
    uint8_t *recon = malloc(fs);
    for (int y = 0; y < h; ++y)
        memcpy(recon + y*w, ry + ro + y*rs, w);
    for (int y = 0; y < h/2; ++y) {
        memcpy(recon + (size_t)w*h + y*(w/2), ru + co + y*cs, w/2);
        memcpy(recon + (size_t)w*h + (size_t)(w/2)*(h/2) + y*(w/2),
               rv + co + y*cs, w/2);
    }
    memset(buf, 0, sizeof(buf)); vk_bs_init(&bs, buf, sizeof(buf));
    vk_encode_p_frame(&bs, &sps, &pps, frames + fs, recon,
                      w, h, qp, 1);
    vk_nal_write(fo, 0x41, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 1);

    fclose(fo); free(frames); free(recon); free(ry); free(ru); free(rv);
    free(nnz_c); free(cnnz_c); free(dcnz_c);
    return 0;
}
