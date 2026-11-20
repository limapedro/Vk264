/* SPDX-License-Identifier: GPL-2.0-or-later
 * Oracle P-slice CLI (static-frame milestone): SPS + PPS + IDR + P-skip.
 * Usage: oracle_pcli input.yuv w h qp slice_qp output.h264
 * input holds 2 frames; the I-frame encoder handles frame 0, then a single
 * P-slice with mb_skip_run=N is emitted for frame 1.
 */
#include "bitstream.h"
#include "param.h"
#include "slice.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 7) { fprintf(stderr, "usage: oracle_pcli in.yuv w h qp slice_qp out.h264\n"); return 2; }
    const char *in = argv[1];
    int w = atoi(argv[2]), h = atoi(argv[3]);
    int qp = atoi(argv[4]), slice_qp = atoi(argv[5]);
    if (w % 16 || h % 16) return 2;
    int mw = w/16, mh = h/16;
    size_t fs = (size_t)w*h*3/2;
    uint8_t *frame = malloc(fs);
    FILE *fi = fopen(in, "rb");
    if (!fi || fread(frame, 1, fs, fi) != fs) return 1;
    fclose(fi);

    vk_param_t p; vk_sps_t sps; vk_pps_t pps; vk_bs_t bs;
    static uint8_t buf[1 << 20];
    FILE *fo = fopen(argv[6], "wb");
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

    /* frame 0: IDR (reuse the I-frame path from oracle_cli is non-trivial here;
     * this milestone only emits the P-slice, the I-slice is produced by the
     * existing oracle_cli and concatenated by the test script). */
    (void)frame; (void)slice_qp; (void)qp;

    memset(buf, 0, sizeof(buf));
    vk_bs_init(&bs, buf, sizeof(buf));
    vk_write_pskip_slice(&bs, &sps, &pps, 1, slice_qp, mw*mh);
    vk_nal_write(fo, 0x41, buf, (size_t)((vk_bs_pos(&bs)+7)/8), 1);

    fclose(fo); free(frame);
    return 0;
}
