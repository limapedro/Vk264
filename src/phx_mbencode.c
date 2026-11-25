/* Function-for-function reconstruction port of x264's macroblock encoder.
 * Each function below names the exact vendored x264 source line it follows.
 * The local FENC/FDEC windows reproduce x264's separate p_fenc/p_fdec
 * buffers (reference/x264/common/common.h:570-571); phx_frame_t exposes only
 * one luma plane, so the caller's plane is the reconstructed (fdec) storage
 * and the source samples are staged into a 16-stride fenc window first.
 *
 * SHADER: B for every function in this file: one serial invocation per MB.
 */
#include "phx_mbencode.h"
#include "phx_rdo.h"
#include "phx_cabac_syntax.h"
#include "phx_tables.h"
#include "phx_predict.h"
#include "phx_dct.h"
#include "phx_quant.h"
#include "phx_chroma.h"

#include <string.h>

/* phx_mbencode.h routes external callers of the phase-B chroma entry point
 * through phx_mb_encode_chroma_trellis().  Keep the plain phx_chroma.c symbol
 * addressable here without recursion.  SHADER: B — no global state. */
#undef phx_macroblock_encode_chroma
extern int phx_macroblock_encode_chroma(const phx_param_t *p, phx_frame_t *f,
                                        phx_mb_cache_t *cache, int mb_x,
                                        int mb_y, phx_mb_decision_t *d);

/* Live CABAC coder handed to phx_macroblock_encode_ex(); consumed by the
 * chroma-DC trellis wrapper for the same MB (x264 rdo.c:1114-1140).
 * SHADER: C — one serial encoder instance, one MB at a time. */
static phx_cabac_t *phx_mb_active_cabac = NULL;

/* common/set.h:32 — CQM_4IY == CQM_8IY == 0 (flat intra luma lists). */
#define PHX_CQM_4IY 0
/* common/set.h:34 — CQM_4IC (flat intra chroma list). */
#define PHX_CQM_4IC 2
/* common/base.h:177 — LUMA_DC = 48; CHROMA_DC = 49 is not addressable in
 * phx_mb_decision_t's 48-entry coefficient arrays (see header). */
#define PHX_LUMA_DC 48
/* One MB in an FDEC_STRIDE=32 window plus the top/left border row/column. */
#define PHX_FDEC_WINDOW_ROWS 18

/* common/macroblock.h:181-187 — x264_zigzag_scan4[0] (frame), transposed
 * with respect to the standard tables (macroblock.h:180).
 * SHADER: B — constant coefficient order for the B->C handoff. */
static const uint8_t phx_zigzag4[16] = {
     0,  4,  1,  2,  5,  8, 12,  9,  6,  3,  7, 10, 13, 14, 11, 15
};

/* common/macroblock.h:188-194 — x264_zigzag_scan8[0] (frame).  Kept for the
 * documented I8x8 path; I8x8 is still a TODO (64 coefficients do not fit in
 * phx_mb_decision_t::dct[48][16]).
 * SHADER: B — constant coefficient order for the B->C handoff. */
static const uint8_t phx_zigzag8[64] = {
     0,  8,  1,  2,  9, 16, 24, 17, 10,  3,  4, 11, 18, 25, 32, 40,
    33, 26, 19, 12,  5,  6, 13, 20, 27, 34, 41, 48, 56, 49, 42, 35,
    28, 21, 14,  7, 15, 22, 29, 36, 43, 50, 57, 58, 51, 44, 37, 30,
    23, 31, 38, 45, 52, 59, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63
};

/* common/macroblock.h:217-220 — block_idx_xy_1d[] puts a 4x4 block's DC
 * coefficient into the 4x4 DC Hadamard lattice.
 * SHADER: B — per-MB constant indexing table. */
static const uint8_t phx_block_idx_xy_1d[16] = {
     0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15
};

/* common/macroblock.h:225-235 — block_idx_xy_fenc[] with FENC_STRIDE=16.
 * SHADER: B — per-MB constant addressing table. */
static const uint8_t phx_block_idx_xy_fenc[16] = {
      0, 4, 64, 68, 8, 12, 72, 76,
     128, 132, 192, 196, 136, 140, 200, 204
};

/* common/macroblock.h:236-246 — block_idx_xy_fdec[] with FDEC_STRIDE=32.
 * SHADER: B — per-MB constant addressing table. */
static const uint16_t phx_block_idx_xy_fdec[16] = {
      0, 4, 128, 132, 8, 12, 136, 140,
    256, 260, 384, 388, 264, 268, 392, 396
};

/* Store one quantized block and its nnz flag exactly where the C-stage
 * entropy readers expect it: encoder/macroblock.h:150/:158 (luma4x4 store),
 * encoder/macroblock.h:112-117 (STORE_8x8_NNZ), common/base.h:180 (scan8),
 * encoder/macroblock.c:946-951 (cbp carries the DC nnz flags).
 * SHADER: B — writes the B-stage coefficient and nnz outputs. */
static void phx_mb_store_block(phx_mb_decision_t *d, phx_mb_cache_t *c,
                               int block, const phx_dctcoef coeff[16], int nz)
{
    const int slot = phx_scan8[block];
    memcpy(d->dct[slot], coeff, 16 * sizeof(phx_dctcoef));
    d->nnz[slot] = (uint8_t)(nz ? 1 : 0);
    c->non_zero_count[slot] = d->nnz[slot];
}

/* Stage the reconstructed luma neighbours of the current MB into the local
 * FDEC window.  Mirrors common/frame.c:535-553 (plane_expand_border: edge
 * samples are replicated) and the p_fdec neighbour reads performed by the
 * predictors via common/macroblock.h:31-41 / common/predict.c:57-884.
 * SHADER: B — per-MB neighbour staging into the B-stage scratch window. */
static void phx_mb_load_fdec_neighbours(const phx_frame_t *f, int mb_x,
                                        int mb_y, uint8_t *p, int stride, const phx_mb_cache_t *cache)
{
    const int x0 = mb_x * 16, y0 = mb_y * 16;
    const int w = f->i_width, h = f->i_height, s = f->i_stride;
    /* x264 keeps p_fenc (source) and p_fdec (reconstruction) separate.  When
     * the caller supplies a populated FDEC (analysis reconstructed into
     * cache->p_fdec), its top/left border samples are the authoritative
     * predictors (frame.c:501-548, macroblock.h:31-41).  Only the standalone
     * API without an FDEC falls back to replicating the source border. */
    if (cache && cache->p_fdec && cache->fdec_stride > 0) {
        const uint8_t *fdec = cache->p_fdec;
        const int fs = cache->fdec_stride;
        for (int y = -1; y < 16; y++)
            p[-1 + y * stride] = fdec[-1 + y * fs];
        for (int x = 0; x < 18; x++)
            p[x - stride] = fdec[x - fs];
        return;
    }
#define PHX_PIX(x,y) f->plane[(y) * s + (x)]
#define PHX_CLAMPX(x) ((x) < 0 ? 0 : ((x) >= w ? w - 1 : (x)))
#define PHX_CLAMPY(y) ((y) < 0 ? 0 : ((y) >= h ? h - 1 : (y)))

    /* left column p[-1 + y*stride] and top-left p[-1-stride]. */
    for (int y = -1; y < 16; y++)
        p[-1 + y * stride] = PHX_PIX(PHX_CLAMPX(x0 - 1), PHX_CLAMPY(y0 + y));
    /* top row p[x-stride] plus the two top-right samples used by planar and
     * diagonal 4x4 modes (predict.c:117, :560, :577). */
    for (int x = 0; x < 18; x++)
        p[x - stride] = PHX_PIX(PHX_CLAMPX(x0 + x), PHX_CLAMPY(y0 - 1));
#undef PHX_PIX
#undef PHX_CLAMPX
#undef PHX_CLAMPY
}

/* Per-4x4-block neighbour flags for the top-right emulation in
 * encoder/macroblock.c:761-763.  The per-block table is initialized at
 * common/macroblock.c:489-498 and refined at common/macroblock.c:1340-1351;
 * this function is that pair of assignments expressed from the MB-wide
 * i_neighbour_intra cache (common/macroblock.c:673).
 * SHADER: B — per-MB neighbour flag expansion. */
static int phx_mb_neighbour4(const phx_mb_cache_t *c, int idx)
{
    const int ni = c->i_neighbour_intra;
    switch (idx) {
    case 6: case 9: case 12: case 14:                       /* :489-492 */
        return PHX_MB_LEFT | PHX_MB_TOP | PHX_MB_TOPLEFT | PHX_MB_TOPRIGHT;
    case 3: case 7: case 11: case 13: case 15:              /* :493-498 */
        return PHX_MB_LEFT | PHX_MB_TOP | PHX_MB_TOPLEFT;
    case 0:                                                 /* :1340-1342 */
        return (ni & (PHX_MB_TOP | PHX_MB_LEFT | PHX_MB_TOPLEFT))
             | ((ni & PHX_MB_TOP) ? PHX_MB_TOPRIGHT : 0);
    case 4: case 1:                                         /* :1343-1344 */
        return PHX_MB_LEFT
             | ((ni & PHX_MB_TOP) ? (PHX_MB_TOP | PHX_MB_TOPLEFT | PHX_MB_TOPRIGHT) : 0);
    case 2: case 8: case 10:                                /* :1345-1348 */
        return PHX_MB_TOP | PHX_MB_TOPRIGHT
             | ((ni & PHX_MB_LEFT) ? (PHX_MB_LEFT | PHX_MB_TOPLEFT) : 0);
    case 5:                                                 /* :1349-1351 */
        return PHX_MB_LEFT | (ni & PHX_MB_TOPRIGHT)
             | ((ni & PHX_MB_TOP) ? (PHX_MB_TOP | PHX_MB_TOPLEFT) : 0);
    default:
        return 0;
    }
}

/* mb_encode_i16x16 — reference/x264/encoder/macroblock.c:126-239.
 * Predict (:142), sub16x16_dct (:162), DC extraction (:168-172),
 * quant_4x4x4 (:190), Hadamard DC + quant_4x4_dc (:215-219), inverse DC
 * (:227-231) and add16x16_idct[_dc] (:235-238).
 * SHADER: B — one invocation per I16x16 luma plane. */
static void phx_mb_encode_i16x16(const phx_quant_tables_t *qt,
                                 const uint8_t *fenc, uint8_t *p_dst,
                                 int i_qp, int i_mode,
                                 phx_mb_decision_t *d, phx_mb_cache_t *c)
{
    phx_dctcoef dct4x4[16][16];
    phx_dctcoef dct_dc4x4[16];
    phx_dctcoef scan[16];
    phx_predict_t pred16[7];
    int nz, nz_dc, block_cbp = 0;

    phx_predict_16x16_init(pred16);                         /* predict.c:889 */
    pred16[i_mode](p_dst, PHX_FDEC_STRIDE);                 /* macroblock.c:142 */

    /* CLEAR_16x16_NNZ(p) — encoder/macroblock.h:119-126. */
    for (int i = 0; i < 16; i++) {
        const int slot = phx_scan8[i];
        c->non_zero_count[slot] = 0;
        d->nnz[slot] = 0;
        memset(d->dct[slot], 0, 16 * sizeof(phx_dctcoef));
    }
    c->non_zero_count[phx_scan8[PHX_LUMA_DC]] = 0;
    d->nnz[phx_scan8[PHX_LUMA_DC]] = 0;
    memset(d->dct[phx_scan8[PHX_LUMA_DC]], 0, 16 * sizeof(phx_dctcoef));

    phx_sub16x16_dct(dct4x4, fenc, PHX_FENC_STRIDE,
                     p_dst, PHX_FDEC_STRIDE);                /* :162 */

    for (int i = 0; i < 16; i++) {                           /* :168-172 */
        dct_dc4x4[phx_block_idx_xy_1d[i]] = dct4x4[i][0];
        dct4x4[i][0] = 0;
    }

    for (int i8x8 = 0; i8x8 < 4; i8x8++) {                   /* :188 */
        nz = phx_quant_4x4x4(&dct4x4[i8x8 * 4],
                             qt->quant4_mf[PHX_CQM_4IY][i_qp],
                             qt->quant4_bias[PHX_CQM_4IY][i_qp]); /* :190 */
        if (!nz) continue;
        block_cbp = 0xf;                                     /* :193 */
        for (int j = 0; j < 4; j++) {
            const int idx = i8x8 * 4 + j;
            if (!(nz & (1 << j))) continue;                  /* FOREACH_BIT :194 */
            for (int k = 0; k < 16; k++)
                scan[k] = dct4x4[idx][phx_zigzag4[k]];       /* :196 */
            phx_mb_store_block(d, c, idx, scan, 1);          /* :199 */
            phx_dequant_4x4(dct4x4[idx],
                            qt->dequant4_mf[PHX_CQM_4IY], i_qp); /* :197 */
        }
    }

    phx_dct4x4dc(dct_dc4x4);                                 /* :215 */
    nz_dc = phx_quant_4x4_dc(dct_dc4x4,
                             qt->quant4_mf[PHX_CQM_4IY][i_qp][0] >> 1,
                             qt->quant4_bias[PHX_CQM_4IY][i_qp][0] << 1); /* :219 */
    c->non_zero_count[phx_scan8[PHX_LUMA_DC]] = (uint8_t)nz_dc; /* :221 */
    d->nnz[phx_scan8[PHX_LUMA_DC]] = (uint8_t)nz_dc;
    if (nz_dc) {
        for (int k = 0; k < 16; k++)                         /* :224 */
            scan[k] = dct_dc4x4[phx_zigzag4[k]];
        phx_mb_store_block(d, c, PHX_LUMA_DC, scan, 1);
        phx_idct4x4dc(dct_dc4x4);                            /* :227 */
        phx_dequant_4x4_dc(dct_dc4x4,
                           qt->dequant4_mf[PHX_CQM_4IY], i_qp); /* :228 */
        if (block_cbp)
            for (int i = 0; i < 16; i++)                     /* :229-231 */
                dct4x4[i][0] = dct_dc4x4[phx_block_idx_xy_1d[i]];
    }

    if (block_cbp)
        phx_add16x16_idct(p_dst, PHX_FDEC_STRIDE, dct4x4);   /* :236 */
    else if (nz_dc)
        phx_add16x16_idct_dc(p_dst, PHX_FDEC_STRIDE, dct_dc4x4); /* :238 */

    d->cbp_luma |= block_cbp;                                /* :213 */
    /* encoder/macroblock.h:1735 — I16x16 stores DC as the 4x4 fallback mode
     * for neighbours that have no explicit intra4x4 decision. */
    for (int i = 0; i < 16; i++)
        c->intra4x4_pred_mode[phx_scan8[i]] = PHX_I_PRED_4x4_DC;
}

/* x264 common/predict.c:508-511 — predict_4x4_v_c() replicates each output
 * column's own top sample (PREDICT_4x4_DC(SRC_X4(0,-1))).  phx_predict.c:426
 * broadcasts SRC(0,-1) instead; that file is not owned by this worker, so the
 * authoritative form is installed over the function-table slot here.
 * SHADER: A — one invocation per 4x4 block. */
static void phx_predict4_v_fixed(uint8_t *dst, int stride)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            dst[x + y*stride] = dst[x - stride];
}

/* x264_mb_encode_i4x4 — reference/x264/encoder/macroblock.h:132-166.
 * Predict (:139-145), sub4x4_dct (:155), x264_quant_4x4 (:157),
 * nnz store (:158), zigzag/dequant/add4x4_idct (:161-164).
 * SHADER: B — one invocation per 4x4 luma block inside the MB. */
static void phx_mb_encode_i4x4(const phx_param_t *param,
                               const phx_quant_tables_t *qt,
                               const uint8_t *fenc, uint8_t *p_dst,
                               int i_qp, const phx_mb_decision_t *d,
                               phx_mb_decision_t *d_out, phx_mb_cache_t *c,
                               phx_cabac_t *cb)
{
    phx_predict_t pred4[12];
    phx_dctcoef dct[16], scan[16];
    int nz;

    phx_predict_4x4_init(pred4);                             /* predict.c:1042 */
    pred4[0] = phx_predict4_v_fixed;                         /* predict.c:508 */
    for (int idx = 0; idx < 16; idx++) {
        const int i_mode = d->intra4x4_pred_mode[idx];
        uint8_t *p = p_dst + phx_block_idx_xy_fdec[idx];     /* macroblock.h:758 */
        int neighbour4 = phx_mb_neighbour4(c, idx);          /* macroblock.c:1340 */

        /* encoder/macroblock.c:761-763 — emulate missing top-right samples. */
        if ((neighbour4 & (PHX_MB_TOPRIGHT | PHX_MB_TOP)) == PHX_MB_TOP)
            for (int k = 0; k < 4; k++)
                p[4 + k - PHX_FDEC_STRIDE] = p[3 - PHX_FDEC_STRIDE];

        pred4[i_mode](p, PHX_FDEC_STRIDE);                   /* macroblock.h:144 */
        c->intra4x4_pred_mode[phx_scan8[idx]] = (int8_t)i_mode; /* macroblock.c:759 */

        phx_sub4x4_dct(dct, fenc + phx_block_idx_xy_fenc[idx], PHX_FENC_STRIDE,
                       p, PHX_FDEC_STRIDE);                  /* macroblock.h:155 */
        int used_trellis = 0;
        if (cb && param->i_trellis > 0 && i_qp <= PHX_QP_MAX_SPEC) {
            /* rdo.c:1142-1157 — QNS uses the live CABAC context state and
             * writes coefficients in zigzag order. */
            nz = phx_quant_4x4_trellis(cb, dct, PHX_CQM_4IY, i_qp,
                                       PHX_DCT_LUMA_4x4, 1);
            if (nz >= 0) {
                used_trellis = 1;
                /* rdo.c:890 writes levels to dct[zigzag[i]] — natural order.
                 * x264 then calls zigzagf.scan_4x4 (macroblock.c:391/410)
                 * before entropy coding; do the same here. */
                for (int k = 0; k < 16; k++)
                    scan[k] = dct[phx_zigzag4[k]];           /* :162 */
            }
        }
        if (!used_trellis) {
            nz = phx_quant_4x4(dct, qt->quant4_mf[PHX_CQM_4IY][i_qp],
                               qt->quant4_bias[PHX_CQM_4IY][i_qp]); /* :157 */
            for (int k = 0; k < 16; k++)
                scan[k] = dct[phx_zigzag4[k]];               /* :162 */
        }
        phx_mb_store_block(d_out, c, idx, scan, nz);         /* :158 */

        if (nz) {                                            /* :159-165 */
            d_out->cbp_luma |= 1 << (idx >> 2);              /* :161 */
            phx_dequant_4x4(dct, qt->dequant4_mf[PHX_CQM_4IY], i_qp); /* :163 */
            phx_add4x4_idct(p, PHX_FDEC_STRIDE, dct);        /* :164 */
        }
    }
}

/* I_PCM luma branch of macroblock_encode_internal —
 * reference/x264/encoder/macroblock.c:628-640, with the PCM nnz convention
 * from common/macroblock.c:1740-1749.  Chroma copy is still a TODO because
 * phx_frame_t has no chroma planes (common/frame.h:37-140).
 * SHADER: B — one invocation per PCM MB. */
static void phx_mb_encode_pcm(const phx_param_t *p, const uint8_t *fenc,
                              uint8_t *p_dst, phx_mb_decision_t *d,
                              phx_mb_cache_t *c)
{
    for (int y = 0; y < 16; y++)
        memcpy(p_dst + y * PHX_FDEC_STRIDE, fenc + y * PHX_FENC_STRIDE, 16);
                                                             /* macroblock.c:632 */
    const int nz = p->b_cabac ? 1 : 16;                      /* macroblock.c:1749 */
    for (int k = 0; k < 48; k++) {
        if (phx_scan8[k] >= 48) continue; /* chroma slots: TODO (no chroma) */
        c->non_zero_count[phx_scan8[k]] = (uint8_t)nz;
        d->nnz[phx_scan8[k]] = (uint8_t)nz;
        memset(d->dct[phx_scan8[k]], 0, 16 * sizeof(phx_dctcoef));
    }
    d->cbp_luma = 0xf;
    d->cbp_chroma = 0xf;                                     /* macroblock.c:1740 */
}

/* x264_macroblock_encode — reference/x264/encoder/macroblock.c:974-982,
 * dispatching macroblock_encode_internal (:618-972).  Cache-nnz and cbp
 * stores mirror common/macroblock.c:1680/1761-1779; reconstructed pixels are
 * written into f->plane because phx_frame_t has no separate fdec plane
 * (common/frame.h:37-140).
 * SHADER: B — the single B-stage entry point for one macroblock. */
int phx_macroblock_encode_ex(const phx_param_t *p, phx_frame_t *f,
                             phx_mb_cache_t *cache, int mb_x, int mb_y,
                             phx_mb_decision_t *d, phx_cabac_t *cb)
{
    if (!p || !f || !cache || !d || !f->plane) return -1;

    /* Serial B-stage carry-over for the chroma-DC trellis wrapper below:
     * x264's rdo.c:1114-1140 reads the live CABAC context state.  SHADER: C. */
    phx_mb_active_cabac = cb;

    const int mb_w = f->i_width / 16, mb_h = f->i_height / 16;
    if (mb_x < 0 || mb_x >= mb_w || mb_y < 0 || mb_y >= mb_h) return -1;
    if (d->i_qp < 0 || d->i_qp > PHX_QUANT_QP_MAX_SPEC) return -1;

    (void)phx_zigzag8; /* retained for the I8x8 TODO below */

    /* TODO (B): I8x8 needs 64 coefficients per block, but
     * phx_mb_decision_t::dct[] has only 16 per slot.
     * TODO (B): chroma prediction/DCT/quant/IDCT (encoder/macroblock.c:259)
     * needs chroma planes that phx_frame_t does not expose.
     * TODO (B): P_L0/P_8x8/B_* and *_SKIP need reference frames and the MC
     * path of encoder/macroblock.c:654-704/769-922.
     * Do not approximate any of these: reject them explicitly. */
    if (d->mb_type != PHX_I_16x16 && d->mb_type != PHX_I_4x4
        && d->mb_type != PHX_I_PCM)
        return -1;
    if (d->mb_type == PHX_I_16x16 && (d->intra16_pred_mode < 0 || d->intra16_pred_mode > 6))
        return -1;
    if (d->mb_type == PHX_I_4x4)
        for (int i = 0; i < 16; i++)
            if (d->intra4x4_pred_mode[i] < 0 || d->intra4x4_pred_mode[i] > 11)
                return -1;

    phx_quant_tables_t qt;
    phx_quant_tables_init(&qt);                              /* set.c:73-205 */

    uint8_t fenc[16 * PHX_FENC_STRIDE];
    uint8_t fdec[PHX_FDEC_STRIDE * PHX_FDEC_WINDOW_ROWS + PHX_FDEC_STRIDE];
    uint8_t *p_dst = fdec + PHX_FDEC_STRIDE + 1;             /* common.h:571 */
    memset(fdec, 0, sizeof(fdec));

    const int x0 = mb_x * 16, y0 = mb_y * 16;
    uint8_t *plane_mb = f->plane + y0 * f->i_stride + x0;    /* common.h:570 */
    for (int y = 0; y < 16; y++)
        memcpy(fenc + y * PHX_FENC_STRIDE, plane_mb + y * f->i_stride, 16);

    phx_mb_load_fdec_neighbours(f, mb_x, mb_y, p_dst, PHX_FDEC_STRIDE, cache);

    /* common/macroblock.c:1683/1722/1779 — keep the caller's p_fdec (the
     * analysis reconstruction frame, common/common.h:571) intact: x264 writes
     * reconstructed samples into fdec, never into fenc.  Standalone callers
     * without an FDEC keep the legacy f->plane target. */
    uint8_t *fdec_target = cache->p_fdec ? cache->p_fdec : plane_mb;
    int fdec_target_stride = cache->p_fdec ? cache->fdec_stride : f->i_stride;
    cache->b_transform_8x8 = 0;                              /* macroblock.c:708/740 */
    cache->i_cbp_luma = 0;                                   /* macroblock.c:624 */
    cache->i_cbp_chroma = 0;                                 /* macroblock.c:265 */
    cache->i_qp = d->i_qp;                                   /* macroblock.c:620 */
    d->cbp_luma = 0;
    d->cbp_chroma = 0;
    d->skip = 0;
    d->b_transform_8x8 = 0;

    if (d->mb_type == PHX_I_16x16) {
        phx_mb_encode_i16x16(&qt, fenc, p_dst, d->i_qp,
                             d->intra16_pred_mode, d, cache);
    } else if (d->mb_type == PHX_I_4x4) {
        phx_mb_encode_i4x4(p, &qt, fenc, p_dst, d->i_qp, d, d, cache, cb);
    } else {
        phx_mb_encode_pcm(p, fenc, p_dst, d, cache);
    }

    /* encoder/macroblock.c:946-951 — cbp = chroma<<4 | luma (+ CABAC DC
     * flags).  d->nnz[phx_scan8[LUMA_DC]] is the luma DC flag. */
    d->i_cbp = (d->cbp_chroma << 4) | d->cbp_luma;
    if (p->b_cabac && d->mb_type == PHX_I_16x16)
        d->i_cbp |= d->nnz[phx_scan8[PHX_LUMA_DC]] << 8;
    cache->i_cbp_luma = d->cbp_luma;
    cache->i_cbp_chroma = d->cbp_chroma;

    for (int y = 0; y < 16; y++)                             /* macroblock.c:632 */
        memcpy(fdec_target + y * fdec_target_stride,
               p_dst + y * PHX_FDEC_STRIDE, 16);
    return 0;
}

/* Plain-deadzone wrapper: unit tests and CAVLC call this path. */
int phx_macroblock_encode(const phx_param_t *p, phx_frame_t *f,
                          phx_mb_cache_t *cache, int mb_x, int mb_y,
                          phx_mb_decision_t *d)
{
    return phx_macroblock_encode_ex(p, f, cache, mb_x, mb_y, d, NULL);
}

/* -------------------------------------------------------------------------
 * Chroma DC trellis handoff (x264 encoder/macroblock.c:259-498, encoder/rdo.c:
 * 1114-1140).  phx_chroma.c implements the non-trellis path faithfully but its
 * 2x2 DC scan is the identity, while x264's zigzag_scan_2x2_dc() (macroblock.c:
 * 34-41) swaps entries 1 and 2.  With trellis enabled x264 also quantizes the
 * 2x2 DC block with x264_quant_chroma_dc_trellis() (:420) instead of
 * quant_2x2_dc() (:425-426).  The wrapper computes x264's residual from the
 * pre-chroma reconstructed neighbours, runs the golden trellis quantizer with
 * the live CABAC context state, and publishes the correctly scanned 2x2 block
 * after phx_chroma.c has finished the AC and reconstruction work.
 * SHADER: B — one serial invocation per MB, no cross-MB coefficient state.
 * ------------------------------------------------------------------------- */

static int phx_mb_clip3i(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* encoder.c:1585 + common/macroblock.h:249-270 (same derivation as
 * phx_chroma.c:224-231). */
static int phx_mb_chroma_qp(const phx_param_t *p, int qp)
{
    int idx = phx_mb_clip3i(PHX_CHROMA_QP_BASE_OFFSET +
                            p->i_chroma_qp_index_offset + qp,
                            0, (int)(sizeof(phx_chroma_qp_base) - 1));
    int base = phx_chroma_qp_base[idx];
    return base + (qp > PHX_QP_MAX_SPEC ? qp - PHX_QP_MAX_SPEC : 0);
}

/* analyse.c:485-492 / phx_chroma.c:97-114 — keep an explicitly requested
 * chroma mode legal at frame edges. */
static int phx_mb_chroma_mode_ok(int neighbour, int mode)
{
    static const int8_t avail[5][5] = {
        { 6, -1, -1, -1, -1 },
        { 4,  1, -1, -1, -1 },
        { 5,  2, -1, -1, -1 },
        { 2,  1,  0, -1, -1 },
        { 2,  1,  0,  3, -1 }
    };
    int idx = neighbour & (PHX_MB_TOP | PHX_MB_LEFT | PHX_MB_TOPLEFT);
    idx = (idx == (PHX_MB_TOP | PHX_MB_LEFT | PHX_MB_TOPLEFT))
        ? 4 : idx & (PHX_MB_TOP | PHX_MB_LEFT);
    for (int i = 0; i < 5 && avail[idx][i] >= 0; i++)
        if (avail[idx][i] == mode) return 1;
    return 0;
}

/* macroblock.c:34-41 — zigzag_scan_2x2_dc(): level = { d0, d2, d1, d3 }. */
static void phx_mb_zigzag2_dc(phx_dctcoef level[4], const phx_dctcoef dct[4])
{
    level[0] = dct[0*2+0];                             /* macroblock.c:37 */
    level[1] = dct[1*2+0];                             /* macroblock.c:39 */
    level[2] = dct[0*2+1];                             /* macroblock.c:38 */
    level[3] = dct[1*2+1];                             /* macroblock.c:40 */
}

/* macroblock.c:82-96 — dct2x2dc(): Hadamard of the four 4x4 DC coefficients. */
static void phx_mb_dct2x2dc(phx_dctcoef d[4], phx_dctcoef dct4x4[4][16])
{
    int d0 = dct4x4[0][0] + dct4x4[1][0];              /* macroblock.c:84 */
    int d1 = dct4x4[2][0] + dct4x4[3][0];              /* macroblock.c:85 */
    int d2 = dct4x4[0][0] - dct4x4[1][0];              /* macroblock.c:86 */
    int d3 = dct4x4[2][0] - dct4x4[3][0];              /* macroblock.c:87 */
    d[0] = (phx_dctcoef)(d0 + d1);                     /* macroblock.c:88 */
    d[2] = (phx_dctcoef)(d2 + d3);                     /* macroblock.c:89 */
    d[1] = (phx_dctcoef)(d0 - d1);                     /* macroblock.c:90 */
    d[3] = (phx_dctcoef)(d2 - d3);                     /* macroblock.c:91 */
}

/* Recompute the unquantized 2x2 DC block exactly as macroblock.c:343-379
 * does, before the phase-B chroma encoder overwrites the frame plane. */
static void phx_mb_chroma_raw_dc(const phx_frame_t *f, const phx_mb_cache_t *cache,
                                int mb_x, int mb_y, int mode,
                                phx_dctcoef raw[2][4],
                                phx_dctcoef ac[2][4][16])
{
    const int x0 = mb_x * 8, y0 = mb_y * 8;
    const int cw = f->i_width_planes[1], chh = f->i_lines_planes[1];
    const int s = f->i_stride_planes[1];
    const uint8_t *plane = f->plane_chroma;
    uint8_t fenc[2][8 * PHX_FENC_STRIDE];
    uint8_t fdec[2][PHX_FDEC_STRIDE * 10 + PHX_FDEC_STRIDE];
    uint8_t *p_dst[2] = { fdec[0] + PHX_FDEC_STRIDE + 1,
                          fdec[1] + PHX_FDEC_STRIDE + 1 };
    phx_predict_t pred8[7];
    int in = cache->i_neighbour_intra;

    memset(fdec, 0, sizeof(fdec));
    for (int c = 0; c < 2; c++) {
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                fenc[c][y*PHX_FENC_STRIDE + x] =
                    plane[(y0+y)*s + 2*(x0+x) + c];    /* frame.c:240 */
        for (int y = -1; y < 8; y++) {
            uint8_t v = 128;
            if (in & PHX_MB_LEFT) {
                int sy = phx_mb_clip3i(y0 + y, 0, chh - 1);
                int sx = phx_mb_clip3i(x0 - 1, 0, cw - 1);
                v = plane[sy*s + 2*sx + c];
            }
            p_dst[c][-1 + y*PHX_FDEC_STRIDE] = v;
        }
        for (int x = 0; x < 8; x++) {
            uint8_t v = 128;
            if (in & PHX_MB_TOP) {
                int sy = phx_mb_clip3i(y0 - 1, 0, chh - 1);
                int sx = phx_mb_clip3i(x0 + x, 0, cw - 1);
                v = plane[sy*s + 2*sx + c];
            }
            p_dst[c][x - PHX_FDEC_STRIDE] = v;
        }
        {
            uint8_t v = 128;
            if (in & PHX_MB_TOPLEFT) {
                int sy = phx_mb_clip3i(y0 - 1, 0, chh - 1);
                int sx = phx_mb_clip3i(x0 - 1, 0, cw - 1);
                v = plane[sy*s + 2*sx + c];
            }
            p_dst[c][-1 - PHX_FDEC_STRIDE] = v;
        }
    }
    phx_predict_8x8c_init(pred8);                      /* predict.c:936-945 */
    pred8[mode](p_dst[0], PHX_FDEC_STRIDE);            /* macroblock.c:934 */
    pred8[mode](p_dst[1], PHX_FDEC_STRIDE);            /* macroblock.c:935 */
    for (int c = 0; c < 2; c++) {
        phx_sub8x8_dct(ac[c], fenc[c], PHX_FENC_STRIDE,
                       p_dst[c], PHX_FDEC_STRIDE);     /* macroblock.c:369-370 */
        phx_dctcoef dct_copy[4][16];
        memcpy(dct_copy, ac[c], sizeof(dct_copy));
        phx_mb_dct2x2dc(raw[c], dct_copy);             /* macroblock.c:379 */
    }
}

/* Phase-B chroma entry point with x264's trellis DC handoff.  Non-trellis
 * callers keep the verbatim phx_chroma.c path. */
int phx_mb_encode_chroma_trellis(const phx_param_t *p, phx_frame_t *f,
                                 phx_mb_cache_t *cache, int mb_x, int mb_y,
                                 phx_mb_decision_t *d)
{
    if (!p || !f || !cache || !d || !f->plane || !f->plane_chroma)
        return phx_macroblock_encode_chroma(p, f, cache, mb_x, mb_y, d);
    if (!p->b_cabac || p->i_trellis <= 0 || !phx_mb_active_cabac ||
        d->i_qp < 0 || d->i_qp > PHX_QP_MAX_SPEC ||
        f->i_width % 16 || f->i_height % 16)
        return phx_macroblock_encode_chroma(p, f, cache, mb_x, mb_y, d);

    int mode = d->chroma_pred_mode;
    if (mode < 0 || mode > 6)
        return phx_macroblock_encode_chroma(p, f, cache, mb_x, mb_y, d);
    if (!phx_mb_chroma_mode_ok(cache->i_neighbour_intra, mode))
        mode = 6;                                      /* analyse.c:487 */

    phx_dctcoef raw[2][4], ac[2][4][16];
    phx_mb_chroma_raw_dc(f, cache, mb_x, mb_y, mode, raw, ac);
    int rc = phx_macroblock_encode_chroma(p, f, cache, mb_x, mb_y, d);
    if (rc < 0) return rc;

    const int chroma_qp = phx_mb_chroma_qp(p, d->i_qp);
    int dc_nnz[2] = { 0, 0 }, ac_nnz = 0;
    for (int ch = 0; ch < 2; ch++) {
        /* macroblock.c:384-397 — trellis QNS for each chroma 4x4 AC block. */
        for (int i = 0; i < 4; i++) {
            int nz = phx_quant_4x4_trellis(phx_mb_active_cabac, ac[ch][i],
                PHX_CQM_4IC, chroma_qp, PHX_DCT_CHROMA_AC, 1);
            if (nz < 0) return -1;
            const int block = 16 + ch*16 + i;           /* macroblock.c:390 */
            uint8_t flag = nz > 0 ? 1 : 0;
            if (flag) {
                ac_nnz = 1;
                /* rdo.c:890 leaves levels in natural order; x264's
                 * zigzagf.scan_4x4() (macroblock.c:391) feeds CABAC.  The
                 * chroma AC residual starts at coefficient 1 (cabac.c:1078,
                 * h->dct.luma4x4[j]+1); the C-stage exposes the same 15-bin
                 * window (phx_encoder.c:505-506), so skip scan[0] here. */
                /* phx_encoder.c:505-506 maps x264's U blocks (16..19) to
                 * chroma_dct[1][1..4]; store U there so CABAC sees the same
                 * plane as x264.  V blocks still hit that writer's out-of-range
                 * ch=2 slot (outside this worker's files), so keep V's decoded
                 * values in the otherwise unused chroma_dct[0][1..4] slots. */
                const int dest = ch == 0 ? 1 : 0;
                for (int k = 0; k < 15; k++)
                    d->chroma_dct[dest][1+i][k] = ac[ch][i][phx_zigzag4[k+1]];
                d->chroma_dct[dest][1+i][15] = 0;
            } else {
                memset(d->chroma_dct[ch][1+i], 0,
                       16 * sizeof(phx_dctcoef));        /* macroblock.c:434 */
            }
            d->chroma_nnz[ch][1+i] = flag;
            d->nnz[block] = flag;                        /* macroblock.c:395 */
            cache->non_zero_count[phx_scan8[block]] = flag; /* :414 */
        }

        phx_dctcoef level[4];
        /* rdo.c:1114-1140 — quant_cat = CQM_4IC+1-b_intra = 2 for intra. */
        int nz = phx_quant_chroma_dc_trellis(phx_mb_active_cabac, raw[ch],
                                             chroma_qp, PHX_DCT_CHROMA_DC, 1);
        if (nz < 0) return -1;
        dc_nnz[ch] = !!nz;
        phx_mb_zigzag2_dc(level, raw[ch]);             /* macroblock.c:457/477 */
        memset(d->chroma_dct[ch][0], 0, 16 * sizeof(phx_dctcoef));
        for (int i = 0; i < 4; i++)
            d->chroma_dct[ch][0][i] = level[i];
        d->chroma_nnz[ch][0] = (uint8_t)dc_nnz[ch];
        cache->non_zero_count[phx_scan8[PHX_CHROMA_DC + ch]] =
            (uint8_t)dc_nnz[ch];
    }
    d->cbp_chroma = ac_nnz + (dc_nnz[0] | dc_nnz[1] | ac_nnz);
    cache->i_cbp_chroma = d->cbp_chroma;               /* macroblock.c:489 */
    cache->i_cbp_chroma = d->cbp_chroma;
    d->i_cbp = (d->cbp_chroma << 4) | d->cbp_luma;
    if (d->mb_type == PHX_I_16x16)
        d->i_cbp |= d->nnz[phx_scan8[PHX_LUMA_DC]] << 8;
    return 0;
}
