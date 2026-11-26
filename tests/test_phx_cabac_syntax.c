/* Golden test for phx_cabac_syntax.{h,c}.
 *
 * Expected bytes were produced by the authoritative x264 C implementation
 * with every context state forced to 0x40 and x264_cabac_encode_flush(h,cb)
 * appended (h->i_frame = 0). Golden harness (temporary, outside the repo)
 * #includes reference/x264/encoder/cabac.c so the *static* syntax writers are
 * reachable, and links the prebuilt C-only reference objects:
 *
 *   cl -nologo -O2 /DBIT_DEPTH=8 "/Dx264_template(w)=x264_glue3(x264,8,w)" \
 *      /D__STDC_CONSTANT_MACROS /DHAVE_CONFIG_H \
 *      /I reference/x264 /I reference/x264/common /I reference/x264/encoder \
 *      -c golden.c /Fo golden.obj
 *   rem  golden_objs.rsp = build/x264_ref/objs.rsp filtered to common_*.obj
 *   link -nologo /OUT:golden.exe @golden_objs.rsp golden.obj
 *
 * Golden harness zero-initialises x264_t, sets x264_quant_init(h,0,&h->quantf)
 * plus common/quant.c:878-882 aliases (4:2:0 chroma DC = coeff_last4), and
 * calls the x264 writer under test, then x264_cabac_encode_flush().
 *
 * SHADER: C — standalone main, returns 0 on pass, 1 on any mismatch.
 */
#include "phx_cabac_syntax.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

static uint8_t buf[256];
static phx_mb_cache_t cache;
static phx_cabac_mb_t mb;

static void setup(phx_cabac_t *cb, int slice_type, int qp)
{
    memset(buf, 0, sizeof(buf));
    memset(&cache, 0, sizeof(cache));
    cache.i_cbp_left = -1;
    cache.i_cbp_top = -1;
    for (int i = 0; i < 40; i++) cache.intra4x4_pred_mode[i] = 0;

    phx_cabac_encode_init(cb, buf, buf + sizeof(buf));
    for (int i = 0; i < 1024; i++) cb->state[i] = 0x40;

    phx_cabac_mb_init(&mb, &cache);
    mb.slice_type = slice_type;
    mb.i_frame = 0;
    mb.i_qp = qp;
    mb.i_last_qp = qp;
    mb.b_transform_8x8_mode = 0;
    cache.b_transform_8x8 = 0;
}

static void check(const char *name, phx_cabac_t *cb,
                  const uint8_t *expected, int n)
{
    phx_cabac_encode_flush(cb, 0);                 /* x264 h->i_frame = 0 */
    int got = (int)(cb->p - buf);
    CHECK(got == n, "byte count");
    if (got != n) {
        printf("  %-28s got %d, expected %d\n", name, got, n);
        return;
    }
    for (int i = 0; i < n; i++)
        if (buf[i] != expected[i]) {
            printf("  %-28s byte %d: got %02X expected %02X\n",
                   name, i, buf[i], expected[i]);
            ++fails;
            return;
        }
    printf("OK   %-28s %d bytes\n", name, n);
}

int main(void)
{
    phx_cabac_t cb;

    /* I-slice intra16x16, cbp_luma=3, cbp_chroma=2, pred=1, left=I_4x4. */
    setup(&cb, PHX_SLICE_I, 26);
    mb.i_type = PHX_I_16x16;
    mb.i_intra16x16_pred_mode = 1;
    cache.i_cbp_luma = 3;
    cache.i_cbp_chroma = 2;
    cache.i_neighbour = PHX_MB_LEFT;
    cache.type_left[0] = PHX_I_4x4;
    phx_cabac_mb_header_i(&cb, &mb, PHX_I_16x16, PHX_SLICE_I, 1);
    check("mb_header_i_I16", &cb, (const uint8_t[]){0xFE,0xDF,0x4D,0x41}, 4);

    /* I_4x4 with left/top non-I4x4 contexts; intra4x4 predictors default 0. */
    setup(&cb, PHX_SLICE_I, 26);
    mb.i_type = PHX_I_4x4;
    cache.i_neighbour = PHX_MB_LEFT | PHX_MB_TOP;
    cache.type_left[0] = PHX_I_16x16;
    cache.type_top = PHX_I_16x16;
    phx_cabac_mb_header_i(&cb, &mb, PHX_I_4x4, PHX_SLICE_I, 1);
    check("mb_header_i_I4", &cb, (const uint8_t[]){0xE6,0xFF,0xFC,0x3B,0x21}, 5);

    /* Regression (worker L1): I_4x4 with PPS transform_8x8_mode=1 must emit
     * transform_size_8x8_flag exactly once, from inside mb_header_i, at the
     * x264 cabac.c:419-420 position.  Golden bytes come from a harness that
     * #includes reference/x264/encoder/cabac.c and calls cabac_mb_header_i()
     * with the same state/context setup; phx_encoder.c must NOT emit the flag
     * a second time after cbp_luma/chroma (x264_transform_allowed[I_4x4]==0,
     * common/macroblock.h:443-460).  SHADER: C. */
    setup(&cb, PHX_SLICE_I, 26);
    mb.i_type = PHX_I_4x4;
    mb.b_transform_8x8_mode = 1;
    cache.b_transform_8x8 = 0;
    for (int i = 0; i < 16; i++)
        cache.intra4x4_pred_mode[phx_scan8[i]] = 2;  /* FIX4(2) == DC == pred */
    phx_cabac_mb_header_i(&cb, &mb, PHX_I_4x4, PHX_SLICE_I, 1);
    check("mb_header_i_I4_t8x8_once", &cb,
          (const uint8_t[]){0xB4,0x24,0x8D,0xD1,0xF0,0x8F,0x85}, 7);

    /* P-slice intra suffix. */
    setup(&cb, PHX_SLICE_P, 26);
    mb.i_type = PHX_I_16x16;
    mb.i_intra16x16_pred_mode = 2;
    cache.i_cbp_luma = 1;
    cache.i_cbp_chroma = 1;
    phx_cabac_mb_header_p(&cb, &mb, PHX_I_16x16, 1);
    check("mb_header_p_I16", &cb, (const uint8_t[]){0xFE,0xFA,0xC4,0x41}, 4);

    /* P_L0 16x16, two refs, non-zero MVD (tests prefix/ref/mvd trees). */
    setup(&cb, PHX_SLICE_P, 26);
    mb.i_type = PHX_P_L0;
    mb.i_partition = PHX_D_16x16;
    mb.i_fref[0] = 2;
    cache.ref[0][phx_scan8[0]] = 1;
    cache.ref[0][phx_scan8[0] - 1] = 1;
    cache.ref[0][phx_scan8[0] - 8] = 0;
    cache.mv[0][phx_scan8[0] - 1][0] = 2;
    cache.mv[0][phx_scan8[0]][0] = 4;
    phx_cabac_mb_header_p(&cb, &mb, PHX_P_L0, 1);
    check("mb_header_p_L0_16x16", &cb, (const uint8_t[]){0xB8,0x97,0x71}, 3);

    /* B_DIRECT with sibling contexts. */
    setup(&cb, PHX_SLICE_B, 26);
    mb.i_type = PHX_B_DIRECT;
    cache.i_neighbour = PHX_MB_LEFT | PHX_MB_TOP;
    cache.type_left[0] = PHX_B_L0_L1;
    cache.type_top = PHX_B_SKIP;
    phx_cabac_mb_header_b(&cb, &mb, PHX_B_DIRECT, 1);
    check("mb_header_b_direct", &cb, (const uint8_t[]){0xE6,0x81}, 2);

    /* B_L1_L1 16x8. */
    setup(&cb, PHX_SLICE_B, 26);
    mb.i_type = PHX_B_L1_L1;
    mb.i_partition = PHX_D_16x8;
    mb.i_fref[1] = 1;
    cache.mv[1][phx_scan8[0]][0] = 1;
    cache.mv[1][phx_scan8[8]][0] = -1;
    phx_cabac_mb_header_b(&cb, &mb, PHX_B_L1_L1, 1);
    check("mb_header_b_L1L1", &cb, (const uint8_t[]){0xFE,0x7F,0x8B,0x85}, 4);

    /* B_8x8, all four sub-partitions direct. */
    setup(&cb, PHX_SLICE_B, 26);
    mb.i_type = PHX_B_8x8;
    mb.i_partition = PHX_D_8x8;
    for (int i = 0; i < 4; i++) mb.i_sub_partition[i] = PHX_D_DIRECT_8x8;
    phx_cabac_mb_header_b(&cb, &mb, PHX_B_8x8, 1);
    check("mb_header_b_8x8", &cb, (const uint8_t[]){0xFE,0xFF,0xEF,0xA1}, 4);

    /* cbp_luma with left=6, top=9, current=9. */
    setup(&cb, PHX_SLICE_P, 26);
    cache.i_cbp_luma = 9;
    cache.i_cbp_left = 0x6;
    cache.i_cbp_top = 0x9;
    phx_cabac_cbp_luma(&cb, &mb);
    check("cbp_luma", &cb, (const uint8_t[]){0xFA,0x3F}, 2);

    /* cbp_chroma=2 with left Cb=1, top Cr=1. */
    setup(&cb, PHX_SLICE_P, 26);
    cache.i_cbp_chroma = 2;
    cache.i_cbp_left = 0x20;
    cache.i_cbp_top = 0x10;
    phx_cabac_cbp_chroma(&cb, &mb);
    check("cbp_chroma", &cb, (const uint8_t[]){0xFE,0xFF}, 2);

    /* transform_size_8x8_flag with neighbour context 2. */
    setup(&cb, PHX_SLICE_P, 26);
    cache.b_transform_8x8 = 1;
    cache.i_neighbour_transform_size = 2;
    phx_cabac_transform_size(&cb, &mb);
    check("transform_size", &cb, (const uint8_t[]){0xFE,0xF1}, 2);

    /* mb_qp_delta = +3 (non-zero last dqp, previous CBP present). */
    setup(&cb, PHX_SLICE_P, 26);
    mb.i_type = PHX_P_L0;
    mb.i_qp = 29;
    mb.i_last_qp = 26;
    mb.i_last_dqp = 3;
    mb.prev_cbp = 3;
    phx_cabac_qp_delta(&cb, &mb);
    check("qp_delta_pos", &cb, (const uint8_t[]){0xFE,0xFF,0x7B}, 3);

    /* mb_qp_delta = -3. */
    setup(&cb, PHX_SLICE_P, 26);
    mb.i_type = PHX_P_L0;
    mb.i_qp = 23;
    mb.i_last_qp = 26;
    phx_cabac_qp_delta(&cb, &mb);
    check("qp_delta_neg", &cb, (const uint8_t[]){0xFE,0xFF,0xD9,0x41}, 4);

    /* Coefficient bins, DCT_LUMA_4x4 with levels 1,-2,3,16. */
    {
        static const phx_dctcoef_t d[16] = {
            1, 0, -2, 0, 0, 3, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0
        };
        setup(&cb, PHX_SLICE_P, 26);
        cache.non_zero_count[phx_scan8[0]] = 4;
        cache.non_zero_count[phx_scan8[0] - 1] = 2;
        cache.non_zero_count[phx_scan8[0] - 8] = 1;
        phx_cabac_block_residual(&cb, &mb, PHX_DCT_LUMA_4x4, d, 0);
        check("block_residual_4x4", &cb,
              (const uint8_t[]){0xF9,0xB7,0xCF,0xFF,0xFF,0xE8,0xFE,0x32,0x99}, 9);
    }

    /* 8x8 coefficients go through the 8x8 sig/last offset tables. */
    {
        phx_dctcoef_t d[64];
        memset(d, 0, sizeof(d));
        d[0] = 2; d[7] = -1; d[31] = 5; d[63] = 1;
        setup(&cb, PHX_SLICE_P, 26);
        phx_cabac_block_residual(&cb, &mb, PHX_DCT_LUMA_8x8, d, 0);
        check("block_residual_8x8", &cb,
              (const uint8_t[]){0xF1,0x71,0x57,0x7F,0x9B,0x29}, 6);
    }

    /* CBF bin + residual wrapper (4x4 luma). */
    {
        static const phx_dctcoef_t d[16] = { 1, 0, 0, 4 };
        setup(&cb, PHX_SLICE_P, 26);
        cache.non_zero_count[phx_scan8[0]] = 2;
        cache.non_zero_count[phx_scan8[0] - 1] = 1;
        cache.non_zero_count[phx_scan8[0] - 8] = 0;
        phx_cabac_block_residual_cbf(&cb, &mb, PHX_DCT_LUMA_4x4, 0, d, 0, 0, 0);
        check("block_residual_cbf", &cb,
              (const uint8_t[]){0xFE,0x45,0xFE,0x3C,0xC1}, 5);
    }

    /* Chroma DC CBF + residual (b_intra=1, b_dc=1). */
    {
        phx_dctcoef_t d[4] = { 3, 0, 0, -1 };
        setup(&cb, PHX_SLICE_I, 26);
        cache.non_zero_count[phx_scan8[PHX_CHROMA_DC]] = 2;
        phx_cabac_block_residual_cbf(&cb, &mb, PHX_DCT_CHROMA_DC,
                                     PHX_CHROMA_DC, d, 1, 1, 0);
        check("block_residual_chroma_dc", &cb,
              (const uint8_t[]){0xFE,0x14,0xBE,0x81}, 4);
    }

    /* Regression (worker M10): chroma-DC coeff_abs_level_minus1 > 1.  The
     * vectors below are byte-for-byte output of the instrumented x264 golden
     * harness: it #includes reference/x264/encoder/cabac.c, zero-initialises
     * x264_t, calls x264_quant_init() plus quant.c:878-882 /
     * encoder.c:1446 coeff_last aliases, sets all 1024 cb.state[] = 0x40,
     * invokes cabac_block_residual_internal() with the literal arrays below
     * and appends x264_cabac_encode_flush(h, &cb).
     *
     *   {13,-7,0,0} is the exact x264 chroma-DC pair at ultrafast bin 932;
     *   {9,-5,0,0} is what Phoenix's upstream trellis currently hands to
     *   this writer.  Both must produce x264's bins: the writer is
     *   coefficient-agnostic and the bin-932 divergence is upstream
     *   (phx_rdo.c chroma-DC QNS), see docs/preset_audit/medium.md:149-166.
     *   The mixed vector walks node_ctx 0/4/5/6 through the levelgt1 chain
     *   (cabac.c:650-662, :672).  SHADER: C. */
    {
        static const phx_dctcoef_t d[16] = {
            13, -7, 0, 0, 0, 0, 0, 0, 13, 0, -7, 0, 0, 0, 0, 0
        };
        setup(&cb, PHX_SLICE_P, 26);
        phx_cabac_block_residual(&cb, &mb, PHX_DCT_CHROMA_DC, d, 0);
        check("block_residual_chroma_dc_gt1", &cb,
              (const uint8_t[]){0xFC,0x6F,0xFF,0xE9,0x7F,0xFF,0xE7,0xF9}, 8);
    }
    {
        static const phx_dctcoef_t d[16] = {
            9, -5, 0, 0, 0, 0, 0, 0, 9, 0, -5, 0, 0, 0, 0, 0
        };
        setup(&cb, PHX_SLICE_P, 26);
        phx_cabac_block_residual(&cb, &mb, PHX_DCT_CHROMA_DC, d, 0);
        check("block_residual_chroma_dc_gt1_phxcoef", &cb,
              (const uint8_t[]){0xFC,0x6F,0xFE,0xF7,0xFF,0xD5,0xD1}, 7);
    }
    {
        static const phx_dctcoef_t d[16] = {
            7, -13, 2, 19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        };
        setup(&cb, PHX_SLICE_P, 26);
        phx_cabac_block_residual(&cb, &mb, PHX_DCT_CHROMA_DC, d, 0);
        check("block_residual_chroma_dc_nodectx", &cb,
              (const uint8_t[]){0xFC,0x30,0xDF,0xFF,0xF6,0x5D,0xFF,0xFF,
                                0xFE,0xFF,0xFF,0x38,0xC1}, 13);
    }
    /* quant.c:878-879 aliases CHROMAU_DC/V_DC to coeff_last16 (4:4:4 path);
     * a nonzero at index 15 must be scanned.  Golden from the same
     * instrumented x264 harness with ctx_block_cat = DCT_CHROMAU_DC.
     * SHADER: C. */
    {
        static const phx_dctcoef_t d[16] = {
            1, 0, 2, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 5
        };
        setup(&cb, PHX_SLICE_P, 26);
        phx_cabac_block_residual(&cb, &mb, PHX_DCT_CHROMAU_DC, d, 0);
        check("block_residual_chromau_dc_16", &cb,
              (const uint8_t[]){0xF9,0xB6,0xC6,0xF2,0x5D,0xCB,0xB4,0xD1}, 8);
    }

    /* mb_skip_flag: P and B use different context bases (11 vs 24). */
    setup(&cb, PHX_SLICE_P, 26);
    cache.i_neighbour_skip = 2;
    phx_cabac_mb_skip(&cb, &mb, 1);
    check("mb_skip_P", &cb, (const uint8_t[]){0xFE,0xF1}, 2);
    setup(&cb, PHX_SLICE_B, 26);
    cache.i_neighbour_skip = 1;
    phx_cabac_mb_skip(&cb, &mb, 0);
    check("mb_skip_B", &cb, (const uint8_t[]){0xE6,0x81}, 2);

    /* Direct MVD component trees: |mvd| > 9 (ue_bypass) and negative sign. */
    setup(&cb, PHX_SLICE_P, 26);
    phx_cabac_mvd_cpn(&cb, &mb, 0, 0, 0, 17, 3);
    check("mvd_cpn_gt9", &cb, (const uint8_t[]){0xFE,0xFF,0xFF,0xB4,0x33}, 5);
    setup(&cb, PHX_SLICE_P, 26);
    phx_cabac_mvd_cpn(&cb, &mb, 0, 0, 1, -7, 5);
    check("mvd_cpn_neg", &cb, (const uint8_t[]){0xFE,0xFF,0xFD,0xEF}, 4);

    /* Reference-index tree with three active refs. */
    setup(&cb, PHX_SLICE_P, 26);
    cache.ref[0][phx_scan8[0]] = 3;
    cache.ref[0][phx_scan8[0] - 1] = 1;
    cache.ref[0][phx_scan8[0] - 8] = 2;
    phx_cabac_ref(&cb, &mb, 0, 0);
    check("ref_p", &cb, (const uint8_t[]){0xFE,0xF5,0xA1}, 3);

    /* Intra4x4 prediction-mode bins. */
    setup(&cb, PHX_SLICE_I, 26);
    phx_cabac_intra4x4_pred_mode(&cb, 2, 6);
    check("intra4x4_pred", &cb, (const uint8_t[]){0xE3,0x4F}, 2);
    setup(&cb, PHX_SLICE_I, 26);
    phx_cabac_intra4x4_pred_mode(&cb, 2, 2);
    check("intra4x4_pred_eq", &cb, (const uint8_t[]){0xFE,0xF1}, 2);

    /* I_PCM: mb_type_intra() already flushes; do not flush again. */
    setup(&cb, PHX_SLICE_I, 26);
    phx_cabac_mb_type_intra(&cb, &mb, PHX_I_PCM, 3, 6, 7, 8, 9, 10);
    {
        static const uint8_t pcm[] = { 0xFE, 0xF1 };
        int got = (int)(cb.p - buf);
        CHECK(got == 2, "ipcm byte count");
        for (int i = 0; i < got && i < 2; i++)
            CHECK(buf[i] == pcm[i], "ipcm bytes");
        if (got == 2) printf("OK   %-28s 2 bytes\n", "mb_type_ipcm");
    }

    printf("%s: phx_cabac_syntax, %d failures\n",
           fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
