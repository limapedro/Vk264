/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cavlc.h"
#include "cavlc_tables.h"

typedef struct { uint16_t i_bits; uint8_t i_size, i_next; } vk_vlc_large_t;

static vk_vlc_large_t level_token[7][VK_LEVEL_TABLE_SIZE];
static uint32_t run_before[1 << 16];

static int clz32(uint32_t x)
{
    if (!x) return 32;
    int n = 0;
    if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xFF000000u)) { n +=  8; x <<=  8; }
    if (!(x & 0xF0000000u)) { n +=  4; x <<=  4; }
    if (!(x & 0xC0000000u)) { n +=  2; x <<=  2; }
    if (!(x & 0x80000000u)) { n +=  1; }
    return n;
}

static int coeff_last16(const int *dct)
{
    for (int i = 15; i >= 0; --i)
        if (dct[i]) return i;
    return -1;
}

static int coeff_level_run(const int *dct, vk_run_level_t *rl)
{
    int last = rl->last = coeff_last16(dct);
    int total = 0, mask = 0;
    while (last >= 0) {
        rl->level[total++] = dct[last];
        mask |= 1 << last;
        while (--last >= 0 && dct[last] == 0) { }
    }
    rl->mask = mask;
    return total;
}

void vk_cavlc_init(void)
{
    for (int i_suffix = 0; i_suffix < 7; ++i_suffix)
        for (int level = -VK_LEVEL_TABLE_SIZE/2; level < VK_LEVEL_TABLE_SIZE/2; ++level) {
            int mask = level >> 15;
            int abs_level = (level ^ mask) - mask;
            int code = abs_level ? abs_level*2-mask-2 : 0;
            int next = i_suffix;
            vk_vlc_large_t *v = &level_token[i_suffix][level+VK_LEVEL_TABLE_SIZE/2];
            if ((code >> i_suffix) < 14) {
                v->i_size = (unsigned char)((code >> i_suffix) + 1 + i_suffix);
                v->i_bits = (uint16_t)((1 << i_suffix) + (code & ((1 << i_suffix)-1)));
            } else if (i_suffix == 0 && code < 30) {
                v->i_size = 19;
                v->i_bits = (uint16_t)((1 << 4) + (code - 14));
            } else if (i_suffix > 0 && (code >> i_suffix) == 14) {
                v->i_size = (unsigned char)(15 + i_suffix);
                v->i_bits = (uint16_t)((1 << i_suffix) + (code & ((1 << i_suffix)-1)));
            } else {
                code -= 15 << i_suffix;
                if (i_suffix == 0) code -= 15;
                v->i_size = 28;
                v->i_bits = (uint16_t)((1 << 12) + code);
            }
            if (next == 0) next++;
            if (abs_level > (3 << (next-1)) && next < 6) next++;
            v->i_next = (unsigned char)next;
        }

    run_before[0] = run_before[1] = 0;
    for (uint32_t i = 2; i < (1u << 16); ++i) {
        vk_run_level_t rl;
        int dct[16];
        int size = 0, bits = 0;
        for (int j = 0; j < 16; ++j) dct[j] = (i >> j) & 1;
        int total = coeff_level_run(dct, &rl);
        int zeros = rl.last + 1 - total;
        uint32_t mask = i << (clz32(i) + 1);
        for (int j = 0; j < total-1 && zeros > 0; ++j) {
            int idx = zeros < 7 ? zeros-1 : 6;
            int run = clz32(mask);
            if (run >= 16) run = 0; /* defensive: x264 tables are 7x16 */
            int len = vk_run_before_init[idx][run].i_size;
            size += len;
            bits = (bits << len) | vk_run_before_init[idx][run].i_bits;
            zeros -= run;
            mask <<= run + 1;
        }
        run_before[i] = ((uint32_t)bits << 5) + (uint32_t)size;
    }
}

static int block_residual_escape(vk_bs_t *s, int suffix, int level)
{
    static const uint16_t next_suffix[7] = {0,3,6,12,24,48,0xffff};
    int prefix = 15;
    int mask = level >> 31;
    int abs_level = (level ^ mask) - mask;
    int code = abs_level*2-mask-2;
    if ((code >> suffix) < 15)
        vk_bs_write(s, (unsigned)((code >> suffix) + 1 + suffix),
                    (uint32_t)((1 << suffix) + (code & ((1 << suffix)-1))));
    else {
        code -= 15 << suffix;
        if (suffix == 0) code -= 15;
        vk_bs_write(s, (unsigned)(prefix+1), 1);
        vk_bs_write(s, (unsigned)(prefix-3), (uint32_t)(code & ((1 << (prefix-3))-1)));
    }
    if (suffix == 0) suffix++;
    if (abs_level > next_suffix[suffix]) suffix++;
    return suffix;
}

void vk_cavlc_block_residual(vk_bs_t *s, int cat, const int *coef, int nC, int chroma_v_shift)
{
    static const uint8_t ctz_index[8] = {3,0,1,0,2,0,1,0};
    static const uint8_t count_cat[14] = {16,15,16,0,15,64,16,15,16,64,16,15,16,64};
    vk_run_level_t rl;
    int total = coeff_level_run(coef, &rl);
    if (total == 0) {
        vk_vlc_t t = vk_coeff0_token[nC];
        vk_bs_write(s, t.i_size, t.i_bits);
        return;
    }
    int total_zero = rl.last + 1 - total;

    rl.level[total+0] = 2;
    rl.level[total+1] = 2;
    int trailing = ((((rl.level[0]+1) | (1-rl.level[0])) >> 31) & 1)
                 | ((((rl.level[1]+1) | (1-rl.level[1])) >> 31) & 2)
                 | ((((rl.level[2]+1) | (1-rl.level[2])) >> 31) & 4);
    trailing = ctz_index[trailing];
    /* x264 uses arithmetic shift on signed levels: (-1 >> 31) & 4 == 4.
     * Casting to uint32_t first would give (1 & 4) == 0 and lose the sign. */
    unsigned sign = ((rl.level[2] >> 31) & 1)
                  | ((rl.level[1] >> 31) & 2)
                  | ((rl.level[0] >> 31) & 4);
    sign >>= 3 - trailing;

    vk_vlc_t tok = vk_coeff_token[nC][total-1][trailing];
    vk_bs_write(s, tok.i_size, tok.i_bits);

    int suffix = (total > 10 && trailing < 3);
    vk_bs_write(s, (unsigned)trailing, sign);

    if (trailing < total) {
        int val = rl.level[trailing];
        int original = val + VK_LEVEL_TABLE_SIZE/2;
        val -= ((val >> 31) | 1) & -(trailing < 3);
        val += VK_LEVEL_TABLE_SIZE/2;
        if ((unsigned)original < VK_LEVEL_TABLE_SIZE) {
            vk_vlc_large_t v = level_token[suffix][val];
            vk_bs_write(s, v.i_size, v.i_bits);
            suffix = level_token[suffix][original].i_next;
        } else
            suffix = block_residual_escape(s, suffix, val-VK_LEVEL_TABLE_SIZE/2);
        for (int i = trailing+1; i < total; ++i) {
            val = rl.level[i] + VK_LEVEL_TABLE_SIZE/2;
            if ((unsigned)val < VK_LEVEL_TABLE_SIZE) {
                vk_vlc_large_t v = level_token[suffix][val];
                vk_bs_write(s, v.i_size, v.i_bits);
                suffix = v.i_next;
            } else
                suffix = block_residual_escape(s, suffix, val-VK_LEVEL_TABLE_SIZE/2);
        }
    }

    if (cat == VK_DCT_CHROMA_DC) {
        if (total < (8 >> chroma_v_shift)) {
            vk_vlc_t z = vk_total_zeros_2x2_dc[total-1][total_zero];
            vk_bs_write(s, z.i_size, z.i_bits);
        }
    } else if ((uint8_t)total < count_cat[cat]) {
        vk_vlc_t z = vk_total_zeros[total-1][total_zero];
        vk_bs_write(s, z.i_size, z.i_bits);
    }

    uint32_t code = run_before[rl.mask];
    vk_bs_write(s, code & 0x1f, code >> 5);
}
