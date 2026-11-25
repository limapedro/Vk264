/* Function-for-function port of x264's 4:2:0 chroma macroblock encode path.
 * Every ported statement names the exact vendored x264 source line it follows.
 * The NV12 chroma plane is interleaved U/V, so the two 8x8 sample windows are
 * de-interleaved into separate FENC_STRIDE/FDEC_STRIDE buffers exactly like
 * x264's p_fenc[1]/p_fdec[1] and p_fenc[2]/p_fdec[2]
 * (reference/x264/common/common.h:570-571).
 *
 * SHADER: B for every function in this file: one serial invocation per MB.
 */
#include "phx_chroma.h"
#include "phx_predict.h"
#include "phx_dct.h"
#include "phx_quant.h"
#include "phx_tables.h"

#include <string.h>

/* common/set.h:34 — CQM_4IC (intra chroma). */
#define PHX_CQM_4IC 2
/* common/base.h:178 — CHROMA_DC = 49; +0 is U, +1 is V. */
#define PHX_CHROMA_DC 49
/* common/common.h:93-108 — X264_MAX(), used by the extended-QP derivation. */
#define PHX_MAX(a, b) ((a) > (b) ? (a) : (b))

/* common/macroblock.h:181-187 — x264_zigzag_scan4[0] (frame), copied from
 * phx_mbencode.c:28-30 so this stage is self-contained.
 * SHADER: B — constant coefficient order for the B->C handoff. */
static const uint8_t phx_zigzag4[16] = {
     0,  4,  1,  2,  5,  8, 12,  9,  6,  3,  7, 10, 13, 14, 11, 15
};

/* encoder/macroblock.c:35-41 — zigzag_scan_2x2_dc().
 * SHADER: B — DC coefficient order written into d->chroma_dct[ch][0]. */
static void phx_zigzag_scan_2x2_dc(phx_dctcoef level[4], const phx_dctcoef dct[4])
{
    level[0] = dct[0*2+0];                             /* macroblock.c:37 */
    level[1] = dct[0*2+1];                             /* macroblock.c:38 */
    level[2] = dct[1*2+0];                             /* macroblock.c:39 */
    level[3] = dct[1*2+1];                             /* macroblock.c:40 */
}

/* encoder/macroblock.c:56-70 — idct_dequant_2x2_dc().  No assembly version
 * exists in x264 (macroblock.c:32), so this is the authoritative C form.
 * SHADER: B — one invocation per chroma DC block. */
static void phx_idct_dequant_2x2_dc(phx_dctcoef dct[4], phx_dctcoef dct4x4[4][16],
                                    const int dequant_mf[6][16], int i_qp)
{
    int d0 = dct[0] + dct[1];                          /* macroblock.c:57 */
    int d1 = dct[2] + dct[3];                          /* macroblock.c:58 */
    int d2 = dct[0] - dct[1];                          /* macroblock.c:59 */
    int d3 = dct[2] - dct[3];                          /* macroblock.c:60 */
    int dmf = dequant_mf[i_qp%6][0] << i_qp/6;         /* macroblock.c:61 */
    dct4x4[0][0] = (phx_dctcoef)((d0 + d1) * dmf >> 5); /* macroblock.c:66 */
    dct4x4[1][0] = (phx_dctcoef)((d0 - d1) * dmf >> 5); /* macroblock.c:67 */
    dct4x4[2][0] = (phx_dctcoef)((d2 + d3) * dmf >> 5); /* macroblock.c:68 */
    dct4x4[3][0] = (phx_dctcoef)((d2 - d3) * dmf >> 5); /* macroblock.c:69 */
}

/* encoder/macroblock.c:72-79 — idct_dequant_2x2_dconly().
 * SHADER: B — one invocation per DC-only chroma block. */
static void phx_idct_dequant_2x2_dconly(phx_dctcoef dct[4],
                                        const int dequant_mf[6][16], int i_qp)
{
    int d0 = dct[0] + dct[1];                          /* macroblock.c:57 */
    int d1 = dct[2] + dct[3];                          /* macroblock.c:58 */
    int d2 = dct[0] - dct[1];                          /* macroblock.c:59 */
    int d3 = dct[2] - dct[3];                          /* macroblock.c:60 */
    int dmf = dequant_mf[i_qp%6][0] << i_qp/6;         /* macroblock.c:61 */
    dct[0] = (phx_dctcoef)((d0 + d1) * dmf >> 5);      /* macroblock.c:75 */
    dct[1] = (phx_dctcoef)((d0 - d1) * dmf >> 5);      /* macroblock.c:76 */
    dct[2] = (phx_dctcoef)((d2 + d3) * dmf >> 5);      /* macroblock.c:77 */
    dct[3] = (phx_dctcoef)((d2 - d3) * dmf >> 5);      /* macroblock.c:78 */
}

/* encoder/macroblock.c:82-96 — dct2x2dc().  The 2x2 Hadamard of the four
 * 4x4 block DC coefficients; the AC coefficients are zeroed after extraction.
 * SHADER: B — one invocation per chroma 8x8 block. */
static void phx_dct2x2dc(phx_dctcoef d[4], phx_dctcoef dct4x4[4][16])
{
    int d0 = dct4x4[0][0] + dct4x4[1][0];              /* macroblock.c:84 */
    int d1 = dct4x4[2][0] + dct4x4[3][0];              /* macroblock.c:85 */
    int d2 = dct4x4[0][0] - dct4x4[1][0];              /* macroblock.c:86 */
    int d3 = dct4x4[2][0] - dct4x4[3][0];              /* macroblock.c:87 */
    d[0] = (phx_dctcoef)(d0 + d1);                     /* macroblock.c:88 */
    d[2] = (phx_dctcoef)(d2 + d3);                     /* macroblock.c:89 */
    d[1] = (phx_dctcoef)(d0 - d1);                     /* macroblock.c:90 */
    d[3] = (phx_dctcoef)(d2 - d3);                     /* macroblock.c:91 */
    for (int i = 0; i < 4; i++)
        dct4x4[i][0] = 0;                              /* macroblock.c:92-95 */
}

/* encoder/analyse.c:485-492 — chroma_mode_available[5][5] indexed with the
 * result of predict_chroma_mode_available (analyse.c:537-542).  Used here only
 * to keep an explicitly requested mode legal at frame edges; the predictor
 * itself is x264's (macroblock.c:934-935).
 * SHADER: B — neighbour-derived mode validation for one MB. */
static int phx_chroma_mode_available(int neighbour, int mode)
{
    int idx = neighbour & (PHX_MB_TOP | PHX_MB_LEFT | PHX_MB_TOPLEFT);
    idx = (idx == (PHX_MB_TOP | PHX_MB_LEFT | PHX_MB_TOPLEFT))
        ? 4 : idx & (PHX_MB_TOP | PHX_MB_LEFT);        /* analyse.c:539-540 */
    /* I_PRED_CHROMA_* values: DC=0, H=1, V=2, P=3, DC_LEFT=4, DC_TOP=5,
     * DC_128=6 (common/predict.h:34-44). */
    static const int8_t avail[5][5] = {
        { 6, -1, -1, -1, -1 },                         /* analyse.c:487 */
        { 4,  1, -1, -1, -1 },                         /* analyse.c:488 */
        { 5,  2, -1, -1, -1 },                         /* analyse.c:489 */
        { 2,  1,  0, -1, -1 },                         /* analyse.c:490 */
        { 2,  1,  0,  3, -1 }                          /* analyse.c:491 */
    };
    for (int i = 0; i < 5 && avail[idx][i] >= 0; i++)
        if (avail[idx][i] == mode) return 1;
    return 0;
}

static int phx_clip3(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Stage the 8x8 chroma source and reconstructed neighbours from the NV12
 * plane into per-channel FENC/FDEC windows.  U is the even byte, V the odd
 * byte of each pair (frame.c:89-99/240).  Out-of-frame neighbours are clamped
 * to the edge sample, which is what plane_expand_border replicates
 * (frame.c:535-553); cache->i_neighbour_intra (macroblock.c:673/783/801)
 * tells which sides really have a coded neighbour.
 * SHADER: B — per-MB neighbour staging into B-stage scratch windows. */
static void phx_chroma_load_windows(const phx_frame_t *f, int mb_x, int mb_y,
                                    uint8_t fenc[2][8*PHX_FENC_STRIDE],
                                    uint8_t *p_dst[2])
{
    const int x0 = mb_x * 8, y0 = mb_y * 8;           /* CHROMA_H/V_SHIFT=1 */
    const int cw = f->i_width_planes[1];
    const int ch = f->i_lines_planes[1];
    const int s = f->i_stride_planes[1];
    const uint8_t *plane = f->plane_chroma;

    for (int c = 0; c < 2; c++) {
        /* Source window: NV12 de-interleave, FENC_STRIDE=16 (common.h:570). */
        for (int y = 0; y < 8; y++)                    /* macroblock.c:345 */
            for (int x = 0; x < 8; x++)                /* macroblock.c:345 */
                fenc[c][y*PHX_FENC_STRIDE + x] =
                    plane[(y0+y)*s + 2*(x0+x) + c];   /* frame.c:240 */

        /* Left column p[-1 + y*FDEC_STRIDE] and top-left p[-1-FDEC_STRIDE].
         * The predictor needs y=-1..7 (predict.c:289). */
        for (int y = -1; y < 8; y++) {
            const int sy = phx_clip3(y0 + y, 0, ch - 1);
            const int sx = phx_clip3(x0 - 1, 0, cw - 1);
            p_dst[c][-1 + y*PHX_FDEC_STRIDE] = plane[sy*s + 2*sx + c];
        }
        /* Top row p[x-FDEC_STRIDE] for x=0..7 (predict.c:272/288), using the
         * already-staged top-left sample at x=-1. */
        for (int x = 0; x < 8; x++) {
            const int sy = phx_clip3(y0 - 1, 0, ch - 1);
            const int sx = phx_clip3(x0 + x, 0, cw - 1);
            p_dst[c][x - PHX_FDEC_STRIDE] = plane[sy*s + 2*sx + c];
        }
    }
}

/* Write the reconstructed 8x8 U and V windows back into the interleaved NV12
 * plane (frame.c:89-99/240).  This is the chroma counterpart of
 * encoder/macroblock.c:632/636-637 (the copy of p_fdec into the frame).
 * SHADER: B — per-MB reconstruction write-back. */
static void phx_chroma_store(phx_frame_t *f, int mb_x, int mb_y,
                             uint8_t *p_dst[2])
{
    const int x0 = mb_x * 8, y0 = mb_y * 8;
    const int s = f->i_stride_planes[1];
    uint8_t *plane = f->plane_chroma;
    for (int y = 0; y < 8; y++)                        /* macroblock.c:632 */
        for (int x = 0; x < 8; x++) {
            plane[(y0+y)*s + 2*(x0+x) + 0] = p_dst[0][y*PHX_FDEC_STRIDE + x];
            plane[(y0+y)*s + 2*(x0+x) + 1] = p_dst[1][y*PHX_FDEC_STRIDE + x];
        }
}

/* x264_mb_encode_chroma / mb_encode_chroma_internal —
 * reference/x264/encoder/macroblock.c:492-498 and :259-498 (4:2:0 branch).
 * SHADER: B — the single B-stage chroma entry point for one macroblock. */
int phx_macroblock_encode_chroma(const phx_param_t *p, phx_frame_t *f,
                                 phx_mb_cache_t *cache, int mb_x, int mb_y,
                                 phx_mb_decision_t *d)
{
    if (!p || !f || !cache || !d || !f->plane || !f->plane_chroma) return -1;
    if (!f->i_width || !f->i_height || f->i_width % 16 || f->i_height % 16)
        return -1;
    /* 4:2:0 NV12 geometry only: frame.c:89-99. */
    if (f->i_width_planes[1] != f->i_width/2 ||
        f->i_lines_planes[1] != f->i_height/2 ||
        f->i_stride_planes[1] != f->i_stride) return -1;

    const int mb_w = f->i_width / 16, mb_h = f->i_height / 16;
    if (mb_x < 0 || mb_x >= mb_w || mb_y < 0 || mb_y >= mb_h) return -1;
    if (d->i_qp < 0 || d->i_qp > PHX_QUANT_QP_MAX_SPEC) return -1;
    if (d->chroma_pred_mode < 0 || d->chroma_pred_mode > 6) return -1;

    /* encoder.c:1585 — chroma_qp_table = i_chroma_qp_table + 12 +
     * pps->i_chroma_qp_index_offset; cost.c:106-110 clips the table index and
     * adds the extended-QP remainder. */
    int qp_idx = phx_clip3(PHX_CHROMA_QP_BASE_OFFSET +
                           p->i_chroma_qp_index_offset + d->i_qp,
                           0, (int)sizeof(phx_chroma_qp_base) - 1);
    const int chroma_qp = phx_chroma_qp_base[qp_idx]
                          + PHX_MAX(d->i_qp - PHX_QP_MAX_SPEC, 0);

    phx_quant_tables_t qt;
    phx_quant_tables_init(&qt);                        /* set.c:73-205 */

    uint8_t fenc[2][8*PHX_FENC_STRIDE];
    uint8_t fdec[2][PHX_FDEC_STRIDE*10 + PHX_FDEC_STRIDE];
    uint8_t *p_dst[2] = { fdec[0] + PHX_FDEC_STRIDE + 1,
                          fdec[1] + PHX_FDEC_STRIDE + 1 }; /* common.h:570-571 */
    memset(fdec, 0, sizeof(fdec));
    phx_chroma_load_windows(f, mb_x, mb_y, fenc, p_dst);

    int mode = d->chroma_pred_mode;
    if (!phx_chroma_mode_available(cache->i_neighbour_intra, mode))
        mode = 6; /* I_PRED_CHROMA_DC_128, analyse.c:487; frame.c:535-553 */
    d->chroma_pred_mode = (int8_t)mode;

    phx_predict_t pred8[7];
    phx_predict_8x8c_init(pred8);                       /* predict.c:936-945 */
    pred8[mode](p_dst[0], PHX_FDEC_STRIDE);             /* macroblock.c:934 */
    pred8[mode](p_dst[1], PHX_FDEC_STRIDE);             /* macroblock.c:935 */

    int cbp_chroma = 0;
    for (int ch = 0; ch < 2; ch++) {                    /* macroblock.c:343 */
        phx_dctcoef dct4x4[4][16];
        phx_dctcoef dct_dc[4];
        phx_dctcoef scan[16];

        /* Clear this MB's chroma nnz flags (macroblock.c:268-271/:291-292).
         * x264's M16 stores four bytes at scan8[16+16*ch] and scan8[18+16*ch];
         * the explicit per-block arrays are cleared too so the B->C record
         * never carries stale coefficients. */
        for (int i = 0; i < 4; i++) {
            const int idx = 16 + ch*16 + i;             /* macroblock.c:390 */
            cache->non_zero_count[phx_scan8[idx]] = 0;  /* macroblock.c:268-271 */
            d->nnz[idx] = 0;
            memset(d->chroma_dct[ch][1+i], 0, 16*sizeof(phx_dctcoef));
            d->chroma_nnz[ch][1+i] = 0;
        }
        cache->non_zero_count[phx_scan8[PHX_CHROMA_DC+ch]] = 0; /* :291-292 */
        memset(d->chroma_dct[ch][0], 0, 16*sizeof(phx_dctcoef));
        d->chroma_nnz[ch][0] = 0;

        phx_sub8x8_dct(dct4x4, fenc[ch], PHX_FENC_STRIDE,
                       p_dst[ch], PHX_FDEC_STRIDE);     /* macroblock.c:369-370 */
        phx_dct2x2dc(dct_dc, dct4x4);                   /* macroblock.c:379 */

        /* 4:2:0 non-trellis path (b_decimate is false for intra):
         * i_decimate_score = 7 (macroblock.c:347). */
        const int nz = phx_quant_4x4x4(dct4x4,
                        qt.quant4_mf[PHX_CQM_4IC][chroma_qp],
                        qt.quant4_bias[PHX_CQM_4IC][chroma_qp]); /* :402-403 */
        int nz_ac = nz;                                 /* macroblock.c:404 */
        for (int i = 0; i < 4; i++) {                   /* FOREACH_BIT :406 */
            if (!(nz & (1 << i))) continue;
            const int idx = 16 + ch*16 + i;             /* macroblock.c:408 */
            for (int k = 0; k < 16; k++)
                scan[k] = dct4x4[i][phx_zigzag4[k]];    /* macroblock.c:410 */
            memcpy(d->chroma_dct[ch][1+i], scan, 16*sizeof(phx_dctcoef));
            phx_dequant_4x4(dct4x4[i], qt.dequant4_mf[PHX_CQM_4IC],
                            chroma_qp);                 /* macroblock.c:411 */
            d->chroma_nnz[ch][1+i] = 1;                 /* macroblock.c:415 */
            d->nnz[idx] = 1;                            /* phx_mb_decision.h:39 */
            cache->non_zero_count[phx_scan8[idx]] = 1;  /* macroblock.c:414 */
        }

        /* 4:2:0 chroma DC: i_qp+3 with the >>1 / <<1 deadzone scaling
         * (macroblock.c:424-426).  chroma_qp <= 39 (macroblock.h:249-270),
         * so chroma_qp+3 stays inside quant4_mf[][0..51]. */
        const int nz_dc = phx_quant_2x2_dc(dct_dc,
            qt.quant4_mf[PHX_CQM_4IC][chroma_qp+3][0] >> 1,
            qt.quant4_bias[PHX_CQM_4IC][chroma_qp+3][0] << 1); /* :425-426 */
        d->chroma_nnz[ch][0] = (uint8_t)nz_dc;          /* macroblock.c:429 */
        cache->non_zero_count[phx_scan8[PHX_CHROMA_DC+ch]] = (uint8_t)nz_dc;
        phx_zigzag_scan_2x2_dc(scan, dct_dc);           /* macroblock.c:457/477 */
        memcpy(d->chroma_dct[ch][0], scan, 4*sizeof(phx_dctcoef));

        if (!nz_ac) {                                   /* macroblock.c:431 */
            /* Decimate the AC blocks; with no AC coefficients they are
             * already empty in the decision record (macroblock.c:434-440). */
            for (int i = 0; i < 4; i++) {
                const int idx = 16 + ch*16 + i;
                cache->non_zero_count[phx_scan8[idx]] = 0;
                d->nnz[idx] = 0;
                d->chroma_nnz[ch][1+i] = 0;
                memset(d->chroma_dct[ch][1+i], 0, 16*sizeof(phx_dctcoef));
            }
            if (!nz_dc) continue;                       /* macroblock.c:442-443 */
            const int dmf = qt.dequant4_mf[PHX_CQM_4IC][chroma_qp%6][0]
                            << (chroma_qp/6);           /* macroblock.c:247 */
            if (dmf <= 32*64 &&                         /* macroblock.c:250 */
                !phx_optimize_chroma_2x2_dc(dct_dc, dmf)) { /* :256/:294 */
                d->chroma_nnz[ch][0] = 0;               /* macroblock.c:446 */
                cache->non_zero_count[phx_scan8[PHX_CHROMA_DC+ch]] = 0;
                memset(d->chroma_dct[ch][0], 0, 16*sizeof(phx_dctcoef));
                continue;
            }
            phx_idct_dequant_2x2_dconly(dct_dc, qt.dequant4_mf[PHX_CQM_4IC],
                                       chroma_qp);      /* macroblock.c:458 */
            phx_add8x8_idct_dc(p_dst[ch], PHX_FDEC_STRIDE, dct_dc); /* :462 */
        } else {
            cbp_chroma = 1;                             /* macroblock.c:466 */
            if (nz_dc)
                phx_idct_dequant_2x2_dc(dct_dc, dct4x4,
                    qt.dequant4_mf[PHX_CQM_4IC], chroma_qp); /* :478 */
            phx_add8x8_idct(p_dst[ch], PHX_FDEC_STRIDE, dct4x4); /* :483 */
        }
    }

    /* 0 = none, 1 = DC only, 2 = DC+AC (macroblock.c:487-489). */
    d->cbp_chroma = cbp_chroma +
        (d->chroma_nnz[0][0] | d->chroma_nnz[1][0] | cbp_chroma);
    cache->i_cbp_chroma = d->cbp_chroma;                /* macroblock.c:1779 */

    d->i_cbp = (d->cbp_chroma << 4) | d->cbp_luma;      /* macroblock.c:946 */
    if (p->b_cabac)                                    /* macroblock.c:947 */
        d->i_cbp |= (d->nnz[phx_scan8[48]] << 8)        /* LUMA_DC :948 */
                 | (d->chroma_nnz[0][0] << 9)           /* CHROMA_DC+0 :949 */
                 | (d->chroma_nnz[1][0] << 10);         /* CHROMA_DC+1 :950 */

    phx_chroma_store(f, mb_x, mb_y, p_dst);             /* macroblock.c:632/636 */
    return 0;
}
