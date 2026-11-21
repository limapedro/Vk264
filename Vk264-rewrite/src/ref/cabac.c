/* SPDX-License-Identifier: GPL-2.0-or-later
 * CABAC arithmetic coder ported from x264 common/cabac.c. */
#include "cabac.h"
#include "cabac_tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int clip3(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static const int8_t (*ctx_init(int slice_type, int model))[2]
{
    if (slice_type == 2) return vk_cabac_context_init_I;  /* SLICE_TYPE_I */
    int m = model < 0 ? 0 : (model > 2 ? 2 : model);
    return vk_cabac_context_init_PB[m];
}

void vk_cabac_init_ctx(vk_cabac_t *cb, int slice_type, int qp, int model)
{
    const int8_t (*tab)[2] = ctx_init(slice_type, model);
    for (int j = 0; j < 460; ++j) {
        int state = clip3((tab[j][0]*qp >> 4) + tab[j][1], 1, 126);
        int mn = state < 127-state ? state : 127-state;
        cb->state[j] = (uint8_t)((mn << 1) | (state >> 6));
    }
}

void vk_cabac_start(vk_cabac_t *cb, uint8_t *data, uint8_t *end)
{
    cb->i_low = 0;
    cb->i_range = 0x01FE;
    cb->i_queue = -9;
    cb->i_bytes_outstanding = 0;
    cb->p_start = data;
    cb->p = data;
    cb->p_end = end;
}

static void putbyte(vk_cabac_t *cb)
{
    if (cb->i_queue >= 0) {
        int out = cb->i_low >> (cb->i_queue+10);
        cb->i_low &= (0x400 << cb->i_queue)-1;
        cb->i_queue -= 8;
        if ((out & 0xff) == 0xff) {
            cb->i_bytes_outstanding++;
        } else {
            int carry = out >> 8;
            int n = cb->i_bytes_outstanding;
            cb->p[-1] = (uint8_t)(cb->p[-1] + carry);
            while (n-- > 0) *cb->p++ = (uint8_t)(carry-1);
            *cb->p++ = (uint8_t)out;
            cb->i_bytes_outstanding = 0;
        }
    }
}

static void renorm(vk_cabac_t *cb)
{
    int shift = vk_cabac_renorm_shift[cb->i_range >> 3];
    cb->i_range <<= shift;
    cb->i_low <<= shift;
    cb->i_queue += shift;
    putbyte(cb);
}

void vk_cabac_decision(vk_cabac_t *cb, int ctx, int b)
{
    int state = cb->state[ctx];
    int lps = vk_cabac_range_lps[state >> 1][(cb->i_range >> 6)-4];
    cb->i_range -= lps;
    if (b != (state & 1)) { cb->i_low += cb->i_range; cb->i_range = lps; }
    cb->state[ctx] = vk_cabac_transition[state][b];
    renorm(cb);
}

void vk_cabac_decision_noup(vk_cabac_t *cb, int ctx, int b)
{
    vk_cabac_decision(cb, ctx, b);
}

void vk_cabac_bypass(vk_cabac_t *cb, int b)
{
    cb->i_low <<= 1;
    cb->i_low += b & cb->i_range;
    cb->i_queue += 1;
    putbyte(cb);
}

static const int bypass_lut[16] = {
    -1, 0x2, 0x14, 0x68, 0x1d0, 0x7a0, 0x1f40, 0x7e80,
    0x1fd00, 0x7fa00, 0x1ff400, 0x7fe800, 0x1ffd000, 0x7ffa000, 0x1fff4000, 0x7ffe8000
};

static int clz(uint32_t x)
{
    if (!x) return 32;
    int n = 0;
    if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xFF000000u)) { n += 8; x <<= 8; }
    if (!(x & 0xF0000000u)) { n += 4; x <<= 4; }
    if (!(x & 0xC0000000u)) { n += 2; x <<= 2; }
    if (!(x & 0x80000000u)) n += 1;
    return n;
}

void vk_cabac_ue_bypass(vk_cabac_t *cb, int exp_bits, int val)
{
    uint32_t v = val + (1u << exp_bits);
    int k = 31 - clz(v);
    uint32_t x = ((uint32_t)bypass_lut[k-exp_bits] << exp_bits) + v;
    k = 2*k + 1 - exp_bits;
    int i = ((k-1)&7)+1;
    do {
        k -= i;
        cb->i_low <<= i;
        cb->i_low += ((x >> k) & 0xff) * cb->i_range;
        cb->i_queue += i;
        putbyte(cb);
        i = 8;
    } while (k > 0);
}

void vk_cabac_terminal(vk_cabac_t *cb)
{
    cb->i_range -= 2;
    renorm(cb);
}

int vk_cabac_flush(vk_cabac_t *cb, int frame)
{
    cb->i_low += cb->i_range - 2;
    cb->i_low |= 1;
    cb->i_low <<= 9;
    cb->i_queue += 9;
    putbyte(cb);
    putbyte(cb);
    cb->i_low <<= -cb->i_queue;
    cb->i_low |= (0x35a4e4f5u >> (frame & 31) & 1) << 10;
    cb->i_queue = 0;
    putbyte(cb);
    while (cb->i_bytes_outstanding > 0) {
        *cb->p++ = 0xff;
        cb->i_bytes_outstanding--;
    }
    return (int)(cb->p - cb->p_start);
}
