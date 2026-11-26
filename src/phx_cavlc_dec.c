/* CAVLC decoder: exact inverse of the x264 CAVLC writers ported in
 * src/phx_cavlc.c and src/phx_cavlc_syntax.c.  See phx_cavlc_dec.h for the
 * full source-line index and the SHADER: C placement rule.
 *
 * Every VLC is decoded by matching the code bits that were written with
 * bs_write(size, bits) (encoder/cavlc.c:65) against the verbatim leaf tables
 * in phx_cavlc.c.  Level prefixes follow H.264 9.2.2.3 / FFmpeg
 * libavcodec/h264_cavlc.c:get_level_prefix() and the x264 escape arithmetic
 * at encoder/cavlc.c:70-119.
 */
#include "phx_cavlc_dec.h"
#include <limits.h>
#include <string.h>

/* Sentinels: ue(v) codes are non-negative so -1 is safe there, but se(v)
 * legitimately decodes to -1, hence a distinct error value for signed reads. */
#define PHX_DEC_ERR INT_MIN
#define PHX_DEC_MAX_VLC_BITS 32
#define PHX_DEC_MAX_PREFIX   28   /* FFmpeg h264_cavlc.c: prefix > 28 invalid */

/* ------------------------------------------------------------------ */
/* Bit reader — mirrors phx_bs_write()/phx_bs_write1() bit-for-bit.     */
/* SHADER: C — serial entropy reader.  Source: common/bitstream.h:125. */
/* ------------------------------------------------------------------ */

static int phx_dec_read_bit(phx_bs_t *s)
{
    int v = (*s->p >> (7 - s->bit)) & 1;
    if (++s->bit == 8) { s->p++; s->bit = 0; }
    return v;
}

/* SHADER: C — serial entropy reader.  bitstream.h:125. */
static uint32_t phx_dec_read_bits(phx_bs_t *s, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (uint32_t)phx_dec_read_bit(s);
    return v;
}

/* SHADER: C — serial entropy reader.  bitstream.h:241 (bs_write_ue inverse). */
static int phx_dec_read_ue(phx_bs_t *s)
{
    int zeros = 0;
    while (phx_dec_read_bit(s) == 0) {
        if (++zeros > 30) return -1;
    }
    if (zeros == 0) return 0;
    uint32_t suffix = phx_dec_read_bits(s, zeros);
    return (int)((1u << zeros) - 1 + suffix);
}

/* SHADER: C — serial entropy reader.  bitstream.h:246 (bs_write_se inverse). */
static int phx_dec_read_se(phx_bs_t *s)
{
    int k = phx_dec_read_ue(s);
    if (k < 0) return PHX_DEC_ERR;
    /* bitstream.h:249-261: bs_write_se() writes the codeNum directly with
     * ue_size_tab[k]; the effective map is k odd -> +(k+1)/2, k even -> -k/2. */
    return (k & 1) ? (k + 1) / 2 : -(k / 2);
}

/* SHADER: C — serial entropy reader.  bitstream.h:264 (bs_write_te inverse). */
static int phx_dec_read_te(phx_bs_t *s, int x)
{
    if (x == 1) return 1 ^ phx_dec_read_bit(s);   /* :99 */
    return phx_dec_read_ue(s);                    /* :100 */
}

/* Decode a left-aligned VLC leaf table written with bs_write_vlc().
 * Returns the flat table index, or -1 when no codeword matches.
 * SHADER: C — serial entropy reader.  tables.c VLC leaves, cavlc.c:65. */
static int phx_dec_vlc(phx_bs_t *s, const phx_vlc_t *tab, int count)
{
    uint32_t acc = 0;
    for (int n = 1; n <= PHX_DEC_MAX_VLC_BITS; n++) {
        acc = (acc << 1) | (uint32_t)phx_dec_read_bit(s);
        for (int i = 0; i < count; i++) {
            if (tab[i].i_size && tab[i].i_size == n && tab[i].i_bits == acc)
                return i;
        }
    }
    return -1;
}

/* SHADER: C — serial entropy reader.  H.264 9.2.2.3 / table 9-6;
 * FFmpeg h264_cavlc.c:get_level_prefix() (leading zeros before the 1). */
static int phx_dec_level_prefix(phx_bs_t *s)
{
    int prefix = 0;
    while (phx_dec_read_bit(s) == 0) {
        if (++prefix > PHX_DEC_MAX_PREFIX) return -1;
    }
    return prefix;
}

/* Next suffixLength: common/vlc.c:67-71 and encoder/cavlc.c:73,114-117. */
static int phx_dec_next_suffix(int suffix, int abs_level)
{
    int next = suffix;
    if (next == 0) next++;                                   /* vlc.c:67-68 */
    if (abs_level > (3 << (next-1)) && next < 6) next++;      /* vlc.c:69-70 */
    return next;
}

/* Number of coefficients in a block, from the quantf function pointer
 * selected at encoder/cavlc.c:131 (common/quant.c:437-445). */
static int phx_dec_num_coeff(int cat, int chroma, int chroma_2x4)
{
    if (chroma || cat == PHX_DCT_CHROMA_DC)
        return chroma_2x4 ? 8 : 4;                            /* :182 */
    if (cat == PHX_DCT_LUMA_AC || cat == PHX_DCT_CHROMA_AC ||
        cat == PHX_DCT_CHROMAU_AC || cat == PHX_DCT_CHROMAV_AC)
        return 15;                                            /* cavlc.c:570 */
    return 16;                                                /* DC / 4x4 / 8x8 */
}

/* ------------------------------------------------------------------ */
/* Residual block — inverse of cavlc.c:121-209.                        */
/* SHADER: C — serial entropy reader, no parallel stage.                */
/* ------------------------------------------------------------------ */

int phx_cavlc_dec_residual(phx_bs_t *bs, int16_t *coef, int i_quant_cat,
                           int nC, int chroma, int chroma_2x4)
{
    int start = phx_bs_pos(bs);
    int max_coeff = phx_dec_num_coeff(i_quant_cat, chroma, chroma_2x4);
    int is_chroma_dc = chroma || i_quant_cat == PHX_DCT_CHROMA_DC;
    int nc = nC;

    if (max_coeff <= 0 || nc < 0 || nc > 5 ||
        i_quant_cat < 0 || i_quant_cat > 13) return -1;
    if (is_chroma_dc && (nc < 4 || nc > 5))
        nc = chroma_2x4 ? 5 : 4;             /* cavlc.c:202-203 */
    memset(coef, 0, (size_t)max_coeff * sizeof(int16_t));

    /* Empty block: x264_coeff0_token[nC] (cavlc.c:205-206). */
    {
        phx_bs_t save = *bs;
        phx_vlc_t zero = phx_coeff0_token[nc];
        if (phx_dec_vlc(bs, &zero, 1) == 0)
            return phx_bs_pos(bs) - start;
        *bs = save;
    }

    /* coeff_token[nC][total-1][trailing] (cavlc.c:148). */
    int idx = phx_dec_vlc(bs, &phx_coeff_token[nc][0][0], 16 * 4);
    if (idx < 0) return -1;
    int total = idx / 4 + 1;
    int trailing = idx % 4;
    if (total > max_coeff || trailing > total || trailing > 3) return -1;

    /* Trailing-one signs, MSB first (cavlc.c:142-151). */
    int level[16] = {0};
    for (int i = 0; i < trailing; i++)
        level[i] = phx_dec_read_bit(bs) ? -1 : 1;

    /* Remaining levels: level_prefix / level_suffix with escape (9.2.2.3). */
    int suffix = (total > 10 && trailing < 3) ? 1 : 0;       /* :150 */
    for (int i = trailing; i < total; i++) {
        int prefix = phx_dec_level_prefix(bs);
        if (prefix < 0) return -1;
        int level_code;
        if (prefix < 14) {
            level_code = (prefix << suffix)
                       + (suffix ? (int)phx_dec_read_bits(bs, suffix) : 0);
        } else if (prefix == 14 && suffix == 0) {
            level_code = 14 + (int)phx_dec_read_bits(bs, 4); /* table 9-6 */
        } else if (prefix == 14) {
            level_code = (14 << suffix) + (int)phx_dec_read_bits(bs, suffix);
        } else {
            /* level_prefix >= 15: escape (cavlc.c:85-112).
             * suffix==0 uses +30 (two 15s), otherwise (15<<suffix). */
            level_code = suffix == 0 ? 30 : (15 << suffix);
            if (prefix >= 16) {
                if (prefix > 28) return -1;                   /* FFmpeg bound */
                level_code += (1 << (prefix - 3)) - 4096;     /* cavlc.c:94-97 */
            }
            level_code += (int)phx_dec_read_bits(bs, prefix - 3); /* :112 */
        }
        /* First non-trailing level has magnitude reduced by one when
         * trailing < 3 (cavlc.c:157); undo it before the sign mapping. */
        if (i == trailing && trailing < 3) level_code += 2;
        int mask = -(level_code & 1);
        level[i] = ((2 + level_code) >> 1) ^ mask;
        level[i] -= mask;
        int abs_level = level[i] < 0 ? -level[i] : level[i];
        suffix = phx_dec_next_suffix(suffix, abs_level);
    }

    /* total_zeros (cavlc.c:180-190, H.264 9.2.2.4). */
    int zeros = 0;
    if (total < max_coeff) {
        int idx_z = -1, count = 0;
        const phx_vlc_t *tab = NULL;
        if (is_chroma_dc) {
            int max_dc = chroma_2x4 ? 8 : 4;                  /* :1061 */
            if (total < max_dc && total >= 1) {
                if (chroma_2x4) {
                    tab = &phx_total_zeros_2x4_dc[total-1][0];  /* :185 */
                    count = 8;
                } else {
                    tab = &phx_total_zeros_2x2_dc[total-1][0];  /* :184 */
                    count = 4;
                }
            }
        } else if (total >= 1 && total <= 15 &&
                   (uint8_t)total < phx_cavlc_count_cat[i_quant_cat]) {
            tab = &phx_total_zeros[total-1][0];               /* :190 */
            count = 16;
        }
        if (tab) {
            idx_z = phx_dec_vlc(bs, tab, count);
            if (idx_z < 0) return -1;
            zeros = idx_z;
        }
    }
    if (zeros < 0 || zeros > max_coeff - total) return -1;

    /* run_before (cavlc.c:192-193, H.264 9.2.2.5; common/quant.c:381-395
     * gives the reverse zigzag placement). */
    int pos = zeros + total - 1;
    if (pos >= max_coeff) return -1;
    coef[pos] = (int16_t)level[0];
    int zeros_left = zeros;
    for (int i = 1; i < total; i++) {
        if (zeros_left > 0) {
            int zidx = (zeros_left < 7 ? zeros_left : 7) - 1;
            int run = phx_dec_vlc(bs, &phx_run_before_init[zidx][0], 16);
            if (run < 0) return -1;
            zeros_left -= run;
            pos -= 1 + run;
        } else {
            pos -= 1;
        }
        if (zeros_left < 0 || pos < 0) return -1;
        coef[pos] = (int16_t)level[i];
    }
    return phx_bs_pos(bs) - start;
}

/* ------------------------------------------------------------------ */
/* MVD helpers — cavlc.c:241-272, common/mvpred.c:30-127.               */
/* SHADER: C — serial entropy reader with the cache as neighbour input.  */
/* ------------------------------------------------------------------ */

int phx_cavlc_dec_mvd(phx_bs_t *bs, int16_t mv[2])
{
    int start = phx_bs_pos(bs);
    int x = phx_dec_read_se(bs);
    if (x == PHX_DEC_ERR) return -1;
    int y = phx_dec_read_se(bs);
    if (y == PHX_DEC_ERR) return -1;
    mv[0] = (int16_t)x;
    mv[1] = (int16_t)y;
    return phx_bs_pos(bs) - start;
}

/* Fill the scan8 rectangle owned by a partition (base.h:180, :155). */
static void phx_dec_fill_rect(int8_t *ref, int16_t (*mv)[2], int slot,
                              int width, int height, int refval,
                              const int16_t mvval[2])
{
    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            int at = slot + j*8 + i;
            if (ref) ref[at] = (int8_t)refval;
            if (mv) { mv[at][0] = mvval[0]; mv[at][1] = mvval[1]; }
        }
    }
}

/* Decode one mvd_lX and add the predictor, updating the borrowed cache. */
static int phx_dec_mvd_to_cache(phx_bs_t *bs, phx_cavlc_mb_t *mb,
                                int list, int idx, int width, int height)
{
    int16_t mvp[2], d[2], mv[2];
    phx_cavlc_predict_mv(mb, list, idx, width, mvp);          /* mvpred.c:30 */
    int n = phx_cavlc_dec_mvd(bs, d);                        /* cavlc.c:246-247 */
    if (n < 0) return -1;
    mv[0] = (int16_t)(mvp[0] + d[0]);
    mv[1] = (int16_t)(mvp[1] + d[1]);
    phx_dec_fill_rect(NULL, mb->cache->mv[list],
                      phx_scan8[idx], width, height, 0, mv);
    return n;
}

/* ref_idx_lX decode (bitstream.h:264 / cavlc.c:347 etc.). */
static int phx_dec_ref_to_cache(phx_bs_t *bs, phx_cavlc_mb_t *mb,
                                int list, int idx, int width, int height, int x)
{
    int ref = phx_dec_read_te(bs, x);
    if (ref < 0) return -1;
    phx_dec_fill_rect(mb->cache->ref[list], NULL,
                      phx_scan8[idx], width, height, ref, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Macroblock header — inverse of cavlc.c:305-543.                      */
/* SHADER: C — serial entropy reader.                                   */
/* ------------------------------------------------------------------ */

/* common/predict.h:92 (FIX4), macroblock.h:420-431. */
static int phx_dec_fix4(int mode)
{
    static const int8_t fix[13] = { -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 2, 2, 2 };
    if (mode < -1 || mode > 11) return -1;
    return fix[mode+1];
}

static int phx_dec_pred_intra4x4(const phx_cavlc_mb_t *mb, int idx)
{
    const phx_mb_cache_t *c = mb->cache;
    int ma = phx_dec_fix4(c->intra4x4_pred_mode[phx_scan8[idx] - 1]);
    int mt = phx_dec_fix4(c->intra4x4_pred_mode[phx_scan8[idx] - 8]);
    int m = ma < mt ? ma : mt;
    return m < 0 ? 2 /* I_PRED_4x4_DC */ : m;
}

/* inverse of cavlc.c:305-335. */
static int phx_dec_header_i(phx_bs_t *bs, phx_cavlc_mb_t *mb, int offset)
{
    int start = phx_bs_pos(bs);
    int code = phx_dec_read_ue(bs);
    if (code < 0) return -1;

    if (code >= offset + 1) {
        /* I16x16: code = offset+1+pred + cbp_chroma*4 + (luma?12:0). */
        int v = code - offset - 1;
        if (v >= 24) return -1;              /* :310-311, 25 == I_PCM */
        mb->i_type = PHX_I_16x16;
        /* 12 (luma) and 4*chroma overlap in the x264 expression, so decode
         * the luma term first, then chroma and pred from the remainder. */
        mb->i_cbp_luma = v >= 12 ? 15 : 0;
        if (mb->i_cbp_luma) v -= 12;
        mb->i_cbp_chroma = v >> 2;           /* pred_mode16x16_fix reverse */
        mb->i_intra16x16_pred_mode = v & 3;
    } else if (code == offset) {
        /* I4x4 / I8x8 (cavlc.c:313-331). */
        int flag = 0;
        if (mb->b_transform_8x8_mode) flag = phx_dec_read_bit(bs); /* :318 */
        mb->i_type = flag ? PHX_I_8x8 : PHX_I_4x4;
        mb->b_transform_8x8 = flag;
        int di = flag ? 4 : 1;                                    /* :315 */
        for (int i = 0; i < 16; i += di) {
            int pred = phx_dec_pred_intra4x4(mb, i);                /* :323 */
            int mode;
            if (phx_dec_read_bit(bs)) mode = pred;                  /* :327 */
            else {
                /* x264 writes 4 bits whose MSB is the zero flag; the remaining
                 * 3 bits are rem_mode (cavlc.c:329, H.264 9.3.3.1). */
                int rem = (int)phx_dec_read_bits(bs, 3);            /* :329 */
                mode = rem + (rem >= pred);
            }
            if (mode < 0 || mode > 8) return -1;
            int slot = phx_scan8[i];
            mb->cache->intra4x4_pred_mode[slot] = (int8_t)mode;
            if (di == 4) {                                          /* :321 */
                mb->cache->intra4x4_pred_mode[slot+1] = (int8_t)mode;
                mb->cache->intra4x4_pred_mode[slot+8] = (int8_t)mode;
                mb->cache->intra4x4_pred_mode[slot+9] = (int8_t)mode;
            }
        }
    } else {
        return -1;
    }

    if (mb->chroma) {                                               /* :333 */
        int cm = phx_dec_read_ue(bs);
        if (cm < 0 || cm > 3) return -1;
        mb->i_chroma_pred_mode = cm;   /* chroma_pred_mode_fix is identity */
    }
    return phx_bs_pos(bs) - start;
}

/* common/macroblock.h:97-109 — x264_mb_type_list_table (B inter types). */
static const uint8_t phx_dec_b_list[19][2][2] = {
    {{0,0},{0,0}}, {{0,0},{0,0}}, {{0,0},{0,0}}, {{0,0},{0,0}},
    {{1,1},{0,0}}, {{0,0},{0,0}}, {{1,1},{0,0}},
    {{0,0},{0,0}},
    {{1,1},{0,0}}, {{1,0},{0,1}}, {{1,1},{0,1}},
    {{0,1},{1,0}}, {{0,0},{1,1}}, {{0,1},{1,1}},
    {{1,1},{1,0}}, {{1,0},{1,1}}, {{1,1},{1,1}},
    {{0,0},{0,0}}, {{0,0},{0,0}}
};

/* common/macroblock.h:143-157 — x264_mb_partition_listX_table. */
static const uint8_t phx_dec_part_list[2][17] = {
    { 1,1,1,1, 0,0,0,0, 1,1,1,1, 0, 0,0,0,0 },
    { 0,0,0,0, 1,1,1,1, 1,1,1,1, 0, 0,0,0,0 }
};

/* cavlc.c:250-272 (P_8x8 MVDs). */
static int phx_dec_p_8x8_mvd(phx_bs_t *bs, phx_cavlc_mb_t *mb, int i)
{
    int bits = 0;
    int base = 4*i;
    switch (mb->i_sub_partition[i]) {
        case PHX_D_L0_8x8:
            bits += phx_dec_mvd_to_cache(bs, mb, 0, base, 2, 2); break; /* :255 */
        case PHX_D_L0_8x4:
            bits += phx_dec_mvd_to_cache(bs, mb, 0, base, 2, 1);       /* :258 */
            bits += phx_dec_mvd_to_cache(bs, mb, 0, base+2, 2, 1); break; /* :259 */
        case PHX_D_L0_4x8:
            bits += phx_dec_mvd_to_cache(bs, mb, 0, base, 1, 2);       /* :262 */
            bits += phx_dec_mvd_to_cache(bs, mb, 0, base+1, 1, 2); break; /* :263 */
        case PHX_D_L0_4x4:
            for (int k = 0; k < 4; k++)
                bits += phx_dec_mvd_to_cache(bs, mb, 0, base+k, 1, 1); /* :266-269 */
            break;
        default: break;
    }
    return bits;
}

/* inverse of cavlc.c:337-409. */
static int phx_dec_header_p(phx_bs_t *bs, phx_cavlc_mb_t *mb)
{
    int start = phx_bs_pos(bs);
    int code = phx_dec_read_ue(bs);
    if (code < 0) return -1;
    if (code >= 5) return phx_dec_header_i(bs, mb, 5);      /* :407-408 */

    mb->i_type = PHX_P_L0;                                  /* :340 */
    int x0 = mb->i_fref[0] - 1;                             /* :342 */
    if (code == 0) {
        mb->i_partition = PHX_D_16x16;                       /* :344 */
        if (mb->i_fref[0] > 1 &&
            phx_dec_ref_to_cache(bs, mb, 0, 0, 4, 4, x0) < 0) return -1; /* :347 */
        if (phx_dec_mvd_to_cache(bs, mb, 0, 0, 4, 4) < 0) return -1;   /* :348 */
    } else if (code == 1) {
        mb->i_partition = PHX_D_16x8;                        /* :352 */
        if (mb->i_fref[0] > 1) {
            if (phx_dec_ref_to_cache(bs, mb, 0, 0, 4, 2, x0) < 0) return -1; /* :355 */
            if (phx_dec_ref_to_cache(bs, mb, 0, 8, 4, 2, x0) < 0) return -1; /* :356 */
        }
        if (phx_dec_mvd_to_cache(bs, mb, 0, 0, 4, 2) < 0) return -1;   /* :358 */
        if (phx_dec_mvd_to_cache(bs, mb, 0, 8, 4, 2) < 0) return -1;   /* :359 */
    } else if (code == 2) {
        mb->i_partition = PHX_D_8x16;                        /* :363 */
        if (mb->i_fref[0] > 1) {
            if (phx_dec_ref_to_cache(bs, mb, 0, 0, 2, 4, x0) < 0) return -1; /* :366 */
            if (phx_dec_ref_to_cache(bs, mb, 0, 4, 2, 4, x0) < 0) return -1; /* :367 */
        }
        if (phx_dec_mvd_to_cache(bs, mb, 0, 0, 2, 4) < 0) return -1;   /* :369 */
        if (phx_dec_mvd_to_cache(bs, mb, 0, 4, 2, 4) < 0) return -1;   /* :370 */
    } else {
        /* P_8x8 (cavlc.c:373-406). */
        mb->i_type = PHX_P_8x8;                              /* :379/:384 */
        mb->i_partition = PHX_D_8x8;
        int b_sub_ref = code == 3;                           /* :380/:385 */
        phx_bs_t save = *bs;
        int marker = 1;
        for (int k = 0; k < 4; k++)
            if (phx_dec_read_bit(bs) != 1) marker = 0;       /* :393 (0xf) */
        int psub = !marker;
        mb->b_psub8x8 = psub;
        if (psub) *bs = save;             /* peeked sub-partition UEs, rewind */
        for (int i = 0; i < 4; i++) {
            int v;
            if (psub) {
                v = phx_dec_read_ue(bs);                     /* :391 */
                if (v < 0 || v > 3) return -1;
                int found = -1;
                for (int p = 0; p < 4; p++)
                    if (phx_cavlc_subpartition_p_to_golomb[p] == v) found = p;
                if (found < 0) return -1;
                mb->i_sub_partition[i] = found;
            } else {
                mb->i_sub_partition[i] = PHX_D_L0_8x8;       /* :393 */
            }
        }
        if (b_sub_ref && mb->i_fref[0] > 1) {
            int slots[4] = { 0, 4, 8, 12 };
            for (int i = 0; i < 4; i++)
                if (phx_dec_ref_to_cache(bs, mb, 0, slots[i], 2, 2, x0) < 0)
                    return -1;                               /* :398-401 */
        }
        for (int i = 0; i < 4; i++)
            if (phx_dec_p_8x8_mvd(bs, mb, i) < 0) return -1;  /* :405 */
    }
    return phx_bs_pos(bs) - start;
}

/* inverse of cavlc.c:411-482. */
static int phx_dec_header_b(phx_bs_t *bs, phx_cavlc_mb_t *mb)
{
    int start = phx_bs_pos(bs);
    int code = phx_dec_read_ue(bs);
    if (code < 0) return -1;
    if (code >= 23) return phx_dec_header_i(bs, mb, 23);     /* :480-481 */
    if (code == 0) {                                         /* :479 */
        mb->i_type = PHX_B_DIRECT;
        return phx_bs_pos(bs) - start;
    }

    if (code == 22) {
        /* B_8x8 (cavlc.c:414-439). */
        mb->i_type = PHX_B_8x8;
        mb->i_partition = PHX_D_8x8;
        for (int i = 0; i < 4; i++) {                        /* :419-420 */
            int v = phx_dec_read_ue(bs);
            if (v < 0 || v > 12) return -1;
            int found = -1;
            for (int p = 0; p < 13; p++)
                if (phx_cavlc_subpartition_b_to_golomb[p] == v) found = p;
            if (found < 0) return -1;
            mb->i_sub_partition[i] = found;
        }
        for (int list = 0; list < 2; list++) {
            if (mb->i_fref[list] <= 1) continue;
            int x = mb->i_fref[list] - 1;
            for (int i = 0; i < 4; i++)
                if (phx_dec_part_list[list][mb->i_sub_partition[i]])
                    if (phx_dec_ref_to_cache(bs, mb, list, 4*i, 2, 2, x) < 0)
                        return -1;                           /* :423-430 */
        }
        for (int list = 0; list < 2; list++)
            for (int i = 0; i < 4; i++)
                if (phx_dec_part_list[list][mb->i_sub_partition[i]])
                    if (phx_dec_mvd_to_cache(bs, mb, list, 4*i, 2, 2) < 0)
                        return -1;                           /* :433-438 */
        return phx_bs_pos(bs) - start;
    }

    /* B_L0_L0 .. B_BI_BI (cavlc.c:440-477). */
    int found_type = -1, found_part = -1;
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 9; col++) {
            if ((int)phx_cavlc_mb_type_b_to_golomb[row][col] == code) {
                found_type = PHX_B_L0_L0 + col;              /* :448 reverse */
                found_part = PHX_D_16x8 + row;
                break;
            }
        }
        if (found_type >= 0) break;
    }
    if (found_type < 0 || found_type > PHX_B_BI_BI) return -1;
    mb->i_type = found_type;
    mb->i_partition = found_part;
    const uint8_t (*bl)[2] = phx_dec_b_list[found_type];
    int x0 = mb->i_fref[0] - 1;
    int x1 = mb->i_fref[1] - 1;

    if (found_part == PHX_D_16x16) {                         /* :449-454 */
        if (x0 > 0 && bl[0][0] &&
            phx_dec_ref_to_cache(bs, mb, 0, 0, 4, 4, x0) < 0) return -1; /* :451 */
        if (x1 > 0 && bl[1][0] &&
            phx_dec_ref_to_cache(bs, mb, 1, 0, 4, 4, x1) < 0) return -1; /* :452 */
        if (bl[0][0] && phx_dec_mvd_to_cache(bs, mb, 0, 0, 4, 4) < 0) return -1;
        if (bl[1][0] && phx_dec_mvd_to_cache(bs, mb, 1, 0, 4, 4) < 0) return -1;
    } else {
        /* :458-461; x264 writes the second ref at scan8[12] for both
         * 16x8 and 8x16 — mirror it exactly. */
        int rw = found_part == PHX_D_16x8 ? 4 : 2;
        int rh = found_part == PHX_D_16x8 ? 2 : 4;
        if (x0 > 0 && bl[0][0] &&
            phx_dec_ref_to_cache(bs, mb, 0, 0, rw, rh, x0) < 0) return -1;
        if (x0 > 0 && bl[0][1] &&
            phx_dec_ref_to_cache(bs, mb, 0, 12, rw, rh, x0) < 0) return -1;
        if (x1 > 0 && bl[1][0] &&
            phx_dec_ref_to_cache(bs, mb, 1, 0, rw, rh, x1) < 0) return -1;
        if (x1 > 0 && bl[1][1] &&
            phx_dec_ref_to_cache(bs, mb, 1, 12, rw, rh, x1) < 0) return -1;
        if (found_part == PHX_D_16x8) {                      /* :462-467 */
            if (bl[0][0] && phx_dec_mvd_to_cache(bs, mb, 0, 0, 4, 2) < 0) return -1;
            if (bl[0][1] && phx_dec_mvd_to_cache(bs, mb, 0, 8, 4, 2) < 0) return -1;
            if (bl[1][0] && phx_dec_mvd_to_cache(bs, mb, 1, 0, 4, 2) < 0) return -1;
            if (bl[1][1] && phx_dec_mvd_to_cache(bs, mb, 1, 8, 4, 2) < 0) return -1;
        } else {                                             /* :469-474 */
            if (bl[0][0] && phx_dec_mvd_to_cache(bs, mb, 0, 0, 2, 4) < 0) return -1;
            if (bl[0][1] && phx_dec_mvd_to_cache(bs, mb, 0, 4, 2, 4) < 0) return -1;
            if (bl[1][0] && phx_dec_mvd_to_cache(bs, mb, 1, 0, 2, 4) < 0) return -1;
            if (bl[1][1] && phx_dec_mvd_to_cache(bs, mb, 1, 4, 2, 4) < 0) return -1;
        }
    }
    return phx_bs_pos(bs) - start;
}

int phx_cavlc_dec_mb_header(phx_bs_t *bs, phx_cavlc_mb_t *mb)
{
    int start = phx_bs_pos(bs);
    phx_bs_t *saved_bs = mb->bs;
    mb->bs = bs;                                  /* use the explicit reader */
    int ret;
    if (mb->slice_type == PHX_SLICE_TYPE_P)
        ret = phx_dec_header_p(bs, mb);            /* cavlc.c:538 */
    else if (mb->slice_type == PHX_SLICE_TYPE_B)
        ret = phx_dec_header_b(bs, mb);            /* cavlc.c:540 */
    else
        ret = phx_dec_header_i(bs, mb, 0);         /* cavlc.c:542-543 */
    mb->bs = saved_bs;
    if (ret < 0) return -1;
    return phx_bs_pos(bs) - start;
}
