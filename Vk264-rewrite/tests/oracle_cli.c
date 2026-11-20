/* SPDX-License-Identifier: GPL-2.0-or-later
 * Oracle encoder CLI: encode one YUV420 frame to Annex-B H.264.
 * Usage: oracle_cli input.yuv w h qp slice_qp output.h264
 * No SEI, no rate control: SPS + PPS + one IDR slice.
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
    size_t fs = (size_t)w*h*3/2;
    uint8_t *frame = malloc(fs);
    FILE *fi = fopen(in, "rb");
    if (!fi || fread(frame, 1, fs, fi) != fs) { fprintf(stderr, "input read failed\n"); return 1; }
    fclose(fi);

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
    /* One-MB frames only for now; w/h must be 16. */
    if (w != 16 || h != 16) { fprintf(stderr, "only 16x16 supported\n"); return 1; }
    uint8_t ry[256], ru[64], rv[64];
    vk_encode_i16_mb(&bs, frame, frame+256, frame+320, ry, ru, rv, slice_qp, 0, 0);
    vk_bs_rbsp_trailing(&bs);
    vk_nal_write(fo, 0x65, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 0);

    fclose(fo);
    free(frame);
    return 0;
}
