/* Function-for-function port of x264's scalar trellis RD quantizer.
 *
 * Every function below cites the exact vendored x264 source lines it follows.
 * HAVE_MMX == 0 / HIGH_BIT_DEPTH == 0 in reference/x264/config.h, so the C
 * trellis state machine (rdo.c:731-895) is the reference, not the asm fast
 * paths guarded by `#if HAVE_MMX && ARCH_X86_64` at rdo.c:708-729.
 *
 * SHADER: C — serial coefficient state machine; no byte-stream writes.
 */
#include "phx_rdo.h"
#include "phx_cabac_syntax.h"   /* PHX_DCT_* (common/macroblock.h:273-289) */
#include "phx_cavlc.h"          /* bit-count helper tables */
#include "phx_bitstream.h"      /* phx_bs_t / phx_bs_init / phx_bs_pos */
#include "phx_tables.h"         /* phx_cabac_transition, lambda2, chroma qp */
#include <stdlib.h>             /* abs */
#include <string.h>

/* rdo.c:378-381. */
#define PHX_TRELLIS_SCORE_MAX  (~(uint64_t)0)
#define PHX_TRELLIS_SCORE_BIAS (1ULL << 60)
#define PHX_CABAC_SIZE_BITS    8
#define PHX_LAMBDA_BITS        4

#define PHX_MIN(a,b) ((a) < (b) ? (a) : (b))
#define PHX_MAX(a,b) ((a) > (b) ? (a) : (b))
#define PHX_CLIP3(a,v,b) ((v) < (a) ? (a) : ((v) > (b) ? (b) : (v)))
/* base.h:?? / rdo.c:450 */
#define PHX_SIGN(x,y) ((x ^ (y >> 31)) - (y >> 31))
#define PHX_XCHG(t,a,b) do { t phx_tmp_ = (a); (a) = (b); (b) = phx_tmp_; } while (0)
/* me.h:80, used by rdo.c:488. */
#define PHX_COPY2_IF_LT(d1,s1,d2,s2) do { \
    if ((s1) < (d1)) { (d1) = (s1); (d2) = (s2); } \
} while (0)

/* common/set.h:32-42. */
#define PHX_CQM_4IY 0
#define PHX_CQM_4PY 1
#define PHX_CQM_4IC 2
#define PHX_CQM_4PC 3
#define PHX_CQM_8IY 0
#define PHX_CQM_8PY 1

/* ------------------------------------------------------------------ */
/* Verbatim tables                                                     */
/* ------------------------------------------------------------------ */

/* common/tables.c:1717-1750 — FIX8(-ln2(p)), FIX8(f) = floor(f*256+0.5). */
static const uint16_t phx_cabac_entropy[128] = {
     7, 1469,    7, 1449,    8, 1430,    8, 1411,
     9, 1392,    9, 1372,   10, 1353,   10, 1334,
    11, 1315,   11, 1295,   12, 1276,   12, 1257,
    13, 1238,   14, 1218,   15, 1199,   15, 1180,
    16, 1161,   17, 1141,   18, 1122,   19, 1103,
    20, 1084,   21, 1064,   22, 1045,   24, 1026,
    25, 1007,   26,  987,   28,  968,   29,  949,
    31,  930,   33,  910,   35,  891,   37,  872,
    39,  853,   41,  833,   43,  814,   46,  795,
    48,  776,   51,  756,   54,  737,   57,  718,
    60,  699,   64,  679,   68,  660,   72,  641,
    76,  622,   80,  602,   85,  583,   90,  564,
    96,  545,  102,  525,  108,  506,  115,  487,
   122,  468,  130,  448,  139,  429,  148,  410,
   157,  391,  168,  371,  180,  352,  192,  333,
   206,  314,  221,  295,  238,  275,  256,  256
};

/* common/tables.c:391-408. W0=256 W1=227 W2=410 W3=241 W4=324 W5=305. */
static const uint32_t phx_dct8_weight_tab[64] = {
    256, 241, 324, 241, 256, 241, 324, 241,
    241, 227, 305, 227, 241, 227, 305, 227,
    324, 305, 410, 305, 324, 305, 410, 305,
    241, 227, 305, 227, 241, 227, 305, 227,
    256, 241, 324, 241, 256, 241, 324, 241,
    241, 227, 305, 227, 241, 227, 305, 227,
    324, 305, 410, 305, 324, 305, 410, 305,
    241, 227, 305, 227, 241, 227, 305, 227
};

/* common/tables.c:410-419. W0=453 W1=286 W2=181. */
static const uint32_t phx_dct4_weight_tab[16] = {
    453, 286, 453, 286,
    286, 181, 286, 181,
    453, 286, 453, 286,
    286, 181, 286, 181
};

/* common/tables.c:421-431. W0=800 W1=320 W2=128. */
static const uint32_t phx_dct4_weight2_tab[16] = {
    800, 320, 800, 320,
    320, 128, 320, 128,
    800, 320, 800, 320,
    320, 128, 320, 128
};

/* common/tables.c:433-450. W0=256 W1=201 W2=656 W3=227 W4=410 W5=363. */
static const uint32_t phx_dct8_weight2_tab[64] = {
    256, 227, 410, 227, 256, 227, 410, 227,
    227, 201, 363, 201, 227, 201, 363, 201,
    410, 363, 656, 363, 410, 363, 656, 363,
    227, 201, 363, 201, 227, 201, 363, 201,
    256, 227, 410, 227, 256, 227, 410, 227,
    227, 201, 363, 201, 227, 201, 363, 201,
    410, 363, 656, 363, 410, 363, 656, 363,
    227, 201, 363, 201, 227, 201, 363, 201
};

/* common/macroblock.h:180-200 — scans are transposed vs the standard. */
const uint8_t phx_rdo_zigzag4[2][16] = {
    { 0,  4,  1,  2,  5,  8, 12,  9,  6,  3,  7, 10, 13, 14, 11, 15 },
    { 0,  1,  4,  2,  3,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 }
};
const uint8_t phx_rdo_zigzag8[2][64] = {
    {  0,  8,  1,  2,  9, 16, 24, 17, 10,  3,  4, 11, 18, 25, 32, 40,
      33, 26, 19, 12,  5,  6, 13, 20, 27, 34, 41, 48, 56, 49, 42, 35,
      28, 21, 14,  7, 15, 22, 29, 36, 43, 50, 57, 58, 51, 44, 37, 30,
      23, 31, 38, 45, 52, 59, 60, 53, 46, 39, 47, 54, 61, 62, 55, 63 },
    {  0,  1,  2,  8,  9,  3,  4, 10, 16, 11,  5,  6,  7, 12, 17, 24,
      18, 13, 14, 15, 19, 25, 32, 26, 20, 21, 22, 23, 27, 33, 40, 34,
      28, 29, 30, 31, 35, 41, 48, 42, 36, 37, 38, 39, 43, 49, 50, 44,
      45, 46, 47, 51, 56, 57, 52, 53, 54, 55, 58, 59, 60, 61, 62, 63 }
};
/* rdo.c:1111 — zigzag_scan2x2 (4:2:0 chroma DC). */
static const uint8_t phx_zigzag2x2[4] = { 0, 1, 2, 3 };

/* common/tables.c:1757-1792 (RDO section). */
static const uint8_t phx_sig_off8[2][64] = {
    { 0,1,2,3,4,5,5,4,4,3,3,4,4,4,5,5,
      4,4,4,4,3,3,6,7,7,7,8,9,10,9,8,7,
      7,6,11,12,13,11,6,7,8,9,14,10,9,8,6,11,
      12,13,11,6,9,14,10,9,11,12,13,11,14,10,12 },
    { 0,1,1,2,2,3,3,4,5,6,7,7,7,8,4,5,
      6,9,10,10,8,11,12,11,9,9,10,10,8,11,12,11,
      9,9,10,10,8,11,12,11,9,9,10,10,8,13,13,9,
      9,10,10,8,13,13,9,9,10,10,14,14,14,14,14 }
};
static const uint8_t phx_last_off8[63] = {
    0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
    3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,
    5,5,5,5,6,6,6,6,7,7,7,7,8,8,8
};
static const uint8_t phx_coeff_flag_off_chroma422dc[7] = { 0,0,1,1,2,2,2 };
static const uint16_t phx_sig_off[2][16] = {
    { 105+0, 105+15, 105+29, 105+44, 105+47, 402, 484+0, 484+15,
      484+29, 660, 528+0, 528+15, 528+29, 718, 0, 0 },
    { 277+0, 277+15, 277+29, 277+44, 277+47, 436, 776+0, 776+15,
      776+29, 675, 820+0, 820+15, 820+29, 733, 0, 0 }
};
static const uint16_t phx_last_off[2][16] = {
    { 166+0, 166+15, 166+29, 166+44, 166+47, 417, 572+0, 572+15,
      572+29, 690, 616+0, 616+15, 616+29, 748, 0, 0 },
    { 338+0, 338+15, 338+29, 338+44, 338+47, 451, 864+0, 864+15,
      864+29, 699, 908+0, 908+15, 908+29, 757, 0, 0 }
};
static const uint16_t phx_level_m1_off[16] = {
    227+0, 227+10, 227+20, 227+30, 227+39, 426,
    952+0, 952+10, 952+20, 708, 982+0, 982+10, 982+20, 766, 0, 0
};
static const uint8_t phx_count_cat_m1[14] =
    { 15, 14, 15, 3, 14, 63, 15, 14, 15, 63, 15, 14, 15, 63 };
/* rdo.c:1145. */
static const uint8_t phx_ctx_ac[14] = { 0,1,0,0,1,0,0,1,0,0,0,1,0,0 };

/* ------------------------------------------------------------------ */
/* Generated tables and configuration                                  */
/* ------------------------------------------------------------------ */

/* rdo.c:35-42 — generated by x264_rdo_init(). */
static uint8_t  phx_transition_unary[15][128];
static uint16_t phx_size_unary[15][128];
static uint8_t  phx_transition_5ones[128];
static uint16_t phx_size_5ones[128];

/* cabac.c:650-662 — residual level state machine (also needed by the
 * RDO_SKIP_BS==1 merged sigmap path at cabac.c:775-858). */
static const uint8_t phx_coeff_abs_level1_ctx[8] = { 1, 2, 3, 4, 0, 0, 0, 0 };
static const uint8_t phx_coeff_abs_levelgt1_ctx[8] = { 5, 5, 5, 5, 6, 7, 8, 9 };
static const uint8_t phx_coeff_abs_level_transition[2][8] = {
    { 1, 2, 3, 3, 4, 5, 6, 7 },  /* :659 — after level == 1 */
    { 4, 4, 4, 4, 5, 6, 7, 7 }   /* :661 — after level > 1 */
};
/* common/predict.h:84-92 — x264_mb_pred_mode4x4_fix[]. */
static const int8_t phx_pred_mode4x4_fix[13] = {
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 2, 2, 2
};
#define PHX_RDO_FIX4(t) phx_pred_mode4x4_fix[(t)+1]

typedef struct {
    uint16_t q4_mf[4][PHX_QUANT_QP_MAX_SPEC+1][16];
    uint16_t q4_bias0[4][PHX_QUANT_QP_MAX_SPEC+1][16];
    int      uq4_mf[4][PHX_QUANT_QP_MAX_SPEC+1][16];
    uint16_t q8_mf[2][PHX_QUANT_QP_MAX_SPEC+1][64];
    uint16_t q8_bias0[2][PHX_QUANT_QP_MAX_SPEC+1][64];
    int      uq8_mf[2][PHX_QUANT_QP_MAX_SPEC+1][64];
} phx_rdo_qtab_t;
static phx_rdo_qtab_t phx_qtab;

static phx_rdo_config_t phx_cfg = { 0, 0, 0, 0, NULL, NULL };
static int phx_ready = 0;

/* Golden-test trace (rdo.c:695/747-752/789-808/862-891).  Written by the
 * serial entropy-stage state machine only; never consulted by the encoder.
 * SHADER: C — one writer per MB, no stream state. */
static phx_rdo_trellis_trace_t phx_trellis_trace;

/* common/cabac.h:108-113 — bit-cost decision that advances a local state. */
static int phx_cabac_size_decision2(uint8_t *state, int b)
{
    int i_state = *state;                                    /* cabac.h:110 */
    *state = phx_cabac_transition[i_state][b];               /* cabac.h:111 */
    return phx_cabac_entropy[i_state ^ b];                   /* cabac.h:112 */
}

/* common/cabac.h:121-124 — bit-cost decision without state update. */
static int phx_cabac_size_decision_noup2(const uint8_t *state, int b)
{
    return phx_cabac_entropy[*state ^ b];                    /* cabac.h:123 */
}

void phx_rdo_init(void)
{
    if (phx_ready)
        return;

    /* rdo.c:384-415 — x264_rdo_init(). */
    for (int i_prefix = 0; i_prefix < 15; i_prefix++) {       /* :386 */
        for (int i_ctx = 0; i_ctx < 128; i_ctx++) {           /* :388 */
            int f8_bits = 0;
            uint8_t ctx = (uint8_t)i_ctx;                     /* :391 */
            for (int i = 1; i < i_prefix; i++)                /* :393 */
                f8_bits += phx_cabac_size_decision2(&ctx, 1);
            if (i_prefix > 0 && i_prefix < 14)                /* :395 */
                f8_bits += phx_cabac_size_decision2(&ctx, 0);
            f8_bits += 1 << PHX_CABAC_SIZE_BITS;              /* :397 */
            phx_size_unary[i_prefix][i_ctx] = (uint16_t)f8_bits; /* :399 */
            phx_transition_unary[i_prefix][i_ctx] = ctx;      /* :400 */
        }
    }

    /* rdo.c:403-414 — cabac_size_5ones / cabac_transition_5ones. */
    for (int i_ctx = 0; i_ctx < 128; i_ctx++) {             /* :403 */
        int f8_bits = 0;                                    /* :405 */
        uint8_t ctx = (uint8_t)i_ctx;                       /* :406 */
        for (int i = 0; i < 5; i++)                         /* :408 */
            f8_bits += phx_cabac_size_decision2(&ctx, 1);
        f8_bits += 1 << PHX_CABAC_SIZE_BITS;                /* :410 */
        phx_size_5ones[i_ctx] = (uint16_t)f8_bits;          /* :412 */
        phx_transition_5ones[i_ctx] = ctx;                  /* :413 */
    }

    /* Flat-CQM quantizer tables: set.c:73-205 via the existing phx port. */
    phx_quant_tables_t qt;
    phx_quant_tables_init(&qt);                              /* set.c:73-205 */
    for (int qp = 0; qp <= PHX_QUANT_QP_MAX_SPEC; qp++) {
        for (int list = 0; list < 4; list++) {
            for (int i = 0; i < 16; i++) {
                /* set.c:170 uses quant4_mf[q%6] from set.c:146-153 (the
                 * unshifted base table).  phx_quant_tables_init applies the
                 * q/6-1 shift at every QP, so undo its shift for q%6 < 6. */
                const uint16_t base = (uint16_t)(qt.quant4_mf[list][qp % 6][i] >> 1);
                /* set.c:170 (1ULL << (q/6 + 15 + 8)) / quant4_mf[q%6]. */
                phx_qtab.uq4_mf[list][qp][i] = (int)((1ULL << (qp/6 + 23)) / base);
                /* set.c:180 (1<<15)/j, j = quant4_mf[list][qp][i]. */
                const uint16_t j = qt.quant4_mf[list][qp][i];
                phx_qtab.q4_bias0[list][qp][i] = j ? (uint16_t)((1<<15)/j) : 0;
                phx_qtab.q4_mf[list][qp][i] = j;
            }
        }
        for (int list = 0; list < 2; list++) {
            for (int i = 0; i < 64; i++) {
                const uint16_t base = qt.quant8_mf[list][qp % 6][i];
                /* set.c:190 (1ULL << (q/6 + 16 + 8)) / quant8_mf[q%6]. */
                phx_qtab.uq8_mf[list][qp][i] = (int)((1ULL << (qp/6 + 24)) / base);
                /* set.c:200 (1<<15)/j. */
                const uint16_t j = qt.quant8_mf[list][qp][i];
                phx_qtab.q8_bias0[list][qp][i] = j ? (uint16_t)((1<<15)/j) : 0;
                phx_qtab.q8_mf[list][qp][i] = j;
            }
        }
    }

    /* rdo.c:53 includes cavlc.c with RDO_SKIP_BS; the generated tables are
     * the same as x264_cavlc_init() (common/vlc.c:33-99). */
    phx_cavlc_init();
    phx_ready = 1;
}

void phx_rdo_set_config(const phx_rdo_config_t *cfg)
{
    if (cfg)
        phx_cfg = *cfg;
}
void phx_rdo_get_config(phx_rdo_config_t *cfg)
{
    if (cfg)
        *cfg = phx_cfg;
}

const uint16_t *phx_rdo_quant4_mf(int list, int qp)
{
    if ((unsigned)list >= 4 || (unsigned)qp > PHX_QUANT_QP_MAX_SPEC) return NULL;
    return phx_qtab.q4_mf[list][qp];
}
const uint16_t *phx_rdo_quant4_bias0(int list, int qp)
{
    if ((unsigned)list >= 4 || (unsigned)qp > PHX_QUANT_QP_MAX_SPEC) return NULL;
    return phx_qtab.q4_bias0[list][qp];
}
const int *phx_rdo_unquant4_mf(int list, int qp)
{
    if ((unsigned)list >= 4 || (unsigned)qp > PHX_QUANT_QP_MAX_SPEC) return NULL;
    return phx_qtab.uq4_mf[list][qp];
}
const uint16_t *phx_rdo_quant8_mf(int list, int qp)
{
    if ((unsigned)list >= 2 || (unsigned)qp > PHX_QUANT_QP_MAX_SPEC) return NULL;
    return phx_qtab.q8_mf[list][qp];
}
const uint16_t *phx_rdo_quant8_bias0(int list, int qp)
{
    if ((unsigned)list >= 2 || (unsigned)qp > PHX_QUANT_QP_MAX_SPEC) return NULL;
    return phx_qtab.q8_bias0[list][qp];
}
const int *phx_rdo_unquant8_mf(int list, int qp)
{
    if ((unsigned)list >= 2 || (unsigned)qp > PHX_QUANT_QP_MAX_SPEC) return NULL;
    return phx_qtab.uq8_mf[list][qp];
}

int phx_rdo_bs_size_ue_big(unsigned int val)
{
    /* common/bitstream.h:283-289. */
    if (val < 255)
        return phx_ue_size_tab[val+1];
    return phx_ue_size_tab[(val+1)>>8] + 16;
}

/* ------------------------------------------------------------------ */
/* Size-only CABAC accumulation: cabac.h:99-124 + rdo.c:31-73          */
/* ------------------------------------------------------------------ */

/* quant.c:878-882, defined below with the trellis helpers. */
static int phx_coeff_last(int ctx_block_cat, const int16_t *l);

void phx_rdo_size_init(phx_cabac_size_t *cb, const phx_cabac_t *live,
                       int slice_type, int qp)
{
    /* rdo.c:71-72 — COPY_CABAC copies f8_bits_encoded followed by the 460
     * context states (4:2:0).  x264's live f8_bits_encoded is unused outside
     * an RDO nest, so start every independent partition estimate at zero. */
    cb->f8_bits_encoded = 0;
    if (live) {
        memcpy(cb->state, live->state, PHX_CABAC_CTX_COUNT);
    } else {
        /* Standalone analysis without a live slice coder: the same context
         * table x264 builds at encoder.c:2798 (cabac.c:32-48). */
        phx_cabac_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        phx_cabac_context_init(&tmp, slice_type, qp, 0);
        memcpy(cb->state, tmp.state, PHX_CABAC_CTX_COUNT);
    }
}

void phx_rdo_size_decision(phx_cabac_size_t *cb, int ctx, int b)
{
    /* common/cabac.h:101-106. */
    int i_state = cb->state[ctx];                             /* :103 */
    cb->state[ctx] = phx_cabac_transition[i_state][b];        /* :104 */
    cb->f8_bits_encoded += phx_cabac_entropy[i_state^b];      /* :105 */
}

void phx_rdo_size_decision_noup(phx_cabac_size_t *cb, int ctx, int b)
{
    /* common/cabac.h:115-119 (no state update). */
    cb->f8_bits_encoded += phx_cabac_entropy[cb->state[ctx]^b]; /* :118 */
}

void phx_rdo_size_terminal(phx_cabac_size_t *cb)
{
    cb->f8_bits_encoded += 7;                                 /* rdo.c:64 */
}
void phx_rdo_size_bypass(phx_cabac_size_t *cb)
{
    cb->f8_bits_encoded += 1 << PHX_CABAC_SIZE_BITS;          /* rdo.c:65 */
}
void phx_rdo_size_ue_bypass(phx_cabac_size_t *cb, int exp_bits, int val)
{
    /* rdo.c:66 — bs_size_ue_big(v+(1<<e)-1)-e, in FIX8 bits. */
    cb->f8_bits_encoded +=
        (phx_rdo_bs_size_ue_big((unsigned)(val + (1 << exp_bits) - 1))
         - exp_bits) << PHX_CABAC_SIZE_BITS;
}

const uint16_t *phx_rdo_size_unary(int prefix)
{
    return (unsigned)prefix < 15 ? phx_size_unary[prefix] : NULL;
}
const uint8_t *phx_rdo_transition_unary(int prefix)
{
    return (unsigned)prefix < 15 ? phx_transition_unary[prefix] : NULL;
}
const uint16_t *phx_rdo_size_5ones(void)
{
    return phx_size_5ones;
}
const uint8_t *phx_rdo_transition_5ones(void)
{
    return phx_transition_5ones;
}

void phx_rdo_size_intra4x4_pred_mode(phx_cabac_size_t *cb,
                                     int i_pred, int i_mode)
{
    /* cabac.c:83-96. */
    if (i_pred == i_mode)
        phx_rdo_size_decision(cb, 68, 1);                     /* :86 */
    else {
        phx_rdo_size_decision(cb, 68, 0);                     /* :89 */
        if (i_mode > i_pred)
            i_mode--;                                        /* :91 */
        phx_rdo_size_decision(cb, 69, (i_mode     ) & 0x01);  /* :92 */
        phx_rdo_size_decision(cb, 69, (i_mode >> 1) & 0x01);  /* :93 */
        phx_rdo_size_decision(cb, 69, (i_mode >> 2)      );   /* :94 */
    }
}

/* cabac.c:775-858 — cabac_block_residual_internal(), RDO_SKIP_BS==1 branch
 * (the "merged sigmap and level coding" fast path used by RDO).
 * b_8x8 selects the 8x8 sigmap offset tables of :777/:824-826/:855-856;
 * chroma422dc is not needed by the 4:2:0 callers.  SHADER: C. */
static void phx_rdo_size_block_residual_ex(phx_cabac_size_t *cb,
                                           int ctx_block_cat,
                                           const phx_dctcoef_t *l,
                                           int b_8x8)
{
    int ctx_sig = phx_sig_off[0][ctx_block_cat];             /* cabac.c:778 */
    int ctx_last = phx_last_off[0][ctx_block_cat];          /* :779 */
    int ctx_level = phx_level_m1_off[ctx_block_cat];        /* :780 */
    int last = phx_coeff_last(ctx_block_cat, l);            /* :781 */
    int coeff_abs = abs(l[last]);                           /* :782 */
    int ctx = phx_coeff_abs_level1_ctx[0] + ctx_level;       /* :783 */
    int node_ctx;

    if (last != (b_8x8 ? 63 : phx_count_cat_m1[ctx_block_cat])) { /* :787 */
        phx_rdo_size_decision(cb, ctx_sig + (b_8x8 ? phx_sig_off8[0][last] : last), 1); /* :789-790 */
        phx_rdo_size_decision(cb, ctx_last + (b_8x8 ? phx_last_off8[last] : last), 1);  /* :791-792 */
    }

    if (coeff_abs > 1) {                                    /* :795 */
        phx_rdo_size_decision(cb, ctx, 1);                   /* :797 */
        ctx = phx_coeff_abs_levelgt1_ctx[0] + ctx_level;     /* :798 */
        if (coeff_abs < 15) {                               /* :799 */
            cb->f8_bits_encoded += phx_size_unary[coeff_abs-1][cb->state[ctx]]; /* :801 */
            cb->state[ctx] = phx_transition_unary[coeff_abs-1][cb->state[ctx]]; /* :802 */
        } else {
            cb->f8_bits_encoded += phx_size_unary[14][cb->state[ctx]]; /* :806 */
            cb->state[ctx] = phx_transition_unary[14][cb->state[ctx]]; /* :807 */
            phx_rdo_size_ue_bypass(cb, 0, coeff_abs - 15);  /* :808 */
        }
        node_ctx = phx_coeff_abs_level_transition[1][0];    /* :810 */
    } else {                                                /* :812 */
        phx_rdo_size_decision(cb, ctx, 0);                   /* :814 */
        node_ctx = phx_coeff_abs_level_transition[0][0];    /* :815 */
        phx_rdo_size_bypass(cb);                            /* :816 (sign) */
    }

    for (int i = last-1; i >= 0; i--) {                     /* :819 */
        int sig_idx = b_8x8 ? phx_sig_off8[0][i] : i;       /* :824-825 */
        int last_idx = b_8x8 ? phx_last_off8[i] : i;        /* :826-827 */
        if (l[i]) {
            coeff_abs = abs(l[i]);                          /* :823 */
            phx_rdo_size_decision(cb, ctx_sig + sig_idx, 1);/* :824-825 */
            phx_rdo_size_decision(cb, ctx_last + last_idx, 0); /* :826-827 */
            ctx = phx_coeff_abs_level1_ctx[node_ctx] + ctx_level; /* :828 */

            if (coeff_abs > 1) {                            /* :830 */
                phx_rdo_size_decision(cb, ctx, 1);          /* :832 */
                ctx = phx_coeff_abs_levelgt1_ctx[node_ctx] + ctx_level; /* :833 */
                if (coeff_abs < 15) {                       /* :834 */
                    cb->f8_bits_encoded += phx_size_unary[coeff_abs-1][cb->state[ctx]]; /* :836 */
                    cb->state[ctx] = phx_transition_unary[coeff_abs-1][cb->state[ctx]]; /* :837 */
                } else {
                    cb->f8_bits_encoded += phx_size_unary[14][cb->state[ctx]]; /* :841 */
                    cb->state[ctx] = phx_transition_unary[14][cb->state[ctx]]; /* :842 */
                    phx_rdo_size_ue_bypass(cb, 0, coeff_abs - 15); /* :843 */
                }
                node_ctx = phx_coeff_abs_level_transition[1][node_ctx]; /* :845 */
            } else {
                phx_rdo_size_decision(cb, ctx, 0);          /* :849 */
                node_ctx = phx_coeff_abs_level_transition[0][node_ctx]; /* :850 */
                phx_rdo_size_bypass(cb);                    /* :851 */
            }
        } else
            phx_rdo_size_decision(cb, ctx_sig + sig_idx, 0);/* :855-856 */
    }
}

/* 4x4 / chroma-DC path (cabac.c:864-866). */
static void phx_rdo_size_block_residual(phx_cabac_size_t *cb,
                                        int ctx_block_cat,
                                        const phx_dctcoef_t *l)
{
    phx_rdo_size_block_residual_ex(cb, ctx_block_cat, l, 0);
}

/* x264_cabac_block_residual_8x8_rd_c() (cabac.c:860-875). */
void phx_rdo_size_block_residual_8x8(phx_cabac_size_t *cb, int ctx_block_cat,
                                     const phx_dctcoef_t *l)
{
    phx_rdo_size_block_residual_ex(cb, ctx_block_cat, l, 1);
}

void phx_rdo_size_block_residual_cbf_ex(phx_cabac_size_t *cb,
                                        phx_cabac_mb_t *mb,
                                        int ctx_block_cat, int i_idx,
                                        const phx_dctcoef_t *l, int b_intra,
                                        int b_dc)
{
    /* cabac.c:892-903. */
    int ctxidxinc = phx_cabac_cbf_ctxidxinc(mb, ctx_block_cat, i_idx,
                                            b_intra, b_dc);    /* :895 */
    if (mb->cache->non_zero_count[phx_scan8[i_idx]]) {       /* :896 */
        phx_rdo_size_decision(cb, ctxidxinc, 1);             /* :898 */
        phx_rdo_size_block_residual(cb, ctx_block_cat, l);   /* :899 */
    } else
        phx_rdo_size_decision(cb, ctxidxinc, 0);             /* :902 */
}

void phx_rdo_size_block_residual_cbf(phx_cabac_size_t *cb, phx_cabac_mb_t *mb,
                                     int ctx_block_cat, int i_idx,
                                     const phx_dctcoef_t *l, int b_intra)
{
    phx_rdo_size_block_residual_cbf_ex(cb, mb, ctx_block_cat, i_idx, l,
                                       b_intra, 0);          /* :905-909 */
}

int phx_rdo_size_partition_i4x4(const phx_cabac_t *live, phx_cabac_mb_t *mb,
                                int i4, int i_pred, int i_mode,
                                const phx_dctcoef_t *l,
                                int slice_type, int qp)
{
    /* cabac.c:1203-1211 — one plane (4:2:0), one 4x4 block. */
    phx_cabac_size_t cb;
    phx_rdo_size_init(&cb, live, slice_type, qp);           /* rdo.c:71-73 */
    phx_rdo_size_intra4x4_pred_mode(&cb, i_pred,
                                    PHX_RDO_FIX4(i_mode));  /* cabac.c:1207-1208 */
    phx_rdo_size_block_residual_cbf(&cb, mb, PHX_DCT_LUMA_4x4,
                                    i4, l, 1);              /* :1210 */
    return cb.f8_bits_encoded;                              /* rdo.c:341 */
}

int phx_rdo_cavlc_predict_nc(const phx_mb_cache_t *cache, int idx)
{
    /* common/macroblock.h:432-442. */
    const int za = cache->non_zero_count[phx_scan8[idx] - 1];
    const int zb = cache->non_zero_count[phx_scan8[idx] - 8];
    int i_ret = za + zb;
    if (i_ret < 0x80)
        i_ret = (i_ret + 1) >> 1;
    i_ret &= 0x7f;
    /* cavlc.c:198 — ct_index[17], already ported as phx_cavlc_ct_index. */
    return phx_cavlc_ct_index[PHX_MIN(i_ret, 16)];
}

/* common/quant.c:366-379 + :878-882 + encoder.c:1446: last nonzero index in
 * the first count_cat_m1[cat]+1 entries of a zigzag-scanned block. */
static int phx_coeff_last(int ctx_block_cat, const int16_t *l)
{
    int count = (unsigned)ctx_block_cat < 14 ? phx_count_cat_m1[ctx_block_cat] + 1 : 16;
    int i_last = count - 1;
    while (i_last >= 0 && l[i_last] == 0)
        i_last--;
    return i_last;
}

/* Effective chroma QP (analyse.c:257 + encoder.c:1585). */
static int phx_rdo_chroma_qp(int qp)
{
    int spec_qp = PHX_MIN(qp, PHX_QP_MAX_SPEC);
    int chroma_base = phx_chroma_qp_base[
        PHX_CLIP3(0, PHX_CHROMA_QP_BASE_OFFSET + phx_cfg.chroma_qp_index_offset + spec_qp,
                  (int)(sizeof(phx_chroma_qp_base)-1))];
    return chroma_base + PHX_MAX(qp - PHX_QP_MAX_SPEC, 0);
}

static int phx_rdo_lambda2(int b_chroma, int b_intra, int qp)
{
    /* analyse.c:264-267. phx_trellis_lambda2_tab[0] is inter, [1] is intra. */
    return phx_trellis_lambda2_tab[b_intra][b_chroma ? phx_rdo_chroma_qp(qp) : qp];
}

/* ------------------------------------------------------------------ */
/* CABAC trellis: rdo.c:417-895                                        */
/* ------------------------------------------------------------------ */

/* rdo.c:417-428. */
typedef struct {
    uint64_t score;
    int level_idx;
    uint8_t cabac_state[4];     /* contexts 0,4,8,9 of level_m1 */
} phx_trellis_node_t;

typedef struct {
    uint16_t next;
    uint16_t abs_level;
} phx_trellis_level_t;

/* rdo.c:452-459. */
#define PHX_SET_LEVEL(ndst,nsrc,l) do { \
    phx_trellis_level_t phx_lv_; \
    phx_lv_.next = (uint16_t)(nsrc).level_idx; \
    phx_lv_.abs_level = (uint16_t)(l); \
    level_tree[levels_used] = phx_lv_; \
    (ndst).level_idx = levels_used; \
    levels_used++; \
} while (0)

/* rdo.c:462-491 — DC-only shortcut. */
static int phx_trellis_dc_shortcut(int sign_coef, int quant_coef, int unquant_mf,
                                   int coef_weight, int lambda2,
                                   const uint8_t *cabac_state, int cost_sig)
{
    uint64_t bscore = PHX_TRELLIS_SCORE_MAX;
    int ret = 0;
    int q = abs(quant_coef);                                  /* :467 */
    for (int abs_level = q-1; abs_level <= q; abs_level++)    /* :468 */
    {
        int unquant_abs_level = (unquant_mf * abs_level + 128) >> 8; /* :470 */
        /* Optimize rounding for DC coefficients in DC-only blocks (rdo.c:472). */
        int d = sign_coef - ((PHX_SIGN(unquant_abs_level, sign_coef) + 8) & ~15); /* :473 */
        uint64_t score = (uint64_t)(int64_t)d*d * (uint64_t)coef_weight; /* :474 */
        if (abs_level)                                        /* :477 */
        {
            unsigned f8_bits = (unsigned)cost_sig;            /* :479 */
            int prefix = PHX_MIN(abs_level - 1, 14);          /* :480 */
            f8_bits += phx_cabac_size_decision_noup2(cabac_state+1, prefix > 0); /* :481 */
            f8_bits += phx_size_unary[prefix][cabac_state[5]]; /* :482 */
            if (abs_level >= 15)                              /* :483 */
                f8_bits += (unsigned)phx_rdo_bs_size_ue_big(abs_level-15) << PHX_CABAC_SIZE_BITS; /* :484 */
            score += (uint64_t)f8_bits * (uint64_t)lambda2 >> (PHX_CABAC_SIZE_BITS - PHX_LAMBDA_BITS); /* :485 */
        }
        PHX_COPY2_IF_LT(bscore, score, ret, abs_level);       /* :488 */
    }
    return PHX_SIGN(ret, sign_coef);                          /* :490 */
}

/* rdo.c:493-531 — code one coefficient in one node context. */
static int phx_trellis_coef(int j, int const_level, int abs_level, int prefix,
                            int suffix_cost, int node_ctx, int level1_ctx,
                            int levelgt1_ctx, uint64_t ssd, int cost_siglast[3],
                            phx_trellis_node_t *nodes_cur,
                            phx_trellis_node_t *nodes_prev,
                            phx_trellis_level_t *level_tree, int levels_used,
                            int lambda2, uint8_t *level_state)
{
    uint64_t score = nodes_prev[j].score + ssd;               /* :500 */
    unsigned f8_bits = (unsigned)cost_siglast[j ? 1 : 2];     /* :502 */
    uint8_t level1_state = (j >= 3) ? nodes_prev[j].cabac_state[level1_ctx>>2]
                                   : level_state[level1_ctx]; /* :503 */
    f8_bits += phx_cabac_entropy[level1_state ^ (const_level > 1)]; /* :504 */
    uint8_t levelgt1_state;
    if (const_level > 1)                                      /* :506 */
    {
        levelgt1_state = j >= 6 ? nodes_prev[j].cabac_state[levelgt1_ctx-6]
                                : level_state[levelgt1_ctx];
        f8_bits += phx_size_unary[prefix][levelgt1_state] + suffix_cost; /* :509 */
    }
    else
        f8_bits += 1 << PHX_CABAC_SIZE_BITS;                  /* :512 */
    score += (uint64_t)f8_bits * (uint64_t)lambda2 >> (PHX_CABAC_SIZE_BITS - PHX_LAMBDA_BITS); /* :513 */

    if (score < nodes_cur[node_ctx].score)                    /* :516 */
    {
        nodes_cur[node_ctx].score = score;                    /* :518 */
        if (j == 2 || (j <= 3 && node_ctx == 4))              /* :519 */
            memcpy(nodes_cur[node_ctx].cabac_state, level_state+12, 4); /* :520 */
        else if (j >= 3)                                      /* :521 */
            memcpy(nodes_cur[node_ctx].cabac_state, nodes_prev[j].cabac_state, 4); /* :522 */
        if (j >= 3)                                           /* :523 */
            nodes_cur[node_ctx].cabac_state[level1_ctx>>2] =
                phx_cabac_transition[level1_state][const_level > 1]; /* :524 */
        if (const_level > 1 && node_ctx == 7)                 /* :525 */
            nodes_cur[node_ctx].cabac_state[levelgt1_ctx-6] =
                phx_transition_unary[prefix][levelgt1_state];  /* :526 */
        nodes_cur[node_ctx].level_idx = nodes_prev[j].level_idx; /* :527 */
        PHX_SET_LEVEL(nodes_cur[node_ctx], nodes_prev[j], abs_level); /* :528 */
    }
    return levels_used;
}

/* rdo.c:569-575. */
#define PHX_COEF(const_level,ctx_hi,j,...) do { \
    if (!j || (int64_t)nodes_prev[j].score >= 0) \
        levels_used = phx_trellis_coef(j, const_level, abs_level, prefix, suffix_cost, \
                    __VA_ARGS__, j ? ssd1 : ssd0, cost_siglast, nodes_cur, nodes_prev, \
                    level_tree, levels_used, lambda2, level_state); \
    else if (!ctx_hi) \
        return levels_used; \
} while (0)

/* rdo.c:537-651 — coefficient encoders (ctx_lo / ctx_hi variants). */
static int phx_trellis_coef0_0(uint64_t ssd0, phx_trellis_node_t *nodes_cur,
                               phx_trellis_node_t *nodes_prev,
                               phx_trellis_level_t *level_tree, int levels_used)
{
    nodes_cur[0].score = nodes_prev[0].score + ssd0;          /* :541 */
    nodes_cur[0].level_idx = nodes_prev[0].level_idx;         /* :542 */
    for (int j = 1; j < 4 && (int64_t)nodes_prev[j].score >= 0; j++) /* :543 */
    {
        nodes_cur[j].score = nodes_prev[j].score;             /* :545 */
        if (j >= 3)
            memcpy(nodes_cur[j].cabac_state, nodes_prev[j].cabac_state, 4); /* :547 */
        PHX_SET_LEVEL(nodes_cur[j], nodes_prev[j], 0);        /* :548 */
    }
    return levels_used;
}
static int phx_trellis_coef0_1(uint64_t ssd0, phx_trellis_node_t *nodes_cur,
                               phx_trellis_node_t *nodes_prev,
                               phx_trellis_level_t *level_tree, int levels_used)
{
    (void)ssd0;
    for (int j = 1; j < 8; j++)                               /* :557 */
        if ((int64_t)nodes_prev[j].score >= 0)                /* :559 */
        {
            nodes_cur[j].score = nodes_prev[j].score;         /* :561 */
            if (j >= 3)
                memcpy(nodes_cur[j].cabac_state, nodes_prev[j].cabac_state, 4); /* :563 */
            PHX_SET_LEVEL(nodes_cur[j], nodes_prev[j], 0);    /* :564 */
        }
    return levels_used;
}
static int phx_trellis_coef1_0(uint64_t ssd0, uint64_t ssd1, int cost_siglast[3],
                               phx_trellis_node_t *nodes_cur, phx_trellis_node_t *nodes_prev,
                               phx_trellis_level_t *level_tree, int levels_used,
                               int lambda2, uint8_t *level_state)
{
    int abs_level = 1, prefix = 1, suffix_cost = 0;           /* :583 */
    PHX_COEF(1, 0, 0, 1, 1, 0);                               /* :584 */
    PHX_COEF(1, 0, 1, 2, 2, 0);                               /* :585 */
    PHX_COEF(1, 0, 2, 3, 3, 0);                               /* :586 */
    PHX_COEF(1, 0, 3, 3, 4, 0);                               /* :587 */
    return levels_used;
}
static int phx_trellis_coef1_1(uint64_t ssd0, uint64_t ssd1, int cost_siglast[3],
                               phx_trellis_node_t *nodes_cur, phx_trellis_node_t *nodes_prev,
                               phx_trellis_level_t *level_tree, int levels_used,
                               int lambda2, uint8_t *level_state)
{
    int abs_level = 1, prefix = 1, suffix_cost = 0;           /* :597 */
    PHX_COEF(1, 1, 1, 2, 2, 0);                               /* :598 */
    PHX_COEF(1, 1, 2, 3, 3, 0);                               /* :599 */
    PHX_COEF(1, 1, 3, 3, 4, 0);                               /* :600 */
    PHX_COEF(1, 1, 4, 4, 0, 0);                               /* :601 */
    PHX_COEF(1, 1, 5, 5, 0, 0);                               /* :602 */
    PHX_COEF(1, 1, 6, 6, 0, 0);                               /* :603 */
    PHX_COEF(1, 1, 7, 7, 0, 0);                               /* :604 */
    return levels_used;
}
static int phx_trellis_coefn_0(int abs_level, uint64_t ssd0, uint64_t ssd1,
                               int cost_siglast[3], phx_trellis_node_t *nodes_cur,
                               phx_trellis_node_t *nodes_prev,
                               phx_trellis_level_t *level_tree, int levels_used,
                               int lambda2, uint8_t *level_state, int levelgt1_ctx)
{
    int prefix = PHX_MIN(abs_level-1, 14);                    /* :614 */
    int suffix_cost = abs_level >= 15 ? phx_rdo_bs_size_ue_big(abs_level-15) << PHX_CABAC_SIZE_BITS : 0; /* :615 */
    PHX_COEF(2, 0, 0, 4, 1, 5);                               /* :616 */
    PHX_COEF(2, 0, 1, 4, 2, 5);                               /* :617 */
    PHX_COEF(2, 0, 2, 4, 3, 5);                               /* :618 */
    PHX_COEF(2, 0, 3, 4, 4, 5);                               /* :619 */
    return levels_used;
}
static int phx_trellis_coefn_1(int abs_level, uint64_t ssd0, uint64_t ssd1,
                               int cost_siglast[3], phx_trellis_node_t *nodes_cur,
                               phx_trellis_node_t *nodes_prev,
                               phx_trellis_level_t *level_tree, int levels_used,
                               int lambda2, uint8_t *level_state, int levelgt1_ctx)
{
    int prefix = PHX_MIN(abs_level-1, 14);                    /* :629 */
    int suffix_cost = abs_level >= 15 ? phx_rdo_bs_size_ue_big(abs_level-15) << PHX_CABAC_SIZE_BITS : 0; /* :630 */
    PHX_COEF(2, 1, 1, 4, 2, 5);                               /* :631 */
    PHX_COEF(2, 1, 2, 4, 3, 5);                               /* :632 */
    PHX_COEF(2, 1, 3, 4, 4, 5);                               /* :633 */
    PHX_COEF(2, 1, 4, 5, 0, 6);                               /* :634 */
    PHX_COEF(2, 1, 5, 6, 0, 7);                               /* :635 */
    PHX_COEF(2, 1, 6, 7, 0, 8);                               /* :636 */
    PHX_COEF(2, 1, 7, 7, 0, levelgt1_ctx);                    /* :637 */
    return levels_used;
}

/* rdo.c:764-861 — main loop, kept as a macro so the two live-node subsets
 * (ctx_lo and ctx_hi) and the `goto next1` transition stay byte-identical to
 * x264.  `next0`/`next1` are the labels at rdo.c:860 in each expansion. */
#define PHX_TRELLIS_LOOP(ctx_hi) \
    for (; i >= b_ac; i--) { \
        int sigindex = !dc && num_coefs == 64 ? phx_sig_off8[b_interlaced][i] : \
                       b_chroma && dc && num_coefs == 8 ? phx_coeff_flag_off_chroma422dc[i] : i; \
        int lastindex = !dc && num_coefs == 64 ? phx_last_off8[i] : \
                        b_chroma && dc && num_coefs == 8 ? phx_coeff_flag_off_chroma422dc[i] : i; \
        /* :766-784: skip coefficients whose quantized value is always zero. */ \
        if (!quant_coefs[i]) { \
            if (!ctx_hi) { \
                uint64_t cost_sig0 = (uint64_t)phx_cabac_size_decision_noup2( \
                                       &cabac_state_sig[sigindex], 0) \
                                   * (uint64_t)lambda2 >> (PHX_CABAC_SIZE_BITS - PHX_LAMBDA_BITS); /* :777-778 */ \
                nodes_cur[0].score -= cost_sig0;              /* :779 */ \
            } \
            for (int j = 1; j < (ctx_hi ? 8 : 4); j++) \
                PHX_SET_LEVEL(nodes_cur[j], nodes_cur[j], 0); /* :782 */ \
            continue; \
        } \
        { \
        int sign_coef = orig_coefs[zigzag[i]];                /* :786 */ \
        int abs_coef = abs(sign_coef);                        /* :787 */ \
        int q = abs(quant_coefs[i]);                          /* :788 */ \
        int cost_siglast[3];                                  /* :789 */ \
        PHX_XCHG(phx_trellis_node_t*, nodes_cur, nodes_prev); /* :790 */ \
        for (int j = ctx_hi; j < 8; j++) \
            nodes_cur[j].score = PHX_TRELLIS_SCORE_MAX;       /* :792 */ \
        if (i < num_coefs-1 || ctx_hi) {                      /* :794 */ \
            cost_siglast[0] = phx_cabac_size_decision_noup2(&cabac_state_sig[sigindex], 0); /* :800 */ \
            int cost_sig1 = phx_cabac_size_decision_noup2(&cabac_state_sig[sigindex], 1); /* :801 */ \
            cost_siglast[1] = phx_cabac_size_decision_noup2(&cabac_state_last[lastindex], 0) + cost_sig1; /* :802 */ \
            if (!ctx_hi) \
                cost_siglast[2] = phx_cabac_size_decision_noup2(&cabac_state_last[lastindex], 1) + cost_sig1; /* :804 */ \
        } else \
            cost_siglast[0] = cost_siglast[1] = cost_siglast[2] = 0; /* :808 */ \
        if (!ctx_hi && i == last_nnz) {                       /* golden trace */ \
            phx_trellis_trace.cost_siglast[0] = cost_siglast[0]; \
            phx_trellis_trace.cost_siglast[1] = cost_siglast[1]; \
            phx_trellis_trace.cost_siglast[2] = cost_siglast[2]; \
        } \
        uint64_t ssd0[2], ssd1[2];                            /* :815 */ \
        for (int k = 0; k < 2; k++) { \
            int abs_level = q-1+k;                            /* :818 */ \
            int unquant_abs_level = (((dc ? (unquant_mf[0] << 1) : unquant_mf[zigzag[i]]) * abs_level + 128) >> 8); /* :819 */ \
            int d = abs_coef - unquant_abs_level;             /* :820 */ \
            if (i_psy_trellis && i && !dc && !b_chroma) {     /* :822 */ \
                int orig_coef = psy_fenc ? psy_fenc[zigzag[i]] : 0; /* :824 */ \
                int predicted_coef = orig_coef - sign_coef;   /* :825 */ \
                int psy_value = abs(unquant_abs_level + PHX_SIGN(predicted_coef, sign_coef)); /* :826 */ \
                int psy_weight = (int)(num_coefs == 64 ? phx_dct8_weight_tab[zigzag[i]] \
                                                      : phx_dct4_weight_tab[zigzag[i]]) * i_psy_trellis; /* :827 */ \
                int64_t tmp = (int64_t)d*d * (int64_t)(dc ? 256 : coef_weight2[zigzag[i]]) \
                            - (int64_t)psy_weight * psy_value; /* :828 */ \
                ssd1[k] = (uint64_t)tmp; \
            } else \
                ssd1[k] = (uint64_t)((int64_t)d*d * (int64_t)(dc ? 256 : coef_weight2[zigzag[i]])); /* :833 */ \
            ssd0[k] = ssd1[k];                                /* :834 */ \
            if (!i && !dc && !ctx_hi) {                       /* :835 */ \
                d = sign_coef - ((PHX_SIGN(unquant_abs_level, sign_coef) + 8) & ~15); /* :838 */ \
                ssd0[k] = (uint64_t)((int64_t)d*d * (int64_t)coef_weight2[zigzag[i]]); /* :839 */ \
            } \
        } \
        switch (q) { \
        case 1: \
            ssd1[0] += (uint64_t)cost_siglast[0] * (uint64_t)lambda2 >> (PHX_CABAC_SIZE_BITS - PHX_LAMBDA_BITS); /* :847 */ \
            levels_used = phx_trellis_coef0_##ctx_hi(ssd0[0]-ssd1[0], nodes_cur, nodes_prev, level_tree, levels_used); /* :848 */ \
            levels_used = phx_trellis_coef1_##ctx_hi(ssd0[1]-ssd1[0], ssd1[1]-ssd1[0], cost_siglast, nodes_cur, nodes_prev, level_tree, levels_used, lambda2, level_state); /* :849 */ \
            goto next##ctx_hi; \
        case 2: \
            levels_used = phx_trellis_coef1_##ctx_hi(ssd0[0], ssd1[0], cost_siglast, nodes_cur, nodes_prev, level_tree, levels_used, lambda2, level_state); /* :852 */ \
            levels_used = phx_trellis_coefn_##ctx_hi(2, ssd0[1], ssd1[1], cost_siglast, nodes_cur, nodes_prev, level_tree, levels_used, lambda2, level_state, levelgt1_ctx); /* :853 */ \
            goto next1; \
        default: \
            levels_used = phx_trellis_coefn_##ctx_hi(q-1, ssd0[0], ssd1[0], cost_siglast, nodes_cur, nodes_prev, level_tree, levels_used, lambda2, level_state, levelgt1_ctx); /* :856 */ \
            levels_used = phx_trellis_coefn_##ctx_hi(q, ssd0[1], ssd1[1], cost_siglast, nodes_cur, nodes_prev, level_tree, levels_used, lambda2, level_state, levelgt1_ctx); /* :857 */ \
            goto next1; \
        } \
        next##ctx_hi:; \
        } \
    }

int phx_rdo_quant_trellis_cabac(phx_cabac_t *cb, int16_t *dct,
                                const uint16_t *quant_mf,
                                const uint16_t *quant_bias0,
                                const int *unquant_mf, const uint8_t *zigzag,
                                int ctx_block_cat, int lambda2, int b_ac,
                                int b_chroma, int dc, int num_coefs, int idx,
                                int i_psy_trellis, const int16_t *psy_fenc,
                                int b_interlaced)
{
    phx_dctcoef orig_coefs[64], quant_coefs[64];              /* rdo.c:647-648 */
    memset(&phx_trellis_trace, 0, sizeof(phx_trellis_trace));  /* golden trace */
    const uint32_t *coef_weight1 = num_coefs == 64 ? phx_dct8_weight_tab : phx_dct4_weight_tab; /* :649 */
    const uint32_t *coef_weight2 = num_coefs == 64 ? phx_dct8_weight2_tab : phx_dct4_weight2_tab; /* :650 */
    uint8_t *cabac_state_sig = &cb->state[phx_sig_off[b_interlaced][ctx_block_cat]]; /* :652 */
    uint8_t *cabac_state_last = &cb->state[phx_last_off[b_interlaced][ctx_block_cat]]; /* :653 */
    int levelgt1_ctx = b_chroma && dc ? 8 : 9;                /* :654 */

    /* coef_weight1 is used by the psy branch inside PHX_TRELLIS_LOOP. */

    if (dc) {                                                 /* :656 */
        if (num_coefs == 16) {
            memcpy(orig_coefs, dct, sizeof(int16_t)*16);      /* :660 */
            if (!phx_quant_4x4_dc(dct, quant_mf[0] >> 1, quant_bias0[0] << 1)) /* :661 */
                return 0;
            for (int i = 0; i < 16; i++) quant_coefs[i] = dct[zigzag[i]]; /* :663 */
        } else {
            memcpy(orig_coefs, dct, sizeof(int16_t)*num_coefs); /* :667 */
            int nz = phx_quant_2x2_dc(&dct[0], quant_mf[0] >> 1, quant_bias0[0] << 1); /* :668 */
            if (num_coefs == 8)
                nz |= phx_quant_2x2_dc(&dct[4], quant_mf[0] >> 1, quant_bias0[0] << 1); /* :670 */
            if (!nz) return 0;                                /* :671 */
            for (int i = 0; i < num_coefs; i++)
                quant_coefs[i] = dct[zigzag[i]];              /* :674 */
        }
    } else {
        if (num_coefs == 64) {                                /* :679 */
            memcpy(orig_coefs, dct, sizeof(int16_t)*64);      /* :681 */
            if (!phx_quant_8x8(dct, quant_mf, quant_bias0))   /* :682 */
                return 0;
            for (int i = 0; i < 64; i++) quant_coefs[i] = dct[zigzag[i]]; /* :684 */
        } else {                                              /* :686 */
            memcpy(orig_coefs, dct, sizeof(int16_t)*16);      /* :688 */
            if (!phx_quant_4x4(dct, quant_mf, quant_bias0))   /* :689 */
                return 0;
            for (int i = 0; i < 16; i++) quant_coefs[i] = dct[zigzag[i]]; /* :691 */
        }
    }

    int last_nnz = phx_coeff_last(ctx_block_cat, quant_coefs+b_ac) + b_ac; /* :695 */
    phx_trellis_trace.last_nnz = last_nnz;                    /* golden trace */
    uint8_t *cabac_state = &cb->state[phx_level_m1_off[ctx_block_cat]];    /* :696 */

    /* rdo.c:698-706 — shortcut for DC-only blocks (no AC coefficients). */
    if (last_nnz == 0 && !dc) {
        int cost_sig = phx_cabac_size_decision_noup2(&cabac_state_sig[0], 1)
                     + phx_cabac_size_decision_noup2(&cabac_state_last[0], 1); /* :702-703 */
        dct[0] = (int16_t)phx_trellis_dc_shortcut(orig_coefs[0], quant_coefs[0],
                     unquant_mf[0], (int)coef_weight2[0], lambda2, cabac_state, cost_sig); /* :704 */
        return !!dct[0];                                      /* :705 */
    }

    phx_trellis_level_t level_tree[64*8*2];                   /* rdo.c:734 */
    int levels_used = 1;                                      /* :735 */
    phx_trellis_node_t nodes[2][8] = {{0}};                   /* :737 */
    phx_trellis_node_t *nodes_cur = nodes[0];                 /* :738 */
    phx_trellis_node_t *nodes_prev = nodes[1];                /* :739 */
    phx_trellis_node_t *bnode = NULL;                         /* :740 */
    for (int j = 1; j < 8; j++)
        nodes_cur[j].score = PHX_TRELLIS_SCORE_MAX;           /* :742 */
    nodes_cur[0].score = PHX_TRELLIS_SCORE_BIAS;              /* :743 */
    nodes_cur[0].level_idx = 0;                               /* :744 */
    level_tree[0].abs_level = 0;                              /* :745 */
    level_tree[0].next = 0;                                   /* :746 */
    uint8_t level_state[16];                                  /* :747 */
    memcpy(level_state, cabac_state, 10);                     /* :748 */
    level_state[12] = cabac_state[0];                         /* :749 */
    level_state[13] = cabac_state[4];                         /* :750 */
    level_state[14] = cabac_state[8];                         /* :751 */
    level_state[15] = cabac_state[9];                         /* :752 */
    memcpy(phx_trellis_trace.level_state, level_state, 16);   /* golden trace */

    idx &= num_coefs == 64 ? 3 : 15;                          /* :754 */
    (void)idx; /* psy_fenc is already selected by the caller */

    int i = last_nnz;                                         /* :763 */
    PHX_TRELLIS_LOOP(0);                                      /* rdo.c:870 */
    /* rdo.c:862-866 — best node of the ctx_lo pass. */
    bnode = &nodes_cur[0];
    for (int j = 1; j < 4; j++)
        if (nodes_cur[j].score < bnode->score)
            bnode = &nodes_cur[j];

    /* rdo.c:872-879. The second loop is only reachable via `goto next1`
     * from inside PHX_TRELLIS_LOOP(0) (rdo.c:881-885). */
    if (bnode == &nodes_cur[0]) {
        if (num_coefs == 16 && !dc)                           /* :876 */
            memset(dct, 0, 16*sizeof(int16_t));
        return 0;
    }

    if (0) {                                                  /* :881-885 */
        PHX_TRELLIS_LOOP(1);
        /* rdo.c:862-866 in the ctx_hi pass. */
        bnode = &nodes_cur[1];
        for (int j = 2; j < 8; j++)
            if (nodes_cur[j].score < bnode->score)
                bnode = &nodes_cur[j];
    }

    /* Golden trace: rdo.c:862-891, the best node's score and level path. */
    phx_trellis_trace.best_score = bnode->score;              /* :863-866 */
    phx_trellis_trace.levels_used = levels_used;              /* :735+ */
    int level = bnode->level_idx;                             /* :887 */
    for (i = b_ac; i <= last_nnz; i++) {                      /* :888 */
        phx_trellis_trace.best_levels[phx_trellis_trace.best_count++] =
            (int16_t)level_tree[level].abs_level;              /* golden trace */
        dct[zigzag[i]] = (int16_t)PHX_SIGN(level_tree[level].abs_level, dct[zigzag[i]]); /* :890 */
        level = level_tree[level].next;                       /* :891 */
    }
    return 1;                                                 /* :894 */
}


/* ------------------------------------------------------------------ */
/* Whole-MB size-only CABAC: rdo.c:31-69 + encoder/cabac.c:917-1086   */
/* ------------------------------------------------------------------ */

/* common/predict.h:45, :84-92. */
static const uint8_t m8_chroma_pred_mode_fix[7] = { 0, 1, 2, 3, 0, 0, 0 };
static const uint8_t m8_pred_mode16x16_fix[7]   = { 0, 1, 2, 3, 2, 2, 2 };

#define M8_DEC(cb,ctx,b)  phx_rdo_size_decision((cb),(ctx),(b))
#define M8_NOUP(cb,ctx,b) phx_rdo_size_decision_noup((cb),(ctx),(b))
#define M8_TERM(c)    phx_rdo_size_terminal((c))
#define M8_BYP(c)     phx_rdo_size_bypass((c))
#define M8_UEBYP(c,e,v) phx_rdo_size_ue_bypass((c),(e),(v))

/* common/macroblock.h:420-431 — x264_mb_predict_intra4x4_mode(). */
static int m8_predict_i4(const phx_cabac_mb_t *mb, int idx)
{
    const phx_mb_cache_t *c = mb->cache;
    int ma = c->intra4x4_pred_mode[phx_scan8[idx] - 1];
    int mbv = c->intra4x4_pred_mode[phx_scan8[idx] - 8];
    int m = PHX_MIN(PHX_RDO_FIX4(ma), PHX_RDO_FIX4(mbv));
    return m < 0 ? 2 : m;                                 /* predict.h:429 */
}

/* encoder/cabac.c:35-67 — cabac_mb_type_intra(). */
static void m8_mb_type_intra(phx_cabac_size_t *cb, phx_cabac_mb_t *mb,
                             int i_mb_type, int c0, int c1, int c2,
                             int c3, int c4, int c5)
{
    if (i_mb_type == PHX_I_4x4 || i_mb_type == PHX_I_8x8) {
        M8_NOUP(cb, c0, 0);                               /* :40 */
    } else {
        int i_pred = m8_pred_mode16x16_fix[mb->i_intra16x16_pred_mode]; /* :51 */
        M8_NOUP(cb, c0, 1);                               /* :53 */
        M8_TERM(cb);                                      /* :54 */
        M8_NOUP(cb, c1, !!mb->cache->i_cbp_luma);         /* :56 */
        if (mb->cache->i_cbp_chroma == 0)
            M8_NOUP(cb, c2, 0);                           /* :58 */
        else {
            M8_DEC(cb, c2, 1);                            /* :61 */
            M8_NOUP(cb, c3, mb->cache->i_cbp_chroma >> 1);/* :62 */
        }
        M8_DEC(cb, c4, i_pred >> 1);                      /* :64 */
        M8_NOUP(cb, c5, i_pred & 1);                      /* :65 */
    }
}

/* encoder/cabac.c:83-96 — cabac_intra4x4_pred_mode(). */
static void m8_intra4x4_pred(phx_cabac_size_t *cb, int i_pred, int i_mode)
{
    if (i_pred == i_mode) {
        M8_DEC(cb, 68, 1);                                /* :86 */
    } else {
        M8_DEC(cb, 68, 0);                                /* :89 */
        if (i_mode > i_pred) i_mode--;                     /* :91 */
        M8_DEC(cb, 69, i_mode & 1);                        /* :92 */
        M8_DEC(cb, 69, (i_mode >> 1) & 1);                 /* :93 */
        M8_DEC(cb, 69, i_mode >> 2);                       /* :94 */
    }
}

/* encoder/cabac.c:98-116 — cabac_intra_chroma_pred_mode(). */
static void m8_chroma_pred(phx_cabac_size_t *cb, phx_cabac_mb_t *mb)
{
    int i_mode = m8_chroma_pred_mode_fix[mb->i_chroma_pred_mode]; /* :100 */
    int ctx = 0;
    if ((mb->cache->i_neighbour & PHX_MB_LEFT) && mb->chroma_pred_mode_left != 0)
        ctx++;                                            /* :104-105 */
    if ((mb->cache->i_neighbour & PHX_MB_TOP) && mb->chroma_pred_mode_top != 0)
        ctx++;                                            /* :106-107 */
    M8_NOUP(cb, 64 + ctx, i_mode > 0);                    /* :109 */
    if (i_mode > 0) {
        M8_DEC(cb, 64 + 3, i_mode > 1);                   /* :112 */
        if (i_mode > 1)
            M8_NOUP(cb, 64 + 3, i_mode > 2);              /* :114 */
    }
}

/* encoder/cabac.c:118-127 — cabac_cbp_luma(). */
static void m8_cbp_luma(phx_cabac_size_t *cb, phx_cabac_mb_t *mb)
{
    int cbp = mb->cache->i_cbp_luma;
    int cbp_l = mb->cache->i_cbp_left;
    int cbp_t = mb->cache->i_cbp_top;
    M8_DEC(cb, 76 - ((cbp_l >> 1) & 1) - ((cbp_t >> 1) & 2), (cbp >> 0) & 1); /* :123 */
    M8_DEC(cb, 76 - ((cbp   >> 0) & 1) - ((cbp_t >> 2) & 2), (cbp >> 1) & 1); /* :124 */
    M8_DEC(cb, 76 - ((cbp_l >> 3) & 1) - ((cbp   << 1) & 2), (cbp >> 2) & 1); /* :125 */
    M8_NOUP(cb, 76 - ((cbp >> 2) & 1) - ((cbp >> 0) & 2), (cbp >> 3) & 1);    /* :126 */
}

/* encoder/cabac.c:129-148 — cabac_cbp_chroma(). */
static void m8_cbp_chroma(phx_cabac_size_t *cb, phx_cabac_mb_t *mb)
{
    int cbp_a = mb->cache->i_cbp_left & 0x30;
    int cbp_b = mb->cache->i_cbp_top  & 0x30;
    int ctx = 0;
    if (cbp_a && mb->cache->i_cbp_left != -1) ctx++;       /* :135 */
    if (cbp_b && mb->cache->i_cbp_top  != -1) ctx += 2;    /* :136 */
    if (mb->cache->i_cbp_chroma == 0) {
        M8_NOUP(cb, 77 + ctx, 0);                          /* :138 */
    } else {
        M8_NOUP(cb, 77 + ctx, 1);                          /* :141 */
        ctx = 4;
        if (cbp_a == 0x20) ctx++;                          /* :144 */
        if (cbp_b == 0x20) ctx += 2;                       /* :145 */
        M8_NOUP(cb, 77 + ctx, mb->cache->i_cbp_chroma >> 1); /* :146 */
    }
}

/* encoder/cabac.c:235-239 — cabac_transform_size(). */
static void m8_transform_size(phx_cabac_size_t *cb, phx_cabac_mb_t *mb)
{
    M8_NOUP(cb, 399 + mb->cache->i_neighbour_transform_size,
            mb->cache->b_transform_8x8);                   /* :237-238 */
}

/* encoder/cabac.c:150-186 — cabac_qp_delta(). */
static void m8_qp_delta(phx_cabac_size_t *cb, phx_cabac_mb_t *mb)
{
    int i_dqp = mb->i_qp - mb->i_last_qp;                   /* :152 */
    if (mb->i_type == PHX_I_16x16 && !mb->curr_cbp && mb->i_qp > mb->i_last_qp)
        i_dqp = 0;                                          /* :158-163 */
    int ctx = mb->i_last_dqp &&
              (mb->prev_type == PHX_I_16x16 || (mb->prev_cbp & 0x3f)); /* :166 */
    if (i_dqp != 0) {
        i_dqp *= 2;                                         /* :172 */
        int val = 1 - i_dqp;
        if (val < 0) val = i_dqp;                           /* :174 */
        val--;                                              /* :175 */
        if (val >= PHX_QUANT_QP_MAX_SPEC && val != PHX_QUANT_QP_MAX_SPEC+1)
            val = 2*PHX_QUANT_QP_MAX_SPEC + 1 - val;        /* :177-178 */
        do {
            M8_DEC(cb, 60 + ctx, 1);                        /* :181 */
            ctx = 2 + (ctx >> 1);                           /* :182 */
        } while (--val);
    }
    M8_NOUP(cb, 60 + ctx, 0);                              /* :185 */
}

/* encoder/cabac.c:241-268 — cabac_ref_internal().  bframe==0 for P. */
static void m8_ref(phx_cabac_size_t *cb, phx_cabac_mb_t *mb,
                   int i_list, int idx, int bframe)
{
    const phx_mb_cache_t *c = mb->cache;
    int i_refa = c->ref[i_list][phx_scan8[idx] - 1];        /* :244 */
    int i_refb = c->ref[i_list][phx_scan8[idx] - 8];        /* :245 */
    int ctx = 0;
    if (i_refa > 0 && (!bframe || !mb->skip[phx_scan8[idx] - 1])) ctx++;    /* :248 */
    if (i_refb > 0 && (!bframe || !mb->skip[phx_scan8[idx] - 8])) ctx += 2; /* :250 */
    for (int i_ref = c->ref[i_list][phx_scan8[idx]]; i_ref > 0; i_ref--) { /* :253 */
        M8_DEC(cb, 54 + ctx, 1);                            /* :255 */
        ctx = (ctx >> 2) + 4;                               /* :256 */
    }
    M8_DEC(cb, 54 + ctx, 0);                                /* :258 */
}

/* encoder/cabac.c:270-327 — cabac_mvd_cpn(), RDO_SKIP_BS branch.
 * Returns the clipped absolute MVD (base.h:X264_MIN(i_abs,66), :324-326). */
static int m8_mvd_cpn(phx_cabac_size_t *cb, phx_cabac_mb_t *mb,
                      int i_list, int idx, int l, int mvd, int ctx)
{
    (void)mb;
    int ctxbase = l ? 47 : 40;                              /* :272 */
    if (mvd == 0) {
        M8_DEC(cb, ctxbase + ctx, 0);                       /* :276 */
        return 0;
    }
    int i_abs = abs(mvd);
    M8_DEC(cb, ctxbase + ctx, 1);                           /* :281 */
    if (i_abs <= 3) {                                       /* :283 */
        for (int i = 1; i < i_abs; i++)
            M8_DEC(cb, ctxbase + i + 2, 1);                 /* :286 */
        M8_DEC(cb, ctxbase + i_abs + 2, 0);                 /* :287 */
        M8_BYP(cb);                                         /* :288 */
    } else {
        M8_DEC(cb, ctxbase + 3, 1);                         /* :292 */
        M8_DEC(cb, ctxbase + 4, 1);                         /* :293 */
        M8_DEC(cb, ctxbase + 5, 1);                         /* :294 */
        if (i_abs < 9) {
            cb->f8_bits_encoded += phx_size_unary[i_abs-3][cb->state[ctxbase+6]]; /* :297 */
            cb->state[ctxbase+6] = phx_transition_unary[i_abs-3][cb->state[ctxbase+6]]; /* :298 */
        } else {
            cb->f8_bits_encoded += phx_size_5ones[cb->state[ctxbase+6]]; /* :302 */
            cb->state[ctxbase+6] = phx_transition_5ones[cb->state[ctxbase+6]]; /* :303 */
            M8_UEBYP(cb, 3, i_abs - 9);                     /* :304 */
        }
    }
    return PHX_MIN(i_abs, 66);                              /* :326 */
}

/* common/base.h:259-266 — x264_cabac_mvd_sum(). */
static int m8_mvd_sum(const int16_t *left, const int16_t *top)
{
    int a0 = left[0] + top[0];
    int a1 = left[1] + top[1];
    return ((a0 > 2) + (a0 > 32)) + (((a1 > 2) + (a1 > 32)) << 8);
}

/* encoder/cabac.c:329-353 — cabac_mvd(), with precomputed MVD input. */
static void m8_mvd(phx_cabac_size_t *cb, phx_cabac_mb_t *mb,
                   const phx_rdo_mb_data_t *data, int i_list, int idx)
{
    phx_mb_cache_t *c = mb->cache;
    const int s8 = phx_scan8[idx];
    int amvd = m8_mvd_sum(c->mvd[i_list][s8 - 1], c->mvd[i_list][s8 - 8]); /* :338 */
    int mdx = m8_mvd_cpn(cb, mb, i_list, idx, 0, data->mvd[i_list][idx/4][0],
                         amvd & 0xff);                        /* :342 */
    int mdy = m8_mvd_cpn(cb, mb, i_list, idx, 1, data->mvd[i_list][idx/4][1],
                         amvd >> 8);                          /* :343 */
    /* encoder/macroblock.c:1150 — x264_macroblock_cache_mvd(). */
    c->mvd[i_list][s8][0] = (int16_t)mdx;
    c->mvd[i_list][s8][1] = (int16_t)mdy;
}

/* encoder/cabac.c:198-213 — cabac_subpartition_p(). */
static void m8_subpart_p(phx_cabac_size_t *cb, int i_sub)
{
    if (i_sub == PHX_D_L0_8x8) {
        M8_DEC(cb, 21, 1); return;                          /* :202 */
    }
    M8_DEC(cb, 21, 0);                                      /* :205 */
    if (i_sub == PHX_D_L0_8x4)
        M8_DEC(cb, 22, 0);                                  /* :207 */
    else {
        M8_DEC(cb, 22, 1);                                  /* :210 */
        M8_DEC(cb, 23, i_sub == PHX_D_L0_4x8);              /* :211 */
    }
}

/* encoder/cabac.c:381-433 — cabac_mb_header_i(). */
static void m8_header_i(phx_cabac_size_t *cb, phx_cabac_mb_t *mb,
                        int i_mb_type, int slice_type)
{
    if (slice_type == PHX_SLICE_I) {
        int ctx = 0;
        if ((mb->cache->i_neighbour & PHX_MB_LEFT) &&
            mb->cache->type_left[0] != PHX_I_4x4) ctx++;    /* :386-387 */
        if ((mb->cache->i_neighbour & PHX_MB_TOP) &&
            mb->cache->type_top != PHX_I_4x4) ctx++;        /* :388-389 */
        m8_mb_type_intra(cb, mb, i_mb_type, 3+ctx, 3+3, 3+4, 3+5, 3+6, 3+7); /* :391 */
    } else if (slice_type == PHX_SLICE_P) {
        M8_NOUP(cb, 14, 1);                                 /* :396 */
        m8_mb_type_intra(cb, mb, i_mb_type, 17, 18, 19, 19, 20, 20); /* :399 */
    } else {
        /* B slice: cabac.c:401-412. */
        M8_NOUP(cb, 27+3, 1); M8_NOUP(cb, 27+4, 1);         /* :404-405 */
        M8_DEC(cb, 27+5, 1); M8_DEC(cb, 27+5, 0);
        M8_DEC(cb, 27+5, 1);                                /* :406-408 */
        m8_mb_type_intra(cb, mb, i_mb_type, 32, 33, 34, 34, 35, 35); /* :411 */
    }
    if (i_mb_type != PHX_I_16x16) {
        if (mb->b_transform_8x8_mode)
            m8_transform_size(cb, mb);                      /* :419-420 */
        int di = mb->cache->b_transform_8x8 ? 4 : 1;        /* :422 */
        for (int i = 0; i < 16; i += di) {
            int i_pred = m8_predict_i4(mb, i);              /* :425 */
            int i_mode = PHX_RDO_FIX4(mb->cache->intra4x4_pred_mode[phx_scan8[i]]); /* :426 */
            m8_intra4x4_pred(cb, i_pred, i_mode);           /* :427 */
        }
    }
    m8_chroma_pred(cb, mb);                                 /* :432 */
}

/* encoder/cabac.c:435-497 — cabac_mb_header_p(). */
static void m8_header_p(phx_cabac_size_t *cb, phx_cabac_mb_t *mb,
                        const phx_rdo_mb_data_t *data, int i_mb_type)
{
    if (i_mb_type == PHX_P_L0) {
        M8_NOUP(cb, 14, 0);                                 /* :439 */
        if (mb->i_partition == PHX_D_16x16) {
            M8_NOUP(cb, 15, 0); M8_NOUP(cb, 16, 0);         /* :442-443 */
            if (data->i_fref[0] > 1) m8_ref(cb, mb, 0, 0, 0);/* :445 */
            m8_mvd(cb, mb, data, 0, 0);                     /* :446 */
        } else if (mb->i_partition == PHX_D_16x8) {
            M8_NOUP(cb, 15, 1); M8_NOUP(cb, 17, 1);         /* :450-451 */
            if (data->i_fref[0] > 1) {
                m8_ref(cb, mb, 0, 0, 0); m8_ref(cb, mb, 0, 8, 0); /* :454-455 */
            }
            m8_mvd(cb, mb, data, 0, 0); m8_mvd(cb, mb, data, 0, 8); /* :457-458 */
        } else { /* D_8x16 */
            M8_NOUP(cb, 15, 1); M8_NOUP(cb, 17, 0);         /* :462-463 */
            if (data->i_fref[0] > 1) {
                m8_ref(cb, mb, 0, 0, 0); m8_ref(cb, mb, 0, 4, 0); /* :466-467 */
            }
            m8_mvd(cb, mb, data, 0, 0); m8_mvd(cb, mb, data, 0, 4); /* :469-470 */
        }
    } else if (i_mb_type == PHX_P_8x8) {
        M8_NOUP(cb, 14, 0); M8_NOUP(cb, 15, 0); M8_NOUP(cb, 16, 1); /* :475-477 */
        for (int i = 0; i < 4; i++)
            m8_subpart_p(cb, data->sub_partition[i]);       /* :481 */
        if (data->i_fref[0] > 1)
            for (int i = 0; i < 4; i++)
                m8_ref(cb, mb, 0, 4*i, 0);                  /* :486-489 */
        for (int i = 0; i < 4; i++) {
            int sub = data->sub_partition[i];
            int n = (sub == PHX_D_L0_8x8) ? 1 :
                    ((sub == PHX_D_L0_8x4 || sub == PHX_D_L0_4x8) ? 2 : 4);
            for (int k = 0; k < n; k++)
                m8_mvd(cb, mb, data, 0, 4*i + (n == 2 ? k*2 : k)); /* :493, cabac.c:355-379 */
        }
    } else
        m8_header_i(cb, mb, i_mb_type, PHX_SLICE_P);         /* :496 */
}

/* encoder/cabac.c:917-1086 — macroblock_write_cabac_internal(),
 * 4:2:0 frame with b_transform_8x8 == 0 (RDO_SKIP_BS bin costs). */
static void m8_header_b(phx_cabac_size_t *cb, phx_cabac_mb_t *mb,
                        const phx_rdo_mb_data_t *data, int i_mb_type);

/* common/macroblock.h:445-461 — x264_transform_allowed[] and the P_8x8
 * sub-partition condition, used by the :973 transform-size gate.  SHADER: A. */
static int m8_transform_allowed(const phx_cabac_mb_t *mb,
                                const phx_rdo_mb_data_t *data)
{
    static const uint8_t allowed[19] = {
        0,0,0,0,1,2,0,1,1,1,1,1,1,1,1,1,1,1,0
    };
    int type = mb->i_type;
    if (type < 0 || type > 18) return 0;
    if (type == PHX_P_8x8) {
        for (int i = 0; i < 4; i++)
            if (data->sub_partition[i] != PHX_D_L0_8x8) return 0; /* :460 */
        return 1;
    }
    return allowed[type];
}

static int m8_macroblock_size_internal(phx_cabac_size_t *cb,
                                       phx_cabac_mb_t *mb,
                                       const phx_rdo_mb_data_t *data,
                                       int slice_type)
{
    const int i_mb_type = mb->i_type;                        /* :919 */
    if (slice_type == PHX_SLICE_P)
        m8_header_p(cb, mb, data, i_mb_type);                /* :933 */
    else if (slice_type == PHX_SLICE_B)
        m8_header_b(cb, mb, data, i_mb_type);                /* :935 */
    else if (slice_type == PHX_SLICE_I)
        m8_header_i(cb, mb, i_mb_type, PHX_SLICE_I);         /* :937 */

    if (i_mb_type != PHX_I_16x16) {                          /* :966 */
        m8_cbp_luma(cb, mb);                                /* :968 */
        m8_cbp_chroma(cb, mb);                              /* :970 */
    }
    if (mb->b_transform_8x8_mode && mb->cache->i_cbp_luma &&
        m8_transform_allowed(mb, data))                    /* :973 */
        m8_transform_size(cb, mb);                          /* :975 */

    if (mb->cache->i_cbp_luma || mb->cache->i_cbp_chroma ||
        i_mb_type == PHX_I_16x16) {                          /* :978 */
        const int b_intra = (i_mb_type == PHX_I_16x16 ||
                             i_mb_type == PHX_I_4x4 ||
                             i_mb_type == PHX_I_8x8 ||
                             i_mb_type == PHX_I_PCM);       /* :980 */
        m8_qp_delta(cb, mb);                                /* :981 */

        if (i_mb_type == PHX_I_16x16) {                     /* :984 */
            phx_rdo_size_block_residual_cbf_ex(cb, mb, PHX_DCT_LUMA_DC,
                PHX_LUMA_DC, data->luma16x16_dc, b_intra, 1);  /* :989 */
            if (mb->cache->i_cbp_luma)
                for (int i = 0; i < 16; i++)
                    phx_rdo_size_block_residual_cbf(cb, mb, PHX_DCT_LUMA_AC,
                        i, data->luma4x4[i]+1, b_intra);    /* :994 */
        } else if (mb->cache->b_transform_8x8) {
            /* cabac.c:1046-1050 — 4:2:0 plane_count==1: one 64-coefficient
             * 8x8 residual per CBP bit, with no per-8x8 CBF (the four luma
             * CBP bins at :968 already signalled them). */
            for (int i8x8 = 0; i8x8 < 4; i8x8++)
                if (mb->cache->i_cbp_luma & (1 << i8x8))
                    phx_rdo_size_block_residual_8x8(cb, PHX_DCT_LUMA_8x8,
                                                    data->luma8x8[i8x8]); /* :1049 */
        } else {
            for (int i8x8 = 0; i8x8 < 4; i8x8++)
                if (mb->cache->i_cbp_luma & (1 << i8x8))
                    for (int i = 0; i < 4; i++) {
                        int idx = i + i8x8*4;
                        phx_rdo_size_block_residual_cbf(cb, mb,
                            PHX_DCT_LUMA_4x4, idx, data->luma4x4[idx], b_intra); /* :1057 */
                    }
        }
        if (mb->cache->i_cbp_chroma) {                      /* :1060 */
            for (int ch = 0; ch < 2; ch++)
                phx_rdo_size_block_residual_cbf_ex(cb, mb, PHX_DCT_CHROMA_DC,
                    PHX_CHROMA_DC + ch, data->chroma_dc[ch], b_intra, 1); /* :1069-1070 */
            if (mb->cache->i_cbp_chroma == 2)               /* :1073 */
                for (int j = 16; j < 32; j++)
                    phx_rdo_size_block_residual_cbf(cb, mb, PHX_DCT_CHROMA_AC,
                        j, data->luma4x4[j]+1, b_intra);    /* :1078 */
        }
    }
    return cb->f8_bits_encoded;                             /* rdo.c:184 */
}


/* ------------------------------------------------------------------ */
/* B-slice mb_type headers + B RD helpers                              */
/* encoder/cabac.c:499-610, analyse.c:2622-2690/3404-3415, rdo.c:162-197 */
/* ------------------------------------------------------------------ */

/* common/macroblock.h:97-109, :143-159 (Phoenix constants are identical). */
static const uint8_t m8_mb_type_list[19][2][2] = {
    {{0,0},{0,0}}, {{0,0},{0,0}}, {{0,0},{0,0}}, {{0,0},{0,0}},
    {{1,1},{0,0}}, {{0,0},{0,0}}, {{1,1},{0,0}},
    {{0,0},{0,0}},                                  /* B_DIRECT */
    {{1,1},{0,0}}, {{1,0},{0,1}}, {{1,1},{0,1}},    /* B_L0_* */
    {{0,1},{1,0}}, {{0,0},{1,1}}, {{0,1},{1,1}},    /* B_L1_* */
    {{1,1},{1,0}}, {{1,0},{1,1}}, {{1,1},{1,1}},    /* B_BI_* */
    {{0,0},{0,0}}, {{0,0},{0,0}}
};
static const uint8_t m8_part_list[2][17] = {
    { 1,1,1,1, 0,0,0,0, 1,1,1,1, 0, 0,0,0,0 },
    { 0,0,0,0, 1,1,1,1, 1,1,1,1, 1, 0,0,0,0 }
};
/* encoder/cabac.c:548-558 — i_mb_bits[B_L0_L0..B_BI_BI][partition]. */
static const uint8_t m8_mb_bits[9*3] = {
    0x31,0x29,0x04, 0x35,0x2d,0x00, 0x43,0x63,0x00,
    0x3d,0x2f,0x00, 0x39,0x25,0x06, 0x53,0x73,0x00,
    0x4b,0x6b,0x00, 0x5b,0x7b,0x00, 0x47,0x67,0x21
};

/* encoder/cabac.c:215-233 — cabac_subpartition_b(). */
static void m8_subpart_b(phx_cabac_size_t *cb, int i_sub)
{
    if (i_sub == PHX_D_DIRECT_8x8) {
        M8_DEC(cb, 36, 0); return;                     /* :218-220 */
    }
    M8_DEC(cb, 36, 1);                                 /* :222 */
    if (i_sub == PHX_D_BI_8x8) {                       /* :223 */
        M8_DEC(cb, 37, 1); M8_DEC(cb, 38, 0);
        M8_DEC(cb, 39, 0); M8_DEC(cb, 39, 0); return;  /* :225-228 */
    }
    M8_DEC(cb, 37, 0);                                 /* :231 */
    M8_DEC(cb, 39, i_sub == PHX_D_L1_8x8);             /* :232 */
}

/* encoder/cabac.c:499-610 — cabac_mb_header_b(). */
static void m8_header_b(phx_cabac_size_t *cb, phx_cabac_mb_t *mb,
                        const phx_rdo_mb_data_t *data, int i_mb_type)
{
    int ctx = 0;                                        /* :501 */
    if ((mb->cache->i_neighbour & PHX_MB_LEFT) &&
        mb->cache->type_left[0] != PHX_B_SKIP &&
        mb->cache->type_left[0] != PHX_B_DIRECT) ctx++;  /* :502-503 */
    if ((mb->cache->i_neighbour & PHX_MB_TOP) &&
        mb->cache->type_top != PHX_B_SKIP &&
        mb->cache->type_top != PHX_B_DIRECT) ctx += 2;   /* :504-505 */

    if (i_mb_type == PHX_B_DIRECT) {
        M8_NOUP(cb, 27 + ctx, 0);                       /* :509 */
        return;
    }
    M8_NOUP(cb, 27 + ctx, 1);                           /* :512 */

    if (i_mb_type == PHX_B_8x8) {
        M8_NOUP(cb, 27+3, 1); M8_NOUP(cb, 27+4, 1);     /* :516-517 */
        M8_DEC(cb, 27+5, 1); M8_DEC(cb, 27+5, 1);
        M8_NOUP(cb, 27+5, 1);                           /* :518-520 */
        for (int i = 0; i < 4; i++) {
            int sub = data->sub_partition[i];
            m8_subpart_b(cb, sub);                      /* :524 */
            if (data->i_fref[0] > 1 && m8_part_list[0][sub])
                m8_ref(cb, mb, 0, 4*i, 1);              /* :527-530 */
            if (data->i_fref[1] > 1 && m8_part_list[1][sub])
                m8_ref(cb, mb, 1, 4*i, 1);              /* :532-535 */
        }
        for (int l = 0; l < 2; l++)
            for (int i = 0; i < 4; i++)
                if (m8_part_list[l][data->sub_partition[i]])
                    m8_mvd(cb, mb, data, l, 4*i);       /* :537-543 */
        return;
    }

    if (i_mb_type >= PHX_B_L0_L0 && i_mb_type <= PHX_B_BI_BI) {
        const uint8_t (*b_list)[2] = m8_mb_type_list[i_mb_type];
        int idx = (i_mb_type - PHX_B_L0_L0)*3 +
                  (mb->i_partition - PHX_D_16x8);       /* :561 */
        int bits = m8_mb_bits[idx];                     /* :562 */
        M8_NOUP(cb, 27+3, bits & 1);                    /* :564 */
        M8_DEC(cb, 27+5-(bits&1), (bits>>1)&1); bits >>= 2; /* :565 */
        if (bits != 1) {
            M8_DEC(cb, 27+5, bits&1); bits >>= 1;       /* :568 */
            M8_DEC(cb, 27+5, bits&1); bits >>= 1;       /* :569 */
            M8_DEC(cb, 27+5, bits&1); bits >>= 1;       /* :570 */
            if (bits != 1)
                M8_NOUP(cb, 27+5, bits&1);             /* :572 */
        }
        for (int l = 0; l < 2; l++) {
            if (data->i_fref[l] > 1) {
                if (b_list[l][0])
                    m8_ref(cb, mb, l, 0, 1);            /* :579/:586 */
                if (b_list[l][1] && mb->i_partition != PHX_D_16x16)
                    m8_ref(cb, mb, l, 8 >> (mb->i_partition == PHX_D_8x16), 1); /* :581/:588 */
            }
        }
        for (int l = 0; l < 2; l++) {
            if (mb->i_partition == PHX_D_16x16) {
                if (b_list[l][0]) m8_mvd(cb, mb, data, l, 0);       /* :594 */
            } else if (mb->i_partition == PHX_D_16x8) {
                if (b_list[l][0]) m8_mvd(cb, mb, data, l, 0);       /* :598 */
                if (b_list[l][1]) m8_mvd(cb, mb, data, l, 8);       /* :599 */
            } else { /* D_8x16 */
                if (b_list[l][0]) m8_mvd(cb, mb, data, l, 0);       /* :603 */
                if (b_list[l][1]) m8_mvd(cb, mb, data, l, 4);       /* :604 */
            }
        }
        return;
    }
    m8_header_i(cb, mb, i_mb_type, PHX_SLICE_B);         /* :609 */
}

/* encoder/analyse.c:2624 — early-termination threshold. */
int phx_rdo_b_rd_threshold(int i_satd_inter, int b_early_terminate, int i_psy_rd)
{
    return b_early_terminate
         ? i_satd_inter * (17 + (i_psy_rd != 0)) / 16 + 1
         : (1 << 28);                                   /* me.h:30 COST_MAX */
}

/* encoder/analyse.c:3404-3415 — RD-based B_SKIP promotion. */
int phx_rdo_b_early_skip(const phx_rdo_b_rd_t *rd,
                         int b_mbrd, int b_early_terminate)
{
    if (!rd || !b_mbrd || !b_early_terminate ||
        rd->i_cost16x16direct > rd->i_cost * 33 / 32)
        return 0;                                       /* :3404 */
    return rd->i_bskip_cost < rd->rd_direct &&
           rd->i_bskip_cost < rd->rd_bi &&
           rd->i_bskip_cost < rd->rd_l0 &&
           rd->i_bskip_cost < rd->rd_l1;                /* :3407-3410 */
}

int phx_rdo_size_macroblock_cabac(phx_cabac_size_t *out,
                                  const phx_cabac_t *live,
                                  phx_cabac_mb_t *mb,
                                  const phx_rdo_mb_data_t *data,
                                  int slice_type, int qp)
{
    if (!out || !mb || !data || !mb->cache)
        return 0;
    phx_rdo_size_init(out, live, slice_type, qp);           /* rdo.c:71-73 */
    if (mb->i_type == PHX_P_SKIP || mb->i_type == PHX_B_SKIP ||
        mb->i_type == PHX_B_DIRECT) {
        out->f8_bits_encoded = 256;                         /* one bit, FIX8 */
        return out->f8_bits_encoded;                        /* rdo.c:176-179 */
    }
    phx_cabac_mb_t local = *mb;
    phx_mb_cache_t cache = *mb->cache;
    local.cache = &cache;
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < 4; i++)
            cache.ref[l][phx_scan8[8*i]] = (int8_t)data->ref[l][i];
    local.i_fref[0] = data->i_fref[0];
    local.i_fref[1] = data->i_fref[1];
    return m8_macroblock_size_internal(out, &local, data, slice_type);
}

int phx_rdo_macroblock_bits(const phx_cabac_t *live,
                            phx_cabac_mb_t *mb,
                            const phx_rdo_mb_data_t *data,
                            int slice_type, int qp, int lambda2)
{
    phx_cabac_size_t cb;
    int f8 = phx_rdo_size_macroblock_cabac(&cb, live, mb, data, slice_type, qp);
    if (mb && (mb->i_type == PHX_P_SKIP || mb->i_type == PHX_B_SKIP ||
               mb->i_type == PHX_B_DIRECT))
        return (lambda2 + 128) >> 8;                        /* rdo.c:178 */
    return (int)(((uint64_t)f8 * (uint64_t)lambda2 + 32768) >> 16); /* :185 */
}

void phx_rdo_get_trellis_trace(phx_rdo_trellis_trace_t *out)
{
    if (out)
        *out = phx_trellis_trace;
}

/* ------------------------------------------------------------------ */
/* CAVLC bounded bit-counter QNS: rdo.c:897-1096                       */
/* ------------------------------------------------------------------ */

/* rdo.c:44-50 redefines the bs_* macros as bit counters.  We reuse the
 * already-ported CAVLC writer with a scratch bitstream and count the bits it
 * emits.  cavlc.c:102-105's 2000-bit overflow penalty is applied explicitly. */
static int phx_cavlc_count_bits(const int16_t *coefs, int ctx_block_cat,
                                int nC, int num_coefs, int profile_idc,
                                int chroma_2x4_dc)
{
    uint8_t scratch[512];
    phx_bs_t bs;
    phx_cavlc_t c;
    int before, after;

    phx_bs_init(&bs, scratch, (int)sizeof(scratch));
    c.bs = &bs;
    c.profile_idc = profile_idc;
    c.b_overflow = 0;
    c.chroma_2x4_dc = chroma_2x4_dc;
    before = phx_bs_pos(&bs);
    phx_cavlc_block_residual_internal(&c, ctx_block_cat, coefs, nC, num_coefs); /* rdo.c:1019/1052 */
    after = phx_bs_pos(&bs);
    if (c.b_overflow)                                         /* cavlc.c:102-105 */
        after += 2000;
    return after - before;
}

int phx_rdo_quant_trellis_cavlc(int16_t *dct, const uint16_t *quant_mf,
                                const int *unquant_mf, const uint8_t *zigzag,
                                int ctx_block_cat, int lambda2, int b_ac,
                                int b_chroma, int dc, int num_coefs, int idx,
                                int b_8x8, int nC, int profile_idc,
                                int chroma_2x4_dc)
{
    int16_t quant_coefs[2][16], coefs[16];                    /* rdo.c:926-927 */
    const uint32_t *coef_weight1 = b_8x8 ? phx_dct8_weight_tab : phx_dct4_weight_tab; /* :928 */
    const uint32_t *coef_weight2 = b_8x8 ? phx_dct8_weight2_tab : phx_dct4_weight2_tab; /* :929 */
    int64_t delta_distortion[16];                             /* :930 */
    int64_t score = 1ULL << 62;                               /* :931 */
    const int f = 1 << 15;                                    /* :933 */
    (void)coef_weight1;

    /* rdo.c:934-935 — nC is provided by the caller (ct_index already applied
     * by common/cavlc.c:198). */
    (void)b_chroma; (void)idx;

    for (int i = 0; i < 16; i++) coefs[i] = 0;                /* :937-938 */

    /* rdo.c:940-951 — 8x8dct -> 4x4dct CAVLC munging. */
    int step = 1;
    int start = b_ac;
    int end = num_coefs - 1;
    if (b_8x8) {
        start = idx & 3;
        end = 60 + start;
        step = 4;
    }
    idx &= 15;

    lambda2 <<= PHX_LAMBDA_BITS;                              /* :953 */

    /* rdo.c:955-961 — find last nonzero coefficient. */
    int i, j;
    for (i = end; i >= start; i -= step)
        if (abs(dct[zigzag[i]]) * (dc ? quant_mf[0] >> 1 : quant_mf[zigzag[i]]) >= f)
            break;
    if (i < start)
        goto phx_zeroblock;                                   /* :961 */

    int last_nnz = b_8x8 ? i >> 2 : i;                        /* :968 */
    int coef_mask = 0;                                        /* :969 */
    int round_mask = 0;                                       /* :970 */
    for (i = b_ac, j = start; i <= last_nnz; i++, j += step) { /* :971 */
        int coef = dct[zigzag[j]];                            /* :973 */
        int abs_coef = abs(coef);                             /* :974 */
        int sign = coef < 0 ? -1 : 1;                         /* :975 */
        int nearest_quant = (f + abs_coef * (dc ? quant_mf[0] >> 1 : quant_mf[zigzag[j]])) >> 16; /* :976 */
        quant_coefs[1][i] = quant_coefs[0][i] = (int16_t)(sign * nearest_quant); /* :977 */
        coefs[i] = quant_coefs[1][i];                         /* :978 */
        if (nearest_quant) {                                  /* :979 */
            int deadzone_quant = (f/2 + abs_coef * (dc ? quant_mf[0] >> 1 : quant_mf[zigzag[j]])) >> 16; /* :985 */
            int unquant1 = (((dc ? unquant_mf[0] << 1 : unquant_mf[zigzag[j]]) * (nearest_quant-0) + 128) >> 8); /* :986 */
            int unquant0 = (((dc ? unquant_mf[0] << 1 : unquant_mf[zigzag[j]]) * (nearest_quant-1) + 128) >> 8); /* :987 */
            int d1 = abs_coef - unquant1;                     /* :988 */
            int d0 = abs_coef - unquant0;                     /* :989 */
            delta_distortion[i] = (int64_t)(d0*d0 - d1*d1) * (int64_t)(dc ? 256 : coef_weight2[zigzag[j]]); /* :990 */
            /* Psy-trellis is intentionally omitted: it needs fenc_dct data
             * that the CAVLC interface does not carry (rdo.c:992-1001). */
            quant_coefs[0][i] = (int16_t)(sign * (nearest_quant-1)); /* :1003 */
            if (deadzone_quant != nearest_quant)              /* :1004 */
                coefs[i] = quant_coefs[0][i];
            else
                round_mask |= 1 << i;                         /* :1007 */
        } else
            delta_distortion[i] = 0;                          /* :1010 */
        coef_mask |= (!!coefs[i]) << i;                       /* :1011 */
    }

    /* rdo.c:1014-1020 — starting cost. */
    if (!coef_mask)
        score = phx_cavlc_coeff0(nC).i_bits * (int64_t)lambda2; /* :1016-1017 */
    else
        score = (int64_t)phx_cavlc_count_bits(coefs+b_ac, ctx_block_cat, nC,
                    phx_count_cat_m1[ctx_block_cat]+1, profile_idc, chroma_2x4_dc) * lambda2; /* :1019-1020 */

    /* rdo.c:1022-1077 — greedy bit-distortion QNS. */
    while (1) {
        int64_t iter_score = score;                           /* :1027 */
        int64_t iter_distortion_delta = 0;                    /* :1028 */
        int iter_coef = -1;                                   /* :1029 */
        int iter_mask = coef_mask;                            /* :1030 */
        int iter_round = round_mask;                          /* :1031 */
        for (i = b_ac; i <= last_nnz; i++) {                  /* :1032 */
            if (!delta_distortion[i]) continue;               /* :1034 */
            int cur_round = round_mask ^ (1 << i);            /* :1038 */
            int round_change = (cur_round >> i) & 1;          /* :1039 */
            int old_coef = coefs[i];                          /* :1040 */
            int new_coef = quant_coefs[round_change][i];      /* :1041 */
            int cur_mask = (coef_mask & ~(1 << i)) | (!!new_coef << i); /* :1042 */
            int64_t cur_distortion_delta = delta_distortion[i] * (round_change ? -1 : 1); /* :1043 */
            int64_t cur_score = cur_distortion_delta;         /* :1044 */
            coefs[i] = (int16_t)new_coef;                     /* :1045 */
            int bits = !cur_mask ? phx_cavlc_coeff0(nC).i_bits
                                 : phx_cavlc_count_bits(coefs+b_ac, ctx_block_cat, nC,
                                       phx_count_cat_m1[ctx_block_cat]+1, profile_idc, chroma_2x4_dc); /* :1049-1052 */
            cur_score += (int64_t)bits * lambda2;             /* :1053 */
            coefs[i] = (int16_t)old_coef;                     /* :1055 */
            if (cur_score < iter_score) {                     /* :1056 */
                iter_score = cur_score;
                iter_coef = i;
                iter_mask = cur_mask;
                iter_round = cur_round;
                iter_distortion_delta = cur_distortion_delta;
            }
        }
        if (iter_coef >= 0) {                                 /* :1065 */
            score = iter_score - iter_distortion_delta;       /* :1067 */
            coef_mask = iter_mask;
            round_mask = iter_round;
            coefs[iter_coef] = quant_coefs[(round_mask >> iter_coef) & 1][iter_coef]; /* :1070 */
            delta_distortion[iter_coef] = 0;                  /* :1073 */
        } else
            break;                                            /* :1076 */
    }

    if (coef_mask) {                                          /* :1079 */
        for (i = b_ac, j = start; i < num_coefs; i++, j += step)
            dct[zigzag[j]] = coefs[i];                        /* :1081-1082 */
        return 1;                                             /* :1083 */
    }

phx_zeroblock:                                                /* rdo.c:1086 */
    if (!dc) {                                                /* :1087 */
        if (b_8x8)
            for (i = start; i <= end; i += step)              /* :1090 */
                dct[zigzag[i]] = 0;
        else
            memset(dct, 0, 16*sizeof(int16_t));               /* :1093 */
    }
    return 0;                                                 /* :1095 */
}

/* ------------------------------------------------------------------ */
/* Public wrappers: rdo.c:1098-1184                                    */
/* ------------------------------------------------------------------ */

int phx_quant_4x4_trellis(phx_cabac_t *cb, int16_t *dct, int i_quant_cat,
                          int i_qp, intptr_t ctx_block_cat, int b_intra)
{
    phx_rdo_init();
    const int cat = (int)ctx_block_cat;
    int b_ac = (unsigned)cat < 14 ? phx_ctx_ac[cat] : 0;      /* rdo.c:1145-1146 */
    int b_chroma = (cat == PHX_DCT_CHROMA_DC || cat == PHX_DCT_CHROMA_AC ||
                    cat >= PHX_DCT_CHROMAU_DC);               /* macroblock.h:273-289 */
    const uint8_t *zigzag = phx_rdo_zigzag4[phx_cfg.b_interlaced ? 1 : 0];
    const int16_t *fenc = phx_cfg.fenc_dct4;
    int idx = phx_cfg.idx & 15;

    if (i_qp < 0 || i_qp > PHX_QUANT_QP_MAX_SPEC || (unsigned)i_quant_cat >= 4)
        return -1;

    return phx_rdo_quant_trellis_cabac(cb, dct,
        phx_rdo_quant4_mf(i_quant_cat, i_qp),
        phx_rdo_quant4_bias0(i_quant_cat, i_qp),
        phx_rdo_unquant4_mf(i_quant_cat, i_qp), zigzag, cat,
        /* rdo.c:1151 reads h->mb.i_trellis_lambda2[b_chroma][b_intra], already
         * indexed with the effective chroma QP at analyse.c:266-267.  Passing
         * a chroma QP through phx_rdo_lambda2() would remap it twice. */
        (b_chroma ? phx_trellis_lambda2_tab[b_intra][i_qp]
                  : phx_rdo_lambda2(0, b_intra, i_qp)),
        b_ac, b_chroma, 0, 16, idx,
        phx_cfg.i_psy_trellis, fenc, phx_cfg.b_interlaced);    /* rdo.c:1147-1151 */
}

/* TODO(8x8): the 64-coefficient state machine is structurally identical to
 * x264 rdo.c:641-895, but golden comparisons against x264's own
 * x264_quant_8x8_trellis show a few differing coefficients.  Do not use this
 * in a parity-critical path until the divergence is resolved. */
int phx_quant_8x8_trellis(phx_cabac_t *cb, int16_t *dct, int i_quant_cat,
                          int i_qp, intptr_t ctx_block_cat, int b_intra)
{
    phx_rdo_init();
    const int cat = (int)ctx_block_cat;
    int b_chroma = (cat == PHX_DCT_CHROMA_DC || cat == PHX_DCT_CHROMA_AC ||
                    cat >= PHX_DCT_CHROMAU_DC);
    const uint8_t *zigzag = phx_rdo_zigzag8[phx_cfg.b_interlaced ? 1 : 0];
    int list = i_quant_cat & 1;                               /* set.h:39-42 (2 lists in 4:2:0) */

    if (i_qp < 0 || i_qp > PHX_QUANT_QP_MAX_SPEC)
        return -1;

    return phx_rdo_quant_trellis_cabac(cb, dct,
        phx_rdo_quant8_mf(list, i_qp),
        phx_rdo_quant8_bias0(list, i_qp),
        phx_rdo_unquant8_mf(list, i_qp), zigzag, cat,
        phx_rdo_lambda2(b_chroma, b_intra, i_qp), 0, b_chroma, 0, 64,
        phx_cfg.idx & 3, phx_cfg.i_psy_trellis, phx_cfg.fenc_dct8,
        phx_cfg.b_interlaced);                                /* rdo.c:1162-1167 */
}

int phx_quant_luma_dc_trellis(phx_cabac_t *cb, int16_t *dct, int i_quant_cat,
                              int i_qp, intptr_t ctx_block_cat, int b_intra)
{
    phx_rdo_init();
    const int cat = (int)ctx_block_cat;
    const uint8_t *zigzag = phx_rdo_zigzag4[phx_cfg.b_interlaced ? 1 : 0];
    if (i_qp < 0 || i_qp > PHX_QUANT_QP_MAX_SPEC || (unsigned)i_quant_cat >= 4)
        return -1;
    return phx_rdo_quant_trellis_cabac(cb, dct,
        phx_rdo_quant4_mf(i_quant_cat, i_qp),
        phx_rdo_quant4_bias0(i_quant_cat, i_qp),
        phx_rdo_unquant4_mf(i_quant_cat, i_qp), zigzag, cat,
        phx_rdo_lambda2(0, b_intra, i_qp), 0, 0, 1, 16,
        phx_cfg.idx, 0, NULL, phx_cfg.b_interlaced);           /* rdo.c:1098-1104 */
}

int phx_quant_chroma_dc_trellis(phx_cabac_t *cb, int16_t *dct, int i_qp,
                                intptr_t ctx_block_cat, int b_intra)
{
    phx_rdo_init();
    const int cat = (int)ctx_block_cat;
    const uint8_t *zigzag = phx_zigzag2x2; /* 4:2:0: zigzag_scan2x2, rdo.c:1111/1127 */
    int quant_cat = PHX_CQM_4IC + 1 - b_intra;                 /* rdo.c:1118 */
    if (i_qp < 0 || i_qp > PHX_QUANT_QP_MAX_SPEC)
        return -1;
    return phx_rdo_quant_trellis_cabac(cb, dct,
        phx_rdo_quant4_mf(quant_cat, i_qp),
        phx_rdo_quant4_bias0(quant_cat, i_qp),
        phx_rdo_unquant4_mf(quant_cat, i_qp), zigzag, cat,
        /* rdo.c:1135 uses h->mb.i_trellis_lambda2[1][b_intra], which analyse.c:266-267
         * already indexed with the *effective chroma* QP.  phx_rdo_lambda2() would
         * map chroma QP a second time (analyse.c:257), so index the table directly. */
        phx_trellis_lambda2_tab[b_intra][i_qp], 0, 1, 1, 4,
        phx_cfg.idx, 0, NULL, phx_cfg.b_interlaced);           /* rdo.c:1131-1135 */
}
