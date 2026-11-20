/* SPDX-License-Identifier: GPL-2.0-or-later
 * Byte-exact IDR slice regression vs x264 CLI:
 *   16x16 flat Y=16 U=V=16, 60fps, qp28, ultrafast, keyint1.
 */
#include "slice.h"
#include <stdio.h>
#include <string.h>

static const uint8_t expected[] = {
    0x65,0x88,0x84,0x3a,0x11,0x8a,0x00,0x02,0x24,0xb1,
    0xc0,0x00,0x42,0x06,0x38,0x00,0x08,0x40,0xe0
};

int main(void)
{
    vk_param_t p; vk_sps_t sps; vk_pps_t pps; vk_slice_header_t sh;
    vk_bs_t bs;
    uint8_t buf[512], y[256], u[64], v[64], ry[256], ru[64], rv[64];

    vk_cavlc_init();
    vk_param_default(&p);
    vk_sps_init(&sps, 0, &p);
    vk_pps_init(&pps, 0, &p, &sps);
    memset(y, 16, sizeof(y));
    memset(u, 16, sizeof(u));
    memset(v, 16, sizeof(v));

    memset(buf, 0, sizeof(buf));
    vk_bs_init(&bs, buf, sizeof(buf));
    vk_bs_write(&bs, 8, 0x65);                    /* NAL IDR, nal_ref_idc=3 */
    vk_slice_header_init(&sh, &sps, &pps, 25);
    vk_slice_header_write(&bs, &sh, &sps, &pps, 3);
    vk_encode_i16_mb(&bs, y, u, v, ry, ru, rv, 25, 0, 0, NULL, 16, 8);
    vk_bs_rbsp_trailing(&bs);

    size_t len = (size_t)((vk_bs_pos(&bs) + 7) / 8);
    if (len != sizeof(expected) || memcmp(buf, expected, len) != 0) {
        fprintf(stderr, "slice mismatch: got %zu bytes, expected %zu\n", len, sizeof(expected));
        fprintf(stderr, "  got: ");
        for (size_t i = 0; i < len; ++i) fprintf(stderr, "%02x ", buf[i]);
        fprintf(stderr, "\n  exp: ");
        for (size_t i = 0; i < sizeof(expected); ++i) fprintf(stderr, "%02x ", expected[i]);
        fprintf(stderr, "\n");
        return 1;
    }
    printf("IDR slice byte-exact vs x264 (16x16 flat qp28 ultrafast)\n");
    return 0;
}
