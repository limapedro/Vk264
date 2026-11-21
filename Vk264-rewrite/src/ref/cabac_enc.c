/* SPDX-License-Identifier: GPL-2.0-or-later
 * CABAC I-slice writer (I16 macroblocks, zero residual DC), ported from
 * x264 encoder/cabac.c. */
#include "cabac_enc.h"
#include "intra16.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* x264_cabac_mb_type_intra for I_16x16 in an I-slice. */
static void cabac_mb_type_i16(vk_cabac_t *cb, int ctx_left, int ctx_top,
                              int mode)
{
    int ctx = 0;
    if (ctx_left) ctx++;   /* left MB type != I_4x4 */
    if (ctx_top) ctx++;
    int ctx0 = 3 + ctx;
    vk_cabac_decision_noup(cb, ctx0, 1);       /* I16 vs I4x4/I8x8/PCM */
    vk_cabac_terminal(cb);
    vk_cabac_decision_noup(cb, 3+3, 0);        /* cbp_luma == 0 */
    vk_cabac_decision_noup(cb, 3+4, 0);        /* cbp_chroma == 0 */
    int pred = mode <= 3 ? mode : 2;
    vk_cabac_decision(cb, 3+6, pred >> 1);
    vk_cabac_decision_noup(cb, 3+7, pred & 1);
}

/* x264 cabac_intra_chroma_pred_mode */
static void cabac_chroma_mode(vk_cabac_t *cb, int mode)
{
    int m = mode <= 3 ? mode : 0;
    vk_cabac_decision_noup(cb, 64, m > 0);
    if (m > 0) {
        vk_cabac_decision(cb, 64+3, m > 1);
        if (m > 1) vk_cabac_decision_noup(cb, 64+3, m > 2);
    }
}

/* x264 cabac_qp_delta with dqp == 0, last_dqp == 0 */
static void cabac_qp_delta0(vk_cabac_t *cb)
{
    vk_cabac_decision_noup(cb, 60, 0);
}

int vk_cabac_encode_idr(vk_bs_t *bs, vk_cabac_t *cb,
                        const uint8_t *frame, int w, int h, int qp,
                        int frame_num)
{
    (void)frame; (void)qp; (void)frame_num;
    int mw = w/16, mh = h/16;
    uint8_t *data = bs->p;
    vk_cabac_start(cb, data, bs->end);

    const char *replay = getenv("CABAC_REPLAY");
    if (replay) {
        FILE *f = fopen(replay, "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                int ctx, b;
                const char *p = line;
                if (p[0] == 'D' && p[1] == ' ') p += 2;
                if (sscanf(p, "ctx=%d b=%d", &ctx, &b) == 2)
                    vk_cabac_decision(cb, ctx, b);
                else if (sscanf(line, "BYPASS %d", &b) == 1)
                    vk_cabac_bypass(cb, b);
                else if (p[0] == 'T') vk_cabac_terminal(cb);
                else if (sscanf(p, "UEB %d %d", &ctx, &b) == 2)
                    vk_cabac_ue_bypass(cb, ctx, b);
            }
            fclose(f);
            int bytes = vk_cabac_flush(cb, 0);
            bs->p = data + bytes;
            return bytes;
        }
    }

    /* x264's I16 mode for flat content: mode 0 (V), chroma mode 0 (DC).
     * All MBs are I16 with cbp=0, qp_delta=0, zero luma DC block. */
    for (int my = 0; my < mh; ++my)
        for (int mx = 0; mx < mw; ++mx) {
            int left = mx > 0, top = my > 0;  /* previous MBs are I16 != I4x4 */
            cabac_mb_type_i16(cb, left, top, 0);
            cabac_chroma_mode(cb, 0);
            cabac_qp_delta0(cb);
            /* luma DC cbf: ctx 85 (left/top cbp bit 8 = 0), b=0 */
            vk_cabac_decision(cb, 85, 0);
        }
    int bytes = vk_cabac_flush(cb, 0);
    bs->p = data + bytes;
    return bytes;
}
