/* SPDX-License-Identifier: GPL-2.0-or-later
 * Byte-exact SPS/PPS regression against x264 CLI output for:
 *   16x16, 60 fps, --qp 28 --preset ultrafast --keyint 1 --frames 1
 * Reference RBSP bytes were captured from references/x264-master/x264.exe.
 */
#include "param.h"
#include <stdio.h>
#include <string.h>

static const uint8_t expected_sps[] = {
    0x42,0xc0,0x0a,0xdd,0xe8,0x40,0x00,0x00,0x00,0x40,
    0x00,0x00,0x1e,0x23,0xc4,0x89,0xe0
};
static const uint8_t expected_pps[] = { 0xce,0x09,0xc8 };

static int compare(const char *name, uint8_t *got, size_t got_len,
                   const uint8_t *exp, size_t exp_len)
{
    if (got_len == exp_len && memcmp(got, exp, exp_len) == 0)
        return 0;
    fprintf(stderr, "%s mismatch (got %zu bytes, expected %zu)\n", name, got_len, exp_len);
    fprintf(stderr, "  got: ");
    for (size_t i = 0; i < got_len; ++i) fprintf(stderr, "%02x ", got[i]);
    fprintf(stderr, "\n  exp: ");
    for (size_t i = 0; i < exp_len; ++i) fprintf(stderr, "%02x ", exp[i]);
    fprintf(stderr, "\n");
    return 1;
}

int main(void)
{
    vk_param_t p;
    vk_sps_t sps;
    vk_pps_t pps;
    uint8_t buf[256];
    vk_bs_t bs;

    vk_param_default(&p);
    vk_sps_init(&sps, 0, &p);
    memset(buf, 0xAA, sizeof(buf));
    vk_bs_init(&bs, buf, sizeof(buf));
    vk_sps_write(&bs, &sps);
    size_t sps_len = (size_t)((vk_bs_pos(&bs) + 7) / 8);
    if (vk_bs_error(&bs)) { fprintf(stderr, "SPS overflow\n"); return 1; }

    vk_pps_init(&pps, 0, &p, &sps);
    memset(buf, 0xAA, sizeof(buf));
    vk_bs_init(&bs, buf, sizeof(buf));
    vk_pps_write(&bs, &sps, &pps);
    size_t pps_len = (size_t)((vk_bs_pos(&bs) + 7) / 8);
    uint8_t pps_buf[256];
    memcpy(pps_buf, buf, pps_len);
    if (vk_bs_error(&bs)) { fprintf(stderr, "PPS overflow\n"); return 1; }

    /* SPS bytes are still in buf only until the PPS writer reused it. */
    vk_bs_init(&bs, buf, sizeof(buf));
    vk_sps_write(&bs, &sps);
    uint8_t sps_buf[256];
    memcpy(sps_buf, buf, sps_len);

    int rc = compare("SPS", sps_buf, sps_len, expected_sps, sizeof(expected_sps));
    rc |= compare("PPS", pps_buf, pps_len, expected_pps, sizeof(expected_pps));
    if (rc == 0)
        printf("SPS/PPS byte-exact vs x264 ultrafast 16x16 qp28\n");
    return rc;
}
