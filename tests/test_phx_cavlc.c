/* Deterministic vectors for the Phoenix CAVLC residual writer.
 *
 * Every expected bit string below is derived by hand from the verbatim x264
 * VLC tables in reference/x264/common/tables.c and the level/run algorithms in
 * common/vlc.c; the decomposition is spelled out next to each vector.
 * Sources: encoder/cavlc.c:70-209, common/tables.c:1799-2509,
 *          common/vlc.c:33-99, common/quant.c:366-395.
 *
 * Build (standalone):
 *   clang -std=c11 -Wall -Wextra -Isrc tests/test_phx_cavlc.c \
 *         src/phx_cavlc.c src/phx_bitstream.c src/phx_tables.c -o test_phx_cavlc
 *
 * NOTE: these vectors test the residual syntax primitives only. They do not
 * claim full H.264 stream parity with x264.
 */
#include "phx_cavlc.h"
#include "phx_bitstream.h"
#include "phx_tables.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

/* Full-table fingerprint over every declared array element (including the
 * implicit zero-fill at the end of short x264 rows), computed from tables.c. */
static uint32_t hash_vlcs(const phx_vlc_t *t, int count)
{
    uint32_t acc = 2166136261u;
    for (int i = 0; i < count; i++) {
        unsigned v = (unsigned)t[i].i_bits * 131u + (unsigned)t[i].i_size * 17u + 1u;
        acc = (acc ^ v) * 16777619u;
    }
    return acc;
}

static void expect_vlc(phx_vlc_t got, unsigned bits, unsigned size, const char *msg)
{
    if (got.i_bits != bits || got.i_size != size) {
        printf("FAIL %s: got {%02X,%d}, want {%02X,%d}\n",
               msg, got.i_bits, got.i_size, bits, size);
        fails++;
    }
}

/* Extract the MSB-first bit string actually emitted into the RBSP buffer. */
static void bits_of_buffer(const uint8_t *buf, int nbits, char *out)
{
    for (int i = 0; i < nbits; i++)
        out[i] = (char)('0' + ((buf[i>>3] >> (7-(i&7))) & 1));
    out[nbits] = '\0';
}

static void check_block(const char *name, const int16_t *dct, int num, int cat,
                        int nC, int profile, int chroma_2x4, int want_total,
                        const char *want_bits, const uint8_t *want_bytes,
                        int want_nbytes)
{
    uint8_t buf[128];
    phx_bs_t bs;
    phx_cavlc_t c;
    memset(buf, 0, sizeof(buf));
    phx_bs_init(&bs, buf, sizeof(buf));
    c.bs = &bs;
    c.profile_idc = profile;
    c.b_overflow = 0;
    c.chroma_2x4_dc = chroma_2x4;

    uint8_t nnz = 0;
    for (int i = 0; i < num; i++)
        if (dct[i]) { nnz = 1; break; }
    phx_cavlc_block_residual(&c, cat, dct, &nnz, nC, num);
    CHECK(nnz == want_total, name);

    int nbits = phx_bs_pos(&bs);
    char got[256];
    bits_of_buffer(buf, nbits, got);
    if (strcmp(got, want_bits) != 0) {
        printf("FAIL %s bits: got %s (%d bits), want %s (%d bits)\n",
               name, got, nbits, want_bits, (int)strlen(want_bits));
        fails++;
    }
    if (want_bytes) {
        phx_bs_align_0(&bs);
        CHECK((int)(bs.p - bs.p_start) == want_nbytes, name);
        for (int i = 0; i < want_nbytes; i++)
            CHECK(buf[i] == want_bytes[i], name);
    }
}

int main(void)
{
    /* ---- verbatim tables (tables.c:1799/1810/2265/2435/2455/2509) ---- */
    expect_vlc(phx_cavlc_coeff0(0), 0x1, 1, "coeff0_token[0] str=1");
    expect_vlc(phx_cavlc_coeff0(3), 0x3, 6, "coeff0_token[3] str=000011");
    expect_vlc(phx_cavlc_coeff0(5), 0x1, 1, "coeff0_token[5] str=1");
    expect_vlc(phx_cavlc_coeff_token_at(0,0,0), 0x5, 6, "coeff_token[0][0][0] str=000101");
    expect_vlc(phx_cavlc_coeff_token_at(0,0,1), 0x1, 2, "coeff_token[0][0][1] str=01");
    expect_vlc(phx_cavlc_coeff_token_at(0,1,2), 0x1, 3, "coeff_token[0][1][2] str=001");
    expect_vlc(phx_cavlc_coeff_token_at(1,0,0), 0xB, 6, "coeff_token[1][0][0] str=001011");
    expect_vlc(phx_cavlc_coeff_token_at(1,0,1), 0x2, 2, "coeff_token[1][0][1] str=10");
    expect_vlc(phx_cavlc_total_zeros_at(0,0), 0x1, 1, "total_zeros[0][0] str=1");
    expect_vlc(phx_cavlc_total_zeros_at(0,1), 0x3, 3, "total_zeros[0][1] str=011");
    expect_vlc(phx_cavlc_total_zeros_at(1,1), 0x6, 3, "total_zeros[1][1] str=110");
    expect_vlc(phx_cavlc_total_zeros_dc_at(0,0,0), 0x1, 1, "tz_2x2_dc[0][0] str=1");
    expect_vlc(phx_cavlc_total_zeros_dc_at(2,1,0), 0x0, 1, "tz_2x2_dc[2][1] str=0");
    expect_vlc(phx_cavlc_total_zeros_dc_at(0,1,1), 0x2, 3, "tz_2x4_dc[0][1] str=010");
    CHECK(phx_cavlc_count_cat[2] == 16 && phx_cavlc_count_cat[3] == 0 &&
          phx_cavlc_count_cat[5] == 64, "count_cat (cavlc.c:125)");
    CHECK(phx_cavlc_ct_index[0] == 0 && phx_cavlc_ct_index[1] == 0 &&
          phx_cavlc_ct_index[2] == 1 && phx_cavlc_ct_index[16] == 3,
          "ct_index (cavlc.c:198)");
    CHECK(hash_vlcs(&phx_coeff0_token[0], 6) == 0x91A8BAAFu, "coeff0_token full table");
    CHECK(hash_vlcs(&phx_coeff_token[0][0][0], 6*16*4) == 0x26A5DD06u, "coeff_token full table");
    CHECK(hash_vlcs(&phx_total_zeros[0][0], 15*16) == 0xB34BB847u, "total_zeros full table");
    CHECK(hash_vlcs(&phx_total_zeros_2x2_dc[0][0], 3*4) == 0x4E977FF7u, "tz_2x2_dc full table");
    CHECK(hash_vlcs(&phx_total_zeros_2x4_dc[0][0], 7*8) == 0xB19C9420u, "tz_2x4_dc full table");
    CHECK(hash_vlcs(&phx_run_before_init[0][0], 7*16) == 0xB4853F40u, "run_before_init full table");

    /* ---- generated level_token / run_before (vlc.c:33-99) ---- */
    phx_cavlc_init();
    /* level = +1 is index 65: code 0 -> size 1, bits 1, next 1. */
    CHECK(phx_level_token[0][65].i_bits == 1 && phx_level_token[0][65].i_size == 1 &&
          phx_level_token[0][65].i_next == 1, "level_token[0][+1]");
    /* level = +2 is index 66: code 2 -> "001", next stays 1. */
    CHECK(phx_level_token[0][66].i_bits == 1 && phx_level_token[0][66].i_size == 3 &&
          phx_level_token[0][66].i_next == 1, "level_token[0][+2]");
    /* level = +4 is index 68: code 6 -> 7 bits, next 2. */
    CHECK(phx_level_token[0][68].i_bits == 1 && phx_level_token[0][68].i_size == 7 &&
          phx_level_token[0][68].i_next == 2, "level_token[0][+4]");
    /* level = -1, suffix 0: code 1 -> "01". */
    CHECK(phx_level_token[0][63].i_bits == 1 && phx_level_token[0][63].i_size == 2,
          "level_token[0][-1]");
    /* level = -64, suffix 0: escape cell, size 28, bits 0x1000+0x811. */
    CHECK(phx_level_token[0][0].i_size == 28 && phx_level_token[0][0].i_bits == 0x1061,
          "level_token[0][-64]");
    /* run_before code packs (bits<<5)|size; zeros=1 with one zero left -> "0". */
    CHECK(phx_run_before[0] == 0 && phx_run_before[1] == 0 &&
          phx_run_before[5] == 1, "run_before[0/1/5]");
    CHECK(phx_run_before[21] == 67 && phx_run_before[85] == 581,
          "run_before[21/85]");
    /* Fingerprints over every generated entry (independent vlc.c model). */
    {
        uint32_t acc = 2166136261u;
        for (int i = 0; i < (1<<16); i++)
            acc = (acc ^ phx_run_before[i]) * 16777619u;
        CHECK(acc == 0x66CC413Bu, "run_before full table");
        acc = 2166136261u;
        for (int s = 0; s < 7; s++) for (int i = 0; i < PHX_CAVLC_LEVEL_TABLE_SIZE; i++) {
            phx_vlc_large_t v = phx_level_token[s][i];
            uint32_t x = v.i_bits*131u + v.i_size*17u + v.i_next*7u + 1u;
            acc = (acc ^ x) * 16777619u;
        }
        CHECK(acc == 0xDA51BAB9u, "level_token full table");
    }

    /* phx_ue_size_tab drives the Golomb size helpers (bitstream.h:278). */
    CHECK(phx_ue_size_tab[0] == 1 && phx_ue_size_tab[1] == 1 &&
          phx_bs_size_ue(0) == 1 && phx_bs_size_ue(1) == 3, "ue size tables");

    /* coeff_level_run16: scan [4,3,0,-2,0...] from the end. */
    {
        int16_t dct[16] = {0};
        phx_cavlc_run_level_t rl;
        dct[1] = -2; dct[3] = 3;
        int total = phx_cavlc_coeff_level_run(dct, 16, &rl);
        CHECK(total == 2 && rl.last == 3 && rl.mask == 10 &&
              rl.level[0] == 3 && rl.level[1] == -2, "coeff_level_run16");
    }

    /* ---- emitted bit strings ---- */
    {
        int16_t dct[16] = {0};
        /* coeff0_token[0] {1,1} = "1"; run_before[0] = 0 bits.
         * Padded byte: 1000 0000 = 0x80. */
        check_block("empty", dct, 16, PHX_DCT_LUMA_4x4, 0, 77, 0, 0,
                    "1", (const uint8_t[]){0x80}, 1);
    }
    {
        int16_t dct[16] = {0}; dct[0] = 1;
        /* coeff_token[0][0][1] {1,2}="01" + 1 sign "0" +
         * total_zeros[0][0] {1,1}="1"; padded 0101 0000 = 0x50. */
        check_block("single +1", dct, 16, PHX_DCT_LUMA_4x4, 0, 77, 0, 1,
                    "0101", (const uint8_t[]){0x50}, 1);
    }
    {
        int16_t dct[16] = {0}; dct[0] = 1; dct[1] = 1;
        /* coeff_token[0][1][2] {1,3}="001" + 2 signs "00" +
         * total_zeros[1][0] {7,3}="111"; byte 0010 0111 = 0x27. */
        check_block("two +1", dct, 16, PHX_DCT_LUMA_4x4, 0, 77, 0, 2,
                    "00100111", (const uint8_t[]){0x27}, 1);
    }
    {
        int16_t dct[16] = {0}; dct[0] = 1; dct[2] = -1;
        /* "001" + signs "10" (level[0]=-1, level[1]=+1) +
         * total_zeros[1][1] {6,3}="110" + run_before[5]="0". */
        check_block("+1,-1 with gap", dct, 16, PHX_DCT_LUMA_4x4, 0, 77, 0, 2,
                    "001101100", (const uint8_t[]){0x36,0x00}, 2);
    }
    {
        int16_t dct[16] = {0}; dct[0] = 2;
        /* coeff_token[0][0][0]={5,6}="000101"; trailing=0 so the first level
         * is decremented +2 -> +1: level_token[0][65]={1,1}="1";
         * next=level_token[0][66].i_next=1; total_zeros[0][0]="1". */
        check_block("single +2", dct, 16, PHX_DCT_LUMA_4x4, 0, 77, 0, 1,
                    "00010111", NULL, 0);
    }
    {
        int16_t dct[16] = {0}; dct[0] = -2;
        /* Same, negative level: level_token[0][63]={1,2}="01". */
        check_block("single -2", dct, 16, PHX_DCT_LUMA_4x4, 0, 77, 0, 1,
                    "000101011", NULL, 0);
    }
    {
        int16_t dct[16] = {0}; dct[0] = 64;
        /* val_original=128 >= 128 -> escape with level 63: level_code=124,
         * suffix 0: 124-30=94, prefix 15 -> "0000000000000001" + 12-bit 94
         * "000001011110"; total_zeros[0][0]="1". */
        check_block("single +64 escape", dct, 16, PHX_DCT_LUMA_4x4, 0, 100, 0, 1,
                    "00010100000000000000010000010111101", NULL, 0);
    }
    {
        int16_t dct[16] = {0}; dct[0] = 3000;
        /* Overflow escape: code after -30 is 5966 >= 4096. Without High
         * Profile x264 sets b_overflow and keeps prefix 15; 12-bit 5966-4096
         * = 1870 = 0x74E -> "011101001110"; then total_zeros "1". */
        phx_bs_t bs;
        uint8_t buf[64];
        phx_cavlc_t c;
        memset(buf, 0, sizeof(buf));
        phx_bs_init(&bs, buf, sizeof(buf));
        c.bs = &bs; c.profile_idc = 77; c.b_overflow = 0; c.chroma_2x4_dc = 0;
        uint8_t nnz = 1;
        phx_cavlc_block_residual(&c, PHX_DCT_LUMA_4x4, dct, &nnz, 0, 16);
        CHECK(c.b_overflow == 1, "overflow flag without High Profile");
        char got[128];
        bits_of_buffer(buf, phx_bs_pos(&bs), got);
        CHECK(strcmp(got, "00010100000000000000010111010011101") == 0,
              "single +3000 escape (no High Profile)");
    }
    {
        int16_t dct[4] = {1, 0, -1, 0};
        /* Chroma DC 2x2: coeff_token[4][1][2]={1,3}="001" + signs "10" +
         * tz_2x2_dc[1][1]={1,2}="01" + run_before[5]="0". */
        check_block("chroma dc 2x2", dct, 4, PHX_DCT_CHROMA_DC, 4, 77, 0, 2,
                    "00110010", NULL, 0);
    }
    {
        int16_t dct[8] = {1,0,0,0,-1,0,0,0};
        /* Chroma DC 2x4: coeff_token[5][1][2]={1,3}="001" + signs "10" +
         * tz_2x4_dc[1][3]={4,3}="100" + run_before[17]={0,2}="00". */
        check_block("chroma dc 2x4", dct, 8, PHX_DCT_CHROMA_DC, 5, 77, 1, 2,
                    "0011010000", NULL, 0);
    }
    {
        int16_t dct[16];
        for (int i = 0; i < 12; i++) dct[i] = 2;
        for (int i = 12; i < 16; i++) dct[i] = 0;
        /* total=12 > 10 and trailing<3 -> suffix starts at 1. */
        check_block("12 x +2", dct, 16, PHX_DCT_LUMA_4x4, 0, 77, 0, 12,
                    "000000000001011100100100100100100100100100100100100000", NULL, 0);
    }
    {
        int16_t dct[16] = {0};
        dct[0] = 2; dct[2] = -2; dct[4] = 3;
        /* mask 0b10101: run_before[21] = (2<<5)|3 -> 3 bits "010". */
        check_block("3 coeff gap gap", dct, 16, PHX_DCT_LUMA_4x4, 1, 77, 0, 3,
                    "0000111001011010110010", NULL, 0);
    }

    printf("%s: cavlc, %d failures\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
