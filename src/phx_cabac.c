/* Function-for-function port of x264 common/cabac.c, with source lines. */
#include "phx_cabac.h"
#include "phx_tables.h"
#include <string.h>

#define PHX_CLIP3(a,v,b) ((v) < (a) ? (a) : ((v) > (b) ? (b) : (v)))
#define PHX_MIN(a,b) ((a) < (b) ? (a) : (b))

/* cabac.c:32 — x264_cabac_init()/context computation, inlined into our init
 * so no hidden global cache is needed. */
void phx_cabac_context_init(phx_cabac_t *cb, int slice_type, int qp, int model)
{
    const int8_t (*tab)[1024][2] = slice_type == PHX_SLICE_I
                                 ? &phx_cabac_context_init_I
                                 : &phx_cabac_context_init_PB[model+1]; /* :37-38 */
    for (int j = 0; j < PHX_CABAC_CTX_COUNT; j++)                       /* :40 */
    {
        int state = PHX_CLIP3(1, (((*tab)[j][0] * qp) >> 4) + (*tab)[j][1], 126);
                                                                       /* :42 */
        cb->state[j] = (uint8_t)((PHX_MIN(state, 127-state) << 1) | (state >> 6));
                                                                       /* :43 */
    }
}

/* cabac.c:53 — x264_cabac_encode_init_core(). */
void phx_cabac_encode_init_core(phx_cabac_t *cb)
{
    cb->i_low = 0;                       /* :55 */
    cb->i_range = 0x01FE;                /* :56 */
    cb->i_queue = -9;                    /* :57 */
    cb->i_bytes_outstanding = 0;         /* :58 */
}

/* cabac.c:61 — x264_cabac_encode_init(). */
void phx_cabac_encode_init(phx_cabac_t *cb, uint8_t *data, uint8_t *end)
{
    phx_cabac_encode_init_core(cb);      /* :63 */
    cb->p_start = data; cb->p = data; cb->p_end = end; /* :64-66 */
}

/* cabac.c:69 — cabac_putbyte(). */
static void phx_cabac_putbyte(phx_cabac_t *cb)
{
    if (cb->i_queue >= 0)                /* :71 */
    {
        int out = cb->i_low >> (cb->i_queue+10);      /* :73 */
        cb->i_low &= (0x400 << cb->i_queue)-1;        /* :74 */
        cb->i_queue -= 8;                             /* :75 */
        if ((out & 0xff) == 0xff)                     /* :77 */
            cb->i_bytes_outstanding++;                /* :78 */
        else {
            int carry = out >> 8;                     /* :81 */
            int bytes_outstanding = cb->i_bytes_outstanding; /* :82 */
            cb->p[-1] += (uint8_t)carry;              /* :89 */
            while (bytes_outstanding > 0) {           /* :90 */
                *(cb->p++) = (uint8_t)(carry-1);      /* :92 */
                bytes_outstanding--;
            }
            *(cb->p++) = (uint8_t)out;                /* :95 */
            cb->i_bytes_outstanding = 0;              /* :96 */
        }
    }
}

/* cabac.c:101 — cabac_encode_renorm(). */
static void phx_cabac_encode_renorm(phx_cabac_t *cb)
{
    int shift = phx_cabac_renorm_shift[cb->i_range >> 3]; /* :103 */
    cb->i_range <<= shift; cb->i_low <<= shift;           /* :104-105 */
    cb->i_queue += shift;                                 /* :106 */
    phx_cabac_putbyte(cb);                                /* :107 */
}

/* cabac.c:113 — x264_cabac_encode_decision_c(). */
void phx_cabac_encode_decision(phx_cabac_t *cb, int ctx, int b)
{
    int i_state = cb->state[ctx];                        /* :115 */
    int range_lps = phx_cabac_range_lps[i_state>>1][(cb->i_range>>6)-4]; /* :116 */
    cb->i_range -= range_lps;                            /* :117 */
    if (b != (i_state & 1)) {                            /* :118 */
        cb->i_low += cb->i_range; cb->i_range = range_lps; /* :119-120 */
    }
    cb->state[ctx] = phx_cabac_transition[i_state][b];   /* :123 */
    phx_cabac_encode_renorm(cb);                         /* :124 */
}

/* cabac.c:128 — x264_cabac_encode_bypass_c() (b is negated by the caller). */
void phx_cabac_encode_bypass(phx_cabac_t *cb, int b)
{
    cb->i_low <<= 1;                    /* :130 */
    cb->i_low += b & cb->i_range;       /* :131 */
    cb->i_queue += 1;                   /* :132 */
    phx_cabac_putbyte(cb);              /* :133 */
}

/* Count leading zeros of a nonzero 32-bit value (x264_clz). */
static int phx_clz(uint32_t v)
{
    int n = 0;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

/* cabac.c:136-140 — bypass_lut[]. */
static const int bypass_lut[16] = {
    -1, 0x2, 0x14, 0x68, 0x1d0, 0x7a0, 0x1f40, 0x7e80,
    0x1fd00, 0x7fa00, 0x1ff400, 0x7fe800, 0x1ffd000, 0x7ffa000,
    0x1fff4000, 0x7ffe8000
};

/* cabac.c:142 — x264_cabac_encode_ue_bypass(). */
void phx_cabac_encode_ue_bypass(phx_cabac_t *cb, int exp_bits, int val)
{
    uint32_t v = (uint32_t)(val + (1<<exp_bits));       /* :144 */
    int k = 31 - phx_clz(v);                            /* :145 */
    uint32_t x = (uint32_t)(bypass_lut[k-exp_bits] << exp_bits) + v; /* :146 */
    k = 2*k+1-exp_bits;                                 /* :147 */
    int i = ((k-1)&7)+1;                                /* :148 */
    do {
        k -= i;                                         /* :150 */
        cb->i_low <<= i;                                /* :151 */
        cb->i_low += (int)(((x>>k)&0xff) * (uint32_t)cb->i_range); /* :152 */
        cb->i_queue += i;                               /* :153 */
        phx_cabac_putbyte(cb);                          /* :154 */
        i = 8;                                          /* :155 */
    } while (k > 0);                                    /* :156 */
}

/* cabac.c:159 — x264_cabac_encode_terminal_c(). */
void phx_cabac_encode_terminal(phx_cabac_t *cb)
{
    cb->i_range -= 2;                    /* :161 */
    phx_cabac_encode_renorm(cb);         /* :162 */
}

/* cabac.c:165 — x264_cabac_encode_flush(). */
void phx_cabac_encode_flush(phx_cabac_t *cb, int frame)
{
    cb->i_low += cb->i_range - 2;                       /* :167 */
    cb->i_low |= 1;                                     /* :168 */
    cb->i_low <<= 9;                                    /* :169 */
    cb->i_queue += 9;                                   /* :170 */
    phx_cabac_putbyte(cb);                              /* :171 */
    phx_cabac_putbyte(cb);                              /* :172 */
    cb->i_low <<= -cb->i_queue;                         /* :173 */
    cb->i_low |= (0x35a4e4f5u >> (frame & 31) & 1) << 10; /* :174 */
    cb->i_queue = 0;                                    /* :175 */
    phx_cabac_putbyte(cb);                              /* :176 */
    while (cb->i_bytes_outstanding > 0) {               /* :178 */
        *(cb->p++) = 0xff; cb->i_bytes_outstanding--;   /* :180-181 */
    }
}
