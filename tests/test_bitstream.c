/* Expected bit patterns come from the H.264 Exp-Golomb mapping implemented
 * in reference/x264/common/bitstream.h:201-307; NAL escape from bitstream.c:29.
 */
#include "phx_bitstream.h"
#include "phx_tables.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond,msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n",msg,__LINE__); ++fails; } } while (0)

static uint8_t enc_ue(unsigned v)
{
    uint8_t buf[4] = {0};
    phx_bs_t s; phx_bs_init(&s, buf, sizeof(buf));
    phx_bs_write_ue(&s, v);
    phx_bs_align_0(&s);
    return buf[0];
}
static uint8_t enc_se(int v)
{
    uint8_t buf[4] = {0};
    phx_bs_t s; phx_bs_init(&s, buf, sizeof(buf));
    phx_bs_write_se(&s, v);
    phx_bs_align_0(&s);
    return buf[0];
}

int main(void)
{
    CHECK(phx_bs_size_ue(0) == 1 && phx_bs_size_ue(1) == 3 &&
          phx_bs_size_ue(2) == 3 && phx_bs_size_ue(3) == 5, "ue sizes");
    CHECK(phx_bs_size_se(0) == 1 && phx_bs_size_se(1) == 3 &&
          phx_bs_size_se(-1) == 3 && phx_bs_size_se(2) == 5, "se sizes");

    CHECK(enc_ue(0) == 0x80, "ue(0)=1");
    CHECK(enc_ue(1) == 0x40, "ue(1)=010");
    CHECK(enc_ue(2) == 0x60, "ue(2)=011");
    CHECK(enc_ue(3) == 0x20, "ue(3)=00100");
    CHECK(enc_se(0) == 0x80, "se(0)=1");
    CHECK(enc_se(1) == 0x40, "se(1)=010");
    CHECK(enc_se(-1) == 0x60, "se(-1)=011");

    uint8_t b[4] = {0};
    phx_bs_t s; phx_bs_init(&s, b, sizeof(b));
    phx_bs_write_te(&s, 1, 0);  /* 1^0 = 1 */
    phx_bs_write_te(&s, 1, 1);  /* 1^1 = 0 */
    phx_bs_align_0(&s);
    CHECK(b[0] == 0x80, "te(1,x) bits");

    phx_bs_t t; phx_bs_init(&t, b, sizeof(b));
    phx_bs_write_ue(&t, 0);     /* 1 */
    phx_bs_rbsp_trailing(&t);   /* 1 + zero padding */
    CHECK(b[0] == 0xC0, "rbsp trailing");

    /* NAL: long start code, ref_idc=3, SPS type=7, escape 00 00 01/03. */
    uint8_t payload[] = {0x00,0x00,0x01,0x02,0x00,0x00,0x03};
    uint8_t out[64] = {0};
    phx_nal_t n = {payload, (int)sizeof(payload), 3, 7, 1};
    phx_nal_encode(out, &n, 1);
    const uint8_t want[] = {0,0,0,1, 0x67, 0,0,3,1,2, 0,0,3,3};
    CHECK(n.i_payload == (int)sizeof(want), "NAL size");
    CHECK(memcmp(out, want, sizeof(want)) == 0, "NAL escape bytes");

    printf("%s: bitstream, %d failures\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
