/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bitstream.h"

void vk_bs_init(vk_bs_t *s, void *data, size_t bytes)
{
    s->start = (uint8_t *)data;
    s->p = (uint8_t *)data;
    s->end = s->p + bytes;
    s->acc = 0;
    s->nbits = 0;
}

ptrdiff_t vk_bs_pos(const vk_bs_t *s)
{
    return 8 * (ptrdiff_t)(s->p - s->start) + (ptrdiff_t)s->nbits;
}

int vk_bs_error(const vk_bs_t *s)
{
    return s->p > s->end;
}

static void vk_bs_put_bit(vk_bs_t *s, uint32_t bit)
{
    s->acc = (s->acc << 1) | (bit & 1u);
    if (++s->nbits == 8) {
        if (s->p < s->end)
            *s->p = (uint8_t)s->acc;
        ++s->p; /* still advances on overflow so bs_pos() keeps growing */
        s->acc = 0;
        s->nbits = 0;
    }
}

void vk_bs_write(vk_bs_t *s, unsigned count, uint32_t bits)
{
    if (count == 0)
        return;
    if (count > 32) {
        vk_bs_write(s, count - 32, 0);
        count = 32;
    }
    for (unsigned i = count; i-- > 0;)
        vk_bs_put_bit(s, bits >> i);
}

void vk_bs_write1(vk_bs_t *s, uint32_t bit)
{
    vk_bs_put_bit(s, bit);
}

unsigned vk_bs_size_ue(uint32_t value)
{
    uint32_t v = value + 1u;
    unsigned n = 0;
    while (v > 1u) { v >>= 1; ++n; }
    return 2 * n + 1;
}

void vk_bs_write_ue(vk_bs_t *s, uint32_t value)
{
    uint32_t v = value + 1u;
    unsigned n = vk_bs_size_ue(value) / 2;
    vk_bs_write(s, n, 0);
    vk_bs_write(s, n + 1, v);
}

unsigned vk_bs_size_se(int32_t value)
{
    uint32_t v = value <= 0 ? (uint32_t)(-2 * (int64_t)value) : (uint32_t)(2 * (int64_t)value - 1);
    return vk_bs_size_ue(v);
}

void vk_bs_write_se(vk_bs_t *s, int32_t value)
{
    uint32_t v = value <= 0 ? (uint32_t)(-2 * (int64_t)value) : (uint32_t)(2 * (int64_t)value - 1);
    vk_bs_write_ue(s, v);
}

unsigned vk_bs_size_te(uint32_t base, uint32_t value)
{
    if (base == 1)
        return vk_bs_size_ue(value);
    unsigned n = 0;
    while ((1u << n) < base + 1u) ++n;
    return n;
}

void vk_bs_write_te(vk_bs_t *s, uint32_t base, uint32_t value)
{
    if (base == 1) {
        vk_bs_write_ue(s, value);
        return;
    }
    vk_bs_write(s, vk_bs_size_te(base, value), value);
}

void vk_bs_rbsp_trailing(vk_bs_t *s)
{
    vk_bs_write1(s, 1);
    if (s->nbits != 0)
        vk_bs_write(s, 8 - s->nbits, 0);
}

void vk_bs_align(vk_bs_t *s)
{
    if (s->nbits != 0)
        vk_bs_write(s, 8 - s->nbits, 0);
}

void vk_nal_write(FILE *f, uint8_t nal_header, const uint8_t *rbsp,
                  size_t bytes, int long_startcode)
{
    if (long_startcode) {
        fputc(0, f); fputc(0, f); fputc(0, f); fputc(1, f);
    } else {
        fputc(0, f); fputc(0, f); fputc(1, f);
    }
    fputc(nal_header, f);
    int zeros = 0;
    for (size_t i = 0; i < bytes; ++i) {
        if (zeros >= 2 && rbsp[i] <= 3) {
            fputc(3, f);
            zeros = 0;
        }
        fputc(rbsp[i], f);
        if (rbsp[i] == 0) ++zeros; else zeros = 0;
    }
}
