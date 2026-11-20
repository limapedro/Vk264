/* SPDX-License-Identifier: GPL-2.0-or-later
 * Byte-exact RBSP bit writer. Mirrors x264 common/bitstream.h semantics:
 * MSB-first accumulation, bs_pos() in bits, ue/se/te, rbsp trailing bits.
 * No x86 intrinsics: correctness first, the GPU port happens in shaders. */
#ifndef VK264_BITSTREAM_H
#define VK264_BITSTREAM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    uint8_t *start;      /* beginning of the RBSP buffer */
    uint8_t *p;          /* next byte to receive a completed byte */
    uint8_t *end;        /* one past the end of the buffer */
    uint32_t acc;        /* partial byte, MSB-first */
    unsigned nbits;      /* bits currently in acc (0..7) */
} vk_bs_t;

void vk_bs_init(vk_bs_t *s, void *data, size_t bytes);
ptrdiff_t vk_bs_pos(const vk_bs_t *s);
int vk_bs_error(const vk_bs_t *s);
void vk_bs_write(vk_bs_t *s, unsigned count, uint32_t bits);
void vk_bs_write1(vk_bs_t *s, uint32_t bit);
void vk_bs_write_ue(vk_bs_t *s, uint32_t value);
void vk_bs_write_se(vk_bs_t *s, int32_t value);
void vk_bs_write_te(vk_bs_t *s, uint32_t base, uint32_t value);
void vk_bs_rbsp_trailing(vk_bs_t *s);
void vk_bs_align(vk_bs_t *s);

/* Annex-B NAL output with x264-compatible emulation prevention. */
void vk_nal_write(FILE *f, uint8_t nal_header, const uint8_t *rbsp,
                  size_t bytes, int long_startcode);

unsigned vk_bs_size_ue(uint32_t value);
unsigned vk_bs_size_se(int32_t value);
unsigned vk_bs_size_te(uint32_t base, uint32_t value);

#endif /* VK264_BITSTREAM_H */
