/* Phoenix macroblock neighbour caches — mirrors x264's x264_scan8 layout and
 * the progressive-path cache load/save.
 *
 * Source: common/base.h:180 (x264_scan8),
 *         common/macroblock.c:662 (left_indices), :673 (neighbours),
 *         :855 (macroblock_cache_load), :1262-1306 (skip/field flags),
 *         :1354/:1359 (progressive/interlaced entry points),
 *         :1638 (macroblock_backup_intra), :1680 (macroblock_cache_save),
 *         common/common.h:646-647 (topright_mv/topright_ref),
 *         encoder/macroblock.h:112 (STORE_8x8_NNZ).
 *
 * SHADER: B — per-MB state with left/top neighbours; load/save is ordered,
 * but the cache arrays are the explicit boundary for shader work.
 */
#ifndef PHX_MBCACHE_H
#define PHX_MBCACHE_H

#include <stdint.h>

#define PHX_MB_LEFT     0x01
#define PHX_MB_TOP      0x02
#define PHX_MB_TOPRIGHT 0x04
#define PHX_MB_TOPLEFT  0x08

#define PHX_I_4x4    0
#define PHX_I_8x8    1
#define PHX_I_16x16  2
#define PHX_I_PCM    3
#define PHX_P_L0     4
#define PHX_P_8x8    5
#define PHX_P_SKIP   6
#define PHX_I_PRED_4x4_DC 2

extern const uint8_t phx_scan8[51];

/* x264_left_table_t (common/macroblock.h). */
/* Field order matches reference/x264/common/common.h x264_left_table_t. */
typedef struct {
    int intra[4], nnz[4], nnz_chroma[4], mv[4], ref[4];
} phx_left_table_t;
extern const phx_left_table_t phx_left_indices[4];

/* Per-frame MB state written by cache_save and read by cache_load. */
typedef struct {
    int mb_width, mb_height, mb_stride, b8_stride, b4_stride, mb_count;
    uint8_t *type;
    int8_t  (*intra4x4_pred_mode)[8];
    uint8_t (*non_zero_count)[48];
    /* common/macroblock.c:1835 — fixed chroma predictor per MB, read by
     * cabac_intra_chroma_pred_mode() (cabac.c:104-107) through the neighbours. */
    uint8_t *chroma_pred_mode;
    int16_t *cbp;
    uint8_t *mb_transform_size;
    int8_t  *ref[2];                /* [mb_count*4] 8x8 blocks */
    int16_t (*mv[2])[2];            /* [mb_count*16] 4x4 blocks */
    int16_t (*mvd[2])[8][2];        /* [mb_count] CABAC MVDs */
    uint8_t *slice_table;
    uint8_t *field;                 /* MBAFF field flags, frame.c/macroblock.c */
    /* common.h:734 / macroblock.c:357-362 / :1645-1675: x264 keeps five sets
     * of row pointers (not fixed 16-byte arrays), each row sized
     * mb_width*16+32 with a +16 emulated-edge offset. */
    uint8_t *intra_border_backup[5][3];
    uint8_t *intra_border_backup_base[5][3]; /* owned allocations */
    int threadslice_start;          /* x264 h->i_threadslice_start */
    int first_mb;
    int b_constrained_intra, b_cabac;
    int b_interlaced;               /* x264 MB_INTERLACED (macroblock.c:675) */
    int b_transform_8x8_mode;       /* x264 pps->b_transform_8x8_mode (:987) */
    int slice_type;                 /* x264 enum: 0=P, 1=B, 2=I */
} phx_mb_state_t;

/* Neighbour cache for the current MB (x264_t.mb.cache, simplified). */
typedef struct {
    int i_mb_x, i_mb_y, i_mb_xy, i_b8_xy, i_b4_xy;
    int top, top_y, left[2], topleft, topright;
    int type_top, type_left[2], type_topleft, type_topright;
    int i_neighbour, i_neighbour_intra, i_neighbour_frame;
    int i_mb_top_mbpair_xy, i_mb_topleft_y, i_mb_topright_y, topleft_partition;
    int left_b8[2], left_b4[2];
    const phx_left_table_t *left_index;
    int field_decoding_flag, b_allow_skip, i_neighbour_skip;
    int i_cbp_top, i_cbp_left, i_neighbour_transform_size;
    int i_cbp_luma, i_cbp_chroma, i_qp, b_transform_8x8;
    int i_chroma_pred_mode;         /* h->mb.i_chroma_pred_mode (raw 0..6) */
    uint8_t *p_fdec;                /* reconstructed MB, FDEC_STRIDE layout */
    /* macroblock.c:537/:546 — the two 4:2:0 chroma windows of the same MB
     * scratch.  Optional: NULL on the legacy cache-test path. */
    uint8_t *p_fdec_u, *p_fdec_v;
    int fdec_stride;
    int8_t  intra4x4_pred_mode[40]; /* X264_SCAN8_LUMA_SIZE = 5*8 */
    uint8_t non_zero_count[120];    /* X264_SCAN8_SIZE = 3*40 */
    int8_t  ref[2][40];
    int16_t mv[2][40][2];
    int16_t mvd[2][40][2];
    /* common/common.h:646-647 / macroblock.c:1150-1171,1226-1228. */
    int8_t  topright_ref[2][3];
    int16_t topright_mv[2][3][2];
} phx_mb_cache_t;

/* Allocate/zero the per-frame MB state arrays. */
phx_mb_state_t *phx_mb_state_new(int mb_width, int mb_height);
void phx_mb_state_delete(phx_mb_state_t *s);

/* macroblock.c:673 — neighbour discovery (b_mbaff=0 progressive, 1 MBAFF). */
void phx_macroblock_cache_load_neighbours(phx_mb_state_t *s, phx_mb_cache_t *c,
                                          int mb_x, int mb_y, int b_mbaff);

/* macroblock.c:855 — load the current MB's neighbour caches. */
void phx_macroblock_cache_load(phx_mb_state_t *s, phx_mb_cache_t *c,
                               int mb_x, int mb_y, int b_mbaff);

/* macroblock.c:568-610 / :1000-1024 — seed the per-MB FDEC scratch from the
 * encoder's full-picture reconstruction.  c->p_fdec must already point at an
 * MB-sized scratch with 2 rows of top padding and a 1-pixel left border
 * (Phoenix p_fdec = fdec_buf + 2*FDEC_STRIDE + 1; x264 macroblock.c:533 uses
 * fdec_buf + 2*FDEC_STRIDE, whose [-1] byte is the last byte of padding row 1).
 * The caller owns the scratch, so standalone cache tests keep their old
 * five-argument load path untouched. */
void phx_macroblock_cache_load_fdec(phx_mb_state_t *s, phx_mb_cache_t *c,
                                    int mb_x, int mb_y, int b_mbaff,
                                    const uint8_t *recon_y, int recon_y_stride,
                                    const uint8_t *recon_uv, int recon_uv_stride);

/* macroblock.c:1638 — back up the bottom rows for intra prediction. */
void phx_macroblock_backup_intra(phx_mb_state_t *s, int mb_x, int mb_y,
                                 int b_mbaff, const uint8_t *p_fdec, int stride);

/* macroblock.c:1680 — write the current MB into the per-frame state. */
void phx_macroblock_cache_save(phx_mb_state_t *s, phx_mb_cache_t *c, int b_mbaff);

/* encoder/macroblock.h:112 — store an 8x8 block's four nnz flags. */
#define PHX_STORE_8x8_NNZ(c,p,idx,nz) do { \
    (c)->non_zero_count[phx_scan8[(p)*16+(idx)*4]+0] = (uint8_t)(nz); \
    (c)->non_zero_count[phx_scan8[(p)*16+(idx)*4]+8] = (uint8_t)(nz); \
} while (0)

#endif
