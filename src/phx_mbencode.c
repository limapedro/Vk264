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
#include "phx_mc.h"

#include <string.h>

/* common/base.h:206 — x264_clip3(); defined with the chroma helpers below and
 * needed earlier by the inter MV clip.  SHADER: B — scalar clamp. */
static int phx_mb_clip3i(int v, int lo, int hi);
/* Forward declarations of the chroma helpers used by rows 18-19; their
 * definitions are further down in this file.  SHADER: B — no static state. */
static int phx_mb_chroma_qp(const phx_param_t *p, int qp);
static void phx_mb_zigzag2_dc(phx_dctcoef level[4], const phx_dctcoef dct[4]);
static void phx_mb_dct2x2dc(phx_dctcoef d[4], phx_dctcoef dct4x4[4][16]);

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

/* common/set.h:29-42 — flat CQM list indices. */
#define PHX_CQM_4IY 0
#define PHX_CQM_4PY 1
#define PHX_CQM_4IC 2
#define PHX_CQM_4PC 3
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

/* common/macroblock.h:188-194 — x264_zigzag_scan8[0] (frame).  Used by the
 * I_8x8 / inter-8x8 paths; the 64 coefficients are published through
 * phx_mb_i8x8_block_t (phx_mbencode.h) because phx_mb_decision_t::dct[48][16]
 * cannot hold them.
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
        /* analyse.c:3724 — the FINAL MB encode sets h->mb.b_trellis =
         * h->param.analyse.i_trellis (no mbrd gate; analyse.c:261 is the
         * analysis-only trial switch).  Medium trellis=1 therefore uses QNS. */
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

/* No-residual inter decision: clear every coefficient/nnz slot the C stage
 * would otherwise read as a residual.  macroblock_encode_internal starts with
 * i_cbp_luma = 0 and clears the LUMA_DC nnz flags (encoder/macroblock.c:624-
 * 626); with cbp_luma == cbp_chroma == 0 no residual is written and
 * macroblock_cache_save (common/macroblock.c:1761-1779) stores zeros.
 * SHADER: B — one serial clear of the B->C handoff arrays. */
static void phx_mb_clear_inter_residual(phx_mb_decision_t *d,
                                        phx_mb_cache_t *c)
{
    for (int i = 0; i < 48; i++) {
        d->nnz[i] = 0;
        memset(d->dct[i], 0, 16 * sizeof(phx_dctcoef));
        c->non_zero_count[phx_scan8[i]] = 0;
    }
    d->nnz[phx_scan8[PHX_LUMA_DC]] = 0;
    memset(d->dct[phx_scan8[PHX_LUMA_DC]], 0, 16 * sizeof(phx_dctcoef));
    c->non_zero_count[phx_scan8[PHX_LUMA_DC]] = 0;
    for (int ch = 0; ch < 2; ch++) {
        memset(d->chroma_dct[ch], 0, sizeof(d->chroma_dct[ch]));
        memset(d->chroma_nnz[ch], 0, sizeof(d->chroma_nnz[ch]));
        c->non_zero_count[phx_scan8[PHX_CHROMA_DC + ch]] = 0;
    }
}

/* Motion-compensated 16x16 luma for P_L0/D_16x16 with cbp == 0.
 * Follows encoder/macroblock.c:654-667 (p_fdec[p] <- mc_luma(..., p_fref[0][0],
 * i_stride[p], mvx, mvy, 16, 16)) and the inter branch at :769-775
 * (x264_mb_mc when MC was not already done in analysis).  The MV is clipped
 * to the frame-edge limits of analyse.c:336-337/:391-392 (macroblock.c:659/
 * :661) and passed in quarter-pel units to phx_mc_luma(), which follows
 * common/mc.c:198-219 exactly: fullpel uses mc_copy (:218), halfpel uses the
 * H/V/C planes from hpel_filter (:174-196), and quarterpel uses the
 * x264_hpel_ref0/ref1 tables plus (a+b+1)>>1 (:63-75/:207-211).
 * Integer MVs keep the old fullpel output byte for byte (qpel_idx == 0).
 * d->ref[0][0] selects list-0 reference 0 -
 * the only list entry Phoenix passes in (macroblock.c:666 p_fref[0][0]).
 * SHADER: B - one serial 16x16 MC block into the B-stage FDEC window. */
static int phx_mb_mc_l0_16x16(const phx_frame_t *fref, uint8_t *p_dst,
                              int mb_x, int mb_y, const phx_mb_decision_t *d,
                              int *mvx_out, int *mvy_out)
{
    if (!fref || !fref->plane || fref->i_stride <= 0 ||
        fref->i_width <= 0 || fref->i_height <= 0)
        return -1;
    if (d->ref[0][0] != 0) /* only L0 ref 0 is supplied (macroblock.c:666) */
        return -1;

    const int mb_w = fref->i_width / 16, mb_h = fref->i_height / 16;
    /* analyse.c:336-337 / :391-392 — allow 24 pixels beyond each frame edge,
     * matching the replicated padding of common/frame.c:535-553. */
    const int mv_min_x = 4 * (-16 * mb_x - 24);
    const int mv_max_x = 4 * (16 * (mb_w - mb_x - 1) + 24);
    const int mv_min_y = 4 * (-16 * mb_y - 24);
    const int mv_max_y = 4 * (16 * (mb_h - mb_y - 1) + 24);
    const int mvx = phx_mb_clip3i(d->mv[0][0][0], mv_min_x, mv_max_x);
                                                             /* macroblock.c:659 */
    const int mvy = phx_mb_clip3i(d->mv[0][0][1], mv_min_y, mv_max_y);
    if (mvx_out) *mvx_out = mvx;
    if (mvy_out) *mvy_out = mvy;   /* shared with chroma MC (macroblock.c:64) */
                                                             /* macroblock.c:661 */
    /* common/macroblock.c:568-640 builds p_fref[0][0][0..3]; phx_mc_luma_mb
     * prepares the H/V/C planes with x264_frame_filter (mc.c:704-726) and
     * points them at the current MB origin.  Fullpel keeps the old mc_copy
     * result exactly (qpel_idx == 0); fractional MVs now use x264's 6-tap
     * HPEL planes and (a+b+1)>>1 qpel average (mc.c:198-219). */
    return phx_mc_luma_mb(fref, p_dst, PHX_FDEC_STRIDE, mb_x, mb_y,
                          mvx, mvy, 16, 16);
}

/* Inter chroma MC for P_L0/D_16x16, 4:2:0 NV12.
 * encoder/macroblock.c:669-681 does mc_chroma for P_SKIP (with a deinterleave
 * shortcut for mv0); the non-skip inter path is common/macroblock.c:61-64,
 * which calls the same mc_chroma with mvx and 2*mvy>>CHROMA_V_SHIFT (4:2:0
 * => mvy>>1).  common/mc.c:252-283 supplies the bilinear coefficients.
 * The planar U/V result is interleaved back into the NV12 frame plane with
 * the mc.c:392-400 store pattern (Phoenix has no separate chroma FDEC).
 * SHADER: B — one serial 8x8 chroma MC block per MB. */
static void phx_mb_mc_chroma_l0(const phx_frame_t *fref, phx_frame_t *f,
                                 int mb_x, int mb_y, int mvx, int mvy)
{
    if (!fref || !fref->plane_chroma || !f || !f->plane_chroma ||
        fref->i_stride_planes[1] <= 0 || f->i_stride_planes[1] <= 0)
        return;

    const int src_stride = fref->i_stride_planes[1];
    const int dst_stride = f->i_stride_planes[1];
    /* common/macroblock.c:568-576: 8*mb_y rows and 8*mb_x chroma samples;
     * NV12 stores a U/V pair per byte position so the x offset is 16 bytes. */
    const size_t src_off = (size_t)(mb_y*8)*src_stride + (size_t)(mb_x*16);
    const size_t dst_off = (size_t)(mb_y*8)*dst_stride + (size_t)(mb_x*16);
    const uint8_t *src = fref->plane_chroma + src_off;
    uint8_t *dst = f->plane_chroma + dst_off;

    uint8_t cu[8*8], cv[8*8];                            /* planar FDEC U/V */
    if (mvx | mvy) {
        /* 4:2:0: CHROMA_V_SHIFT == 1 => mvy >> 1 (macroblock.c:53/64). */
        phx_mc_chroma(cu, 8, cv, 8, src, src_stride,
                      mvx, mvy >> 1, 8, 8);               /* mc.c:252-283 */
    } else {
        /* macroblock.c:674-681 — mv0 deinterleave shortcut. */
        phx_mc_deinterleave_chroma(cu, 8, cv, 8, src, src_stride, 8, 8);
    }
    for (int y = 0; y < 8; y++)                          /* mc.c:392-400 */
        for (int x = 0; x < 8; x++) {
            dst[y*dst_stride + 2*x]   = cu[y*8 + x];     /* U */
            dst[y*dst_stride + 2*x+1] = cv[y*8 + x];     /* V */
        }
}

/* -------------------------------------------------------------------------
 * Rows 18-19: residual-carrying inter MBs and the 64-coefficient I_8x8 path.
 * Every helper below cites the exact x264 source it mirrors; no coefficient
 * is copied from an x264 binary.  SHADER: B — one serial invocation per MB.
 * ------------------------------------------------------------------------- */

/* common/tables.c:373-380 — decimation score lookup tables. */
static const uint8_t phx_decimate4[16] = {
    3,2,2,1,1,1,0,0,0,0,0,0,0,0,0,0
};
static const uint8_t phx_decimate8[64] = {
    3,3,3,3,2,2,2,2,2,2,2,2,1,1,1,1,
    1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* common/quant.c:326-363 — decimate_score15/16/64.  A score below 4 (8x8)
 * or 6 (whole MB) lets x264 zero the nnz flags without touching the DCT
 * buffers; the B->C handoff mirrors that exactly. */
static int phx_mb_decimate_score(const phx_dctcoef *dct, int n)
{
    const uint8_t *tab = (n == 64) ? phx_decimate8 : phx_decimate4;
    int score = 0, idx = n - 1;
    while (idx >= 0 && dct[idx] == 0) idx--;
    while (idx >= 0) {
        int run;
        if ((unsigned)(dct[idx--] + 1) > 2) return 9;
        run = 0;
        while (idx >= 0 && dct[idx] == 0) { idx--; run++; }
        score += tab[run];
    }
    return score;
}

/* macroblock.c:54-68 — idct_dequant_2x2_dc() / _dconly() (4:2:0 chroma DC
 * reconstruction).  dmf = dequant_mf[i_qp%6][0] << i_qp/6. */
static void phx_mb_idct_dequant_2x2_dc(phx_dctcoef dst[4][16],
                                        const phx_dctcoef dct[4],
                                        const int dequant_mf[6][16], int i_qp)
{
    int d0 = dct[0] + dct[1], d1 = dct[2] + dct[3];
    int d2 = dct[0] - dct[1], d3 = dct[2] - dct[3];
    int dmf = dequant_mf[i_qp % 6][0] << (i_qp / 6);
    dst[0][0] = (phx_dctcoef)((d0 + d1) * dmf >> 5);
    dst[1][0] = (phx_dctcoef)((d0 - d1) * dmf >> 5);
    dst[2][0] = (phx_dctcoef)((d2 + d3) * dmf >> 5);
    dst[3][0] = (phx_dctcoef)((d2 - d3) * dmf >> 5);
}
static void phx_mb_idct_dequant_2x2_dconly(phx_dctcoef dct[4],
                                            const int dequant_mf[6][16], int i_qp)
{
    int d0 = dct[0] + dct[1], d1 = dct[2] + dct[3];
    int d2 = dct[0] - dct[1], d3 = dct[2] - dct[3];
    int dmf = dequant_mf[i_qp % 6][0] << (i_qp / 6);
    dct[0] = (phx_dctcoef)((d0 + d1) * dmf >> 5);
    dct[1] = (phx_dctcoef)((d0 - d1) * dmf >> 5);
    dct[2] = (phx_dctcoef)((d2 + d3) * dmf >> 5);
    dct[3] = (phx_dctcoef)((d2 - d3) * dmf >> 5);
}

/* macroblock.c:261-269 — b_decimate early-termination pixel variance. */
static int phx_mb_var2_8x8(const uint8_t *fenc0, const uint8_t *fenc1,
                           const uint8_t *fdec0, const uint8_t *fdec1,
                           int ssd[2])
{
    int su = 0, sv = 0, qu = 0, qv = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int du = (int)fenc0[y*PHX_FENC_STRIDE+x] - fdec0[y*PHX_FDEC_STRIDE+x];
            int dv = (int)fenc1[y*PHX_FENC_STRIDE+x] - fdec1[y*PHX_FDEC_STRIDE+x];
            su += du; sv += dv; qu += du*du; qv += dv*dv;
        }
    }
    ssd[0] = qu; ssd[1] = qv;
    return qu - (int)(((int64_t)su*su) >> 6)
         + qv - (int)(((int64_t)sv*sv) >> 6);
}

/* macroblock.c:221-228 — mb_optimize_chroma_dc(), dmf>32*64 shortcut. */
static int phx_mb_optimize_chroma_dc(const phx_quant_tables_t *qt,
                                      phx_dctcoef dct[4], int i_qp)
{
    int dmf = qt->dequant4_mf[PHX_CQM_4PC][i_qp % 6][0] << (i_qp / 6);
    if (dmf > 32 * 64) return 1;
    return phx_optimize_chroma_2x2_dc(dct, dmf);
}

/* macroblock.c:112-117 — STORE_8x8_NNZ(): four 4x4 nnz bytes per 8x8 block. */
static void phx_mb_store_8x8_nnz(phx_mb_decision_t *d, phx_mb_cache_t *c,
                                  int idx, int nz)
{
    const int base[4] = { 0, 2, 8, 10 };             /* macroblock.h:113-116 */
    for (int j = 0; j < 4; j++) {
        const int slot = phx_scan8[base[idx] + j];
        d->nnz[slot] = (uint8_t)(nz ? 1 : 0);
        c->non_zero_count[slot] = d->nnz[slot];
    }
}

/* Deinterleave / interleave the NV12 chroma plane into planar 8x8 windows
 * (frame.c:45-57/:240).  SHADER: B — one 8x8 window per plane. */
static void phx_mb_chroma_window(const phx_frame_t *f, int mb_x, int mb_y,
                                 uint8_t *out[2], int stride, int store)
{
    const int x0 = mb_x * 8, y0 = mb_y * 8, s = f->i_stride_planes[1];
    uint8_t *plane = f->plane_chroma;
    for (int c = 0; c < 2; c++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                uint8_t *p = &plane[(y0+y)*s + 2*(x0+x) + c];
                uint8_t *w = &out[c][y*stride + x];
                if (store) *p = *w; else *w = *p;
            }
}

/* x264_mb_encode_chroma() with b_inter == 1 — encoder/macroblock.c:259-498,
 * :492-498, called from macroblock_encode_internal's inter branch (:940-943).
 * Unlike phx_chroma.c (intra only), the prediction is the MC'd FDEC already
 * in f->plane_chroma and the quant category is CQM_4PC (set.h:33), not CQM_4IC.
 * SHADER: B — one serial inter-chroma encode per MB. */
static void phx_mb_encode_inter_chroma(const phx_param_t *p,
                                        const phx_quant_tables_t *qt,
                                        phx_frame_t *f, int mb_x, int mb_y,
                                        uint8_t *fenc[2], uint8_t *p_dst[2],
                                        int i_qp, phx_mb_decision_t *d,
                                        phx_mb_cache_t *c, phx_cabac_t *cb)
{
    const int cat = 3;                                  /* CQM_4PC, set.h:33 */
    const int chroma_qp = phx_mb_chroma_qp(p, i_qp);    /* encoder.c:1585 */
    const int b_decimate = p->b_dct_decimate;           /* macroblock.c:261 */
    const int b_trellis = cb && p->i_trellis > 0 && i_qp <= PHX_QP_MAX_SPEC;

    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < 4; i++) {
            const int idx = 16 + ch*16 + i;
            d->chroma_nnz[ch][1+i] = 0;
            d->nnz[idx] = 0;
            c->non_zero_count[phx_scan8[idx]] = 0;
        }

    /* macroblock.c:278-335 — early termination on residual variance. */
    if (b_decimate && chroma_qp >= (b_trellis ? 12 : 18)) {
        int ssd[2];
        int thresh = (phx_lambda2_tab[chroma_qp < 0 ? 0 :
                      (chroma_qp > PHX_QUANT_QP_MAX_SPEC ? PHX_QUANT_QP_MAX_SPEC : chroma_qp)] + 32) >> 6;
        if (phx_mb_var2_8x8(fenc[0], fenc[1], p_dst[0], p_dst[1], ssd) < thresh*4) {
            int dc_nnz = 0;
            for (int ch = 0; ch < 2; ch++) {
                d->chroma_nnz[ch][0] = 0;
                c->non_zero_count[phx_scan8[PHX_CHROMA_DC+ch]] = 0;
                memset(d->chroma_dct[ch][0], 0, 16*sizeof(phx_dctcoef));
                if (ssd[ch] <= thresh) continue;
                phx_dctcoef dct_dc[4];
                phx_sub8x8_dct_dc(dct_dc, fenc[ch], PHX_FENC_STRIDE,
                                   p_dst[ch], PHX_FDEC_STRIDE); /* :303-305 */
                int nz_dc = b_trellis
                    ? phx_quant_chroma_dc_trellis(cb, dct_dc, chroma_qp,
                                                  PHX_DCT_CHROMA_DC, 0)
                    : phx_quant_2x2_dc(dct_dc,
                          qt->quant4_mf[cat][chroma_qp][0] >> 1,
                          qt->quant4_bias[cat][chroma_qp][0] << 1); /* :315 */
                if (!nz_dc) continue;
                if (!phx_mb_optimize_chroma_dc(qt, dct_dc, chroma_qp)) continue;
                phx_dctcoef level[4];
                phx_mb_zigzag2_dc(level, dct_dc);          /* :322 */
                memcpy(d->chroma_dct[ch][0], level, 4*sizeof(phx_dctcoef));
                d->chroma_nnz[ch][0] = 1;
                c->non_zero_count[phx_scan8[PHX_CHROMA_DC+ch]] = 1;
                phx_dctcoef block[4][16];
                memset(block, 0, sizeof(block));
                phx_mb_idct_dequant_2x2_dconly(dct_dc,
                    qt->dequant4_mf[cat], chroma_qp);       /* :326 */
                for (int k = 0; k < 4; k++) block[k][0] = dct_dc[k];
                phx_add8x8_idct_dc(p_dst[ch], PHX_FDEC_STRIDE, dct_dc); /* :331 */
                dc_nnz = 1;
            }
            d->cbp_chroma = dc_nnz;                         /* :334 */
            phx_mb_chroma_window(f, mb_x, mb_y, p_dst, PHX_FDEC_STRIDE, 1);
            return;
        }
    }

    int cbp_chroma = 0, dc_any = 0;
    for (int ch = 0; ch < 2; ch++) {
        phx_dctcoef dct4x4[4][16], dct_dc[4];
        phx_sub8x8_dct(dct4x4, fenc[ch], PHX_FENC_STRIDE,
                        p_dst[ch], PHX_FDEC_STRIDE);        /* :369-370 */
        phx_mb_dct2x2dc(dct_dc, dct4x4);                    /* :379 */
        for (int i = 0; i < 4; i++) dct4x4[i][0] = 0;       /* macroblock.c:92 */

        int i_decimate_score = b_decimate ? 0 : 7;          /* :347 */
        int nz_ac = 0;
        for (int i = 0; i < 4; i++) {
            int nz;
            if (b_trellis) {
                nz = phx_quant_4x4_trellis(cb, dct4x4[i], cat, chroma_qp,
                                           PHX_DCT_CHROMA_AC, 0); /* :388 */
                for (int k = 0; k < 16; k++)
                    d->chroma_dct[ch][1+i][k] = dct4x4[i][phx_zigzag4[k]];
            } else {
                nz = phx_quant_4x4(dct4x4[i], qt->quant4_mf[cat][chroma_qp],
                                    qt->quant4_bias[cat][chroma_qp]); /* :402 */
                for (int k = 0; k < 16; k++)
                    d->chroma_dct[ch][1+i][k] = dct4x4[i][phx_zigzag4[k]];
            }
            if (!nz) continue;
            nz_ac = 1;
            const int idx = 16 + ch*16 + i;                 /* :390/:408 */
            phx_dequant_4x4(dct4x4[i], qt->dequant4_mf[cat], chroma_qp);
            d->chroma_nnz[ch][1+i] = 1;
            d->nnz[idx] = 1;
            c->non_zero_count[phx_scan8[idx]] = 1;          /* :414-415 */
            if (i_decimate_score < 7)
                i_decimate_score += phx_mb_decimate_score(d->chroma_dct[ch][1+i], 16);
        }

        int nz_dc;
        if (b_trellis)
            nz_dc = phx_quant_chroma_dc_trellis(cb, dct_dc, chroma_qp,
                                                PHX_DCT_CHROMA_DC, 0); /* :420 */
        else
            nz_dc = phx_quant_2x2_dc(dct_dc,
                     qt->quant4_mf[cat][chroma_qp][0] >> 1,
                     qt->quant4_bias[cat][chroma_qp][0] << 1); /* :425-426 */
        phx_dctcoef level[4];
        phx_mb_zigzag2_dc(level, dct_dc);                   /* :457/:477 */
        memcpy(d->chroma_dct[ch][0], level, 4*sizeof(phx_dctcoef));
        d->chroma_nnz[ch][0] = (uint8_t)nz_dc;
        c->non_zero_count[phx_scan8[PHX_CHROMA_DC+ch]] = (uint8_t)nz_dc;
        dc_any |= nz_dc;

        if (i_decimate_score < 7 || !nz_ac) {              /* :431 */
            for (int i = 0; i < 4; i++) {
                const int idx = 16 + ch*16 + i;
                d->chroma_nnz[ch][1+i] = 0;
                d->nnz[idx] = 0;
                c->non_zero_count[phx_scan8[idx]] = 0;
                memset(d->chroma_dct[ch][1+i], 0, 16*sizeof(phx_dctcoef));
            }
            if (!nz_dc) continue;                          /* :440 */
            if (!phx_mb_optimize_chroma_dc(qt, dct_dc, chroma_qp)) {
                d->chroma_nnz[ch][0] = 0;
                c->non_zero_count[phx_scan8[PHX_CHROMA_DC+ch]] = 0;
                dc_any = 0;
                continue;
            }
            phx_mb_idct_dequant_2x2_dconly(dct_dc, qt->dequant4_mf[cat], chroma_qp);
            phx_add8x8_idct_dc(p_dst[ch], PHX_FDEC_STRIDE, dct_dc); /* :462 */
        } else {
            cbp_chroma |= 1;                               /* :470 */
            if (nz_dc)
                phx_mb_idct_dequant_2x2_dc(dct4x4, dct_dc, qt->dequant4_mf[cat], chroma_qp);
            phx_add8x8_idct(p_dst[ch], PHX_FDEC_STRIDE, dct4x4); /* :483 */
        }
    }
    d->cbp_chroma = cbp_chroma + (dc_any || cbp_chroma);   /* :487-489 */
    phx_mb_chroma_window(f, mb_x, mb_y, p_dst, PHX_FDEC_STRIDE, 1);
}

/* x264_mb_encode_i8x8 — encoder/macroblock.h:168-211, called from
 * macroblock_encode_internal's I_8x8 branch (macroblock.c:713-737).
 * h->dct.luma8x8[4] is published through i8x8_out (64 coefficients each).
 * SHADER: B — one invocation per 8x8 block. */
static void phx_mb_encode_i8x8(const phx_quant_tables_t *qt,
                                const uint8_t *fenc, uint8_t *p_dst,
                                int i_qp, int mode, int neighbour8,
                                int idx, phx_mb_decision_t *d,
                                phx_mb_cache_t *c, phx_cabac_t *cb,
                                const phx_param_t *p,
                                phx_mb_i8x8_block_t i8x8_out[4])
{
    static const uint8_t pred_neighbours[12] = {          /* macroblock.h:43 */
        PHX_MB_TOP, PHX_MB_LEFT, PHX_MB_LEFT|PHX_MB_TOP,
        PHX_MB_TOP|PHX_MB_TOPRIGHT,
        PHX_MB_LEFT|PHX_MB_TOPLEFT|PHX_MB_TOP,
        PHX_MB_LEFT|PHX_MB_TOPLEFT|PHX_MB_TOP,
        PHX_MB_LEFT|PHX_MB_TOPLEFT|PHX_MB_TOP,
        PHX_MB_TOP|PHX_MB_TOPRIGHT, PHX_MB_LEFT, PHX_MB_LEFT,
        PHX_MB_TOP, 0
    };
    /* phx_predict.c:857-859 installs DC at slot 0 and V at slot 2, while
     * x264's I_PRED_4x4 enum (common/predict.h:75-88, reused by I8x8) has
     * V=0 and DC=2.  Remap here instead of editing the unowned predictor
     * module.  SHADER: B — constant mode mapping. */
    static const uint8_t pf_index[12] = { 2, 1, 0, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    phx_predict8x8_t pred8[12];
    void (*filter)(uint8_t *, uint8_t *, int, int, int);
    uint8_t edge[36];
    phx_dctcoef dct8[64];
    const int x = (idx & 1) * 8, y = (idx >> 1) * 8;
    uint8_t *dst = p_dst + y * PHX_FDEC_STRIDE + x;
    const uint8_t *src = fenc + y * PHX_FENC_STRIDE + x;

    phx_predict_8x8_init(pred8, &filter);                 /* predict.c:977 */
    filter(dst, edge, PHX_FDEC_STRIDE, neighbour8,
           pred_neighbours[mode & 15]);                   /* macroblock.h:180 */
    pred8[pf_index[mode & 15]](dst, edge, PHX_FDEC_STRIDE); /* :184 */

    phx_sub8x8_dct8(dct8, src, PHX_FENC_STRIDE, dst, PHX_FDEC_STRIDE); /* :199 */
    int nz;
    if (cb && p && p->i_trellis > 0 && i_qp <= PHX_QP_MAX_SPEC) {
        /* rdo.c:1159-1184; M8 documents that the 64-coefficient trellis is
         * not bit-exact yet (phx_rdo.h Unsupported #1), so the golden path
         * for row 19 is the deadzone quantizer below. */
        nz = phx_quant_8x8_trellis(cb, dct8, 0, i_qp, PHX_DCT_LUMA_8x8, 1);
    } else {
        nz = phx_quant_8x8(dct8, qt->quant8_mf[0][i_qp],
                            qt->quant8_bias[0][i_qp]);    /* macroblock.h:206 */
    }
    if (i8x8_out) {
        for (int k = 0; k < 64; k++)
            i8x8_out[idx].coef[k] = dct8[phx_zigzag8[k]];  /* :204 */
        i8x8_out[idx].nnz = (uint8_t)(nz ? 1 : 0);
    }
    if (nz) {
        d->cbp_luma |= 1 << idx;                           /* :202 */
        phx_dequant_8x8(dct8, qt->dequant8_mf[0], i_qp);   /* :205 */
        phx_add8x8_idct8(dst, PHX_FDEC_STRIDE, dct8);      /* :206 */
    }
    phx_mb_store_8x8_nnz(d, c, idx, nz);                   /* :207/:210 */
}

/* Inter luma residual — macroblock_encode_internal's inter branch,
 * encoder/macroblock.c:801-914.  `b8` selects the 8x8 transform (:801-838)
 * or the 4x4x4 path (:843-914); both feed FDEC through x264's IDCT.
 * SHADER: B — one serial residual encode per MB. */
static void phx_mb_encode_inter_luma(const phx_param_t *p,
                                      const phx_quant_tables_t *qt,
                                      const uint8_t *fenc, uint8_t *p_dst,
                                      int i_qp, int b8,
                                      phx_mb_decision_t *d, phx_mb_cache_t *c,
                                      phx_cabac_t *cb,
                                      phx_mb_i8x8_block_t i8x8_out[4])
{
    const int b_trellis = cb && p->i_trellis > 0 && i_qp <= PHX_QP_MAX_SPEC;
    int decimate = p->b_dct_decimate;
    int decimate_mb = 0;

    if (b8) {
        /* macroblock.c:801-838 — quant_cat CQM_8PY (set.h:38), stored in
         * phx_quant_tables_t's luma 8x8 list index 1. */
        phx_dctcoef dct8x8[4][64];
        decimate &= !(b_trellis && p->b_cabac);            /* :804 */
        phx_sub16x16_dct8(dct8x8, fenc, PHX_FENC_STRIDE,
                           p_dst, PHX_FDEC_STRIDE);         /* :810 */
        int plane_cbp = 0;
        for (int idx = 0; idx < 4; idx++) {
            int nz = b_trellis
                ? phx_quant_8x8_trellis(cb, dct8x8[idx], 1, i_qp,
                                        PHX_DCT_LUMA_8x8, 0)
                : phx_quant_8x8(dct8x8[idx], qt->quant8_mf[1][i_qp],
                                 qt->quant8_bias[1][i_qp]); /* :816 */
            if (!nz) continue;
            if (i8x8_out)
                for (int k = 0; k < 64; k++)
                    i8x8_out[idx].coef[k] = dct8x8[idx][phx_zigzag8[k]]; /* :820 */
            if (decimate) {
                int s = phx_mb_decimate_score(
                    i8x8_out ? i8x8_out[idx].coef : dct8x8[idx], 64); /* :823 */
                decimate_mb += s;
                if (s >= 4) plane_cbp |= 1 << idx;
            } else {
                plane_cbp |= 1 << idx;
            }
        }
        if (decimate_mb >= 6 || !decimate) {
            d->cbp_luma |= plane_cbp;                      /* :832 */
            for (int idx = 0; idx < 4; idx++) {
                if (!(plane_cbp & (1 << idx))) continue;
                phx_dequant_8x8(dct8x8[idx], qt->dequant8_mf[1], i_qp); /* :834 */
                phx_add8x8_idct8(p_dst + (idx&1)*8 + (idx>>1)*8*PHX_FDEC_STRIDE,
                                  PHX_FDEC_STRIDE, dct8x8[idx]);       /* :835 */
                phx_mb_store_8x8_nnz(d, c, idx, 1);        /* :836 */
            }
        }
    } else {
        /* macroblock.c:843-914 — quant_cat CQM_4PY (set.h:31). */
        phx_dctcoef dct4x4[16][16];
        phx_sub16x16_dct(dct4x4, fenc, PHX_FENC_STRIDE,
                          p_dst, PHX_FDEC_STRIDE);          /* :849 */
        int plane_cbp = 0;
        for (int i8 = 0; i8 < 4; i8++) {
            int i_decimate_8x8 = decimate ? 0 : 6;          /* :855 */
            int nnz8x8 = 0;
            if (b_trellis) {
                for (int j = 0; j < 4; j++) {
                    const int idx = i8*4 + j;
                    int nz = phx_quant_4x4_trellis(cb, dct4x4[idx], 1, i_qp,
                                                   PHX_DCT_LUMA_4x4, 0);
                    if (!nz) continue;
                    phx_dctcoef scan[16];
                    for (int k = 0; k < 16; k++) scan[k] = dct4x4[idx][phx_zigzag4[k]];
                    phx_mb_store_block(d, c, idx, scan, 1);  /* :866 */
                    phx_dequant_4x4(dct4x4[idx], qt->dequant4_mf[1], i_qp);
                    if (i_decimate_8x8 < 6)
                        i_decimate_8x8 += phx_mb_decimate_score(scan, 16);
                    nnz8x8 = 1;
                }
            } else {
                int nz = phx_quant_4x4x4(&dct4x4[i8*4], qt->quant4_mf[1][i_qp],
                                          qt->quant4_bias[1][i_qp]); /* :887 */
                nnz8x8 = nz;
                for (int j = 0; j < 4; j++) {
                    if (!(nz & (1 << j))) continue;
                    const int idx = i8*4 + j;
                    phx_dctcoef scan[16];
                    for (int k = 0; k < 16; k++) scan[k] = dct4x4[idx][phx_zigzag4[k]];
                    phx_mb_store_block(d, c, idx, scan, 1);  /* :892 */
                    phx_dequant_4x4(dct4x4[idx], qt->dequant4_mf[1], i_qp);
                    if (i_decimate_8x8 < 6)
                        i_decimate_8x8 += phx_mb_decimate_score(scan, 16);
                }
            }
            if (nnz8x8) {
                decimate_mb += i_decimate_8x8;
                if (i_decimate_8x8 >= 4) plane_cbp |= 1 << i8; /* :900-903 */
                else phx_mb_store_8x8_nnz(d, c, i8, 0);        /* :901 */
            }
        }
        if (decimate_mb < 6) {
            for (int i = 0; i < 16; i++) {                    /* :907 */
                const int slot = phx_scan8[i];
                d->nnz[slot] = 0;
                c->non_zero_count[slot] = 0;
            }
        } else {
            d->cbp_luma |= plane_cbp;                          /* :911 */
            for (int i8 = 0; i8 < 4; i8++)
                if (plane_cbp & (1 << i8))
                    phx_add8x8_idct(p_dst + (i8&1)*8 + (i8>>1)*8*PHX_FDEC_STRIDE,
                                     PHX_FDEC_STRIDE, &dct4x4[i8*4]); /* :913 */
        }
    }
}

/* x264_mb_mc() for P_L0 with a single L0 reference — common/macroblock.c:
 * 200-244 and :34-72 (mb_mc_0xywh).  Every partition's MV is clipped first
 * (analyse.c:336-337) and then offset by 16*x / 16*y qpel, exactly as x264
 * does when it points mc_luma at the MB origin.  SHADER: B — serial MC per
 * partition, read-only fref. */
static int phx_mb_mc_l0_part(const phx_param_t *p, const phx_frame_t *fref,
                              phx_frame_t *f, uint8_t *p_dst, int mb_x, int mb_y,
                              const phx_mb_decision_t *d)
{
    (void)p;
    if (!fref || !fref->plane || fref->i_width != f->i_width ||
        fref->i_height != f->i_height || f->i_width <= 0 || f->i_height <= 0)
        return -1;
    const int mb_w = f->i_width / 16, mb_h = f->i_height / 16;
    const int mv_min_x = 4 * (-16 * mb_x - 24);
    const int mv_max_x = 4 * (16 * (mb_w - mb_x - 1) + 24);
    const int mv_min_y = 4 * (-16 * mb_y - 24);
    const int mv_max_y = 4 * (16 * (mb_h - mb_y - 1) + 24);
    /* (x,y,w,h) in 4x4-block units + the 4x4 MV cache index, per
     * x264_mb_mc()'s mb_mc_0xywh() calls. */
    static const int d1616[1][5] = { {0,0,4,4,0} };
    static const int d168[2][5]  = { {0,0,4,2,0}, {0,2,4,2,8} };
    static const int d816[2][5]  = { {0,0,2,4,0}, {2,0,2,4,2} };
    static const int d88[4][5]   = { {0,0,2,2,0}, {2,0,2,2,2},
                                     {0,2,2,2,8}, {2,2,2,2,10} };
    const int (*part)[5];
    int n;
    if (d->i_partition == PHX_D_16x16)      { part = d1616; n = 1; }
    else if (d->i_partition == PHX_D_16x8)  { part = d168;  n = 2; }
    else if (d->i_partition == PHX_D_8x16)  { part = d816;  n = 2; }
    else if (d->i_partition == PHX_D_8x8)   { part = d88;   n = 4; }
    else return -1;

    for (int i = 0; i < n; i++) {
        const int x = part[i][0], y = part[i][1];
        const int w = part[i][2], h = part[i][3], blk = part[i][4];
        if (d->ref[0][blk] != 0) return -1;    /* only L0 ref 0 is supplied */
        const int mvx = phx_mb_clip3i(d->mv[0][blk][0], mv_min_x, mv_max_x)
                      + 16 * x;                /* macroblock.c:37 */
        const int mvy = phx_mb_clip3i(d->mv[0][blk][1], mv_min_y, mv_max_y)
                      + 16 * y;                /* macroblock.c:38 */
        if (phx_mc_luma_mb(fref, p_dst + y*4*PHX_FDEC_STRIDE + x*4,
                            PHX_FDEC_STRIDE, mb_x, mb_y, mvx, mvy,
                            4*w, 4*h) < 0) return -1;
        /* common/macroblock.c:61-64 — 4:2:0 chroma MC, v_shift == 1. */
        const int cs = f->i_stride_planes[1];
        const size_t c_off = (size_t)((mb_y*16 + y*4)/2)*cs
                           + (size_t)((mb_x*16 + x*4)/2)*2;
        const size_t r_off = (size_t)((mb_y*16 + y*4)/2)*fref->i_stride_planes[1]
                           + (size_t)((mb_x*16 + x*4)/2)*2;
        uint8_t cu[16*16], cv[16*16];
        if (mvx | mvy)
            phx_mc_chroma(cu, 16, cv, 16, fref->plane_chroma + r_off,
                          fref->i_stride_planes[1], mvx, 2*mvy >> 1,
                          2*w, 2*h);
        else
            phx_mc_deinterleave_chroma(cu, 16, cv, 16,
                                        fref->plane_chroma + r_off,
                                        fref->i_stride_planes[1], 2*w, 2*h);
        for (int cy = 0; cy < 2*h; cy++)
            for (int cx = 0; cx < 2*w; cx++) {
                f->plane_chroma[c_off + (size_t)cy*cs + 2*cx]   = cu[cy*16+cx];
                f->plane_chroma[c_off + (size_t)cy*cs + 2*cx+1] = cv[cy*16+cx];
            }
    }
    return 0;
}

/* x264_macroblock_encode — reference/x264/encoder/macroblock.c:974-982,
 * dispatching macroblock_encode_internal (:618-972).  Cache-nnz and cbp
 * stores mirror common/macroblock.c:1680/1761-1779; reconstructed pixels are
 * written into f->plane (or the caller's cache->p_fdec) because phx_frame_t
 * has no separate fdec plane (common/frame.h:37-140).  Rows 18-19 add the
 * residual-carrying P_L0 and I_8x8 branches (macroblock.c:713-737/:768-914).
 * SHADER: B — the single B-stage entry point for one macroblock. */
int phx_macroblock_encode_ex2(const phx_param_t *p, phx_frame_t *f,
                              const phx_frame_t *fref,
                              phx_mb_cache_t *cache, int mb_x, int mb_y,
                              phx_mb_decision_t *d, phx_cabac_t *cb,
                              phx_mb_i8x8_block_t i8x8_out[4])
{
    if (!p || !f || !cache || !d || !f->plane) return -1;

    /* Serial B-stage carry-over for the chroma-DC trellis wrapper below:
     * x264's rdo.c:1114-1140 reads the live CABAC context state.  SHADER: C. */
    phx_mb_active_cabac = cb;

    const int mb_w = f->i_width / 16, mb_h = f->i_height / 16;
    if (mb_x < 0 || mb_x >= mb_w || mb_y < 0 || mb_y >= mb_h) return -1;
    if (d->i_qp < 0 || d->i_qp > PHX_QUANT_QP_MAX_SPEC) return -1;

    const int inter_l0 = (d->mb_type == PHX_P_L0 &&
                          (d->i_partition == PHX_D_16x16 ||
                           d->i_partition == PHX_D_16x8 ||
                           d->i_partition == PHX_D_8x16 ||
                           d->i_partition == PHX_D_8x8));
    if (d->mb_type != PHX_I_16x16 && d->mb_type != PHX_I_4x4
        && d->mb_type != PHX_I_8x8 && d->mb_type != PHX_I_PCM && !inter_l0)
        return -1;
    if (inter_l0) {
        /* macroblock.c:666 reads p_fref[0][0]; only list-0 reference 0 is
         * supplied.  Inter chroma MC/residual needs both NV12 planes. */
        if (!fref || !fref->plane || !fref->plane_chroma ||
            !f->plane_chroma || fref->i_width != f->i_width ||
            fref->i_height != f->i_height ||
            fref->i_stride_planes[1] <= 0 || f->i_stride_planes[1] <= 0)
            return -1;
    }
    if (d->mb_type == PHX_I_8x8)
        for (int i = 0; i < 4; i++)
            if (d->intra8x8_pred_mode[i] < 0 || d->intra8x8_pred_mode[i] > 11)
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

    /* x264 keeps fenc/fdec separate (common/common.h:570-571).  Stage the
     * inter chroma source before MC overwrites f->plane_chroma. */
    uint8_t chroma_fenc[2][8*PHX_FENC_STRIDE];
    uint8_t *chroma_src[2] = { chroma_fenc[0], chroma_fenc[1] };
    uint8_t chroma_fdec_buf[2][PHX_FDEC_STRIDE*10 + PHX_FDEC_STRIDE];
    uint8_t *chroma_fdec[2] = { chroma_fdec_buf[0] + PHX_FDEC_STRIDE + 1,
                                chroma_fdec_buf[1] + PHX_FDEC_STRIDE + 1 };
    if (inter_l0) {
        phx_mb_chroma_window(f, mb_x, mb_y, chroma_src, PHX_FENC_STRIDE, 0);
        memset(chroma_fdec_buf, 0, sizeof(chroma_fdec_buf));
    }

    phx_mb_load_fdec_neighbours(f, mb_x, mb_y, p_dst, PHX_FDEC_STRIDE, cache);

    /* common/macroblock.c:1683/1722/1779 — keep the caller's p_fdec (the
     * analysis reconstruction frame, common/common.h:571) intact: x264 writes
     * reconstructed samples into fdec, never into fenc.  Standalone callers
     * without an FDEC keep the legacy f->plane target. */
    uint8_t *fdec_target = cache->p_fdec ? cache->p_fdec : plane_mb;
    int fdec_target_stride = cache->p_fdec ? cache->fdec_stride : f->i_stride;
    cache->b_transform_8x8 = (d->mb_type == PHX_I_8x8 ||
                              (inter_l0 && d->b_transform_8x8)) ? 1 : 0; /* :715/:779 */
    cache->i_cbp_luma = 0;                                   /* macroblock.c:624 */
    cache->i_cbp_chroma = 0;                                 /* macroblock.c:265 */
    cache->i_qp = d->i_qp;                                   /* macroblock.c:620 */
    /* Capture the analysis record's residual decision before the B stage
     * recomputes cbp from the quantized coefficients (macroblock.c:624/946). */
    const int dec_cbp_luma = d->cbp_luma, dec_cbp_chroma = d->cbp_chroma;
    d->cbp_luma = 0;
    d->cbp_chroma = 0;
    d->skip = 0;
    d->b_transform_8x8 = cache->b_transform_8x8;

    if (inter_l0) {
        /* macroblock.c:654-667/:769-775 + common/macroblock.c:200-244/:34-72.
         * The analysis record decides whether a residual is coded: cbp == 0
         * keeps the established no-residual inter handoff (macroblock.c:
         * 624-626/946-951); any residual-carrying decision takes the full
         * inter encode (macroblock.c:801-914/:259-498/:940-943). */
        const int no_residual = (dec_cbp_luma == 0 && dec_cbp_chroma == 0);
        if (phx_mb_mc_l0_part(p, fref, f, p_dst, mb_x, mb_y, d) < 0) return -1;
        if (no_residual) {
            phx_mb_clear_inter_residual(d, cache);
        } else {
            phx_mb_chroma_window(f, mb_x, mb_y, chroma_fdec, PHX_FDEC_STRIDE, 0);
            phx_mb_encode_inter_luma(p, &qt, fenc, p_dst, d->i_qp,
                                      cache->b_transform_8x8, d, cache, cb, i8x8_out);
            phx_mb_encode_inter_chroma(p, &qt, f, mb_x, mb_y,
                                        chroma_src, chroma_fdec, d->i_qp, d, cache, cb);
        }
    } else if (d->mb_type == PHX_I_8x8) {
        /* macroblock.c:713-737 + macroblock.h:168-211. */
        int n8[4] = { phx_mb_neighbour4(cache, 0),
                      phx_mb_neighbour4(cache, 5),
                      phx_mb_neighbour4(cache, 2),
                      PHX_MB_LEFT | PHX_MB_TOP | PHX_MB_TOPLEFT }; /* :489-498 */
        for (int i = 0; i < 4; i++)
            phx_mb_encode_i8x8(&qt, fenc, p_dst, d->i_qp,
                                d->intra8x8_pred_mode[i], n8[i], i,
                                d, cache, cb, p, i8x8_out);
    } else if (d->mb_type == PHX_I_16x16) {
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

    /* encoder/macroblock.c:632 — copy the 16 reconstructed rows back to the
     * caller's FDEC window.  The braces are required: without them the memcpy
     * ran once after the loop with y == 16 and wrote past the destination.
     * SHADER: B — one invocation per MB. */
    for (int y = 0; y < 16; y++) {                           /* macroblock.c:632 */
        memcpy(fdec_target + y * fdec_target_stride,
               p_dst + y * PHX_FDEC_STRIDE, 16);
    }
    return 0;
}

/* Backward-compatible entry point: callers that do not need the 64-coefficient
 * arrays (all existing C-stage writers) keep using this symbol. */
int phx_macroblock_encode_ex(const phx_param_t *p, phx_frame_t *f,
                             const phx_frame_t *fref,
                             phx_mb_cache_t *cache, int mb_x, int mb_y,
                             phx_mb_decision_t *d, phx_cabac_t *cb)
{
    return phx_macroblock_encode_ex2(p, f, fref, cache, mb_x, mb_y, d, cb, NULL);
}

/* Plain-deadzone wrapper: unit tests and CAVLC call this path. */
int phx_macroblock_encode(const phx_param_t *p, phx_frame_t *f,
                          phx_mb_cache_t *cache, int mb_x, int mb_y,
                          phx_mb_decision_t *d)
{
    return phx_macroblock_encode_ex(p, f, NULL, cache, mb_x, mb_y, d, NULL);
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
    /* macroblock.c:384/419 — chroma QNS uses the same final-encode flag:
     * analyse.c:3724 sets b_trellis = i_trellis (no mbrd gate). */
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
                /* encoder/cabac.c:1074 writes U blocks (scan8 16..19) before
                 * V blocks (32..35).  phx_encoder.c:505-508 maps those scan8
                 * ranges to chroma_dct[0][1+i] and chroma_dct[1][1+i]
                 * respectively, so plane ch must be stored in slot ch.  A
                 * ch==0?1:0 swap here made U/V arrive at the C stage in the
                 * wrong order (first divergent CABAC bin: chroma AC U sig flag
                 * ctx=152).  SHADER: B — one serial store per block. */
                const int dest = ch;
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
