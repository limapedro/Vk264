/* Byte-exact I16 luma DC reconstruction golden, produced by x264's OWN
 * scalar C objects (build/x264_ref/obj/common_dct.obj + common_quant.obj).
 *
 * Harness (temporary, outside the repo):
 *   x264_dct_init(0,&dctf); x264_quant_init(NULL,0,&qf);  // cpu=0 => C path
 *   dctf.idct4x4dc(d); qf.dequant_4x4_dc(d,dmf,25);
 *   dctf.add16x16_idct_dc(dst32,d);                      // dct.c:462-471
 * with dst32 = 128 on all 256 samples (x264's I_PRED_16x16_DC_128, mode 6).
 *
 * Inputs are the exact quantized luma DC coefficients of frame 0 / MB0 in
 * build/oracle_full/diag.yuv (64x64, 30 frames), encoded with
 *   x264 --preset ultrafast --qp 28 --cabac --input-res 64x64
 * so the MB QP is 25 (slice_qp_delta = -3).  An instrumented
 * encoder/macroblock.c:219/227/228/238 dump gives:
 *   dc=102, nnz=1 (DC block), block_cbp=15, pred16=6
 *   after_quant : 102 2 -1 1 5 -3 1 -2 -4 2 0 1 4 -2 0 -1
 *   after_idct  : 105 105 105 101 107 107 107 95 107 107 107 87 109 109 101 73
 *   after_dequant: 4620 ... (below)
 * The bottom row of the resulting 16x16 (203,203,...) is the reconstructed
 * top border seen by MB4's I16 search; x264 picks mode 0 (V) there.
 *
 * Citations: reference/x264/common/dct.c:78-107 (idct4x4dc),
 *            common/dct.c:442-452 (add4x4_idct_dc),
 *            common/dct.c:462-471 (add16x16_idct_dc),
 *            common/quant.c:148-165 (dequant_4x4_dc),
 *            common/set.c:31-48 (dequant4_scale),
 *            common/set.c:146-160 (dequant4_mf = scale * 16).
 * SHADER: A — one invocation per 4x4 block; per-sample clip is one lane.
 */
#include "phx_dct.h"
#include "phx_quant.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

/* x264 macroblock.c:219 dump — quantized I16 luma DC, natural order. */
static const phx_dctcoef in_quant_dc[16] = {
     102,   2,  -1,   1,   5,  -3,   1,  -2,
      -4,   2,   0,   1,   4,  -2,   0,  -1,
};

/* x264 dct.c:78-107 idct4x4dc of in_quant_dc. */
static const phx_dctcoef exp_idct_dc[16] = {
     105, 105, 105, 101, 107, 107, 107,  95,
     107, 107, 107,  87, 109, 109, 101,  73,
};

/* x264 quant.c:148-165 dequant_4x4_dc at QP25, CQM_4IY.
 * i_qbits = 25/6 - 6 = -2, i_dmf = dequant4_mf[1][0] = 11*16 = 176,
 * f = 2: dct[i] = (dct[i]*176 + 2) >> 2. */
static const phx_dctcoef exp_dequant_dc[16] = {
    4620, 4620, 4620, 4444, 4708, 4708, 4708, 4180,
    4708, 4708, 4708, 3828, 4796, 4796, 4444, 3212,
};

/* x264 dct.c:462-471 add16x16_idct_dc on an all-128 pred plane, FDEC_STRIDE=32.
 * 256 bytes, row-major.  Each 4x4 sub-block dc = (dct + 32) >> 6 then clipped
 * by common.h:145-148.  Bottom row x=0..11 is 203 (MB4's top border). */
static const uint8_t exp_recon16[256] = {
    200,200,200,200,200,200,200,200,200,200,200,200,197,197,197,197,
    200,200,200,200,200,200,200,200,200,200,200,200,197,197,197,197,
    200,200,200,200,200,200,200,200,200,200,200,200,197,197,197,197,
    200,200,200,200,200,200,200,200,200,200,200,200,197,197,197,197,
    202,202,202,202,202,202,202,202,202,202,202,202,193,193,193,193,
    202,202,202,202,202,202,202,202,202,202,202,202,193,193,193,193,
    202,202,202,202,202,202,202,202,202,202,202,202,193,193,193,193,
    202,202,202,202,202,202,202,202,202,202,202,202,193,193,193,193,
    202,202,202,202,202,202,202,202,202,202,202,202,188,188,188,188,
    202,202,202,202,202,202,202,202,202,202,202,202,188,188,188,188,
    202,202,202,202,202,202,202,202,202,202,202,202,188,188,188,188,
    202,202,202,202,202,202,202,202,202,202,202,202,188,188,188,188,
    203,203,203,203,203,203,203,203,197,197,197,197,178,178,178,178,
    203,203,203,203,203,203,203,203,197,197,197,197,178,178,178,178,
    203,203,203,203,203,203,203,203,197,197,197,197,178,178,178,178,
    203,203,203,203,203,203,203,203,197,197,197,197,178,178,178,178,
};

static void check_i(const char *name, const phx_dctcoef *got,
                    const phx_dctcoef *exp, int n)
{
    for (int i = 0; i < n; i++)
        if (got[i] != exp[i]) {
            printf("FAIL %s at %d: got %d exp %d\n", name, i, got[i], exp[i]);
            ++fails; return;
        }
}

static void check_u(const char *name, const uint8_t *got,
                    const uint8_t *exp, int n)
{
    for (int i = 0; i < n; i++)
        if (got[i] != exp[i]) {
            printf("FAIL %s at %d (y=%d,x=%d): got %u exp %u\n",
                   name, i, i/16, i%16, got[i], exp[i]);
            ++fails; return;
        }
}

int main(void)
{
    phx_dctcoef d[16];
    uint8_t dst[PHX_FDEC_STRIDE * 16];
    phx_quant_tables_t t;

    phx_quant_tables_init(&t);
    /* set.c:31-48 / :146-160, QP25 (qp%6=1) CQM_4IY DC. */
    CHECK(t.dequant4_mf[0][25 % 6][0] == 176, "dequant4_mf CQM_4IY QP25 DC");

    memcpy(d, in_quant_dc, sizeof(d));
    phx_idct4x4dc(d);
    check_i("idct4x4dc fixture", d, exp_idct_dc, 16);

    phx_dequant_4x4_dc(d, t.dequant4_mf[0], 25);
    check_i("dequant_4x4_dc fixture", d, exp_dequant_dc, 16);

    memset(dst, 128, sizeof(dst));
    phx_add16x16_idct_dc(dst, PHX_FDEC_STRIDE, d);
    /* Golden is row-major 16-wide; dst is FDEC_STRIDE-wide. */
    {
        uint8_t packed[256];
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                packed[y * 16 + x] = dst[y * PHX_FDEC_STRIDE + x];
        check_u("add16x16_idct_dc full 16x16", packed, exp_recon16, 256);
    }
    CHECK(dst[15 * PHX_FDEC_STRIDE + 0] == 203 &&
          dst[15 * PHX_FDEC_STRIDE + 1] == 203,
          "MB4 reconstructed top border 203,203");

    printf("%s: I16 DC golden %d/256 bytes, %d failures\n",
           fails ? "FAILED" : "OK", 256 - fails, fails);
    return fails ? 1 : 0;
}
