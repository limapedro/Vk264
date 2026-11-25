/* Function-for-function port of x264's scalar common/quant.c, with the
 * flat-CQM quantizer/dequantizer tables built exactly as in common/set.c.
 * Each citation is the exact source line of the vendored x264 reference.
 * SHADER: A for all quantizers/dequantizers: per coefficient, vectorizable.
 */
#include "phx_quant.h"

/* set.c:28 — SHIFT(x,s).
 * SHADER: B — table setup only (see phx_quant_tables_init). */
static int phx_shift(int x, int s)
{
    return s <= 0 ? x << -s : (x + (1 << (s - 1))) >> s;
}

/* set.c:29 — DIV(n,d).
 * SHADER: B — table setup only (see phx_quant_tables_init). */
static int phx_div(int n, int d)
{
    return (n + (d >> 1)) / d;
}

/* set.c:31-48 — dequant4_scale[q][j] and quant4_scale[q][j]. */
static const uint8_t dequant4_scale[6][3] = {
    {10, 13, 16}, {11, 14, 18}, {13, 16, 20},
    {14, 18, 23}, {16, 20, 25}, {18, 23, 29}
};
static const uint16_t quant4_scale[6][3] = {
    {13107, 8066, 5243}, {11916, 7490, 4660}, {10082, 6554, 4194},
    {9362, 5825, 3647}, {8192, 5243, 3355}, {7282, 4559, 2893}
};

/* set.c:50-71 — quant8_scan, dequant8_scale, quant8_scale. */
static const uint8_t quant8_scan[16] = {
    0, 3, 4, 3, 3, 1, 5, 1, 4, 5, 2, 5, 3, 1, 5, 1
};
static const uint8_t dequant8_scale[6][6] = {
    {20, 18, 32, 19, 25, 24}, {22, 19, 35, 21, 28, 26},
    {26, 23, 42, 24, 33, 31}, {28, 25, 45, 26, 35, 33},
    {32, 28, 51, 30, 40, 38}, {36, 32, 58, 34, 46, 43}
};
static const uint16_t quant8_scale[6][6] = {
    {13107, 11428, 20972, 12222, 16777, 15481},
    {11916, 10826, 19174, 11058, 14980, 14290},
    {10082, 8943, 15978, 9675, 12710, 11985},
    {9362, 8228, 14913, 8931, 11984, 11259},
    {8192, 7346, 13159, 7740, 10486, 9777},
    {7282, 6428, 11570, 6830, 9118, 8640}
};

/* set.c:81-83 with base.c:456-457 defaults i_luma_deadzone[0]=21, [1]=11:
 * deadzone = {32-11, 32-21, 32-11, 32-21}. */
static const int phx_deadzone[4] = {21, 11, 21, 11};

/* set.c:73-205 — x264_cqm_init() restricted to the default flat scaling
 * lists (every entry 16, base.c:462-467). Reproduces the exact integer
 * formulas, including rounded SHIFT and truncation to uint16_t.
 * SHADER: B — table construction once per encode, not a per-block kernel. */
void phx_quant_tables_init(phx_quant_tables_t *t)
{
    int def_quant4[6][16], def_dequant4[6][16];
    int def_quant8[6][64], def_dequant8[6][64];
    int quant4_base[4][6][16], quant8_base[2][6][64];

    /* set.c:130-144. */
    for (int q = 0; q < 6; q++) {
        for (int i = 0; i < 16; i++) {
            int j = (i & 1) + ((i >> 2) & 1);
            def_dequant4[q][i] = dequant4_scale[q][j];
            def_quant4[q][i] = quant4_scale[q][j];
        }
        for (int i = 0; i < 64; i++) {
            int j = quant8_scan[((i >> 1) & 12) | (i & 3)];
            def_dequant8[q][i] = dequant8_scale[q][j];
            def_quant8[q][i] = quant8_scale[q][j];
        }
    }

    /* set.c:146-160 with scaling_list[i] == 16. */
    for (int q = 0; q < 6; q++) {
        for (int list = 0; list < 4; list++) {
            for (int i = 0; i < 16; i++) {
                t->dequant4_mf[list][q][i] = def_dequant4[q][i] * 16;
                quant4_base[list][q][i] = phx_div(def_quant4[q][i] * 16, 16);
            }
        }
        for (int list = 0; list < 2; list++) {
            for (int i = 0; i < 64; i++) {
                t->dequant8_mf[list][q][i] = def_dequant8[q][i] * 16;
                quant8_base[list][q][i] = phx_div(def_quant8[q][i] * 16, 16);
            }
        }
    }

    /* set.c:164-205 — quant_mf and deadzone bias for QP 0..51. */
    for (int qp = 0; qp <= PHX_QUANT_QP_MAX_SPEC; qp++) {
        for (int list = 0; list < 4; list++) {
            for (int i = 0; i < 16; i++) {
                int j = phx_shift(quant4_base[list][qp % 6][i], qp / 6 - 1);
                t->quant4_mf[list][qp][i] = (uint16_t)j;
                if (j)
                    t->quant4_bias[list][qp][i] =
                        (uint16_t)(phx_div(phx_deadzone[list] << 10, j) < (1 << 15) / j
                                   ? phx_div(phx_deadzone[list] << 10, j)
                                   : (1 << 15) / j);
            }
        }
        for (int list = 0; list < 2; list++) {
            for (int i = 0; i < 64; i++) {
                int j = phx_shift(quant8_base[list][qp % 6][i], qp / 6);
                t->quant8_mf[list][qp][i] = (uint16_t)j;
                if (j)
                    t->quant8_bias[list][qp][i] =
                        (uint16_t)(phx_div(phx_deadzone[list] << 10, j) < (1 << 15) / j
                                   ? phx_div(phx_deadzone[list] << 10, j)
                                   : (1 << 15) / j);
            }
        }
    }
}

/* quant.c:59-65 — quant_8x8().
 * SHADER: A — one invocation per 8x8 coefficient block. */
int phx_quant_8x8(phx_dctcoef dct[64], const uint16_t mf[64], const uint16_t bias[64])
{
    int nz = 0;
    for (int i = 0; i < 64; i++)
        dct[i] = phx_quant_one(dct[i], mf[i], bias[i], &nz);
    return !!nz;
}

/* quant.c:67-73 — quant_4x4().
 * SHADER: A — one invocation per 4x4 coefficient block. */
int phx_quant_4x4(phx_dctcoef dct[16], const uint16_t mf[16], const uint16_t bias[16])
{
    int nz = 0;
    for (int i = 0; i < 16; i++)
        dct[i] = phx_quant_one(dct[i], mf[i], bias[i], &nz);
    return !!nz;
}

/* quant.c:75-86 — quant_4x4x4(), returning a 4-bit nz map.
 * SHADER: A — one invocation per four 4x4 blocks. */
int phx_quant_4x4x4(phx_dctcoef dct[4][16], const uint16_t mf[16], const uint16_t bias[16])
{
    int nza = 0;
    for (int j = 0; j < 4; j++) {
        int nz = 0;
        for (int i = 0; i < 16; i++)
            dct[j][i] = phx_quant_one(dct[j][i], mf[i], bias[i], &nz);
        nza |= (!!nz) << j;
    }
    return nza;
}

/* quant.c:88-94 — quant_4x4_dc() (one mf/bias for the whole block).
 * SHADER: A — one invocation per DC-only 4x4 block. */
int phx_quant_4x4_dc(phx_dctcoef dct[16], int mf, int bias)
{
    int nz = 0;
    for (int i = 0; i < 16; i++)
        dct[i] = phx_quant_one(dct[i], (uint16_t)mf, (uint16_t)bias, &nz);
    return !!nz;
}

/* quant.c:96-104 — quant_2x2_dc().
 * SHADER: A — one invocation per 2x2 chroma DC block. */
int phx_quant_2x2_dc(phx_dctcoef dct[4], int mf, int bias)
{
    int nz = 0;
    for (int i = 0; i < 4; i++)
        dct[i] = phx_quant_one(dct[i], (uint16_t)mf, (uint16_t)bias, &nz);
    return !!nz;
}

/* quant.c:106-110 — DEQUANT_SHL / DEQUANT_SHR macros. */
#define PHX_DEQUANT_SHL(x) dct[x] = (phx_dctcoef)((dct[x] * dequant_mf[i_mf][x]) * (1 << i_qbits))
#define PHX_DEQUANT_SHR(x) dct[x] = (phx_dctcoef)((dct[x] * dequant_mf[i_mf][x] + f) >> (-i_qbits))

/* quant.c:112-128 — dequant_4x4().
 * SHADER: A — per coefficient. */
void phx_dequant_4x4(phx_dctcoef dct[16], const int dequant_mf[6][16], int i_qp)
{
    const int i_mf = i_qp % 6;
    const int i_qbits = i_qp / 6 - 4;
    if (i_qbits >= 0) {
        for (int i = 0; i < 16; i++) PHX_DEQUANT_SHL(i);
    } else {
        const int f = 1 << (-i_qbits - 1);
        for (int i = 0; i < 16; i++) PHX_DEQUANT_SHR(i);
    }
}

/* quant.c:130-146 — dequant_8x8().
 * SHADER: A — per coefficient. */
void phx_dequant_8x8(phx_dctcoef dct[64], const int dequant_mf[6][64], int i_qp)
{
    const int i_mf = i_qp % 6;
    const int i_qbits = i_qp / 6 - 6;
    if (i_qbits >= 0) {
        for (int i = 0; i < 64; i++) PHX_DEQUANT_SHL(i);
    } else {
        const int f = 1 << (-i_qbits - 1);
        for (int i = 0; i < 64; i++) PHX_DEQUANT_SHR(i);
    }
}

/* quant.c:148-165 — dequant_4x4_dc().
 * SHADER: A — per coefficient. */
void phx_dequant_4x4_dc(phx_dctcoef dct[16], const int dequant_mf[6][16], int i_qp)
{
    const int i_qbits = i_qp / 6 - 6;
    if (i_qbits >= 0) {
        const int i_dmf = dequant_mf[i_qp % 6][0] << i_qbits;
        for (int i = 0; i < 16; i++)
            dct[i] = (phx_dctcoef)(dct[i] * i_dmf);
    } else {
        const int i_dmf = dequant_mf[i_qp % 6][0];
        const int f = 1 << (-i_qbits - 1);
        for (int i = 0; i < 16; i++)
            dct[i] = (phx_dctcoef)((dct[i] * i_dmf + f) >> (-i_qbits));
    }
}

/* quant.c:167-183 — IDCT_DEQUANT_2X4_START macro. */
#define PHX_IDCT_DEQUANT_2X4_START \
    int a0 = dct[0] + dct[1]; \
    int a1 = dct[2] + dct[3]; \
    int a2 = dct[4] + dct[5]; \
    int a3 = dct[6] + dct[7]; \
    int a4 = dct[0] - dct[1]; \
    int a5 = dct[2] - dct[3]; \
    int a6 = dct[4] - dct[5]; \
    int a7 = dct[6] - dct[7]; \
    int b0 = a0 + a1; \
    int b1 = a2 + a3; \
    int b2 = a4 + a5; \
    int b3 = a6 + a7; \
    int b4 = a0 - a1; \
    int b5 = a2 - a3; \
    int b6 = a4 - a5; \
    int b7 = a6 - a7;\

/* quant.c:185-197 — idct_dequant_2x4_dc().
 * SHADER: A — one invocation per chroma DC block. */
void phx_idct_dequant_2x4_dc(phx_dctcoef dct[8], phx_dctcoef dct4x4[8][16],
                             const int dequant_mf[6][16], int i_qp)
{
    PHX_IDCT_DEQUANT_2X4_START
    int dmf = dequant_mf[i_qp % 6][0] << (i_qp / 6);
    dct4x4[0][0] = (phx_dctcoef)(((b0 + b1) * dmf + 32) >> 6);
    dct4x4[1][0] = (phx_dctcoef)(((b2 + b3) * dmf + 32) >> 6);
    dct4x4[2][0] = (phx_dctcoef)(((b0 - b1) * dmf + 32) >> 6);
    dct4x4[3][0] = (phx_dctcoef)(((b2 - b3) * dmf + 32) >> 6);
    dct4x4[4][0] = (phx_dctcoef)(((b4 - b5) * dmf + 32) >> 6);
    dct4x4[5][0] = (phx_dctcoef)(((b6 - b7) * dmf + 32) >> 6);
    dct4x4[6][0] = (phx_dctcoef)(((b4 + b5) * dmf + 32) >> 6);
    dct4x4[7][0] = (phx_dctcoef)(((b6 + b7) * dmf + 32) >> 6);
}

/* quant.c:199-211 — idct_dequant_2x4_dconly().
 * SHADER: A — one invocation per chroma DC block. */
void phx_idct_dequant_2x4_dconly(phx_dctcoef dct[8],
                                 const int dequant_mf[6][16], int i_qp)
{
    PHX_IDCT_DEQUANT_2X4_START
    int dmf = dequant_mf[i_qp % 6][0] << (i_qp / 6);
    dct[0] = (phx_dctcoef)(((b0 + b1) * dmf + 32) >> 6);
    dct[1] = (phx_dctcoef)(((b2 + b3) * dmf + 32) >> 6);
    dct[2] = (phx_dctcoef)(((b0 - b1) * dmf + 32) >> 6);
    dct[3] = (phx_dctcoef)(((b2 - b3) * dmf + 32) >> 6);
    dct[4] = (phx_dctcoef)(((b4 - b5) * dmf + 32) >> 6);
    dct[5] = (phx_dctcoef)(((b6 - b7) * dmf + 32) >> 6);
    dct[6] = (phx_dctcoef)(((b4 + b5) * dmf + 32) >> 6);
    dct[7] = (phx_dctcoef)(((b6 + b7) * dmf + 32) >> 6);
}

/* quant.c:213-224 — optimize_chroma_idct_dequant_2x4().
 * SHADER: A — one invocation per chroma DC block. */
void phx_optimize_chroma_idct_dequant_2x4(phx_dctcoef out[8],
                                          const phx_dctcoef dct[8], int dmf)
{
    PHX_IDCT_DEQUANT_2X4_START
    out[0] = (phx_dctcoef)(((b0 + b1) * dmf + 2080) >> 6); /* 2080 = 32 + (32<<6) */
    out[1] = (phx_dctcoef)(((b2 + b3) * dmf + 2080) >> 6);
    out[2] = (phx_dctcoef)(((b0 - b1) * dmf + 2080) >> 6);
    out[3] = (phx_dctcoef)(((b2 - b3) * dmf + 2080) >> 6);
    out[4] = (phx_dctcoef)(((b4 - b5) * dmf + 2080) >> 6);
    out[5] = (phx_dctcoef)(((b6 - b7) * dmf + 2080) >> 6);
    out[6] = (phx_dctcoef)(((b4 + b5) * dmf + 2080) >> 6);
    out[7] = (phx_dctcoef)(((b6 + b7) * dmf + 2080) >> 6);
}

/* quant.c:227-237 — optimize_chroma_idct_dequant_2x2().
 * SHADER: A — one invocation per chroma DC block. */
void phx_optimize_chroma_idct_dequant_2x2(phx_dctcoef out[4],
                                          const phx_dctcoef dct[4], int dmf)
{
    int d0 = dct[0] + dct[1];
    int d1 = dct[2] + dct[3];
    int d2 = dct[0] - dct[1];
    int d3 = dct[2] - dct[3];
    out[0] = (phx_dctcoef)(((d0 + d1) * dmf >> 5) + 32);
    out[1] = (phx_dctcoef)(((d0 - d1) * dmf >> 5) + 32);
    out[2] = (phx_dctcoef)(((d2 + d3) * dmf >> 5) + 32);
    out[3] = (phx_dctcoef)(((d2 - d3) * dmf >> 5) + 32);
}

/* quant.c:239-252 — optimize_chroma_round().
 * SHADER: A — one invocation per chroma DC block. */
int phx_optimize_chroma_round(const phx_dctcoef *ref, const phx_dctcoef *dct,
                              int dequant_mf, int chroma422)
{
    phx_dctcoef out[8];
    if (chroma422)
        phx_optimize_chroma_idct_dequant_2x4(out, dct, dequant_mf);
    else
        phx_optimize_chroma_idct_dequant_2x2(out, dct, dequant_mf);
    int sum = 0;
    for (int i = 0; i < (chroma422 ? 8 : 4); i++)
        sum |= ref[i] ^ out[i];
    return sum >> 6;
}

/* quant.c:254-292 — optimize_chroma_dc_internal().
 * SHADER: A — one invocation per chroma DC block. */
int phx_optimize_chroma_dc_internal(phx_dctcoef *dct, int dequant_mf, int chroma422)
{
    phx_dctcoef dct_orig[8];
    if (chroma422)
        phx_optimize_chroma_idct_dequant_2x4(dct_orig, dct, dequant_mf);
    else
        phx_optimize_chroma_idct_dequant_2x2(dct_orig, dct, dequant_mf);

    int sum = 0;
    for (int i = 0; i < (chroma422 ? 8 : 4); i++)
        sum |= dct_orig[i];
    if (!(sum >> 6))
        return 0;

    int nz, coeff;
    for (nz = 0, coeff = (chroma422 ? 7 : 3); coeff >= 0; coeff--) {
        int level = dct[coeff];
        int sign = (level >> 31) | 1; /* dct[coeff] < 0 ? -1 : 1 */
        while (level) {
            dct[coeff] = (phx_dctcoef)(level - sign);
            if (phx_optimize_chroma_round(dct_orig, dct, dequant_mf, chroma422)) {
                nz = 1;
                dct[coeff] = (phx_dctcoef)level;
                break;
            }
            level -= sign;
        }
    }
    return nz;
}

/* quant.c:294-297 — optimize_chroma_2x2_dc().
 * SHADER: A — one invocation per 2x2 chroma DC block. */
int phx_optimize_chroma_2x2_dc(phx_dctcoef dct[4], int dequant_mf)
{
    return phx_optimize_chroma_dc_internal(dct, dequant_mf, 0);
}

/* quant.c:299-302 — optimize_chroma_2x4_dc().
 * SHADER: A — one invocation per 2x4 chroma DC block. */
int phx_optimize_chroma_2x4_dc(phx_dctcoef dct[8], int dequant_mf)
{
    return phx_optimize_chroma_dc_internal(dct, dequant_mf, 1);
}

/* quant.c:304-315 — denoise_dct().
 * SHADER: A — per coefficient. */
void phx_denoise_dct(phx_dctcoef *dct, uint32_t *sum,
                     const phx_udctcoef *offset, int size)
{
    for (int i = 0; i < size; i++) {
        int level = dct[i];
        int sign = level >> 31;
        level = (level + sign) ^ sign;
        sum[i] += (uint32_t)level;
        level -= offset[i];
        dct[i] = (phx_dctcoef)(level < 0 ? 0 : (level ^ sign) - sign);
    }
}
