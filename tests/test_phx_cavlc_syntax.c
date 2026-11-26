/* Golden bit-vector tests for the CAVLC MB header / MVD port.
 *
 * Every expected bit string below was derived by hand from the x264 source:
 *   encoder/cavlc.c:36-63    (syntax mapping tables, checked verbatim in test_tables)
 *   encoder/cavlc.c:305-482  (writer call order / branch selection)
 *   common/bitstream.h:169,241,246,264 (bs_write1/ue/se/te)
 *     ue(v): codeNum=v+1, 2*floor(log2(codeNum))+1 bits, MSB first
 *     se(v): codeNum = 2v-1 (v>0), -2v (v<=0)
 *     te(x,v): x==1 ? 1^v : ue(v)
 *   common/mvpred.c:30-127   (MV predictor used by cavlc_mvd)
 * The mb_skip_run vectors (test_skip_run_golden) are instead captured from
 * an instrumented x264 build replaying encoder.c:2883-2890 and :3087-3088
 * with x264's own bs_write_ue() on fixed skip/non-skip MB sequences.
 *
 * SHADER: C — the tested writers are the serial entropy stage.
 */
#include "phx_cavlc_syntax.h"
#include "phx_mbcache.h"
#include <stdio.h>
#include <string.h>

static int n_tests, n_failed;

#define CHECK(cond, ...) do { \
    n_tests++; \
    if (!(cond)) { \
        n_failed++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/* Compare all emitted bits (MSB first) with a '0'/'1' golden string. */
static void check_bits(const char *name, phx_bs_t *s, const char *golden)
{
    int bits = (int)strlen(golden);
    int pos = phx_bs_pos(s);
    int i, bad = -1;
    unsigned char bad_byte = 0;
    CHECK(pos == bits, "%s: bs_pos=%d, expected %d bits", name, pos, bits);
    for (i = 0; i < bits && bad < 0; i++)
    {
        int got = (s->p_start[i >> 3] >> (7 - (i & 7))) & 1;
        if (got != (golden[i] - '0')) { bad = i; bad_byte = s->p_start[i >> 3]; }
    }
    CHECK(bad < 0, "%s: bit %d mismatch (got buffer byte %02x), expected %s",
          name, bad, bad_byte, golden);
    if (bad < 0) { n_tests++; printf("  ok  %-32s %2d bits  %s\n", name, bits, golden); }
}

static void flat_cache(phx_mb_cache_t *c)
{
    memset(c, 0, sizeof(*c));
    for (int l = 0; l < 2; l++) {
        for (int i = 0; i < 40; i++) {
            c->ref[l][i] = -2;
            c->mv[l][i][0] = 0;
            c->mv[l][i][1] = 0;
        }
        for (int i = 0; i < 40; i++) { c->mvd[l][i][0] = 0; c->mvd[l][i][1] = 0; }
    }
    for (int i = 0; i < 40; i++) c->intra4x4_pred_mode[i] = -1;
}

/* x264 tests ref[]==0 for all four 8x8 blocks of P_8x8 (cavlc.c:376-377);
 * flat_cache uses -2 (unavailable), so set valid refs explicitly. */
static void zero_p8x8_refs(phx_mb_cache_t *c)
{
    const int idx[4] = { 0, 4, 8, 12 };
    for (int i = 0; i < 4; i++) c->ref[0][phx_scan8[idx[i]]] = 0;
}

static void setup(phx_cavlc_mb_t *mb, phx_bs_t *s, uint8_t *buf,
                  phx_mb_cache_t *cache, int slice_type, int i_type)
{
    phx_bs_init(s, buf, 64);
    flat_cache(cache);
    phx_cavlc_mb_init(mb, s, cache);
    mb->slice_type = slice_type;
    mb->i_type = i_type;
    mb->i_partition = PHX_D_16x16;
    mb->i_fref[0] = 1;
    mb->i_fref[1] = 1;
    mb->chroma = 1;
    mb->b_psub8x8 = 1;
    for (int i = 0; i < 4; i++) mb->i_sub_partition[i] = PHX_D_L0_8x8;
}

static void test_tables(void)
{
    const uint8_t (*c)[2][48] = phx_cavlc_cbp_to_golomb;
    CHECK(c[0][0][0]==0 && c[0][0][5]==6 && c[0][0][15]==9, "cbp_to_golomb 400 inter");
    CHECK(c[0][1][0]==1 && c[0][1][15]==0, "cbp_to_golomb 400 intra");
    CHECK(c[1][0][16]==1 && c[1][0][47]==12, "cbp_to_golomb 420 inter");
    CHECK(c[1][1][0]==3 && c[1][1][47]==0, "cbp_to_golomb 420 intra");
    CHECK(phx_cavlc_mb_type_b_to_golomb[0][0]==4 &&
          phx_cavlc_mb_type_b_to_golomb[0][8]==20 &&
          phx_cavlc_mb_type_b_to_golomb[1][0]==5 &&
          phx_cavlc_mb_type_b_to_golomb[1][8]==21 &&
          phx_cavlc_mb_type_b_to_golomb[2][0]==1 &&
          phx_cavlc_mb_type_b_to_golomb[2][4]==2 &&
          phx_cavlc_mb_type_b_to_golomb[2][8]==3 &&
          phx_cavlc_mb_type_b_to_golomb[2][1]==(uint8_t)-1, "mb_type_b_to_golomb");
    CHECK(phx_cavlc_subpartition_p_to_golomb[0]==3 &&
          phx_cavlc_subpartition_p_to_golomb[1]==1 &&
          phx_cavlc_subpartition_p_to_golomb[2]==2 &&
          phx_cavlc_subpartition_p_to_golomb[3]==0, "subpartition_p_to_golomb");
    CHECK(phx_cavlc_subpartition_b_to_golomb[0]==10 &&
          phx_cavlc_subpartition_b_to_golomb[3]==1 &&
          phx_cavlc_subpartition_b_to_golomb[12]==0, "subpartition_b_to_golomb");
}

static void test_predict_mv(void)
{
    phx_bs_t s; uint8_t buf[64]; phx_mb_cache_t cache;
    phx_cavlc_mb_t mb; int16_t mvp[2];

    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_L0);
    /* Left/top/top-right same-ref neighbours: median((4,6),(-2,3),(10,1)) */
    cache.ref[0][phx_scan8[0]] = 0; /* current block ref: compare against it */
    cache.ref[0][phx_scan8[0]-1] = 0;    cache.mv[0][phx_scan8[0]-1][0] =  4; cache.mv[0][phx_scan8[0]-1][1] = 6;
    cache.ref[0][phx_scan8[0]-8] = 0;    cache.mv[0][phx_scan8[0]-8][0] = -2; cache.mv[0][phx_scan8[0]-8][1] = 3;
    cache.ref[0][phx_scan8[0]-4] = 0;    cache.mv[0][phx_scan8[0]-4][0] = 10; cache.mv[0][phx_scan8[0]-4][1] = 1;
    phx_cavlc_predict_mv(&mb, 0, 0, 4, mvp);   /* mvpred.c:107-113 */
    CHECK(mvp[0]==4 && mvp[1]==3, "predict_mv median: (%d,%d) != (4,3)", mvp[0], mvp[1]);

    /* Exactly one matching ref -> that MV (mvpred.c:114-122). */
    flat_cache(&cache);
    cache.ref[0][phx_scan8[0]] = 0;
    cache.ref[0][phx_scan8[0]-1] = 0;
    cache.mv[0][phx_scan8[0]-1][0] = 9; cache.mv[0][phx_scan8[0]-1][1] = -5;
    phx_cavlc_predict_mv(&mb, 0, 0, 4, mvp);
    CHECK(mvp[0]==9 && mvp[1]==-5, "predict_mv single: (%d,%d) != (9,-5)", mvp[0], mvp[1]);

    /* 16x8 special case: top replaces the predictor (mvpred.c:68-77). */
    flat_cache(&cache);
    mb.i_partition = PHX_D_16x8;
    cache.ref[0][phx_scan8[0]] = 0;
    cache.ref[0][phx_scan8[0]-8] = 0;
    cache.mv[0][phx_scan8[0]-8][0] = 3; cache.mv[0][phx_scan8[0]-8][1] = 7;
    cache.mv[0][phx_scan8[0]-1][0] = 1; cache.mv[0][phx_scan8[0]-1][1] = 1;
    phx_cavlc_predict_mv(&mb, 0, 0, 4, mvp);
    CHECK(mvp[0]==3 && mvp[1]==7, "predict_mv 16x8: (%d,%d) != (3,7)", mvp[0], mvp[1]);
}

static void test_mvd(void)
{
    phx_bs_t s; uint8_t buf[64]; phx_mb_cache_t cache; phx_cavlc_mb_t mb;
    int bits;

    /* mvp=(0,0); mvd=(+3,-4): se(3)="00110", se(-4)="0001001". */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_L0);
    cache.mv[0][phx_scan8[0]][0] = 3;
    cache.mv[0][phx_scan8[0]][1] = -4;
    bits = phx_cavlc_mvd(&mb, 0, 0, 4);
    CHECK(bits == 12, "mvd return %d != 12", bits);
    check_bits("mvd_l0(+3,-4)", &s, "001100001001");

    /* List 1, mvd=(-1,+2): se(-1)="011", se(+2)="00100". */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_B, PHX_B_L0_L1);
    cache.mv[1][phx_scan8[0]][0] = -1;
    cache.mv[1][phx_scan8[0]][1] = 2;
    bits = phx_cavlc_mvd(&mb, 1, 0, 4);
    CHECK(bits == 8, "mvd_l1 return %d != 8", bits);
    check_bits("mvd_l1(-1,+2)", &s, "01100100");
}

static void test_header_p(void)
{
    phx_bs_t s; uint8_t buf[64]; phx_mb_cache_t cache; phx_cavlc_mb_t mb;
    int bits;

    /* P_L0 16x16, 1 ref: mb_type_flag "1" + mvd(2,1) = "00100"+"010". */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_L0);
    cache.mv[0][phx_scan8[0]][0] = 2;
    cache.mv[0][phx_scan8[0]][1] = 1;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 9, "P_L0 16x16 return %d != 9", bits);
    check_bits("P_L0 16x16 1ref", &s, "100100010");

    /* ref idx te(2,2)=ue(2)="011" inserted before the same mvd. */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_L0);
    mb.i_fref[0] = 3;
    cache.ref[0][phx_scan8[0]] = 2;
    cache.mv[0][phx_scan8[0]][0] = 2;
    cache.mv[0][phx_scan8[0]][1] = 1;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 12, "P_L0 16x16 3ref return %d != 12", bits);
    check_bits("P_L0 16x16 3ref", &s, "101100100010");

    /* P_L0 16x8: ue(1)="010" + mvd(0,0) + mvd(0,1)="1010". */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_L0);
    mb.i_partition = PHX_D_16x8;
    cache.mv[0][phx_scan8[8]][1] = 1;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 9, "P_L0 16x8 return %d != 9", bits);
    check_bits("P_L0 16x8", &s, "010111010");

    /* P_L0 8x16: ue(2)="011" + se(1)se(-1) + se(-2)se(2). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_L0);
    mb.i_partition = PHX_D_8x16;
    cache.mv[0][phx_scan8[0]][0] = 1; cache.mv[0][phx_scan8[0]][1] = -1;
    cache.mv[0][phx_scan8[4]][0] = -2; cache.mv[0][phx_scan8[4]][1] = 2;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 19, "P_L0 8x16 return %d != 19", bits);
    check_bits("P_L0 8x16", &s, "0110100110010100100");

    /* P_8x8: ue(4) + 4 subpartition ue(0) + sub MVDs. */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_8x8);
    zero_p8x8_refs(&cache);
    cache.mv[0][phx_scan8[0]][0] = 1;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 19, "P_8x8 return %d != 19", bits);
    check_bits("P_8x8 all-ref0", &s, "0010111110101111111");

    /* analyse.inter without PSUB8x8 writes 4 bits of 1 instead of 4 ue(0):
     * identical bit pattern, asserted explicitly (cavlc.c:389-393). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_8x8);
    zero_p8x8_refs(&cache);
    mb.b_psub8x8 = 0;
    cache.mv[0][phx_scan8[0]][0] = 1;
    bits = phx_cavlc_mb_header(&mb);
    check_bits("P_8x8 no psub8x8", &s, "0010111110101111111");

    /* P_8x8 with one non-zero sub ref: ue(3), te(1,ref[]) for the four 8x8s. */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_8x8);
    zero_p8x8_refs(&cache);
    mb.i_fref[0] = 2;
    cache.ref[0][phx_scan8[4]] = 1;
    cache.mv[0][phx_scan8[0]][0] = 1;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 23, "P_8x8 sub-ref return %d != 23", bits);
    check_bits("P_8x8 sub-ref", &s, "00100111110110101111111");

    /* D_L0_8x4 subs: ue(1) per sub, two 8x4 mvds per 8x8 (cavlc.c:258-259). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_8x8);
    zero_p8x8_refs(&cache);
    for (int i = 0; i < 4; i++) mb.i_sub_partition[i] = PHX_D_L0_8x4;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 33, "P_8x8 8x4 return %d != 33", bits);
    check_bits("P_8x8 L0_8x4 subs", &s,
        "00101" "010010010010" "1111111111111111");

    /* D_L0_4x4 subs: ue(3) per sub, four 4x4 mvds per 8x8 (cavlc.c:266-269). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_8x8);
    zero_p8x8_refs(&cache);
    for (int i = 0; i < 4; i++) mb.i_sub_partition[i] = PHX_D_L0_4x4;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 57, "P_8x8 4x4 return %d != 57", bits);
    check_bits("P_8x8 L0_4x4 subs", &s,
        "0010100100001000010000100" "11111111111111111111111111111111");

    /* Intra fallback inside a P slice uses i_mb_i_offset=5 (cavlc.c:407-408). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_I_16x16);
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 6, "P slice I_16x16 return %d != 6", bits);
    check_bits("P slice I16x16 offset5", &s, "001111");
}

static void test_header_i(void)
{
    phx_bs_t s; uint8_t buf[64]; phx_mb_cache_t cache; phx_cavlc_mb_t mb;
    int bits;

    /* I slice I_16x16 V/DC0: ue(0+1+0+0+0)="010", chroma ue(0)="1". */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_I, PHX_I_16x16);
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 4, "I_16x16 return %d != 4", bits);
    check_bits("I_16x16 V cbp0", &s, "0101");

    /* I_16x16 P mode, cbp_chroma=1, cbp_luma!=0 -> ue(20), chroma ue(2). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_I, PHX_I_16x16);
    mb.i_intra16x16_pred_mode = 3;
    mb.i_chroma_pred_mode = 2;
    mb.i_cbp_chroma = 1;
    mb.i_cbp_luma = 15;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 12, "I_16x16 cbp return %d != 12", bits);
    check_bits("I_16x16 P cbp", &s, "000010101011");

    /* I_4x4: ue(0), transform flag 0, block0 mode V vs predicted DC -> 0000,
     * remaining 15 blocks DC==DC -> 1. chroma disabled. */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_I, PHX_I_4x4);
    mb.chroma = 0;
    mb.b_transform_8x8_mode = 1;
    mb.b_transform_8x8 = 0;
    for (int i = 0; i < 16; i++) cache.intra4x4_pred_mode[phx_scan8[i]] = 2; /* DC */
    cache.intra4x4_pred_mode[phx_scan8[0]] = 0; /* I_PRED_4x4_V */
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 21, "I_4x4 return %d != 21", bits);
    check_bits("I_4x4 pred mismatch", &s, "100000111111111111111");
}

static void test_header_b(void)
{
    phx_bs_t s; uint8_t buf[64]; phx_mb_cache_t cache; phx_cavlc_mb_t mb;
    int bits;

    /* B_8x8: ue(22)="000010111" (9 bits, codeNum=23) + 4*ue(1) + 4*mvd(0,0). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_B, PHX_B_8x8);
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 29, "B_8x8 return %d != 29", bits);
    check_bits("B_8x8 L0 subs", &s, "00001011101001001001011111111");

    /* B_8x8 with a bi-directional sub: sub_b[11]=3, L0 mvd for all four
     * sub-partitions plus one L1 mvd (cavlc.c:433-438). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_B, PHX_B_8x8);
    mb.i_sub_partition[0] = PHX_D_BI_8x8;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 33, "B_8x8 BI return %d != 33", bits);
    check_bits("B_8x8 BI sub", &s,
        "000010111" "00100" "010010010" "1111111111");

    /* B_L0_L0 16x16: mb_type_b[D_16x16][B_L0_L0]=1 -> ue(1)="010" + L0 mvd. */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_B, PHX_B_L0_L0);
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 5, "B_L0_L0 return %d != 5", bits);
    check_bits("B_L0_L0 16x16", &s, "01011");

    /* B_L1_L1 8x16: mb_type_b[1][4]=7 -> ue(7)="0001000", two L1 mvds. */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_B, PHX_B_L1_L1);
    mb.i_partition = PHX_D_8x16;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 11, "B_L1_L1 return %d != 11", bits);
    check_bits("B_L1_L1 8x16", &s, "00010001111");

    /* B_BI_BI 16x8: mb_type_b[0][8]=20 -> ue(20), four mvds (L0/L1 x2). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_B, PHX_B_BI_BI);
    mb.i_partition = PHX_D_16x8;
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 17, "B_BI_BI return %d != 17", bits);
    check_bits("B_BI_BI 16x8", &s, "00001010111111111");

    /* B_DIRECT: only the one-bit flag (direct MV derivation is out of scope). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_B, PHX_B_DIRECT);
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 1, "B_DIRECT return %d != 1", bits);
    check_bits("B_DIRECT flag", &s, "1");

    /* Intra fallback inside a B slice uses offset 23 (cavlc.c:480-481). */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_B, PHX_I_16x16);
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == 10, "B slice I_16x16 return %d != 10", bits);
    check_bits("B slice I16x16 offset23", &s, "0000110011");
}

static void test_skip_and_pcm(void)
{
    phx_bs_t s; uint8_t buf[64]; phx_mb_cache_t cache; phx_cavlc_mb_t mb;
    int run = 0, bits;

    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_P, PHX_P_SKIP);
    CHECK(phx_cavlc_mb_header(&mb) == 0, "P_SKIP header must emit nothing");
    CHECK(phx_cavlc_mb_skip(&s, &run, 1, 0) == 0 && run == 1, "skip accumulate 1");
    CHECK(phx_cavlc_mb_skip(&s, &run, 1, 0) == 0 && run == 2, "skip accumulate 2");
    bits = phx_cavlc_mb_skip(&s, &run, 0, 0);
    CHECK(bits == 3 && run == 0, "skip flush bits=%d run=%d", bits, run);
    check_bits("skip run 2", &s, "011");

    phx_bs_init(&s, buf, 64);
    run = 1;
    bits = phx_cavlc_mb_skip(&s, &run, 0, 1);
    CHECK(bits == 3 && run == 0, "final skip run bits=%d run=%d", bits, run);
    check_bits("final skip run 1", &s, "010");

    /* I_PCM needs the raw-sample path (cavlc.c:511-535): explicitly refused. */
    setup(&mb, &s, buf, &cache, PHX_SLICE_TYPE_I, PHX_I_PCM);
    bits = phx_cavlc_mb_header(&mb);
    CHECK(bits == -1 && phx_bs_pos(&s) == 0, "I_PCM must be refused without writing");
}

/* Worker M10 / row 17 writer half: mb_skip_run from instrumented x264
 * encoder.c:2883-2890 + :3087-3088.  Golden bit strings / bytes:
 *   coded only            ue(0)                -> "1"       80
 *   run 2 then coded      ue(2)                -> "011"     60
 *   mixed                 ue(0)ue(2)ue(0)ue(1) -> 10111010 BA
 *   final run 5           ue(5)                -> "00110"   30
 *   B mixed               ue(1)ue(2)           -> "010011"  4C
 * The `ue(0)` at a coded P/B MB is mandatory (encoder.c:2889); only the
 * slice-end flush at :3087 is conditional.  SHADER: C. */
static void skip_seq(const int *types, int n, phx_bs_t *s, uint8_t *buf)
{
    int run = 0;
    phx_bs_init(s, buf, 64);
    for (int i = 0; i < n; i++)
        phx_cavlc_mb_skip(s, &run, types[i], 0);
    phx_cavlc_mb_skip(s, &run, 0, 1);             /* encoder.c:3087-3088 */
    CHECK(run == 0, "skip_run not reset after slice-end flush");
}

static void test_skip_run_golden(void)
{
    phx_bs_t s; uint8_t buf[64];

    { const int t[] = { 0 };
      skip_seq(t, 1, &s, buf);
      CHECK(phx_bs_pos(&s) == 1 && buf[0] == 0x80,
            "coded_only: pos=%d byte=%02x expected 1/80", phx_bs_pos(&s), buf[0]);
      check_bits("golden ue(0) at coded MB", &s, "1"); }

    { const int t[] = { 1, 1, 0 };
      skip_seq(t, 3, &s, buf);
      CHECK(phx_bs_pos(&s) == 3 && buf[0] == 0x60,
            "run2_then_coded: pos=%d byte=%02x expected 3/60", phx_bs_pos(&s), buf[0]);
      check_bits("golden run2 then coded", &s, "011"); }

    { const int t[] = { 0, 1, 1, 0, 0, 1 };
      skip_seq(t, 6, &s, buf);
      CHECK(phx_bs_pos(&s) == 8 && buf[0] == 0xBA,
            "mixed: pos=%d byte=%02x expected 8/BA", phx_bs_pos(&s), buf[0]);
      check_bits("golden mixed skip runs", &s, "10111010"); }

    { const int t[] = { 1, 1, 1, 1, 1 };
      skip_seq(t, 5, &s, buf);
      CHECK(phx_bs_pos(&s) == 5 && buf[0] == 0x30,
            "final_run5: pos=%d byte=%02x expected 5/30", phx_bs_pos(&s), buf[0]);
      check_bits("golden final run5", &s, "00110"); }

    { const int t[] = { 1, 0, 1, 1, 0 };
      skip_seq(t, 5, &s, buf);
      CHECK(phx_bs_pos(&s) == 6 && buf[0] == 0x4C,
            "B_mixed: pos=%d byte=%02x expected 6/4C", phx_bs_pos(&s), buf[0]);
      check_bits("golden B mixed skip runs", &s, "010011"); }
}

/* Instrumented x264 golden: real x264_macroblock_write_cavlc() with a fixed
 * P_L0/D_16x16 MB (cbp=0/0, one list-0 ref, all neighbour MVs zero), preceded
 * by bs_write_ue(0) for the skip run (encoder.c:2889).  Captured bytes:
 *   MV (8,0)   -> C2 18       (16 bits)
 *   MV (8,-4)  -> C2 02 60    (24 bits)
 *   MV (0,-20) -> E0 A6       (16 bits)
 *   MV (0,0)   -> F8          (8 bits)
 * Phoenix's phx_cavlc_mb_skip + phx_cavlc_mb_header + CBP ue reproduces all
 * four exactly: the CAVLC writer is not the source of the frame-21 decoder
 * error.  SHADER: C. */
static void p_l0_golden(const char *name, int mvx, int mvy,
                        int bits, uint8_t b0, uint8_t b1, uint8_t b2,
                        const char *golden)
{
    phx_bs_t s; uint8_t buf[64];
    phx_mb_cache_t cache; phx_cavlc_mb_t mb; int run = 0;
    memset(buf, 0, sizeof(buf));
    flat_cache(&cache);
    for (int i = 0; i < 40; i++) {
        cache.ref[0][i] = 0;
        cache.mv[0][i][0] = 0; cache.mv[0][i][1] = 0;
    }
    cache.mv[0][phx_scan8[0]][0] = (int16_t)mvx;
    cache.mv[0][phx_scan8[0]][1] = (int16_t)mvy;
    phx_bs_init(&s, buf, sizeof(buf));
    phx_cavlc_mb_init(&mb, &s, &cache);
    mb.slice_type = PHX_SLICE_TYPE_P;
    mb.i_type = PHX_P_L0;
    mb.i_partition = PHX_D_16x16;
    mb.i_cbp_luma = mb.i_cbp_chroma = 0;
    mb.i_fref[0] = 1; mb.i_fref[1] = 0;
    mb.chroma = 1;
    phx_cavlc_mb_skip(&s, &run, 0, 0);          /* encoder.c:2889 */
    phx_cavlc_mb_header(&mb);                   /* cavlc.c:340-349 */
    phx_bs_write_ue(&s, phx_cavlc_cbp_to_golomb[1][0][0]); /* cavlc.c:548 */
    phx_bs_align_0(&s);
    CHECK(phx_bs_pos(&s) == bits, "%s: pos=%d expected %d", name, phx_bs_pos(&s), bits);
    CHECK(buf[0] == b0 && buf[1] == b1 && buf[2] == b2,
          "%s: bytes %02X %02X %02X", name, buf[0], buf[1], buf[2]);
    check_bits(name, &s, golden);
}

static void test_p_l0_golden(void)
{
    p_l0_golden("golden P_L0 mv(8,0)",    8,  0, 16, 0xC2,0x18,0x00,
                "1100001000011000");
    p_l0_golden("golden P_L0 mv(8,-4)",   8, -4, 24, 0xC2,0x02,0x60,
                "110000100000001001100000");
    p_l0_golden("golden P_L0 mv(0,-20)",  0,-20, 16, 0xE0,0xA6,0x00,
                "1110000010100110");
    p_l0_golden("golden P_L0 mv(0,0)",    0,  0,  8, 0xF8,0x00,0x00,
                "11111000");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("phx_cavlc_syntax golden tests (source: x264 encoder/cavlc.c)\n");
    test_tables();
    test_predict_mv();
    test_mvd();
    test_header_p();
    test_header_i();
    test_header_b();
    test_skip_and_pcm();
    test_skip_run_golden();
    test_p_l0_golden();
    printf("%s: %d checks, %d failed\n", n_failed ? "FAIL" : "PASS", n_tests, n_failed);
    return n_failed ? 1 : 0;
}
