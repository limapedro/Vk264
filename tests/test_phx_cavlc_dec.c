/* Round-trip tests for the Phoenix CAVLC decoder (W3).
 *
 * Every case encodes with src/phx_cavlc.c (the verbatim x264 writer) and then
 * decodes with src/phx_cavlc_dec.c, asserting the EXACT coefficient array and
 * the exact bit position.  No bitstream parity with x264 is claimed here.
 *
 * Sources: reference/x264/encoder/cavlc.c:65-209, :241-482;
 *          common/tables.c VLC leaves; common/vlc.c:33-99; H.264 9.2.
 *
 * Standalone MSVC build (from the repo root):
 *   cl /nologo /W3 /D_CRT_SECURE_NO_WARNINGS /Isrc ^
 *      tests/test_phx_cavlc_dec.c src/phx_cavlc_dec.c src/phx_cavlc.c ^
 *      src/phx_bitstream.c src/phx_tables.c src/phx_cavlc_syntax.c ^
 *      src/phx_mbcache.c /Fe:test_phx_cavlc_dec.exe
 *
 * SHADER: C — serial entropy test harness.
 */
#include "phx_cavlc_dec.h"
#include "phx_cavlc.h"
#include "phx_cavlc_syntax.h"
#include "phx_bitstream.h"
#include "phx_tables.h"
#include "phx_mbcache.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", (msg), __LINE__); ++fails; } } while (0)

static int phx_dec_num_coeff_for_test(int cat, int chroma, int chroma_2x4)
{
    if (chroma || cat == PHX_DCT_CHROMA_DC) return chroma_2x4 ? 8 : 4;
    if (cat == PHX_DCT_LUMA_AC || cat == PHX_DCT_CHROMA_AC) return 15;
    return 16;
}

/* Reader over the bytes already written by the encoder.  phx_bs_init()
 * zeroes its buffer, so it must NOT be used to open the read side. */
static phx_bs_t reader_of(uint8_t *buf)
{
    phx_bs_t r;
    r.p_start = buf;
    r.p = buf;
    r.bit = 0;
    return r;
}

static void check_residual(const char *name, const int16_t *dct, int num,
                           int cat, int nC, int chroma, int chroma_2x4,
                           const int16_t *want, int want_n)
{
    uint8_t buf[256];
    phx_bs_t w;
    phx_bs_init(&w, buf, sizeof(buf));
    phx_cavlc_t c;
    c.bs = &w;
    c.profile_idc = 100;             /* High: escape prefix extension allowed */
    c.b_overflow = 0;
    c.chroma_2x4_dc = chroma_2x4;

    uint8_t nnz = 0;
    for (int i = 0; i < num; i++)
        if (dct[i]) { nnz = 1; break; }
    phx_cavlc_block_residual(&c, cat, dct, &nnz, nC, num);
    int wbits = phx_bs_pos(&w);

    phx_bs_t r = reader_of(buf);
    int16_t got[16];
    memset(got, 0x7f, sizeof(got));
    int rbits = phx_cavlc_dec_residual(&r, got, cat, nC, chroma, chroma_2x4);
    if (rbits != wbits) {
        printf("FAIL %s bits: encoded %d, decoded %d\n", name, wbits, rbits);
        ++fails;
    }
    for (int i = 0; i < want_n; i++) {
        if (got[i] != want[i]) {
            printf("FAIL %s coef[%d]: got %d, want %d\n",
                   name, i, got[i], want[i]);
            ++fails;
        }
    }
    /* VLC decoding is defined only for the block's declared capacity. */
    for (int i = want_n; i < 16; i++)
        CHECK(got[i] == 0x7f7f, name);
}

static void check_mvd(const char *name, int16_t dx, int16_t dy)
{
    phx_mb_cache_t cache;
    memset(&cache, 0, sizeof(cache));
    int slot = phx_scan8[0];
    cache.ref[0][slot] = 0;
    cache.ref[0][slot-1] = 0;
    cache.ref[0][slot-8] = 0;
    cache.ref[0][slot-8+4] = 0;

    phx_cavlc_mb_t writer_mb, reader_mb;
    uint8_t buf[64];
    phx_bs_t w;
    phx_bs_init(&w, buf, sizeof(buf));
    phx_cavlc_mb_init(&writer_mb, &w, &cache);
    writer_mb.i_partition = PHX_D_16x16;
    writer_mb.cache->mv[0][slot][0] = dx;
    writer_mb.cache->mv[0][slot][1] = dy;
    int wbits = phx_cavlc_mvd(&writer_mb, 0, 0, 4);

    phx_bs_t r = reader_of(buf);
    phx_mb_cache_t cache2;
    memcpy(&cache2, &cache, sizeof(cache2));
    cache2.mv[0][slot][0] = 0;
    cache2.mv[0][slot][1] = 0;
    phx_cavlc_mb_init(&reader_mb, &r, &cache2);
    int16_t mv[2] = { 1234, 1234 };
    int rbits = phx_cavlc_dec_mvd(&r, mv);
    CHECK(rbits == wbits, name);
    CHECK(mv[0] == dx && mv[1] == dy, name);
}

int main(void)
{
    phx_cavlc_init();

    /* ---- empty blocks: coeff0_token[nC] (cavlc.c:205-206) ---- */
    {
        int16_t zero[16] = {0};
        check_residual("empty nC=0", zero, 16, PHX_DCT_LUMA_4x4, 0, 0, 0, zero, 16);
        check_residual("empty nC=3", zero, 16, PHX_DCT_LUMA_4x4, 3, 0, 0, zero, 16);
        check_residual("empty chromaDC420", zero, 4, PHX_DCT_CHROMA_DC, 4, 1, 0, zero, 4);
        check_residual("empty chromaDC422", zero, 8, PHX_DCT_CHROMA_DC, 5, 1, 1, zero, 8);
    }

    /* ---- trailing ones: signs and coeff_token (H.264 9.2.2.1/.2) ---- */
    {
        int16_t d[16] = { 1, -1, 1, 0 };
        check_residual("trailing3", d, 16, PHX_DCT_LUMA_4x4, 0, 0, 0, d, 16);
    }
    {
        int16_t d[16] = { 1, -1, 2, 0 };
        check_residual("trailing2+level", d, 16, PHX_DCT_LUMA_4x4, 1, 0, 0, d, 16);
    }
    {
        int16_t d[16] = { -1, -1, -1, -1 };
        check_residual("trailing4-", d, 16, PHX_DCT_LUMA_4x4, 2, 0, 0, d, 16);
    }
    {
        int16_t d[16] = { 1, -1, 1, -2, 3, -3, 0 };
        check_residual("mixed-levels", d, 16, PHX_DCT_LUMA_4x4, 3, 0, 0, d, 16);
    }

    /* ---- zero runs: total_zeros + run_before (H.264 9.2.2.4/.5) ---- */
    {
        int16_t d[16] = {0};
        d[15] = 1;
        check_residual("single-last", d, 16, PHX_DCT_LUMA_4x4, 0, 0, 0, d, 16);
    }
    {
        int16_t d[16] = {0};
        d[0] = -1; d[7] = 2; d[15] = -3;
        check_residual("spread-runs", d, 16, PHX_DCT_LUMA_4x4, 2, 0, 0, d, 16);
    }
    {
        int16_t d[15] = {0};
        d[1] = 1; d[14] = -1;
        check_residual("lumaAC15", d, 15, PHX_DCT_LUMA_AC, 1, 0, 0, d, 15);
    }

    /* ---- escape levels: level_prefix >= 15 (cavlc.c:70-119) ---- */
    {
        int16_t d[16] = { 1, -1, 100, -100, 0 };
        check_residual("escape-100", d, 16, PHX_DCT_LUMA_4x4, 1, 0, 0, d, 16);
    }
    {
        int16_t d[16] = { 1000, -3000, 0 };
        check_residual("escape-large", d, 16, PHX_DCT_LUMA_4x4, 0, 0, 0, d, 16);
    }
    {
        int16_t d[16] = { 63, -63, 64, -64, 0 };
        check_residual("escape-boundary", d, 16, PHX_DCT_LUMA_4x4, 0, 0, 0, d, 16);
    }
    {
        int16_t d[16] = { 10000, -20000, 30000, 0 };
        check_residual("escape-high-prefix", d, 16, PHX_DCT_LUMA_4x4, 0, 0, 0, d, 16);
    }

    /* ---- chroma DC 2x2 / 2x4 (cavlc.c:180-187) ---- */
    {
        int16_t d[4] = { 3, -2, 1, 0 };
        check_residual("chromaDC420", d, 4, PHX_DCT_CHROMA_DC, 4, 1, 0, d, 4);
    }
    {
        int16_t d[8] = { 4, -4, 2, -2, 1, -1, 3, 0 };
        check_residual("chromaDC422", d, 8, PHX_DCT_CHROMA_DC, 5, 1, 1, d, 8);
    }
    {
        int16_t d[8] = {0};
        d[7] = -5;
        check_residual("chromaDC422-run", d, 8, PHX_DCT_CHROMA_DC, 5, 1, 1, d, 8);
    }

    /* ---- deterministic pseudo-random residual sweep ---- */
    {
        uint32_t rng = 0x13579bdfu;
        for (int trial = 0; trial < 400; trial++) {
            rng = rng * 1103515245u + 12345u;
            int cat_idx = (int)((rng >> 8) % 5);
            int cats[5] = { PHX_DCT_LUMA_DC, PHX_DCT_LUMA_AC,
                            PHX_DCT_LUMA_4x4, PHX_DCT_CHROMA_DC,
                            PHX_DCT_CHROMA_AC };
            int cat = cats[cat_idx];
            int chroma = cat == PHX_DCT_CHROMA_DC;
            int chroma_2x4 = chroma && ((rng >> 16) & 1);
            int num = phx_dec_num_coeff_for_test(cat, chroma, chroma_2x4);
            int nC = cat == PHX_DCT_CHROMA_DC
                   ? (chroma_2x4 ? 5 : 4) : (int)((rng >> 20) % 4);
            int16_t d[16] = {0};
            rng = rng * 1103515245u + 12345u;
            int nnz = (int)(rng % (unsigned)(num + 1));
            for (int k = 0; k < nnz; k++) {
                rng = rng * 1103515245u + 12345u;
                int pos = (int)(rng % (unsigned)num);
                while (d[pos]) pos = (pos + 1) % num;
                rng = rng * 1103515245u + 12345u;
                int mag = (int)(rng % 2001u) + 1;
                if ((rng >> 12) % 3 == 0) mag = 1;   /* force trailing ones */
                d[pos] = (int16_t)((rng & 0x8000u) ? -mag : mag);
            }
            char name[32];
            snprintf(name, sizeof(name), "sweep-%d", trial);
            check_residual(name, d, num, cat, nC, chroma, chroma_2x4, d, num);
        }
    }

    /* ---- MVD: two se(v) with mvp from the borrowed cache ---- */
    check_mvd("mvd-zero", 0, 0);
    check_mvd("mvd-pos", 17, -9);
    check_mvd("mvd-negative", -123, 456);

    /* ---- MB header: I16x16 / I4x4 / P16x16 round trip ---- */
    {
        uint8_t buf[256];
        phx_bs_t w;
        phx_bs_init(&w, buf, sizeof(buf));
        phx_mb_cache_t cache;
        memset(&cache, 0, sizeof(cache));
        phx_cavlc_mb_t mb;
        phx_cavlc_mb_init(&mb, &w, &cache);
        mb.slice_type = PHX_SLICE_TYPE_I;
        mb.chroma = 1;
        mb.i_type = PHX_I_16x16;
        mb.i_intra16x16_pred_mode = 2;
        mb.i_cbp_luma = 15;
        mb.i_cbp_chroma = 1;
        mb.i_chroma_pred_mode = 3;
        int wbits = phx_cavlc_mb_header(&mb);
        CHECK(wbits > 0, "I16x16 header write");

        phx_bs_t r = reader_of(buf);
        phx_mb_cache_t cache2;
        memset(&cache2, 0, sizeof(cache2));
        phx_cavlc_mb_t out;
        phx_cavlc_mb_init(&out, &r, &cache2);
        out.slice_type = PHX_SLICE_TYPE_I;
        out.chroma = 1;
        int rbits = phx_cavlc_dec_mb_header(&r, &out);
        CHECK(rbits == wbits, "I16x16 header bits");
        CHECK(out.i_type == PHX_I_16x16, "I16x16 type");
        CHECK(out.i_intra16x16_pred_mode == 2, "I16x16 pred");
        CHECK(out.i_cbp_luma == 15 && out.i_cbp_chroma == 1, "I16x16 cbp");
        CHECK(out.i_chroma_pred_mode == 3, "I16x16 chroma pred");
    }
    {
        uint8_t buf[256];
        phx_bs_t w;
        phx_bs_init(&w, buf, sizeof(buf));
        phx_mb_cache_t cache;
        memset(&cache, 0, sizeof(cache));
        for (int i = 0; i < 40; i++) cache.intra4x4_pred_mode[i] = 2;
        /* x264's writer predicts from the cache holding this MB's chosen
         * modes, so seed all 16 slots before encoding. */
        for (int i = 0; i < 16; i++)
            cache.intra4x4_pred_mode[phx_scan8[i]] = 2;
        phx_cavlc_mb_t mb;
        phx_cavlc_mb_init(&mb, &w, &cache);
        mb.slice_type = PHX_SLICE_TYPE_I;
        mb.chroma = 1;
        mb.i_type = PHX_I_4x4;
        mb.i_chroma_pred_mode = 1;
        cache.intra4x4_pred_mode[phx_scan8[0]] = 0;
        cache.intra4x4_pred_mode[phx_scan8[5]] = 3;
        int wbits = phx_cavlc_mb_header(&mb);
        CHECK(wbits > 0, "I4x4 header write");

        phx_bs_t r = reader_of(buf);
        phx_mb_cache_t cache2;
        memcpy(&cache2, &cache, sizeof(cache2));
        phx_cavlc_mb_t out;
        phx_cavlc_mb_init(&out, &r, &cache2);
        out.slice_type = PHX_SLICE_TYPE_I;
        out.chroma = 1;
        int rbits = phx_cavlc_dec_mb_header(&r, &out);
        CHECK(rbits == wbits, "I4x4 header bits");
        CHECK(out.i_type == PHX_I_4x4, "I4x4 type");
        CHECK(cache2.intra4x4_pred_mode[phx_scan8[0]] == 0, "I4x4 mode0");
        CHECK(cache2.intra4x4_pred_mode[phx_scan8[5]] == 3, "I4x4 mode5");
    }
    {
        uint8_t buf[256];
        phx_bs_t w;
        phx_bs_init(&w, buf, sizeof(buf));
        phx_mb_cache_t cache;
        memset(&cache, 0, sizeof(cache));
        phx_cavlc_mb_t mb;
        phx_cavlc_mb_init(&mb, &w, &cache);
        mb.slice_type = PHX_SLICE_TYPE_P;
        mb.i_type = PHX_P_L0;
        mb.i_partition = PHX_D_16x16;
        mb.i_fref[0] = 1;
        int slot = phx_scan8[0];
        mb.cache->mv[0][slot][0] = 8;
        mb.cache->mv[0][slot][1] = -5;
        int wbits = phx_cavlc_mb_header(&mb);
        CHECK(wbits > 0, "P16x16 header write");

        phx_bs_t r = reader_of(buf);
        phx_mb_cache_t cache2;
        memcpy(&cache2, &cache, sizeof(cache2));
        cache2.mv[0][slot][0] = 0;
        cache2.mv[0][slot][1] = 0;
        phx_cavlc_mb_t out;
        phx_cavlc_mb_init(&out, &r, &cache2);
        out.slice_type = PHX_SLICE_TYPE_P;
        out.i_fref[0] = 1;
        int rbits = phx_cavlc_dec_mb_header(&r, &out);
        CHECK(rbits == wbits, "P16x16 header bits");
        CHECK(out.i_type == PHX_P_L0 && out.i_partition == PHX_D_16x16,
              "P16x16 type/partition");
        CHECK(cache2.mv[0][slot][0] == 8 && cache2.mv[0][slot][1] == -5,
              "P16x16 mv");
    }

    /* ---- P_8x8: sub-partition decode + 8x8/4x4 MVD rectangles ---- */
    {
        uint8_t buf[256];
        phx_bs_t w;
        phx_bs_init(&w, buf, sizeof(buf));
        phx_mb_cache_t cache;
        memset(&cache, 0, sizeof(cache));
        phx_cavlc_mb_t mb;
        phx_cavlc_mb_init(&mb, &w, &cache);
        mb.slice_type = PHX_SLICE_TYPE_P;
        mb.i_type = PHX_P_8x8;
        mb.i_partition = PHX_D_8x8;
        mb.b_psub8x8 = 1;
        mb.i_fref[0] = 1;
        int sub[4] = { PHX_D_L0_8x8, PHX_D_L0_4x4,
                       PHX_D_L0_8x8, PHX_D_L0_8x8 };
        for (int i = 0; i < 4; i++) mb.i_sub_partition[i] = sub[i];
        /* One MVD per partition: the rectangles below are disjoint, so the
         * writer's final cache values are also its predictors' inputs. */
        for (int i = 0; i < 16; i++) {
            cache.mv[0][phx_scan8[i]][0] = 0;
            cache.mv[0][phx_scan8[i]][1] = 0;
        }
        int slots0[4] = { phx_scan8[0], phx_scan8[1], phx_scan8[2], phx_scan8[3] };
        int slots1[4] = { phx_scan8[4], phx_scan8[5], phx_scan8[6], phx_scan8[7] };
        int slots2[4] = { phx_scan8[8], phx_scan8[9], phx_scan8[10], phx_scan8[11] };
        int slots3[4] = { phx_scan8[12], phx_scan8[13], phx_scan8[14], phx_scan8[15] };
        for (int i = 0; i < 4; i++) {
            cache.mv[0][slots0[i]][0] = 1;  cache.mv[0][slots0[i]][1] = -1;
            cache.mv[0][slots1[i]][0] = (int16_t)(2+i);
            cache.mv[0][slots1[i]][1] = (int16_t)(-2-i);
            cache.mv[0][slots2[i]][0] = 9;  cache.mv[0][slots2[i]][1] = -9;
            cache.mv[0][slots3[i]][0] = 13; cache.mv[0][slots3[i]][1] = -13;
        }
        int wbits = phx_cavlc_mb_header(&mb);
        CHECK(wbits > 0, "P8x8 header write");

        phx_bs_t r = reader_of(buf);
        phx_mb_cache_t cache2;
        memcpy(&cache2, &cache, sizeof(cache2));
        for (int i = 0; i < 16; i++) {
            cache2.mv[0][phx_scan8[i]][0] = 0;
            cache2.mv[0][phx_scan8[i]][1] = 0;
        }
        phx_cavlc_mb_t out;
        phx_cavlc_mb_init(&out, &r, &cache2);
        out.slice_type = PHX_SLICE_TYPE_P;
        out.i_fref[0] = 1;
        int rbits = phx_cavlc_dec_mb_header(&r, &out);
        CHECK(rbits == wbits, "P8x8 header bits");
        CHECK(out.i_type == PHX_P_8x8 && out.b_psub8x8, "P8x8 sub flag");
        for (int i = 0; i < 4; i++)
            CHECK(out.i_sub_partition[i] == sub[i], "P8x8 sub partition");
        for (int i = 0; i < 16; i++)
            CHECK(cache2.mv[0][phx_scan8[i]][0] == cache.mv[0][phx_scan8[i]][0] &&
                  cache2.mv[0][phx_scan8[i]][1] == cache.mv[0][phx_scan8[i]][1],
                  "P8x8 mv rect");
    }
    {
        uint8_t buf[256];
        phx_bs_t w;
        phx_bs_init(&w, buf, sizeof(buf));
        phx_mb_cache_t cache;
        memset(&cache, 0, sizeof(cache));
        phx_cavlc_mb_t mb;
        phx_cavlc_mb_init(&mb, &w, &cache);
        mb.slice_type = PHX_SLICE_TYPE_P;
        mb.i_type = PHX_P_8x8;
        mb.i_partition = PHX_D_8x8;
        mb.b_psub8x8 = 0;                       /* 4-bit 0xf marker */
        mb.i_fref[0] = 1;
        for (int i = 0; i < 4; i++) mb.i_sub_partition[i] = PHX_D_L0_8x8;
        int wbits = phx_cavlc_mb_header(&mb);
        phx_bs_t r = reader_of(buf);
        phx_mb_cache_t cache2;
        memcpy(&cache2, &cache, sizeof(cache2));
        phx_cavlc_mb_t out;
        phx_cavlc_mb_init(&out, &r, &cache2);
        out.slice_type = PHX_SLICE_TYPE_P;
        out.i_fref[0] = 1;
        int rbits = phx_cavlc_dec_mb_header(&r, &out);
        CHECK(rbits == wbits, "P8x8 0xf marker bits");
        CHECK(!out.b_psub8x8, "P8x8 0xf marker flag");
    }

    /* ---- B headers: B_Direct and B_L0_L0_16x16 (cavlc.c:411-482) ---- */
    {
        uint8_t buf[256];
        phx_bs_t w;
        phx_bs_init(&w, buf, sizeof(buf));
        phx_mb_cache_t cache;
        memset(&cache, 0, sizeof(cache));
        phx_cavlc_mb_t mb;
        phx_cavlc_mb_init(&mb, &w, &cache);
        mb.slice_type = PHX_SLICE_TYPE_B;
        mb.i_type = PHX_B_DIRECT;
        int wbits = phx_cavlc_mb_header(&mb);
        phx_bs_t r = reader_of(buf);
        phx_mb_cache_t cache2;
        memset(&cache2, 0, sizeof(cache2));
        phx_cavlc_mb_t out;
        phx_cavlc_mb_init(&out, &r, &cache2);
        out.slice_type = PHX_SLICE_TYPE_B;
        int rbits = phx_cavlc_dec_mb_header(&r, &out);
        CHECK(rbits == wbits, "B_Direct bits");
        CHECK(out.i_type == PHX_B_DIRECT, "B_Direct type");
    }
    {
        uint8_t buf[256];
        phx_bs_t w;
        phx_bs_init(&w, buf, sizeof(buf));
        phx_mb_cache_t cache;
        memset(&cache, 0, sizeof(cache));
        phx_cavlc_mb_t mb;
        phx_cavlc_mb_init(&mb, &w, &cache);
        mb.slice_type = PHX_SLICE_TYPE_B;
        mb.i_type = PHX_B_L0_L0;
        mb.i_partition = PHX_D_16x16;
        mb.i_fref[0] = 1; mb.i_fref[1] = 1;
        int slot = phx_scan8[0];
        cache.mv[0][slot][0] = 6;
        cache.mv[0][slot][1] = -7;
        int wbits = phx_cavlc_mb_header(&mb);
        phx_bs_t r = reader_of(buf);
        phx_mb_cache_t cache2;
        memcpy(&cache2, &cache, sizeof(cache2));
        cache2.mv[0][slot][0] = 0;
        cache2.mv[0][slot][1] = 0;
        phx_cavlc_mb_t out;
        phx_cavlc_mb_init(&out, &r, &cache2);
        out.slice_type = PHX_SLICE_TYPE_B;
        out.i_fref[0] = 1; out.i_fref[1] = 1;
        int rbits = phx_cavlc_dec_mb_header(&r, &out);
        CHECK(rbits == wbits, "B_L0_L0_16x16 bits");
        CHECK(out.i_type == PHX_B_L0_L0 &&
              out.i_partition == PHX_D_16x16, "B_L0_L0 type/partition");
        CHECK(cache2.mv[0][slot][0] == 6 && cache2.mv[0][slot][1] == -7,
              "B_L0_L0 mv");
    }

    if (fails) {
        printf("%d CAVLC decoder round-trip check(s) failed\n", fails);
        return 1;
    }
    printf("phx_cavlc_dec: all round-trip checks passed\n");
    return 0;
}
