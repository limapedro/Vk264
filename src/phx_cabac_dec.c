/* Phoenix CABAC arithmetic decoder.  See phx_cabac_dec.h for provenance.
 *
 * Every arithmetic operation below is the inverse of a line in
 * src/phx_cabac.c / reference/x264/common/cabac.c, using the SAME tables from
 * src/phx_tables.h (no approximations, no new tables):
 *
 *   encoder                         decoder
 *   cabac.c:53-59  low=0,range=510  H.264 9.3.1.2: codIRange=510,
 *                  queue=-9                       codIOffset=9 stream bits
 *   cabac.c:115    i_state          H.264 9.3.3.2.1: same packed state
 *   cabac.c:116    range_lps        same rangeTabLPS lookup
 *   cabac.c:117-122 low/range split offset compare against reduced range
 *   cabac.c:123    transition       same transition table (transition[b])
 *   cabac.c:101-108 renorm          H.264 9.3.3.2.2: shift range AND read
 *                                  `shift` bits into codIOffset
 *   cabac.c:130-133 bypass          H.264 9.3.3.2.3: offset<<=1 + 1 bit,
 *                                  compare/subtract range (no renorm)
 *   cabac.c:161-162 terminal        H.264 9.3.3.2.4: range-=2, compare,
 *                                  unconditional renorm (encoder :162)
 *
 * SHADER: C — strictly serial arithmetic decode state; keep in entropy stage.
 */
#include "phx_cabac_dec.h"
#include "phx_tables.h"

#define DEC_CLIP3(a,v,b) ((v) < (a) ? (a) : ((v) > (b) ? (b) : (v)))
#define DEC_MIN(a,b) ((a) < (b) ? (a) : (b))

/* H.264 9.3.1.1 / x264 cabac.c:32-51.  This is the same computation as
 * phx_cabac_context_init() in src/phx_cabac.c:54-65, repeated only because
 * phx_cabac_dec_t and phx_cabac_t are distinct structs; the tables are the
 * same phx_cabac_context_init_I/PB arrays from src/phx_tables.h. */
static void phx_dec_context_init(phx_cabac_dec_t *d, int slice_type,
                                 int qp, int model)
{
    const int8_t (*tab)[1024][2] = slice_type == PHX_SLICE_I
                                 ? &phx_cabac_context_init_I
                                 : &phx_cabac_context_init_PB[model+1]; /* :37-38 */
    for (int j = 0; j < PHX_CABAC_CTX_COUNT; j++) {              /* :40 */
        int state = DEC_CLIP3(1, (((*tab)[j][0] * qp) >> 4) + (*tab)[j][1], 126);
                                                                /* :42 */
        d->state[j] = (uint8_t)((DEC_MIN(state, 127-state) << 1) | (state >> 6));
                                                                /* :43 */
    }
}

/* Read the next RBSP bit, MSB first (H.264 9.3.3.2.2 bit supply).  One byte is
 * buffered at a time; the bit sequence is identical to FFmpeg's 16-bit
 * refill()/refill2() but expressed in codIOffset units. */
static int phx_dec_read_bit(phx_cabac_dec_t *d)
{
    if (d->bits_left == 0) {
        if (d->p >= d->p_end) {
            d->error = 1;
            return 0;
        }
        d->bitbuf = *d->p++;
        d->bits_left = 8;
    }
    d->bits_left--;
    d->bit_pos++;
    return (int)((d->bitbuf >> d->bits_left) & 1u);
}

/* H.264 9.3.3.2.2 renormalization; shift count is exactly x264's
 * cabac_encode_renorm() (common/cabac.c:103-107) via the same table. */
static void phx_dec_renorm(phx_cabac_dec_t *d)
{
    if (d->i_range < 2 || d->i_range > 511) {
        d->error = 1;
        return;
    }
    int shift = phx_cabac_renorm_shift[d->i_range >> 3]; /* cabac.c:103 */
    d->i_range <<= shift;                                /* cabac.c:104 */
    while (shift-- > 0)
        d->i_offset = (d->i_offset << 1) | phx_dec_read_bit(d); /* :105-106 */
}

int phx_cabac_dec_init(phx_cabac_dec_t *d, const uint8_t *data, int bytes,
                       int slice_qp, int slice_type, int cabac_init_idc)
{
    if (!d || !data || bytes < 2 || slice_qp < 0 || slice_qp > PHX_QP_MAX_SPEC ||
        (slice_type != PHX_SLICE_P && slice_type != PHX_SLICE_B &&
         slice_type != PHX_SLICE_I) ||
        cabac_init_idc < -1 || cabac_init_idc > 2)
        return -1;

    d->p_start = data;
    d->p = data;
    d->p_end = data + bytes;
    d->bitbuf = 0;
    d->bits_left = 0;
    d->bit_pos = 0;
    d->error = 0;

    /* 9.3.1.1 / x264 common/cabac.c:32-51 — same context states as encoder. */
    phx_dec_context_init(d, slice_type, slice_qp,
                         cabac_init_idc < 0 ? 0 : cabac_init_idc);

    /* 9.3.1.2: codIRange = 510; codIOffset = value of the first 9 RBSP bits.
     * Equivalent to FFmpeg ff_init_cabac_decoder(): (byte0<<1)|(byte1>>7). */
    d->i_range = 0x01FE;                                  /* cabac.c:56 */
    d->i_offset = 0;
    for (int i = 0; i < 9; i++)
        d->i_offset = (d->i_offset << 1) | phx_dec_read_bit(d);
    return d->error ? -1 : 0;
}

/* H.264 9.3.3.2.1 / x264 cabac.c:113-124. */
int phx_cabac_dec_decode_decision(phx_cabac_dec_t *d, int ctx)
{
    if (d->error || ctx < 0 || ctx >= PHX_CABAC_CTX_COUNT ||
        d->i_range < 256 || d->i_range > 511)
        return -1;

    int state = d->state[ctx];                            /* cabac.c:115 */
    int mps = state & 1;
    int range_lps = phx_cabac_range_lps[state >> 1][(d->i_range >> 6) - 4];
                                                          /* cabac.c:116 */
    int bin;
    d->i_range -= range_lps;                              /* cabac.c:117 */
    if (d->i_offset >= d->i_range) {                      /* LPS branch */
        bin = 1 - mps;
        d->i_offset -= d->i_range;                        /* 9.3.3.2.1 */
        d->i_range = range_lps;
    } else {
        bin = mps;                                        /* MPS branch */
    }
    d->state[ctx] = (uint8_t)phx_cabac_transition[state][bin]; /* :123 */
    phx_dec_renorm(d);                                    /* cabac.c:124 */
    return d->error ? -1 : bin;
}

/* H.264 9.3.3.2.3 / inverse of x264 cabac.c:128-134 (x264 callers pass a
 * mask, e.g. coeff >> 31 = 0/-1; the decoded bin here is the actual 0/1 bin). */
int phx_cabac_dec_decode_bypass(phx_cabac_dec_t *d)
{
    if (d->error)
        return -1;

    /* Encoder: low <<= 1; low += b & range (cabac.c:130-131).  Decoder shifts
     * the offset by one bit and pins one stream bit, then decides. */
    d->i_offset = (d->i_offset << 1) | phx_dec_read_bit(d);
    int bin = 0;
    if (d->i_offset >= d->i_range) {
        d->i_offset -= d->i_range;
        bin = 1;
    }
    return d->error ? -1 : bin;
}

/* H.264 9.3.3.2.4 / inverse of x264 cabac.c:159-163.  The terminal bin uses
 * rangeLPS = 2 and no context-state update; x264 renormalises
 * unconditionally, so the decoder does too (even for bin=1). */
int phx_cabac_dec_decode_terminal(phx_cabac_dec_t *d)
{
    if (d->error)
        return -1;

    d->i_range -= 2;                                      /* cabac.c:161 */
    int bin;
    if (d->i_offset >= d->i_range) {
        bin = 1;
        d->i_offset -= d->i_range;
        d->i_range = 2;
    } else {
        bin = 0;
    }
    phx_dec_renorm(d);                                    /* cabac.c:162 */
    return d->error ? -1 : bin;
}

/* Inverse of x264 cabac.c:142-157.  From bypass_lut (cabac.c:136-140) the
 * emitted bins are: (k-exp_bits) ones, a zero, then the k low bits of
 * v = val + 2^exp_bits (MSB first), where k = floor(log2(v)); total
 * 2*k+1-exp_bits bins (cabac.c:147).  No new table is needed. */
int phx_cabac_dec_decode_ue_bypass(phx_cabac_dec_t *d, int exp_bits)
{
    if (d->error || exp_bits < 0 || exp_bits > 31)
        return -1;

    int ones = 0;
    for (;;) {
        int b = phx_cabac_dec_decode_bypass(d);
        if (b < 0)
            return -1;
        if (b == 0)
            break;
        if (++ones > 31) {
            d->error = 1;
            return -1;
        }
    }
    int k = ones + exp_bits;
    if (k > 30) {                 /* keep 1u<<k and int conversion defined */
        d->error = 1;
        return -1;
    }
    uint32_t v = 1u << k;         /* implicit MSB of v (cabac.c:144-145) */
    for (int i = k - 1; i >= 0; i--) {
        int b = phx_cabac_dec_decode_bypass(d);
        if (b < 0)
            return -1;
        v |= (uint32_t)b << i;
    }
    return (int)v - (1 << exp_bits);                      /* cabac.c:144 */
}

int phx_cabac_dec_bit_pos(const phx_cabac_dec_t *d)
{
    return d ? d->bit_pos : -1;
}

int phx_cabac_dec_byte_pos(const phx_cabac_dec_t *d)
{
    return d ? (d->bit_pos + 7) / 8 : -1;
}
