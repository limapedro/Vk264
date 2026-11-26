/* Round-trip tests for the Phoenix CABAC arithmetic decoder.
 *
 * Every bin sequence below is produced with src/phx_cabac.c (the
 * function-for-function x264 encoder port, itself checked against x264 golden
 * bytes in tests/test_cabac.c) and decoded with src/phx_cabac_dec.c.  No
 * approximations and no stream-parity claims: these are coder-level tests.
 *
 * SHADER: C — strictly serial encode -> decode, one bin at a time.
 */
#include "phx_cabac.h"
#include "phx_cabac_dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

/* x264_cabac_pos() (common/cabac.h:94-97) with the -8 queue offset:
 * encoder bit position = (p - start)*8 + i_queue + 8.  The decoder consumes
 * exactly 10 more bits than that (9 init bits + 1 guard bit). */
static int enc_bit_pos(const phx_cabac_t *cb, const uint8_t *start)
{
    return (int)(cb->p - start) * 8 + cb->i_queue + 8;
}

/* Golden x264 bytes from tests/test_cabac.c:16 decisions + 2 bypass + terminal. */
static void test_golden_bytes(void)
{
    static const int seq[16] = {1,0,1,1,0,0,1,0,1,0,0,1,1,1,0,0};
    uint8_t storage[128];
    memset(storage, 0, sizeof(storage));
    /* x264 cabac_putbyte() (cabac.c:89) may add a carry at p[-1], which in a
     * real slice lands in the preceding slice-header byte.  Reserve a prefix. */
    uint8_t *buf = storage + 8;
    phx_cabac_t cb;
    phx_cabac_encode_init(&cb, buf, storage + sizeof(storage));
    for (int i = 0; i < 1024; i++) cb.state[i] = 0x40;
    for (int i = 0; i < 16; i++)
        phx_cabac_encode_decision(&cb, (i % 7) + 3, seq[i]);
    /* x264_cabac_encode_bypass_c() (cabac.c:128-134) takes a mask: callers
     * pass coeff >> 31 (arithmetic shift => 0 or -1), never a literal 1. */
    phx_cabac_encode_bypass(&cb, 0);
    phx_cabac_encode_bypass(&cb, -1);
    phx_cabac_encode_terminal(&cb);
    int enc_pos = enc_bit_pos(&cb, buf);

    int n = (int)(cb.p - buf);
    CHECK(n == 3 && buf[0] == 0xFC && buf[1] == 0x64 && buf[2] == 0x25,
          "golden x264 bytes FC 64 25");

    /* The 3 golden bytes omit the final carry/flush bits; append them so the
     * decoder has the complete stream.  Bytes 0..2 stay unchanged. */
    phx_cabac_encode_flush(&cb, 0);
    n = (int)(cb.p - buf);

    phx_cabac_dec_t d;
    CHECK(phx_cabac_dec_init(&d, buf, n, 28, PHX_SLICE_P, 0) == 0,
          "decoder init (golden)");
    CHECK(phx_cabac_dec_bit_pos(&d) == 9, "init consumes 9 bits");
    for (int i = 0; i < 1024; i++) d.state[i] = 0x40; /* match test_cabac.c */
    for (int i = 0; i < 16; i++) {
        int got = phx_cabac_dec_decode_decision(&d, (i % 7) + 3);
        CHECK(got == seq[i], "golden decision bin");
    }
    CHECK(phx_cabac_dec_decode_bypass(&d) == 0, "golden bypass bin 0");
    CHECK(phx_cabac_dec_decode_bypass(&d) == 1, "golden bypass bin 1");
    CHECK(phx_cabac_dec_decode_terminal(&d) == 0, "golden terminal bin (MPS)");
    CHECK(d.error == 0, "golden decoder no error");
    CHECK(phx_cabac_dec_bit_pos(&d) == enc_pos + 10,
          "golden decoder bit position == encoder position + 10");
    CHECK(phx_cabac_dec_byte_pos(&d) == (enc_pos + 10 + 7) / 8,
          "golden byte position");
    CHECK(phx_cabac_dec_byte_pos(&d) <= n,
          "golden byte position within flushed stream");
    printf("golden: %d bytes, decoder bit_pos=%d\n",
           n, phx_cabac_dec_bit_pos(&d));
}

static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

/* Mixed decision / bypass / ue_bypass stream, encoded first, then decoded. */
static void test_mixed_roundtrip(int slice_type, int qp, int model)
{
    uint8_t storage[8192];
    memset(storage, 0, sizeof(storage));
    uint8_t *buf = storage + 8; /* carry can touch p[-1] (cabac.c:89) */
    phx_cabac_t cb;
    phx_cabac_context_init(&cb, slice_type, qp, model);
    phx_cabac_encode_init(&cb, buf, storage + sizeof(storage));

    enum { NDEC = 600, NBYP = 300, NUE = 120 };
    int dec_bin[NDEC], dec_ctx[NDEC], byp_bin[NBYP], ue_exp[NUE], ue_val[NUE];
    rng_state = 0x12345678u;
    for (int i = 0; i < NDEC; i++) {
        dec_bin[i] = (int)(rnd() & 1);
        dec_ctx[i] = (int)(rnd() % 200);
        phx_cabac_encode_decision(&cb, dec_ctx[i], dec_bin[i]);
    }
    int enc_pos_dec = enc_bit_pos(&cb, buf);
    for (int i = 0; i < NBYP; i++) {
        byp_bin[i] = (int)(rnd() & 1);
        phx_cabac_encode_bypass(&cb, byp_bin[i] ? -1 : 0); /* mask, not bin */
    }
    int enc_pos_byp = enc_bit_pos(&cb, buf);
    for (int i = 0; i < NUE; i++) {
        ue_exp[i] = (int)(rnd() % 4);
        /* Keep val >= 2^exp_bits: phx_cabac.c:175 casts bypass_lut[] to
         * uint32 AFTER shifting, so k==exp_bits (lut[0] == -1) would shift a
         * negative int (UB in the encoder, not owned by this test).  x264
         * casts before shifting.  Values here always index lut[>=1]. */
        ue_val[i] = (int)(rnd() % 16u) + (1 << ue_exp[i]);
        phx_cabac_encode_ue_bypass(&cb, ue_exp[i], ue_val[i]);
    }
    int enc_pos_ue = enc_bit_pos(&cb, buf);
    phx_cabac_encode_terminal(&cb);
    int enc_pos_end = enc_bit_pos(&cb, buf);
    phx_cabac_encode_flush(&cb, 0);
    int nbytes = (int)(cb.p - buf);
    CHECK(nbytes > 0 && cb.p < storage + sizeof(storage),
          "mixed stream byte count");

    phx_cabac_dec_t d;
    CHECK(phx_cabac_dec_init(&d, buf, nbytes, qp, slice_type, model) == 0,
          "decoder init (mixed)");
    CHECK(phx_cabac_dec_bit_pos(&d) == 9, "mixed init bit position");
    for (int i = 0; i < NDEC; i++) {
        int got = phx_cabac_dec_decode_decision(&d, dec_ctx[i]);
        CHECK(got == dec_bin[i], "mixed decision bin");
    }
    int pos_after_dec = phx_cabac_dec_bit_pos(&d);
    for (int i = 0; i < NBYP; i++) {
        int got = phx_cabac_dec_decode_bypass(&d);
        CHECK(got == byp_bin[i], "mixed bypass bin");
    }
    int pos_after_byp = phx_cabac_dec_bit_pos(&d);
    for (int i = 0; i < NUE; i++) {
        int got = phx_cabac_dec_decode_ue_bypass(&d, ue_exp[i]);
        CHECK(got == ue_val[i], "mixed ue_bypass value");
    }
    int pos_after_ue = phx_cabac_dec_bit_pos(&d);
    CHECK(phx_cabac_dec_decode_terminal(&d) == 0, "mixed terminal bin");
    CHECK(pos_after_dec == enc_pos_dec + 10,
          "exact bit position after decisions");
    CHECK(pos_after_byp == enc_pos_byp + 10,
          "exact bit position after bypass bins");
    CHECK(pos_after_ue == enc_pos_ue + 10,
          "exact bit position after ue_bypass");
    CHECK(phx_cabac_dec_bit_pos(&d) == enc_pos_end + 10,
          "exact bit position after ue_bypass + terminal");
    CHECK(d.error == 0, "mixed decoder no error");
    CHECK(pos_after_dec > 9 && pos_after_byp > pos_after_dec,
          "monotonic byte/bit positions");
    CHECK(phx_cabac_dec_bit_pos(&d) <= nbytes * 8,
          "decoder never reads past encoded bytes");
    printf("mixed st=%d qp=%d model=%d: %d bytes, bits dec=%d byp=%d end=%d\n",
           slice_type, qp, model, nbytes, pos_after_dec, pos_after_byp,
           phx_cabac_dec_bit_pos(&d));
}

/* Invalid arguments and truncated streams must be reported, not guessed. */
static void test_errors(void)
{
    uint8_t buf[16] = {0};
    phx_cabac_dec_t d;
    CHECK(phx_cabac_dec_init(NULL, buf, 16, 26, PHX_SLICE_P, 0) == -1,
          "null decoder rejected");
    CHECK(phx_cabac_dec_init(&d, buf, 1, 26, PHX_SLICE_P, 0) == -1,
          "short stream rejected");
    CHECK(phx_cabac_dec_init(&d, buf, 16, 52, PHX_SLICE_P, 0) == -1,
          "bad qp rejected");
    CHECK(phx_cabac_dec_init(&d, buf, 16, 26, PHX_SLICE_P, 3) == -1,
          "bad init idc rejected");
    CHECK(phx_cabac_dec_init(&d, buf, 16, 26, PHX_SLICE_P, 0) == 0,
          "valid init accepted");
    CHECK(phx_cabac_dec_init(&d, buf, 16, 26, PHX_SLICE_P, -1) == 0,
          "cabac_init_idc -1 inferred to 0");
    CHECK(phx_cabac_dec_decode_decision(&d, -1) == -1, "bad ctx rejected");
    CHECK(phx_cabac_dec_decode_decision(&d, PHX_CABAC_CTX_COUNT) == -1,
          "bad ctx upper bound rejected");
    CHECK(phx_cabac_dec_decode_ue_bypass(&d, -1) == -1, "bad exp_bits rejected");
}

/* Optional smoke test on a real x264 CABAC stream: find the first CABAC
 * slice NAL in build/parity/synth-medium-cabac.ref.h264, strip emulation
 * prevention bytes and run the arithmetic engine over it.  This checks that
 * the decoder tolerates real CABAC bytes; it is NOT a bin-parity test (we do
 * not claim to reproduce x264 slice syntax). */
static void test_golden_stream_smoke(void)
{
    const char *path = "build/parity/synth-medium-cabac.ref.h264";
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("golden stream: %s not found (skipped)\n", path);
        return;
    }
    uint8_t file[16384];
    size_t sz = fread(file, 1, sizeof(file), f);
    fclose(f);
    size_t i = 0;
    while (i + 4 < sz && !(file[i] == 0 && file[i+1] == 0 && file[i+2] == 1)) i++;
    int found = 0, n = 0;
    uint8_t payload[8192];
    while (i + 4 < sz) {
        while (i + 4 < sz && !(file[i] == 0 && file[i+1] == 0 && file[i+2] == 1)) i++;
        if (i + 4 >= sz) break;
        size_t nal = i + 3;
        int type = file[nal] & 0x1f;
        size_t j = nal + 1, k = 0;
        while (j < sz && k < sizeof(payload)) {
            if (j + 2 < sz && file[j] == 0 && file[j+1] == 0 && file[j+2] == 1) break;
            if (k >= 3 && file[j] == 3 && file[k-1] == 0 && file[k-2] == 0) { j++; continue; }
            payload[k++] = file[j++];
        }
        if ((type == 1 || type == 5) && k > 16) {
            phx_cabac_dec_t d;
            if (phx_cabac_dec_init(&d, payload, (int)k, 26,
                                   type == 5 ? PHX_SLICE_I : PHX_SLICE_P, 0) == 0) {
                int bins = 0;
                while (bins < 512 && !d.error &&
                       phx_cabac_dec_bit_pos(&d) + 8 < (int)k * 8) {
                    int b = phx_cabac_dec_decode_decision(&d, bins % 200);
                    if (b < 0) break;
                    bins++;
                }
                CHECK(d.error == 0 && bins > 100, "golden stream decodes bins");
                printf("golden stream: NAL type %d, %d payload bytes, %d bins\n",
                       type, (int)k, bins);
                found = 1;
                break;
            }
        }
        i = nal;
        n++;
        if (n > 32) break;
    }
    if (!found) printf("golden stream: no CABAC slice found (skipped)\n");
}

int main(void)
{
    test_golden_bytes();
    test_mixed_roundtrip(PHX_SLICE_I, 26, 0);
    test_mixed_roundtrip(PHX_SLICE_P, 28, 0);
    test_mixed_roundtrip(PHX_SLICE_B, 32, 2);
    test_errors();
    test_golden_stream_smoke();
    printf("%s: cabac decoder, %d failures\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
