/* Phoenix bitstream writer — mirrors common/bitstream.{h,c}.
 *
 * x264's bs_write accumulates bits in a 64-bit word and flushes 32-bit words
 * with endian_fix(); on little-endian hosts that yields the same MSB-first
 * byte stream as the straightforward bit-by-bit writer used here. Byte output
 * is therefore identical; the simpler accumulator is easier to verify and to
 * map to a shader later.
 *
 * Source: common/bitstream.h:86-307 (bs_*, Golomb, sizes),
 *         common/bitstream.c:29-104 (nal_escape_c, x264_nal_encode).
 *
 * SHADER: C — a single ordered output stream; run in the serial entropy stage.
 */
#ifndef PHX_BITSTREAM_H
#define PHX_BITSTREAM_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *p_start;               /* first byte of the RBSP buffer */
    uint8_t *p;                     /* current byte */
    int bit;                        /* next bit position inside *p (0..7) */
} phx_bs_t;

typedef struct {
    uint8_t *p_payload;             /* in: RBSP, out: encoded NAL */
    int i_payload;                  /* in/out: size */
    int i_ref_idc;                  /* nal_ref_idc */
    int i_type;                     /* nal_unit_type */
    int b_long_startcode;
} phx_nal_t;

void phx_bs_init(phx_bs_t *s, void *data, int size);   /* bitstream.h:86 */
int  phx_bs_pos(const phx_bs_t *s);                    /* bitstream.h:100 */
void phx_bs_write(phx_bs_t *s, int count, uint32_t bits); /* :125 */
void phx_bs_write1(phx_bs_t *s, uint32_t bit);         /* :169 */
void phx_bs_align_0(phx_bs_t *s);                      /* :182 */
void phx_bs_align_10(phx_bs_t *s);                     /* :192 */
void phx_bs_write_ue(phx_bs_t *s, unsigned val);       /* :241 */
void phx_bs_write_ue_big(phx_bs_t *s, unsigned val);   /* :221 */
void phx_bs_write_se(phx_bs_t *s, int val);            /* :246 */
void phx_bs_write_te(phx_bs_t *s, int x, int val);     /* :264 */
void phx_bs_rbsp_trailing(phx_bs_t *s);                /* :272 */

int phx_bs_size_ue(unsigned val);                      /* :278 */
int phx_bs_size_se(int val);                           /* :291 */

/* bitstream.c:29 — insert 0x03 before 00 00 {0,1,2,3}. Returns dst end. */
uint8_t *phx_nal_escape(uint8_t *dst, uint8_t *src, uint8_t *end);

/* bitstream.c:55 — emit Annex-B start code + NAL header + escaped payload. */
void phx_nal_encode(uint8_t *dst, phx_nal_t *nal, int b_annexb);

#endif
