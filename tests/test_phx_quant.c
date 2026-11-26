/* Golden values generated from the exact x264 scalar formulas in
 * reference/x264/common/{dct,quant,set}.c (8-bit HIGH_BIT_DEPTH=0).
 * Covers DCT/Hadamard/IDCT, deadzone quantizers, dequantizers and
 * chroma DC optimizations against deterministic input vectors.
 */
#include "phx_quant.h"
#include "phx_cabac_syntax.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n", msg, __LINE__); ++fails; } } while (0)

static const int16_t exp_sub4[16] = {
    960, -320, -512, 320, 432, 768, -768, -1536, 0, 0, 0, 0,
    -304, -256, 256, 512,
};

static const int16_t exp_sub8[64] = {
    960, -320, -512, 320, 432, 768, -768, -1536, 0, 0, 0, 0,
    -304, -256, 256, 512, -576, 1728, 256, 64, 176, -1024, 512, -512,
    256, -512, 256, -256, 208, -512, 256, -256, -1088, -576, 1024, -448,
    -80, 0, -768, 1280, 0, 256, -512, 768, 720, -1280, 256, 0,
    448, -832, -768, 64, -592, 256, 1280, 2048, -256, -512, -256, -256,
    464, 768, 0, -256,
};

static const int16_t exp_sub16[256] = {
    960, -320, -512, 320, 432, 768, -768, -1536, 0, 0, 0, 0,
    -304, -256, 256, 512, -576, 1728, 256, 64, 176, -1024, 512, -512,
    256, -512, 256, -256, 208, -512, 256, -256, -1088, -576, 1024, -448,
    -80, 0, -768, 1280, 0, 256, -512, 768, 720, -1280, 256, 0,
    448, -832, -768, 64, -592, 256, 1280, 2048, -256, -512, -256, -256,
    464, 768, 0, -256, -576, -576, 0, -448, -80, 3072, -768, 256,
    0, 256, 512, 768, 720, 256, 256, -512, 192, -576, 0, -448,
    -592, -3072, 768, -256, 0, 256, 512, 768, -816, -256, -256, 512,
    704, -64, 256, -192, 944, -256, -1280, 512, 256, 256, -256, -512,
    -48, 512, 0, -1024, -320, 2240, 0, 320, -336, 0, 0, 0,
    0, 0, 0, 0, -48, 0, 0, 0, 704, 1728, 256, 64,
    176, -1024, 512, -512, 256, -512, 256, -256, 208, -512, 256, -256,
    -64, 192, -256, 576, 944, 1792, -256, -1024, 256, 512, 256, 256,
    -48, 256, 512, 768, -320, 704, 256, -448, -592, 2560, -256, 1280,
    -256, -512, -256, -256, 464, 0, 512, 0, -64, -2368, -256, 576,
    176, -2560, 512, 2560, -256, 512, -256, 256, 208, 0, 256, -1280,
    -1088, 192, 1024, -704, -336, 0, 0, 0, 0, 0, 0, 0,
    -48, 0, 0, 0, 960, -832, -768, 64, -80, 512, 256, 256,
    -256, 0, 768, 1280, -560, -1024, -512, -512, 704, 1728, -256, 64,
    176, 1024, 512, 512, -256, -512, -256, -256, 208, 512, 256, 256,
    -320, -320, 0, 320, 688, 0, -1024, 0, 0, 512, 0, -1024,
    464, 0, -512, 0,
};

static const int16_t exp_dc4[1] = {
    960,
};

static const int16_t exp_dc8[4] = {
    -256, 1024, 0, 3072,
};

static const int16_t exp_dc8x16[8] = {
    0, 512, -512, -512, 0, 2048, 2048, 4096,
};

static const int16_t exp_dct4dc[16] = {
    212, -416, 0, -208, -104, 0, 0, 0, 0, 0, 0, 0,
    -52, 0, 0, 0,
};

static const int16_t exp_idct4dc[16] = {
    424, -832, 0, -416, -208, 0, 0, 0, 0, 0, 0, 0,
    -104, 0, 0, 0,
};

static const int16_t exp_dct2x4[8] = {
    68, -36, -144, 0, 0, 0, -72, 0,
};

static const int16_t exp_dc4x4in[8] = {
    -23, -14, -5, 4, 13, 22, 31, 40,
};

static const int16_t exp_quant4[16] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    -1, 0, 0, 0,
};

static const int16_t exp_quant8[64] = {
    -2, 0, 1, -1, 0, 2, -1, 1, -2, 0, 2, -1,
    0, 2, -1, 1, -2, 0, 3, -1, 1, -3, -1, 1,
    -1, 0, 2, -1, 1, -2, 0, 1, -1, 0, 3, 0,
    1, -2, 0, 2, -1, 0, -3, 0, 1, -1, 0, 2,
    -1, 1, -3, 0, 2, -1, 1, 3, 0, 1, -2, 0,
    2, -1, 1, -2,
};

static const int16_t exp_quant4x4[64] = {
    0, 0, 0, 1, 0, 0, 1, -1, 0, 0, 1, 0,
    -1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, -1,
    0, 0, 1, 0, -1, 0, 1, 0, 0, 0, 0, -1,
    0, 0, -1, 1, 0, 0, -1, 0, 1, 0, -1, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    -1, 0, 0, 0,
};

static const int16_t exp_quant4dc[16] = {
    0, 0, 0, 0, 0, 0, 1, -1, 0, 0, 0, 0,
    -1, 0, 0, 0,
};

static const int16_t exp_quant2dc[4] = {
    0, 0, 0, 0,
};

static const int16_t exp_dequant4[16] = {
    0, 320, -512, 960, -320, 0, 640, -1200, 256, 0, -256, 640,
    0, -800, 320, -400,
};

static const int16_t exp_dequant8[64] = {
    -320, 120, -160, 300, 64, -180, 320, 0, -240, 168, -76, -280,
    120, -112, 380, 56, -240, 304, 0, -304, 240, -76, -510, 152,
    -120, 280, 76, -168, 240, 0, -304, 168, -64, -300, 160, -120,
    320, 60, -240, 240, 0, -224, 228, -56, -300, 112, -152, 280,
    80, -228, 408, 0, -320, 228, -102, -380, 120, -112, 380, 56,
    -180, 224, 0, -224,
};

static const int16_t exp_dequant4dc[16] = {
    0, 32, -64, 96, -32, 0, 64, -96, 32, 0, -32, 64,
    0, -64, 32, -32,
};

static const int16_t exp_dq2x4dc[8] = {
    832, -576, 1088, -2880, 704, 320, -320, 832,
};

static const int16_t exp_opt2x2[4] = {
    1952, 416, -3424, 1184,
};

static const int16_t exp_opt2x4[8] = {
    864, -544, 1120, -2848, 736, 352, -288, 864,
};

static const int16_t exp_optdc2d[4] = {
    0, 9, -6, 12,
};

static const int16_t exp_optdc4d[8] = {
    0, 9, -6, 12, 3, -8, 5, -2,
};

static const int16_t exp_add8_8coef[64] = {
    -447, -397, -317, -235, -155, -73, 7, 90, -1288, -1341, -1361, -1382,
    -1403, -1423, -1444, -1465, 298, 280, 296, 310, 326, 341, 357, 371,
    -556, -590, -590, -593, -593, -595, -596, -598, 134, 114, 126, 139,
    151, 163, 176, 188, -368, -404, -408, -412, -416, -421, -425, -429,
    78, 57, 65, 76, 85, 95, 104, 115, -83, -119, -123, -127,
    -131, -135, -139, -144,
};

static const uint8_t exp_add4[16] = {
    2, 12, 46, 66, 221, 249, 14, 37, 192, 213, 238, 5,
    160, 181, 206, 228,
};

static const uint8_t exp_add8[64] = {
    2, 12, 46, 66, 95, 104, 138, 158, 221, 249, 14, 37,
    57, 85, 106, 129, 192, 213, 238, 5, 28, 49, 74, 97,
    160, 181, 206, 228, 252, 18, 42, 64, 132, 139, 174, 194,
    225, 231, 10, 31, 93, 121, 142, 165, 185, 213, 234, 1,
    65, 85, 110, 133, 157, 177, 202, 225, 32, 53, 78, 101,
    124, 145, 170, 193,
};

static const uint8_t exp_add8_8[64] = {
    0, 0, 81, 1, 108, 68, 146, 146, 211, 255, 1, 55,
    53, 96, 101, 134, 192, 184, 246, 0, 32, 42, 76, 94,
    152, 188, 204, 230, 250, 20, 41, 66, 126, 126, 180, 187,
    223, 236, 12, 31, 91, 129, 139, 169, 186, 214, 233, 2,
    62, 68, 114, 125, 158, 173, 203, 223, 31, 64, 76, 105,
    123, 150, 169, 194,
};

static const int16_t exp_denoise[16] = {
    -71, -55, -39, -23, -7, 0, 6, 16, 26, 36, 46, 56,
    83, 93, 103, 113,
};

static const uint16_t exp_table_q4mf[5] = {
    26214, 6554, 1024, 1024, 419,
};

static const uint16_t exp_table_q4bias[5] = {
    1, 3, 21, 21, 51,
};

static const uint16_t exp_table_q8mf[4] = {
    13107, 3277, 512, 459,
};

static const uint16_t exp_table_q8bias[4] = {
    2, 7, 42, 47,
};

static const uint16_t exp_dmf[1] = {
    4096,
};

static const uint16_t exp_table_dq4[3] = {
    160, 288, 400,
};

static const uint16_t exp_table_dq8[3] = {
    320, 544, 400,
};

static const uint8_t exp_quant4nz[1] = {
    1,
};

static const uint8_t exp_quant8nz[1] = {
    1,
};

static const uint8_t exp_quant4x4nz[1] = {
    15,
};

static const uint8_t exp_quant4dcnz[1] = {
    1,
};

static const uint8_t exp_quant2dcnz[1] = {
    0,
};

static const uint8_t exp_optround[2] = {
    0, 0,
};

static const uint8_t exp_optdc2[1] = {
    1,
};

/* R1 golden from x264's own objects (build/x264_ref/obj/common_set.obj and
 * common_quant.obj) for default flat CQM, i_luma_deadzone={21,11}.
 * bias0 is set.c:180/200; plain bias is set.c:179/199. */
static const uint16_t exp_q4bias0[8] = {
     1, 2, 6, 13, 32, 72, 159, 352,   /* qp = 0,7,14,21,28,35,42,49 */
};
static const uint16_t exp_q8bias0[8] = {
     2, 5, 12, 28, 64, 143, 321, 697,
};

/* MB0 block0 of the medium/cabac parity fixture: qp=25, CQM_4IY,
 * mf[0]=1490, deadzone bias[0]=14, bias0[0]=21.  The pre-quant DCT vector
 * was dumped from x264's encoder/macroblock.h:155 before quant.  x264's
 * plain quant_4x4 (analysis, b_trellis=0, analyse.c:261) gives -35; its
 * trellis QNS (final encode, b_trellis=i_trellis, analyse.c:3724 ->
 * macroblock.h:96-99 -> rdo.c bias0) gives the -36 golden. */
static const int16_t in_mb0_b0[16] = {
     -1568, -364, 0, -52, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const int16_t exp_mb0_b0_plain[16] = {
     -35, -5, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const int16_t exp_mb0_b0_trellis[16] = {
     -36, -5, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const uint8_t exp_optdc4[1] = {
    1,
};

static const int16_t in_c16[16] = {
    -71, -58, -45, -32, -19, -6, 7, 20, 33, 46, 59, 72,
    85, 98, 111, 124,
};

static const int16_t in_c64[64] = {
    -311, -300, -289, -278, -267, -256, -245, -234, -223, -212, -201, -190,
    -179, -168, -157, -146, -135, -124, -113, -102, -91, -80, -69, -58,
    -47, -36, -25, -14, -3, 8, 19, 30, 41, 52, 63, 74,
    85, 96, 107, 118, 129, 140, 151, 162, 173, 184, 195, 206,
    217, 228, 239, 250, 261, 272, 283, 294, 305, 316, 327, 338,
    349, 360, 371, 382,
};

static const int16_t in_qc16[16] = {
    0, 17, -19, 41, -7, 0, 63, -55, 12, -3, 28, 0,
    -71, 5, 33, -13,
};

static const int16_t in_qc64[64] = {
    -48, -11, 26, -34, 3, 40, -20, 17, -43, -6, 31, -29,
    8, 45, -15, 22, -38, -1, 36, -24, 13, -47, -10, 27,
    -33, 4, 41, -19, 18, -42, -5, 32, -28, 9, 46, -14,
    23, -37, 0, 37, -23, 14, -46, -9, 28, -32, 5, 42,
    -18, 19, -41, -4, 33, -27, 10, 47, -13, 24, -36, 1,
    38, -22, 15, -45,
};

static const int16_t in_dq16[16] = {
    0, 1, -2, 3, -1, 0, 2, -3, 1, 0, -1, 2,
    0, -2, 1, -1,
};

static const int16_t in_dq64[64] = {
    -5, 2, -2, 5, 1, -3, 4, 0, -4, 3, -1, -5,
    2, -2, 5, 1, -3, 4, 0, -4, 3, -1, -5, 2,
    -2, 5, 1, -3, 4, 0, -4, 3, -1, -5, 2, -2,
    5, 1, -3, 4, 0, -4, 3, -1, -5, 2, -2, 5,
    1, -3, 4, 0, -4, 3, -1, -5, 2, -2, 5, 1,
    -3, 4, 0, -4,
};

static const int16_t in_dc8[8] = {
    0, 9, -6, 12, 3, -8, 5, -2,
};

static const uint16_t in_off16[16] = {
    0, 3, 6, 9, 12, 15, 1, 4, 7, 10, 13, 16,
    2, 5, 8, 11,
};

static const uint8_t p1[256] = {
    5, 34, 63, 92, 121, 150, 179, 208, 237, 10, 39, 68,
    97, 126, 155, 184, 213, 242, 15, 44, 73, 102, 131, 160,
    189, 218, 247, 20, 49, 78, 107, 136, 165, 194, 223, 252,
    25, 54, 83, 112, 141, 170, 199, 228, 1, 30, 59, 88,
    117, 146, 175, 204, 233, 6, 35, 64, 93, 122, 151, 180,
    209, 238, 11, 40, 69, 98, 127, 156, 185, 214, 243, 16,
    45, 74, 103, 132, 161, 190, 219, 248, 21, 50, 79, 108,
    137, 166, 195, 224, 253, 26, 55, 84, 113, 142, 171, 200,
    229, 2, 31, 60, 89, 118, 147, 176, 205, 234, 7, 36,
    65, 94, 123, 152, 181, 210, 239, 12, 41, 70, 99, 128,
    157, 186, 215, 244, 17, 46, 75, 104, 133, 162, 191, 220,
    249, 22, 51, 80, 109, 138, 167, 196, 225, 254, 27, 56,
    85, 114, 143, 172, 201, 230, 3, 32, 61, 90, 119, 148,
    177, 206, 235, 8, 37, 66, 95, 124, 153, 182, 211, 240,
    13, 42, 71, 100, 129, 158, 187, 216, 245, 18, 47, 76,
    105, 134, 163, 192, 221, 250, 23, 52, 81, 110, 139, 168,
    197, 226, 255, 28, 57, 86, 115, 144, 173, 202, 231, 4,
    33, 62, 91, 120, 149, 178, 207, 236, 9, 38, 67, 96,
    125, 154, 183, 212, 241, 14, 43, 72, 101, 130, 159, 188,
    217, 246, 19, 48, 77, 106, 135, 164, 193, 222, 251, 24,
    53, 82, 111, 140, 169, 198, 227, 0, 29, 58, 87, 116,
    145, 174, 203, 232,
};

static const uint8_t p2[512] = {
    3, 20, 37, 54, 71, 88, 105, 122, 139, 156, 173, 190,
    207, 224, 241, 2, 19, 36, 53, 70, 87, 104, 121, 138,
    155, 172, 189, 206, 223, 240, 1, 18, 35, 52, 69, 86,
    103, 120, 137, 154, 171, 188, 205, 222, 239, 0, 17, 34,
    51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238,
    255, 16, 33, 50, 67, 84, 101, 118, 135, 152, 169, 186,
    203, 220, 237, 254, 15, 32, 49, 66, 83, 100, 117, 134,
    151, 168, 185, 202, 219, 236, 253, 14, 31, 48, 65, 82,
    99, 116, 133, 150, 167, 184, 201, 218, 235, 252, 13, 30,
    47, 64, 81, 98, 115, 132, 149, 166, 183, 200, 217, 234,
    251, 12, 29, 46, 63, 80, 97, 114, 131, 148, 165, 182,
    199, 216, 233, 250, 11, 28, 45, 62, 79, 96, 113, 130,
    147, 164, 181, 198, 215, 232, 249, 10, 27, 44, 61, 78,
    95, 112, 129, 146, 163, 180, 197, 214, 231, 248, 9, 26,
    43, 60, 77, 94, 111, 128, 145, 162, 179, 196, 213, 230,
    247, 8, 25, 42, 59, 76, 93, 110, 127, 144, 161, 178,
    195, 212, 229, 246, 7, 24, 41, 58, 75, 92, 109, 126,
    143, 160, 177, 194, 211, 228, 245, 6, 23, 40, 57, 74,
    91, 108, 125, 142, 159, 176, 193, 210, 227, 244, 5, 22,
    39, 56, 73, 90, 107, 124, 141, 158, 175, 192, 209, 226,
    243, 4, 21, 38, 55, 72, 89, 106, 123, 140, 157, 174,
    191, 208, 225, 242, 3, 20, 37, 54, 71, 88, 105, 122,
    139, 156, 173, 190, 207, 224, 241, 2, 19, 36, 53, 70,
    87, 104, 121, 138, 155, 172, 189, 206, 223, 240, 1, 18,
    35, 52, 69, 86, 103, 120, 137, 154, 171, 188, 205, 222,
    239, 0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170,
    187, 204, 221, 238, 255, 16, 33, 50, 67, 84, 101, 118,
    135, 152, 169, 186, 203, 220, 237, 254, 15, 32, 49, 66,
    83, 100, 117, 134, 151, 168, 185, 202, 219, 236, 253, 14,
    31, 48, 65, 82, 99, 116, 133, 150, 167, 184, 201, 218,
    235, 252, 13, 30, 47, 64, 81, 98, 115, 132, 149, 166,
    183, 200, 217, 234, 251, 12, 29, 46, 63, 80, 97, 114,
    131, 148, 165, 182, 199, 216, 233, 250, 11, 28, 45, 62,
    79, 96, 113, 130, 147, 164, 181, 198, 215, 232, 249, 10,
    27, 44, 61, 78, 95, 112, 129, 146, 163, 180, 197, 214,
    231, 248, 9, 26, 43, 60, 77, 94, 111, 128, 145, 162,
    179, 196, 213, 230, 247, 8, 25, 42, 59, 76, 93, 110,
    127, 144, 161, 178, 195, 212, 229, 246, 7, 24, 41, 58,
    75, 92, 109, 126, 143, 160, 177, 194, 211, 228, 245, 6,
    23, 40, 57, 74, 91, 108, 125, 142, 159, 176, 193, 210,
    227, 244, 5, 22, 39, 56, 73, 90, 107, 124, 141, 158,
    175, 192, 209, 226, 243, 4, 21, 38, 55, 72, 89, 106,
    123, 140, 157, 174, 191, 208, 225, 242,
};

static const uint8_t recon[512] = {
    9, 32, 55, 78, 101, 124, 147, 170, 193, 216, 239, 6,
    29, 52, 75, 98, 121, 144, 167, 190, 213, 236, 3, 26,
    49, 72, 95, 118, 141, 164, 187, 210, 233, 0, 23, 46,
    69, 92, 115, 138, 161, 184, 207, 230, 253, 20, 43, 66,
    89, 112, 135, 158, 181, 204, 227, 250, 17, 40, 63, 86,
    109, 132, 155, 178, 201, 224, 247, 14, 37, 60, 83, 106,
    129, 152, 175, 198, 221, 244, 11, 34, 57, 80, 103, 126,
    149, 172, 195, 218, 241, 8, 31, 54, 77, 100, 123, 146,
    169, 192, 215, 238, 5, 28, 51, 74, 97, 120, 143, 166,
    189, 212, 235, 2, 25, 48, 71, 94, 117, 140, 163, 186,
    209, 232, 255, 22, 45, 68, 91, 114, 137, 160, 183, 206,
    229, 252, 19, 42, 65, 88, 111, 134, 157, 180, 203, 226,
    249, 16, 39, 62, 85, 108, 131, 154, 177, 200, 223, 246,
    13, 36, 59, 82, 105, 128, 151, 174, 197, 220, 243, 10,
    33, 56, 79, 102, 125, 148, 171, 194, 217, 240, 7, 30,
    53, 76, 99, 122, 145, 168, 191, 214, 237, 4, 27, 50,
    73, 96, 119, 142, 165, 188, 211, 234, 1, 24, 47, 70,
    93, 116, 139, 162, 185, 208, 231, 254, 21, 44, 67, 90,
    113, 136, 159, 182, 205, 228, 251, 18, 41, 64, 87, 110,
    133, 156, 179, 202, 225, 248, 15, 38, 61, 84, 107, 130,
    153, 176, 199, 222, 245, 12, 35, 58, 81, 104, 127, 150,
    173, 196, 219, 242, 9, 32, 55, 78, 101, 124, 147, 170,
    193, 216, 239, 6, 29, 52, 75, 98, 121, 144, 167, 190,
    213, 236, 3, 26, 49, 72, 95, 118, 141, 164, 187, 210,
    233, 0, 23, 46, 69, 92, 115, 138, 161, 184, 207, 230,
    253, 20, 43, 66, 89, 112, 135, 158, 181, 204, 227, 250,
    17, 40, 63, 86, 109, 132, 155, 178, 201, 224, 247, 14,
    37, 60, 83, 106, 129, 152, 175, 198, 221, 244, 11, 34,
    57, 80, 103, 126, 149, 172, 195, 218, 241, 8, 31, 54,
    77, 100, 123, 146, 169, 192, 215, 238, 5, 28, 51, 74,
    97, 120, 143, 166, 189, 212, 235, 2, 25, 48, 71, 94,
    117, 140, 163, 186, 209, 232, 255, 22, 45, 68, 91, 114,
    137, 160, 183, 206, 229, 252, 19, 42, 65, 88, 111, 134,
    157, 180, 203, 226, 249, 16, 39, 62, 85, 108, 131, 154,
    177, 200, 223, 246, 13, 36, 59, 82, 105, 128, 151, 174,
    197, 220, 243, 10, 33, 56, 79, 102, 125, 148, 171, 194,
    217, 240, 7, 30, 53, 76, 99, 122, 145, 168, 191, 214,
    237, 4, 27, 50, 73, 96, 119, 142, 165, 188, 211, 234,
    1, 24, 47, 70, 93, 116, 139, 162, 185, 208, 231, 254,
    21, 44, 67, 90, 113, 136, 159, 182, 205, 228, 251, 18,
    41, 64, 87, 110, 133, 156, 179, 202, 225, 248, 15, 38,
    61, 84, 107, 130, 153, 176, 199, 222, 245, 12, 35, 58,
    81, 104, 127, 150, 173, 196, 219, 242,
};

static const int16_t in_c64b[64] = {
    -71, -58, -45, -32, -19, -6, 7, 20, 33, 46, 59, 72,
    85, 98, 111, 124, -66, -53, -40, -27, -14, -1, 12, 25,
    38, 51, 64, 77, 90, 103, 116, 129, -61, -48, -35, -22,
    -9, 4, 17, 30, 43, 56, 69, 82, 95, 108, 121, 134,
    -56, -43, -30, -17, -4, 9, 22, 35, 48, 61, 74, 87,
    100, 113, 126, 139,
};
static const int16_t exp_sub8_8[64] = {
    -256, 1008, 1408, -656, 0, -176, 384, 48, -4, 2816, -1184, -3660,
    1088, -836, -352, 296, 384, 64, 512, 688, -1664, -240, -64, -528,
    -60, -260, 192, 1200, -544, 312, -1744, 1040, 0, 256, -128, 32,
    -512, 1216, -384, -480, 372, -428, -320, 108, 0, -180, 640, 556,
    -128, -608, 576, -416, 128, 240, 128, 256, 628, -1064, 112, 20,
    544, 352, -224, -368,
};

static void check_i(const char *name, const int16_t *got, const int16_t *exp, int n)
{
    for (int i = 0; i < n; i++)
        if (got[i] != exp[i]) {
            printf("FAIL %s at %d: got %d exp %d\n", name, i, got[i], exp[i]);
            ++fails; return;
        }
}
static void check_u(const char *name, const uint8_t *got, const uint8_t *exp, int n)
{
    for (int i = 0; i < n; i++)
        if (got[i] != exp[i]) {
            printf("FAIL %s at %d: got %u exp %u\n", name, i, got[i], exp[i]);
            ++fails; return;
        }
}

/* ---- I16 luma DC golden (ultrafast MB0, build/parity/fixture.yuv frame 0) ----
 *
 * Source-aligned with x264's encoder/macroblock.c:168-172 (DC extraction),
 * :215 (dct4x4dc, common/dct.c:47-76), :219 (quant_4x4_dc with
 * quant4_mf>>1 / quant4_bias<<1, common/quant.c:50-57/:88-94), :224
 * (zigzag_scan_4x4, common/macroblock.h:181-184), and the instrumented
 * encoder/cabac.c:989 L1BLK dump for this fixture:
 *   L1BLK 0 48 88: 9 -17 -52 -23 -23 -11 -9 11 23 -14 -23 -11 -11 11 11 -11
 * Fixture source samples (tools/parity_harness.py:69-71) are
 * fenc(x,y) = (x*7 + y*13) & 255; x264's analysis picks I_PRED_16x16_DC_128
 * (mode 6) for MB0, so the prediction plane is the constant 128.
 * SHADER: A/B — source-formula golden over the owned quant/DCT port.
 */
static void phx_test_i16_luma_dc_golden(void)
{
    static const int idx_1d[16] = {
        0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15
    }; /* common/macroblock.h:217-220 block_idx_xy_1d */
    static const int zig4[16] = {
        0,4,1,2,5,8,12,9,6,3,7,10,13,14,11,15
    }; /* common/macroblock.h:181-184 x264_zigzag_scan4[0] */
    static const int16_t golden_hadamard[16] = {
        768, -4608, -2048, -1280, -1536, -2048, 2048, -2048,
        -1024, 1024, -1024, 1024, -768, -1024, 1024, -1024
    };
    static const int16_t golden_dc[16] = {
        9, -17, -52, -23, -23, -11, -9, 11, 23, -14, -23, -11, -11, 11, 11, -11
    };
    uint8_t fenc[16 * 16], pred[16 * 16];
    phx_dctcoef dct4x4[16][16], dc[16], scan[16];
    phx_quant_tables_t t;
    int x, y;

    phx_quant_tables_init(&t);
    /* set.c:164-180 at QP 25, flat CQM_4IY = 0. */
    CHECK(t.quant4_mf[0][25][0] == 1490, "I16 DC q4mf QP25");
    CHECK(t.quant4_bias[0][25][0] == 14, "I16 DC q4bias QP25");

    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++) {
            fenc[y * 16 + x] = (uint8_t)((x * 7 + y * 13) & 255);
            pred[y * 16 + x] = 128;             /* I_PRED_16x16_DC_128 */
        }
    phx_sub16x16_dct(dct4x4, fenc, 16, pred, 16);   /* macroblock.c:162 */
    for (int i = 0; i < 16; i++) {                  /* macroblock.c:168-172 */
        dc[idx_1d[i]] = dct4x4[i][0];
    }
    phx_dct4x4dc(dc);                               /* macroblock.c:215 */
    check_i("I16 DC Hadamard", dc, golden_hadamard, 16);
    phx_quant_4x4_dc(dc, t.quant4_mf[0][25][0] >> 1,
                     t.quant4_bias[0][25][0] << 1); /* macroblock.c:219 */
    for (int k = 0; k < 16; k++)                    /* macroblock.c:224 */
        scan[k] = dc[zig4[k]];
    check_i("I16 luma DC golden (x264 L1BLK 0 48)", scan, golden_dc, 16);
    printf("I16 luma DC golden: 16/16 coefficients, last index 15\n");
}

/* x264's quant.c:878-882 assigns coeff_last[DCT_LUMA_DC] = coeff_last16
 * (the I16 luma DC block carries 16 zigzag coefficients, tables.c:1792).
 * This golden catches the entropy-side mapping that determines when the
 * last_significant_coeff_flag bin is emitted (cabac.c:675-717). */
static void phx_test_i16_luma_dc_coeff_last(void)
{
    static const int16_t golden_dc[16] = {
        9, -17, -52, -23, -23, -11, -9, 11, 23, -14, -23, -11, -11, 11, 11, -11
    };
    phx_cabac_mb_t mb;
    phx_cabac_mb_init(&mb, NULL);
    CHECK(mb.coeff_last[PHX_DCT_LUMA_DC](golden_dc) == 15,
          "x264 quant.c:878 coeff_last[DCT_LUMA_DC] == coeff_last16");
    printf("coeff_last[DCT_LUMA_DC]: %d (x264 golden 15)\n",
           mb.coeff_last[PHX_DCT_LUMA_DC](golden_dc));
}

int main(void)
{
    phx_quant_tables_t t;
    phx_quant_tables_init(&t);
    int16_t c16[16], c64[64];
    uint8_t recon_c[32 * 16];

    /* set.c:130-205 table construction */
    CHECK(t.quant4_mf[0][0][0] == exp_table_q4mf[0], "q4mf[0][0][0]");
    CHECK(t.quant4_mf[0][12][0] == exp_table_q4mf[1], "q4mf[0][12][0]");
    CHECK(t.quant4_mf[0][28][0] == exp_table_q4mf[2], "q4mf[0][28][0]");
    CHECK(t.quant4_mf[2][28][0] == exp_table_q4mf[3], "q4mf[2][28][0]");
    CHECK(t.quant4_mf[0][28][15] == exp_table_q4mf[4], "q4mf[0][28][15]");
    CHECK(t.quant4_bias[0][0][0] == exp_table_q4bias[0], "q4bias[0][0][0]");
    CHECK(t.quant4_bias[0][12][0] == exp_table_q4bias[1], "q4bias[0][12][0]");
    CHECK(t.quant4_bias[0][28][0] == exp_table_q4bias[2], "q4bias[0][28][0]");
    CHECK(t.quant4_bias[2][28][0] == exp_table_q4bias[3], "q4bias[2][28][0]");
    CHECK(t.quant4_bias[0][28][15] == exp_table_q4bias[4], "q4bias[0][28][15]");
    CHECK(t.quant8_mf[0][0][0] == exp_table_q8mf[0], "q8mf[0][0][0]");
    CHECK(t.quant8_mf[0][12][0] == exp_table_q8mf[1], "q8mf[0][12][0]");
    CHECK(t.quant8_mf[0][28][0] == exp_table_q8mf[2], "q8mf[0][28][0]");
    CHECK(t.quant8_mf[0][28][63] == exp_table_q8mf[3], "q8mf[0][28][63]");
    CHECK(t.quant8_bias[0][0][0] == exp_table_q8bias[0], "q8bias[0][0][0]");
    CHECK(t.quant8_bias[0][28][0] == exp_table_q8bias[2], "q8bias[0][28][0]");
    CHECK(t.dequant4_mf[0][0][0] == exp_table_dq4[0], "dq4[0][0][0]");
    CHECK(t.dequant4_mf[0][3][1] == exp_table_dq4[1], "dq4[0][3][1]");
    CHECK(t.dequant4_mf[2][4][15] == exp_table_dq4[2], "dq4[2][4][15]");
    CHECK(t.dequant8_mf[0][0][0] == exp_table_dq8[0], "dq8[0][0][0]");
    CHECK(t.dequant8_mf[0][5][7] == exp_table_dq8[1], "dq8[0][5][7]");
    CHECK(t.dequant8_mf[0][3][63] == exp_table_dq8[2], "dq8[0][3][63]");

    /* set.c:180/200 — trellis bias0 golden from x264's own tables. */
    for (int k = 0; k < 8; k++) {
        const int qp = k * 7;
        CHECK(t.quant4_bias0[0][qp][0] == exp_q4bias0[k], "q4bias0 golden");
        CHECK(t.quant8_bias0[0][qp][0] == exp_q8bias0[k], "q8bias0 golden");
    }
    CHECK(t.quant4_bias0[0][25][0] == 21, "q4bias0 qp25");

    /* dct.c forward transforms */
    phx_sub4x4_dct(c16, p1, 16, p2, 32); check_i("sub4x4_dct", c16, exp_sub4, 16);
    { int16_t b[4][16]; phx_sub8x8_dct(b, p1, 16, p2, 32);
      check_i("sub8x8_dct", &b[0][0], exp_sub8, 64); }
    { int16_t b[16][16]; phx_sub16x16_dct(b, p1, 16, p2, 32);
      check_i("sub16x16_dct", &b[0][0], exp_sub16, 256); }
    CHECK(phx_sub4x4_dct_dc(p1, 16, p2, 32) == exp_dc4[0], "sub4x4_dct_dc");
    { int16_t b[4]; phx_sub8x8_dct_dc(b, p1, 16, p2, 32); check_i("sub8x8_dct_dc", b, exp_dc8, 4); }
    { int16_t b[8]; phx_sub8x16_dct_dc(b, p1, 16, p2, 32); check_i("sub8x16_dct_dc", b, exp_dc8x16, 8); }
    memcpy(c16, in_c16, sizeof(c16)); phx_dct4x4dc(c16); check_i("dct4x4dc", c16, exp_dct4dc, 16);
    memcpy(c16, in_c16, sizeof(c16)); phx_idct4x4dc(c16); check_i("idct4x4dc", c16, exp_idct4dc, 16);
    { int16_t in[8][16]; int16_t out[8]; memset(in, 0, sizeof(in));
      for (int i = 0; i < 8; i++) in[i][0] = exp_dc4x4in[i];
      phx_dct2x4dc(out, in); check_i("dct2x4dc", out, exp_dct2x4, 8); }
    { int16_t b[64]; phx_sub8x8_dct8(b, p1, 16, p2, 32);
      check_i("sub8x8_dct8", b, exp_sub8_8, 64); }

    /* dct.c inverse transforms (add to reconstruction) */
    memcpy(recon_c, recon, sizeof(recon_c));
    phx_add4x4_idct(recon_c + 1 + 32, 32, in_c16);
    { uint8_t block[16]; for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        block[y * 4 + x] = recon_c[1 + x + (1 + y) * 32];
      check_u("add4x4_idct", block, exp_add4, 16); }
    memcpy(recon_c, recon, sizeof(recon_c));
    { int16_t b[4][16]; memcpy(b, in_c64b, sizeof(b));
      phx_add8x8_idct(recon_c + 1 + 32, 32, b); }
    { uint8_t block[64]; for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        block[y * 8 + x] = recon_c[1 + x + (1 + y) * 32];
      check_u("add8x8_idct", block, exp_add8, 64); }
    memcpy(recon_c, recon, sizeof(recon_c)); memcpy(c64, in_c64, sizeof(c64));
    phx_add8x8_idct8(recon_c + 1 + 32, 32, c64);
    { uint8_t block[64]; for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++)
        block[y * 8 + x] = recon_c[1 + x + (1 + y) * 32];
      check_u("add8x8_idct8", block, exp_add8_8, 64); }
    check_i("add8x8_idct8 coeffs", c64, exp_add8_8coef, 64);
    { uint32_t sum[16] = {0}; memcpy(c16, in_c16, sizeof(c16));
      phx_denoise_dct(c16, sum, in_off16, 16); check_i("denoise_dct", c16, exp_denoise, 16); }

    /* quant.c deadzone quantizers */
    memcpy(c16, in_qc16, sizeof(c16));
    CHECK(phx_quant_4x4(c16, t.quant4_mf[0][28], t.quant4_bias[0][28]) == exp_quant4nz[0], "quant_4x4 nz");
    check_i("quant_4x4", c16, exp_quant4, 16);
    memcpy(c64, in_qc64, sizeof(c64));
    CHECK(phx_quant_8x8(c64, t.quant8_mf[0][12], t.quant8_bias[0][12]) == exp_quant8nz[0], "quant_8x8 nz");
    check_i("quant_8x8", c64, exp_quant8, 64);
    { int16_t b[4][16]; for (int j = 0; j < 4; j++) memcpy(b[j], in_qc16, sizeof(c16));
      for (int i = 0; i < 16; i++) { b[1][i] += 3; b[2][i] = -b[2][i]; b[3][i] /= 2; }
      CHECK(phx_quant_4x4x4(b, t.quant4_mf[1][20], t.quant4_bias[1][20]) == exp_quant4x4nz[0], "quant_4x4x4 nz");
      check_i("quant_4x4x4", &b[0][0], exp_quant4x4, 64); }
    memcpy(c16, in_qc16, sizeof(c16));
    CHECK(phx_quant_4x4_dc(c16, t.quant4_mf[2][28][0], t.quant4_bias[2][28][0]) == exp_quant4dcnz[0], "quant_4x4_dc nz");
    check_i("quant_4x4_dc", c16, exp_quant4dc, 16);
    memcpy(c16, in_qc16, sizeof(c16));
    CHECK(phx_quant_2x2_dc(c16, t.quant4_mf[2][28][0], t.quant4_bias[2][28][0]) == exp_quant2dcnz[0], "quant_2x2_dc nz");
    check_i("quant_2x2_dc", c16, exp_quant2dc, 4);

    /* R1 MB0 block0 golden (qp=25, CQM_4IY): plain deadzone quant matches
     * x264's analysis-stage quant_4x4 (-35); the -36 golden of the final
     * encode is x264's trellis QNS with bias0 (set.c:180, analyse.c:3724).
     * No constant is fitted: both come from QUANT_ONE (quant.c:50-57). */
    memcpy(c16, in_mb0_b0, sizeof(c16));
    CHECK(phx_quant_4x4(c16, t.quant4_mf[0][25], t.quant4_bias[0][25]) == 1,
          "mb0 b0 plain nz");
    check_i("mb0 b0 plain (x264 quant_4x4)", c16, exp_mb0_b0_plain, 16);
    memcpy(c16, in_mb0_b0, sizeof(c16));
    CHECK(phx_quant_4x4(c16, t.quant4_mf[0][25], t.quant4_bias0[0][25]) == 1,
          "mb0 b0 bias0 nz");
    check_i("mb0 b0 trellis bias0 golden (-36)", c16, exp_mb0_b0_trellis, 16);
    int nz_mb0 = 0;
    CHECK(phx_quant_one(-1568, t.quant4_mf[0][25][0],
                        t.quant4_bias0[0][25][0], &nz_mb0) == -36,
          "mb0 b0 QUANT_ONE bias0 == -36");
    CHECK(nz_mb0 == -36, "mb0 b0 QUANT_ONE nz");

    /* quant.c dequantizers and chroma DC kernels */
    memcpy(c16, in_dq16, sizeof(c16)); phx_dequant_4x4(c16, t.dequant4_mf[1], 28);
    check_i("dequant_4x4", c16, exp_dequant4, 16);
    memcpy(c64, in_dq64, sizeof(c64)); phx_dequant_8x8(c64, t.dequant8_mf[0], 22);
    check_i("dequant_8x8", c64, exp_dequant8, 64);
    memcpy(c16, in_dq16, sizeof(c16)); phx_dequant_4x4_dc(c16, t.dequant4_mf[2], 22);
    check_i("dequant_4x4_dc", c16, exp_dequant4dc, 16);
    { int16_t d[8][16]; memset(d, 0, sizeof(d)); memcpy(c16, in_dc8, sizeof(c16));
      phx_idct_dequant_2x4_dc(c16, d, t.dequant4_mf[2], 28);
      { int16_t out[8]; for (int i = 0; i < 8; i++) out[i] = d[i][0];
        check_i("idct_dequant_2x4_dc", out, exp_dq2x4dc, 8); } }
    memcpy(c16, in_dc8, sizeof(c16)); phx_idct_dequant_2x4_dconly(c16, t.dequant4_mf[2], 28);
    check_i("idct_dequant_2x4_dconly", c16, exp_dq2x4dc, 8);
    { int16_t out[8]; phx_optimize_chroma_idct_dequant_2x2(out, in_dc8, exp_dmf[0]);
      check_i("optimize_chroma_idct_dequant_2x2", out, exp_opt2x2, 4); }
    { int16_t out[8]; phx_optimize_chroma_idct_dequant_2x4(out, in_dc8, exp_dmf[0]);
      check_i("optimize_chroma_idct_dequant_2x4", out, exp_opt2x4, 8); }
    CHECK(phx_optimize_chroma_round(exp_opt2x2, in_dc8, exp_dmf[0], 0) == exp_optround[0], "optimize_chroma_round 2x2");
    CHECK(phx_optimize_chroma_round(exp_opt2x4, in_dc8, exp_dmf[0], 1) == exp_optround[1], "optimize_chroma_round 2x4");
    memcpy(c16, in_dc8, sizeof(c16)); CHECK(phx_optimize_chroma_dc_internal(c16, exp_dmf[0], 0) == exp_optdc2[0], "optimize_chroma_dc_internal 2x2 nz");
    check_i("optimize_chroma_dc_internal 2x2", c16, exp_optdc2d, 4);
    memcpy(c16, in_dc8, sizeof(c16)); CHECK(phx_optimize_chroma_dc_internal(c16, exp_dmf[0], 1) == exp_optdc4[0], "optimize_chroma_dc_internal 2x4 nz");
    check_i("optimize_chroma_dc_internal 2x4", c16, exp_optdc4d, 8);
    memcpy(c16, in_dc8, sizeof(c16)); CHECK(phx_optimize_chroma_2x2_dc(c16, exp_dmf[0]) == exp_optdc2[0], "optimize_chroma_2x2_dc nz");
    check_i("optimize_chroma_2x2_dc", c16, exp_optdc2d, 4);
    memcpy(c16, in_dc8, sizeof(c16)); CHECK(phx_optimize_chroma_2x4_dc(c16, exp_dmf[0]) == exp_optdc4[0], "optimize_chroma_2x4_dc nz");
    check_i("optimize_chroma_2x4_dc", c16, exp_optdc4d, 8);

    phx_test_i16_luma_dc_golden();
    phx_test_i16_luma_dc_coeff_last();

    printf("%s: %s and %s, %d failures\n", fails ? "FAILED" : "OK",
           "DCT/Hadamard/IDCT", "quant/dequant", fails);
    return fails ? 1 : 0;
}
