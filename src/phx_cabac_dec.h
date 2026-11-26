/* Phoenix CABAC arithmetic DECODER — counterpart to src/phx_cabac.{h,c}.
 *
 * The decoder follows the H.264 arithmetic decoding engine
 * (ITU-T Rec. H.264, clauses 9.3.1.2 / 9.3.3.2 / 9.3.3.2.1 / 9.3.3.2.2 /
 * 9.3.3.2.3 / 9.3.3.2.4; rangeTabLPS = Table 9-33 in the 2003 edition /
 * Table 9-44 in the 2012+ edition, state transition = Table 9-34 / 9-45).
 * All lookup tables are the x264 tables already in src/phx_tables.h, i.e. the
 * very tables the Phoenix encoder (src/phx_cabac.c) uses.  Cross-checks:
 * FFmpeg libavcodec/cabac.c ff_init_cabac_decoder() (9-bit offset init),
 * libavcodec/cabac_functions.h get_cabac_inline(), get_cabac_bypass(),
 * get_cabac_terminate().  x264 encoder lines mirrored here:
 * reference/x264/common/cabac.c:53-59 (init), :101-108 (renorm),
 * :113-124 (decision), :128-134 (bypass), :136-157 (ue_bypass), :159-163
 * (terminal).  No new tables are introduced.
 *
 * `data` must point at the first byte of the CABAC slice payload (RBSP with
 * emulation-prevention bytes already removed, as after the byte-aligned
 * slice-header padding in phx_encoder.c:1367-1372).
 *
 * SHADER: C — strictly serial arithmetic decode state; keep in entropy stage.
 */
#ifndef PHX_CABAC_DEC_H
#define PHX_CABAC_DEC_H

#include <stdint.h>

/* Slice type / context count values are shared with the encoder. */
#include "phx_cabac.h"

typedef struct {
    int i_range;                 /* codIRange, initialised to 0x01FE (510) */
    int i_offset;                /* codIOffset (value - interval lower bound) */
    const uint8_t *p_start;      /* first byte of the CABAC payload */
    const uint8_t *p;            /* next byte to be buffered */
    const uint8_t *p_end;        /* one past the last available byte */
    unsigned int bitbuf;         /* current byte, MSB-first bit consumption */
    int bits_left;               /* unread bits in bitbuf */
    int bit_pos;                 /* bits consumed from p_start (incl. 9 init bits) */
    int error;                   /* nonzero after an out-of-range read/state */
    uint8_t state[1024];         /* same packed state as phx_cabac_t.state */
} phx_cabac_dec_t;

/* H.264 9.3.1.1 / x264 common/cabac.c:32-51 context initialisation,
 * then 9.3.1.2 arithmetic-engine init.  Returns 0 on success, -1 on bad args.
 * slice_qp in [0,51]; slice_type is PHX_SLICE_{P,B,I}; cabac_init_idc in
 * [-1,2] (-1 means "not present", inferred to 0 per the standard). */
int phx_cabac_dec_init(phx_cabac_dec_t *d, const uint8_t *data, int bytes,
                       int slice_qp, int slice_type, int cabac_init_idc);

/* H.264 9.3.3.2.1 decision, 9.3.3.2.3 bypass, 9.3.3.2.4 terminal, and the
 * Rice/exp-Golomb bypass sequence produced by x264_cabac_encode_ue_bypass()
 * (common/cabac.c:142-157).  Return decoded bin/value, or -1 on error. */
int phx_cabac_dec_decode_decision(phx_cabac_dec_t *d, int ctx);
int phx_cabac_dec_decode_bypass(phx_cabac_dec_t *d);
int phx_cabac_dec_decode_terminal(phx_cabac_dec_t *d);
int phx_cabac_dec_decode_ue_bypass(phx_cabac_dec_t *d, int exp_bits);

/* Observers for position assertions in tests (no state change). */
int phx_cabac_dec_bit_pos(const phx_cabac_dec_t *d);
int phx_cabac_dec_byte_pos(const phx_cabac_dec_t *d);

#endif /* PHX_CABAC_DEC_H */
