/* SPDX-License-Identifier: GPL-2.0-or-later
 * Oracle CLI: SPS+PPS+IDR with CABAC entropy coding (I16 flat-frame path).
 * Usage: oracle_cabac_cli input.yuv w h qp slice_qp output.h264
 */
#include "bitstream.h"
#include "param.h"
#include "slice.h"
#include "cabac.h"
#include "cabac_enc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr, "usage: oracle_cabac_cli in.yuv w h qp slice_qp out.h264\n");
        return 2;
    }
    int w = atoi(argv[2]), h = atoi(argv[3]);
    int qp = atoi(argv[4]), slice_qp = atoi(argv[5]);
    if (w % 16 || h % 16) return 2;
    size_t fs = (size_t)w*h*3/2;
    uint8_t *frame = malloc(2*fs);
    FILE *fi = fopen(argv[1], "rb");
    size_t got = fread(frame, 1, 2*fs, fi);
    if (!fi || got < fs) return 1;
    int two_frames = (got >= 2*fs);
    fclose(fi);

    vk_param_t p; vk_sps_t sps; vk_pps_t pps; vk_slice_header_t sh;
    vk_bs_t bs; vk_cabac_t cb;
    static uint8_t buf[1 << 20];
    FILE *fo = fopen(argv[6], "wb");
    if (!fo) return 1;
    vk_cavlc_init();
    vk_param_default(&p);
    p.i_width = w; p.i_height = h; p.i_qp_constant = qp;
    p.b_cabac = 1;
    p.i_keyint_max = two_frames ? 30 : 1;
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
    /* x264: bs_align_1 before CABAC (fill to byte with 1s, no extra byte) */
    vk_bs_align_1(&bs);
    vk_cabac_init_ctx(&cb, 2, slice_qp, 0);
    vk_cabac_encode_idr(&bs, &cb, frame, w, h, slice_qp, 0);
    vk_nal_write(fo, 0x65, buf, (size_t)(bs.p - buf), 0);

    if (two_frames) {
        /* P-slice: slice_qp = qp (P QP), frame_num = 1 */
        memset(buf, 0, sizeof(buf));
        vk_bs_init(&bs, buf, sizeof(buf));
        memset(&sh, 0, sizeof(sh));
        sh.i_type = 0;  /* SLICE_TYPE_P */
        sh.i_pps_id = 0;
        sh.i_frame_num = 1;
        sh.i_idr_pic_id = -1;
        sh.i_qp_delta = qp - pps.i_pic_init_qp;
        sh.i_disable_deblocking_filter_idc = 1;
        vk_slice_header_write(&bs, &sh, &sps, &pps, 2);
        vk_bs_align_1(&bs);
        vk_cabac_init_ctx(&cb, 0, qp, 0);  /* P contexts */
        vk_cabac_encode_pskip(&bs, &cb, w, h);
        vk_nal_write(fo, 0x41, buf, (size_t)(bs.p - buf), 1);
    }

    fclose(fo); free(frame);
    return 0;
}
