/* Port of x264 common/bitstream.h / common/bitstream.c. */
#include "phx_bitstream.h"
#include "phx_tables.h"
#include <string.h>

/* bitstream.h:86 — bs_init(). The caller's buffer is zeroed so that partial
 * bytes read as zero, matching x264's aligned 32-bit writes. */
void phx_bs_init(phx_bs_t *s, void *data, int size)
{
    memset(data, 0, (size_t)size);                      /* x264 writes whole words */
    s->p_start = (uint8_t*)data;
    s->p = (uint8_t*)data;
    s->bit = 0;
}

/* bitstream.h:100 — bs_pos(). */
int phx_bs_pos(const phx_bs_t *s)
{
    return 8*(int)(s->p - s->p_start) + s->bit;
}

/* bitstream.h:125 — bs_write(): append count MSB-first bits. */
void phx_bs_write(phx_bs_t *s, int count, uint32_t bits)
{
    for (int i = count-1; i >= 0; i--) {
        *s->p |= (uint8_t)(((bits >> i) & 1) << (7 - s->bit));
        if (++s->bit == 8) { s->p++; s->bit = 0; }
    }
}

/* bitstream.h:169 — bs_write1(). */
void phx_bs_write1(phx_bs_t *s, uint32_t bit)
{
    *s->p |= (uint8_t)((bit & 1) << (7 - s->bit));
    if (++s->bit == 8) { s->p++; s->bit = 0; }
}

/* bitstream.h:182 — bs_align_0(): pad with zeros, flush the partial byte. */
void phx_bs_align_0(phx_bs_t *s)
{
    if (s->bit) { s->p++; s->bit = 0; }
}

/* bitstream.h:192 — bs_align_10(): a single 1 then zeros to byte boundary. */
void phx_bs_align_10(phx_bs_t *s)
{
    if (s->bit) phx_bs_write1(s, 1);
    phx_bs_align_0(s);
}

/* bitstream.h:278 — bs_size_ue(). */
int phx_bs_size_ue(unsigned val)
{
    return phx_ue_size_tab[val+1];
}

/* bitstream.h:291 — bs_size_se(). */
int phx_bs_size_se(int val)
{
    int tmp = 1 - val*2;
    if (tmp < 0) tmp = val*2;
    if (tmp < 256) return phx_ue_size_tab[tmp];
    return phx_ue_size_tab[tmp>>8]+16;
}

/* bitstream.h:221 — bs_write_ue_big(). */
void phx_bs_write_ue_big(phx_bs_t *s, unsigned val)
{
    int size = 0;
    unsigned tmp = ++val;
    if (tmp >= 0x10000) { size = 32; tmp >>= 16; }
    if (tmp >= 0x100) { size += 16; tmp >>= 8; }
    size += phx_ue_size_tab[tmp];
    phx_bs_write(s, size>>1, 0);
    phx_bs_write(s, (size>>1)+1, val);
}

/* bitstream.h:241 — bs_write_ue() (values < 255). */
void phx_bs_write_ue(phx_bs_t *s, unsigned val)
{
    phx_bs_write(s, phx_ue_size_tab[val+1], val+1);
}

/* bitstream.h:246 — bs_write_se(). */
void phx_bs_write_se(phx_bs_t *s, int val)
{
    int size = 0;
    int tmp = 1 - val*2;                                /* :251 */
    if (tmp < 0) tmp = val*2;                           /* :252 */
    val = tmp;                                          /* :253 */
    if (tmp >= 0x100) { size = 16; tmp >>= 8; }         /* :255-257 */
    size += phx_ue_size_tab[tmp];                       /* :260 */
    phx_bs_write(s, size, (uint32_t)val);               /* :261 */
}

/* bitstream.h:264 — bs_write_te(). */
void phx_bs_write_te(phx_bs_t *s, int x, int val)
{
    if (x == 1) phx_bs_write1(s, 1 ^ (uint32_t)val);
    else phx_bs_write_ue(s, (unsigned)val);
}

/* bitstream.h:272 — bs_rbsp_trailing(). */
void phx_bs_rbsp_trailing(phx_bs_t *s)
{
    phx_bs_write1(s, 1);
    if (s->bit) phx_bs_write(s, 8 - s->bit, 0);         /* x264: bs_write(i_left&7,0) */
    phx_bs_align_0(s);
}

/* bitstream.c:29 — nal_escape_c(). */
uint8_t *phx_nal_escape(uint8_t *dst, uint8_t *src, uint8_t *end)
{
    if (src < end) *dst++ = *src++;                     /* :31 */
    if (src < end) *dst++ = *src++;                     /* :32 */
    while (src < end) {                                 /* :33 */
        if (src[0] <= 0x03 && !dst[-2] && !dst[-1])
            *dst++ = 0x03;                              /* :35-36 */
        *dst++ = *src++;                                /* :37 */
    }
    return dst;                                         /* :39 */
}

/* bitstream.c:55 — x264_nal_encode(). */
void phx_nal_encode(uint8_t *dst, phx_nal_t *nal, int b_annexb)
{
    uint8_t *src = nal->p_payload;                      /* :57 */
    uint8_t *end = nal->p_payload + nal->i_payload;     /* :58 */
    uint8_t *orig_dst = dst;                            /* :59 */

    if (b_annexb) {                                     /* :61 */
        if (nal->b_long_startcode) *dst++ = 0x00;       /* :63 */
        *dst++ = 0x00; *dst++ = 0x00; *dst++ = 0x01;    /* :65-67 */
    } else dst += 4;                                    /* :70 */

    *dst++ = (uint8_t)((nal->i_ref_idc << 5) | nal->i_type); /* :73 */
    dst = phx_nal_escape(dst, src, end);                /* :75 */
    nal->i_payload = (int)(dst - orig_dst);             /* :101 */
    nal->p_payload = orig_dst;                          /* :102 */
}
