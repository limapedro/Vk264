/* Expectations from reference/x264/common/base.h:180, macroblock.c:662/855/1680.
 * scan8[0]=4+1*8=12, scan8[1]=13, scan8[2]=20, scan8[16]=52.
 * Left table progressive (index 3): intra {4,5,6,3}, nnz {3,7,11,15}.
 */
#include "phx_mbcache.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond,msg) do { if (!(cond)) { \
    printf("FAIL %s (line %d)\n",msg,__LINE__); ++fails; } } while (0)

int main(void)
{
    CHECK(phx_scan8[0] == 12 && phx_scan8[1] == 13 &&
          phx_scan8[2] == 20 && phx_scan8[16] == 52, "scan8 layout");
    CHECK(phx_scan8[48] == 0 && phx_scan8[49] == 40 && phx_scan8[50] == 80,
          "scan8 chroma DC slots");
    const phx_left_table_t *lt = &phx_left_indices[3];
    CHECK(lt->intra[0] == 4 && lt->intra[3] == 3 && lt->nnz[2] == 11 &&
          lt->nnz_chroma[3] == 37, "left table progressive");

    phx_mb_state_t *s = phx_mb_state_new(4, 2);
    CHECK(s != NULL && s->mb_count == 8, "state allocation");
    if (!s) return 1;
    CHECK(s->b8_stride == 8 && s->b4_stride == 16, "block strides");

    /* MB (1,0) is the top neighbour of MB (1,1); MB (0,1) is its left. */
    int top = 1, left = 4;
    for (int k = 0; k < 8; k++) s->intra4x4_pred_mode[top][k] = (int8_t)(10+k);
    for (int k = 0; k < 48; k++) s->non_zero_count[top][k] = (uint8_t)(k+1);
    s->cbp[top] = 123; s->mb_transform_size[top] = 1;
    for (int k = 0; k < 8; k++) s->intra4x4_pred_mode[left][k] = (int8_t)(20+k);
    for (int k = 0; k < 48; k++) s->non_zero_count[left][k] = (uint8_t)(k+40);
    s->cbp[left] = 45;
    s->type[top] = PHX_I_16x16; s->type[left] = PHX_I_16x16;

    phx_mb_cache_t c;
    memset(&c, 0, sizeof(c));
    phx_macroblock_cache_load(s, &c, 1, 1, 0);
    CHECK(c.i_mb_xy == 5 && c.top == top && c.left[0] == left, "neighbour indices");
    CHECK((c.i_neighbour & (PHX_MB_TOP|PHX_MB_LEFT|PHX_MB_TOPLEFT|PHX_MB_TOPRIGHT))
          == (PHX_MB_TOP|PHX_MB_LEFT|PHX_MB_TOPLEFT|PHX_MB_TOPRIGHT), "neighbour flags");
    CHECK(c.i_cbp_top == 123 && c.i_cbp_left == 45, "cbp cache");
    CHECK(c.intra4x4_pred_mode[phx_scan8[0]-8] == 10, "top intra4x4");
    CHECK(c.intra4x4_pred_mode[phx_scan8[0]-1] == 20+lt->intra[0], "left intra4x4");
    CHECK(c.non_zero_count[phx_scan8[0]-8] == 13, "top nnz (nnz[top][12])");
    CHECK(c.non_zero_count[phx_scan8[0]-1] == 40+lt->nnz[0], "left nnz");
    CHECK(c.i_neighbour_transform_size == 1, "transform-size neighbour flag");

    /* Border defaults at MB (0,0). */
    phx_mb_cache_t b;
    memset(&b, 0, sizeof(b));
    phx_macroblock_cache_load(s, &b, 0, 0, 0);
    CHECK(b.i_neighbour == 0, "no neighbours at (0,0)");
    CHECK(b.i_cbp_top == -1 && b.i_cbp_left == -1, "cbp defaults");
    CHECK(b.intra4x4_pred_mode[phx_scan8[0]-8] == -1, "intra default");
    CHECK(b.non_zero_count[phx_scan8[0]-8] == 0x80, "nnz default");

    /* STORE_8x8_NNZ then save; MB types/MVs/CBP live in the caller's state. */
    s->type[5] = PHX_P_L0;
    c.i_cbp_luma = 3; c.i_cbp_chroma = 2; c.b_transform_8x8 = 1;
    for (int k = 0; k < 16; k++) {
        c.mv[0][phx_scan8[k]][0] = (int16_t)(k+1);
        c.mv[0][phx_scan8[k]][1] = (int16_t)(-k-1);
    }
    PHX_STORE_8x8_NNZ(&c, 0, 0, 7);
    phx_macroblock_cache_save(s, &c, 0);
    CHECK(s->non_zero_count[5][0] == 7 && s->non_zero_count[5][4] == 7,
          "STORE_8x8_NNZ saved");
    CHECK(s->mb_transform_size[5] == 1, "transform size saved");
    CHECK(s->cbp[5] == ((2<<4)|3|0x1700), "cbp saved");
    CHECK(s->mv[0][c.i_b4_xy+0][0] == 1 && s->mv[0][c.i_b4_xy+15][0] == 16,
          "mv saved");

    /* Loading MB (2,1): left neighbour is the saved MB (1,1). */
    /* The left luma block 0 reads nnz[5][lt->nnz[0]] = nnz[5][3]. */
    s->non_zero_count[5][3] = 7;
    phx_mb_cache_t c2;
    memset(&c2, 0, sizeof(c2));
    phx_macroblock_cache_load(s, &c2, 2, 1, 0);
    CHECK(c2.non_zero_count[phx_scan8[0]-1] == 7, "left nnz after save");
    /* Its first left MV comes from iv = i_b4_xy-1 = 71, i.e. saved block 4. */
    CHECK(c2.mv[0][phx_scan8[0]-1][0] == 4, "left mv after save");

    /* MBAFF: current MB (1,1) interlaced, left field (0,0) frame.
     * macroblock.c:719-725 selects left MB xy-1-stride and left_indices[2]. */
    s->field[1] = 0; s->field[5] = 1;   /* pair (mb_x=1): frame / field */
    s->field[0] = 0; s->field[4] = 0;   /* pair (mb_x=0): frame / frame */
    memset(&c, 0, sizeof(c));
    phx_macroblock_cache_load(s, &c, 1, 1, 1);
    CHECK(c.left[0] == 0 && c.left[1] == 4, "MBAFF left MB indices");
    CHECK(c.left_index == &phx_left_indices[2], "MBAFF interlaced left table");
    CHECK(c.i_neighbour & PHX_MB_LEFT, "MBAFF left neighbour");
    CHECK(c.left_b4[0] == c.i_b4_xy - 4 - 4*s->b4_stride, "MBAFF left_b4");

    /* :1275-1277 ? skip is disabled on a field boundary after a skip MB. */
    s->type[1] = PHX_P_SKIP;
    memset(&c, 0, sizeof(c));
    phx_macroblock_cache_load(s, &c, 1, 1, 1);
    CHECK(c.b_allow_skip == 0, "MBAFF b_allow_skip");

    /* macroblock.c:1638 ? interlaced MBAFF bottom row backup goes to slot 2. */
    uint8_t fdec[16*16 + 64];
    for (int k = 0; k < (int)sizeof(fdec); k++) fdec[k] = (uint8_t)(k & 255);
    phx_macroblock_backup_intra(s, 1, 1, 1, fdec, 16);
    for (int k = 0; k < 16; k++)
        CHECK(s->intra_border_backup[2][0][16+k] == fdec[7*16+k],
              "MBAFF interlaced border backup");

    phx_mb_state_delete(s);
    printf("%s: macroblock cache, %d failures\n", fails ? "FAILED" : "OK", fails);
    return fails ? 1 : 0;
}
