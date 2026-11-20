/* SPDX-License-Identifier: GPL-2.0-or-later
 * Self-test: Exp-Golomb encoding must match H.264 table 9-1 bit-for-bit. */
#include "bitstream.h"
#include <stdio.h>
#include <string.h>

static int check_ue(uint32_t value, const char *expected)
{
    uint8_t buf[32] = {0};
    vk_bs_t bs;
    vk_bs_init(&bs, buf, sizeof(buf));
    vk_bs_write_ue(&bs, value);
    unsigned nbits = (unsigned)vk_bs_pos(&bs);
    vk_bs_align(&bs); /* flush the partial byte into the buffer */
    char got[65] = {0};
    for (unsigned i = 0; i < nbits; ++i)
        got[i] = (buf[i/8] >> (7 - (i%8))) & 1 ? '1' : '0';
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "ue(%u): got %s, expected %s\n", value, got, expected);
        return 1;
    }
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= check_ue(0, "1");
    rc |= check_ue(1, "010");
    rc |= check_ue(2, "011");
    rc |= check_ue(3, "00100");
    rc |= check_ue(4, "00101");
    rc |= check_ue(7, "0001000");
    rc |= check_ue(15, "000010000");
    /* se(0)=ue(0), se(1)=ue(1), se(-1)=ue(2), se(2)=ue(3) */
    if (vk_bs_size_se(0) != 1 || vk_bs_size_se(1) != 3 ||
        vk_bs_size_se(-1) != 3 || vk_bs_size_se(2) != 5) {
        fprintf(stderr, "bs_size_se mismatch\n");
        rc = 1;
    }
    if (rc == 0)
        printf("bitstream self-test OK\n");
    return rc;
}
