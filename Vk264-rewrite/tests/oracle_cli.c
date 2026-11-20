/* SPDX-License-Identifier: GPL-2.0-or-later
 * Oracle encoder CLI: encode one YUV420 I-frame to Annex-B H.264.
 * Usage: oracle_cli input.yuv w h qp slice_qp output.h264
 * No SEI, no rate control: SPS + PPS + one IDR slice with all MBs.
 */
#include "bitstream.h"
#include "param.h"
#include "slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: oracle_cli in.yuv w h qp slice_qp out.h264\n");
        return 2;
    }
    const char *in = argv[1];
    int w = atoi(argv[2]), h = atoi(argv[3]);
    int qp = atoi(argv[4]), slice_qp = atoi(argv[5]);
    const char *out = argv[6];
    if (w % 16 || h % 16) { fprintf(stderr, "w/h must be multiples of 16\n"); return 2; }
    int mw = w/16, mh = h/16;
    size_t fs = (size_t)w*h*3/2;
    uint8_t *frame = malloc(fs);
    FILE *fi = fopen(in, "rb");
    if (!fi || fread(frame, 1, fs, fi) != fs) { fprintf(stderr, "input read failed\n"); return 1; }
    fclose(fi);

    /* Reconstruction planes with a 16-pixel border, initialised to 128. */
    int rs = w + 32, rh = h + 32, ro = 16*rs + 16;
    uint8_t *ry = malloc((size_t)rs*rh), *ru = malloc((size_t)(w/2+32)*(h/2+32));
    uint8_t *rv = malloc((size_t)(w/2+32)*(h/2+32));
    memset(ry, 128, (size_t)rs*rh);
    memset(ru, 128, (size_t)(w/2+32)*(h/2+32));
    memset(rv, 128, (size_t)(w/2+32)*(h/2+32));

    vk_param_t p; vk_sps_t sps; vk_pps_t pps; vk_slice_header_t sh;
    vk_bs_t bs;
    static uint8_t buf[1 << 20];
    FILE *fo = fopen(out, "wb");
    if (!fo) return 1;

    vk_cavlc_init();
    vk_param_default(&p);
    p.i_width = w; p.i_height = h; p.i_qp_constant = qp;
    vk_sps_init(&sps, 0, &p);
    vk_pps_init(&pps, 0, &p, &sps);

    memset(buf, 0, sizeof(buf));
    vk_bs_init(&bs, buf, sizeof(buf));
    vk_sps_write(&bs, &sps);
    vk_nal_write(fo, 0x67, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 1);

    memset(buf, 0, sizeof(buf));
    vk_bs_init(&bs, buf, sizeof(buf));
    vk_pps_write(&bs, &sps, &pps);
    vk_nal_write(fo, 0x68, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 1);

    memset(buf, 0, sizeof(buf));
    vk_bs_init(&bs, buf, sizeof(buf));
    vk_slice_header_init(&sh, &sps, &pps, slice_qp);
    vk_slice_header_write(&bs, &sh, &sps, &pps, 3);

    for (int my = 0; my < mh; ++my)
        for (int mx = 0; mx < mw; ++mx) {
            int px = mx*16, py = my*16;
            int avail = (my ? 1 : 0) | (mx ? 2 : 0) | (my && mx ? 4 : 0);
            vk_neighbours_t nb;
            memset(&nb, 0, sizeof(nb));
            nb.avail = avail;
            nb.qp = slice_qp;
            uint8_t top[16], left[16], tup[8], lup[8], tvp[8], lvp[8];
            if (my) for (int i = 0; i < 16; ++i) top[i] = ry[ro+(py-1)*rs+px+i];
            else   for (int i = 0; i < 16; ++i) top[i] = 128;
            if (mx) for (int i = 0; i < 16; ++i) left[i] = ry[ro+(py+i)*rs+px-1];
            else   for (int i = 0; i < 16; ++i) left[i] = 128;
            nb.top_y = top; nb.left_y = left;
            nb.tl_y = (my && mx) ? ry[ro+(py-1)*rs+px-1] : 128;
            int cs = w/2+32, co = 8*cs+8;
            if (my) for (int i = 0; i < 8; ++i) { tup[i] = ru[co+(py/2-1)*cs+px/2+i]; tvp[i] = rv[co+(py/2-1)*cs+px/2+i]; }
            else   for (int i = 0; i < 8; ++i) { tup[i] = 128; tvp[i] = 128; }
            if (mx) for (int i = 0; i < 8; ++i) { lup[i] = ru[co+(py/2+i)*cs+px/2-1]; lvp[i] = rv[co+(py/2+i)*cs+px/2-1]; }
            else   for (int i = 0; i < 8; ++i) { lup[i] = 128; lvp[i] = 128; }
            nb.top_u = tup; nb.left_u = lup; nb.top_v = tvp; nb.left_v = lvp;
            nb.tl_u = nb.tl_v = 128;
            uint8_t *sy = frame + py*w + px;
            uint8_t *su = frame + w*h + (py/2)*(w/2) + px/2;
            uint8_t *sv = su + (w/2)*(h/2);
            uint8_t out_y[256], out_u[64], out_v[64];
            vk_encode_i16_mb(&bs, sy, su, sv, out_y, out_u, out_v,
                             slice_qp, mx, my, &nb);
            for (int y = 0; y < 16; ++y)
                memcpy(ry + ro + (py+y)*rs + px, out_y + y*16, 16);
            for (int y = 0; y < 8; ++y) {
                memcpy(ru + co + (py/2+y)*cs + px/2, out_u + y*8, 8);
                memcpy(rv + co + (py/2+y)*cs + px/2, out_v + y*8, 8);
            }
        }

    vk_bs_rbsp_trailing(&bs);
    vk_nal_write(fo, 0x65, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 0);
    fclose(fo);
    free(frame); free(ry); free(ru); free(rv);
    return 0;
}
