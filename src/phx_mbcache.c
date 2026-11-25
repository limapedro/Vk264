/* Progressive path of x264's macroblock cache load/save, line by line.
 * Interlaced (MBAFF) paths are deliberately not ported here; see docs.
 */
#include "phx_mbcache.h"
#include <stdlib.h>
#include <string.h>

/* common/base.h:180 — 16 luma 4x4 blocks x3 planes + LUMA_DC/CHROMA_DC/U/V. */
const uint8_t phx_scan8[51] = {
    4+1*8, 5+1*8, 4+2*8, 5+2*8,
    6+1*8, 7+1*8, 6+2*8, 7+2*8,
    4+3*8, 5+3*8, 4+4*8, 5+4*8,
    6+3*8, 7+3*8, 6+4*8, 7+4*8,
    4+6*8, 5+6*8, 4+7*8, 5+7*8,
    6+6*8, 7+6*8, 6+7*8, 7+7*8,
    4+8*8, 5+8*8, 4+9*8, 5+9*8,
    6+8*8, 7+8*8, 6+9*8, 7+9*8,
    4+11*8, 5+11*8, 4+12*8, 5+12*8,
    6+11*8, 7+11*8, 6+12*8, 7+12*8,
    4+13*8, 5+13*8, 4+14*8, 5+14*8,
    6+13*8, 7+13*8, 6+14*8, 7+14*8,
    0+0*8, 0+5*8, 0+10*8
};

/* common/macroblock.c:662 — left_indices[4] (progressive = index 3). */
const phx_left_table_t phx_left_indices[4] = {
    {{4,4,5,5}, {3,3,7,7}, {16+1,16+1,32+1,32+1}, {0,0,1,1}, {0,0,0,0}},
    {{6,6,3,3}, {11,11,15,15}, {16+5,16+5,32+5,32+5}, {2,2,3,3}, {1,1,1,1}},
    {{4,6,4,6}, {3,11,3,11}, {16+1,16+1,32+1,32+1}, {0,2,0,2}, {0,1,0,1}},
    {{4,5,6,3}, {3,7,11,15}, {16+1,16+5,32+1,32+5}, {0,1,2,3}, {0,0,1,1}}
};

phx_mb_state_t *phx_mb_state_new(int mb_width, int mb_height)
{
    phx_mb_state_t *s = calloc(1, sizeof(*s));           /* frame.c:81 pattern */
    if (!s) return NULL;
    s->mb_width = mb_width; s->mb_height = mb_height;
    s->mb_stride = mb_width;                             /* common.h mb layout */
    s->b8_stride = 2*mb_width;
    s->b4_stride = 4*mb_width;
    s->mb_count = mb_width*mb_height;
    int n = s->mb_count;
    s->type = calloc(n, 1);
    s->intra4x4_pred_mode = calloc(n, sizeof(*s->intra4x4_pred_mode));
    s->non_zero_count = calloc(n, sizeof(*s->non_zero_count));
    s->cbp = calloc(n, sizeof(int16_t));
    s->mb_transform_size = calloc(n, 1);
    s->ref[0] = calloc(n*4, 1);
    s->ref[1] = calloc(n*4, 1);
    s->mv[0] = calloc(n*16, sizeof(int16_t[2]));
    s->mv[1] = calloc(n*16, sizeof(int16_t[2]));
    s->mvd[0] = calloc(n, sizeof(int16_t[8][2]));
    s->mvd[1] = calloc(n, sizeof(int16_t[8][2]));
    s->slice_table = calloc(n, 1);
    s->slice_type = 1;                                   /* P slice default */
    s->b_cabac = 1;
    return s;
}

void phx_mb_state_delete(phx_mb_state_t *s)
{
    if (!s) return;
    free(s->type); free(s->intra4x4_pred_mode); free(s->non_zero_count);
    free(s->cbp); free(s->mb_transform_size);
    free(s->ref[0]); free(s->ref[1]); free(s->mv[0]); free(s->mv[1]);
    free(s->mvd[0]); free(s->mvd[1]); free(s->slice_table);
    free(s);
}

/* common/macroblock.c:673 — macroblock_cache_load_neighbours(), progressive
 * branch (b_interlaced = 0). The slice_table check mirrors x264's
 * "not from the previous threadslice" rule. */
void phx_macroblock_cache_load_neighbours(phx_mb_state_t *s, phx_mb_cache_t *c,
                                          int mb_x, int mb_y)
{
    int top_y = mb_y - 1;                                /* macroblock.c:676 */
    int top = top_y*s->mb_stride + mb_x;                 /* :677 */

    c->i_mb_x = mb_x; c->i_mb_y = mb_y;                  /* :679-680 */
    c->i_mb_xy = mb_y*s->mb_stride + mb_x;               /* :681 */
    c->i_b8_xy = 2*(mb_y*s->b8_stride + mb_x);           /* :682 */
    c->i_b4_xy = 4*(mb_y*s->b4_stride + mb_x);           /* :683 */
    c->top = -1; c->top_y = -1;                          /* :691-692 */
    c->left[0] = c->left[1] = -1; c->topleft = -1; c->topright = -1; /* :693-695 */
    c->type_top = -1; c->type_left[0] = c->type_left[1] = -1;
    c->type_topleft = c->type_topright = -1;             /* :696-699 */
    c->i_neighbour = c->i_neighbour_intra = 0;           /* :688-689 */

    int topleft_y = top_y, topright_y = top_y;           /* :703-704 */
    int left = c->i_mb_xy - 1;                           /* :707 */

    if (mb_x > 0) {                                      /* :771 */
        c->i_neighbour |= PHX_MB_LEFT;                   /* :773 */
        c->left[0] = c->left[1] = left;                  /* :774-775 */
        c->type_left[0] = c->type_left[1] = s->type[left]; /* :776-777 */
        if (s->slice_table[left] == s->first_mb) {       /* :778 */
            c->i_neighbour |= PHX_MB_LEFT;
            int intra = s->type[left] == PHX_I_4x4 || s->type[left] == PHX_I_8x8
                     || s->type[left] == PHX_I_16x16 || s->type[left] == PHX_I_PCM;
            if (!s->b_constrained_intra || intra)
                c->i_neighbour_intra |= PHX_MB_LEFT;     /* :783-784 */
        }
    }
    if (top >= 0) {                                      /* :791 */
        c->top = top; c->top_y = top_y;                  /* :794-795 */
        c->type_top = s->type[top];                      /* :796 */
        if (s->slice_table[top] == s->first_mb) {        /* :797 */
            c->i_neighbour |= PHX_MB_TOP;                /* :799 */
            int intra = c->type_top == PHX_I_4x4 || c->type_top == PHX_I_8x8
                     || c->type_top == PHX_I_16x16 || c->type_top == PHX_I_PCM;
            if (!s->b_constrained_intra || intra)
                c->i_neighbour_intra |= PHX_MB_TOP;      /* :801-802 */
        }
    }
    if (mb_x > 0 && topleft_y >= 0) {                    /* :816 */
        c->topleft = s->mb_stride*topleft_y + mb_x - 1;  /* :819 */
        c->type_topleft = s->type[c->topleft];           /* :821 */
        if (s->slice_table[c->topleft] == s->first_mb)   /* :822 */
            c->i_neighbour |= PHX_MB_TOPLEFT;            /* :824 */
    }
    if (mb_x < s->mb_width - 1 && topright_y >= 0) {     /* :831 */
        c->topright = s->mb_stride*topright_y + mb_x + 1;/* :834 */
        c->type_topright = s->type[c->topright];         /* :836 */
        if (s->slice_table[c->topright] == s->first_mb)  /* :837 */
            c->i_neighbour |= PHX_MB_TOPRIGHT;           /* :839 */
    }
}

/* common/macroblock.c:855 — macroblock_cache_load(), progressive branch. */
void phx_macroblock_cache_load(phx_mb_state_t *s, phx_mb_cache_t *c,
                               int mb_x, int mb_y)
{
    phx_macroblock_cache_load_neighbours(s, c, mb_x, mb_y); /* :857 */
    int top = c->top, top_y = c->top_y;
    int s8x8 = s->b8_stride, s4x4 = s->b4_stride;        /* :862-863 */
    int top_8x8 = (2*top_y+1)*s8x8 + 2*mb_x;            /* :864 */
    int top_4x4 = (4*top_y+3)*s4x4 + 4*mb_x;            /* :865 */
    int lists = (1 << s->slice_type) & 3;                /* :866 */
    const phx_left_table_t *lt = &phx_left_indices[3];   /* :700 progressive */

    if (c->i_neighbour & PHX_MB_TOP) {                   /* :879 */
        c->i_cbp_top = s->cbp[top];                      /* :881 */
        memcpy(&c->intra4x4_pred_mode[phx_scan8[0]-8], s->intra4x4_pred_mode[top], 4);
                                                         /* :883 */
        memcpy(&c->non_zero_count[phx_scan8[0]-8], &s->non_zero_count[top][12], 4);
        memcpy(&c->non_zero_count[phx_scan8[16]-8],
               &s->non_zero_count[top][16-4+8], 4);      /* :887 */
        memcpy(&c->non_zero_count[phx_scan8[32]-8],
               &s->non_zero_count[top][32-4+8], 4);      /* :888 */
    } else {                                             /* :902 */
        c->i_cbp_top = -1;                               /* :904 */
        for (int i = 0; i < 4; i++)
            c->intra4x4_pred_mode[phx_scan8[0]-8+i] = -1; /* :907 */
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 4; i++)
                c->non_zero_count[phx_scan8[16*j]-8+i] = 0x80; /* :910-912 */
    }

    if (c->i_neighbour & PHX_MB_LEFT) {                  /* :915 */
        int ltop = c->left[0];                           /* :917 */
        c->i_cbp_left = s->cbp[ltop];                    /* :926 */
        c->intra4x4_pred_mode[phx_scan8[ 0]-1] = s->intra4x4_pred_mode[ltop][lt->intra[0]];
        c->intra4x4_pred_mode[phx_scan8[ 2]-1] = s->intra4x4_pred_mode[ltop][lt->intra[1]];
        c->intra4x4_pred_mode[phx_scan8[ 8]-1] = s->intra4x4_pred_mode[ltop][lt->intra[2]];
        c->intra4x4_pred_mode[phx_scan8[10]-1] = s->intra4x4_pred_mode[ltop][lt->intra[3]];
                                                         /* :929-932 */
        c->non_zero_count[phx_scan8[ 0]-1] = s->non_zero_count[ltop][lt->nnz[0]];
        c->non_zero_count[phx_scan8[ 2]-1] = s->non_zero_count[ltop][lt->nnz[1]];
        c->non_zero_count[phx_scan8[ 8]-1] = s->non_zero_count[ltop][lt->nnz[2]];
        c->non_zero_count[phx_scan8[10]-1] = s->non_zero_count[ltop][lt->nnz[3]];
        c->non_zero_count[phx_scan8[16+ 0]-1] = s->non_zero_count[ltop][lt->nnz_chroma[0]];
        c->non_zero_count[phx_scan8[16+ 2]-1] = s->non_zero_count[ltop][lt->nnz_chroma[1]];
        c->non_zero_count[phx_scan8[32+ 0]-1] = s->non_zero_count[ltop][lt->nnz_chroma[2]];
        c->non_zero_count[phx_scan8[32+ 2]-1] = s->non_zero_count[ltop][lt->nnz_chroma[3]];
                                                         /* :935-957 (4:2:0) */
    } else {                                             /* :960 */
        c->i_cbp_left = -1;                              /* :962 */
        const int pred_idx[] = {0,2,8,10};
        for (int i = 0; i < 4; i++)
            c->intra4x4_pred_mode[phx_scan8[pred_idx[i]]-1] = -1; /* :964-967 */
        const int idx[] = {0,2,8,10,16,18,32,34};
        for (unsigned i = 0; i < sizeof(idx)/sizeof(idx[0]); i++)
            c->non_zero_count[phx_scan8[idx[i]]-1] = 0x80; /* :970-977 */
    }

    /* :987-992 — 8x8 transform neighbour flag. */
    c->i_neighbour_transform_size =
        ((c->i_neighbour & PHX_MB_LEFT) && s->mb_transform_size[c->left[0]])
      + ((c->i_neighbour & PHX_MB_TOP) && s->mb_transform_size[top]);

    /* :1048-1198 — ref/mv/mvd caches, progressive path. */
    for (int l = 0; l < lists; l++) {
        int i8 = phx_scan8[0] - 1 - 8;                   /* :1054 */
        if (c->i_neighbour & PHX_MB_TOPLEFT) {           /* :1055 */
            int ir = top_8x8 - 1, iv = top_4x4 - 1;     /* :1057-1058 */
            c->ref[l][i8] = s->ref[l][ir];               /* :1066 */
            memcpy(c->mv[l][i8], s->mv[l][iv], sizeof(int16_t[2])); /* :1067 */
        } else {
            c->ref[l][i8] = -2;                          /* :1071 */
            memset(c->mv[l][i8], 0, sizeof(int16_t[2])); /* :1072 */
        }
        i8 = phx_scan8[0] - 8;                           /* :1075 */
        if (c->i_neighbour & PHX_MB_TOP) {               /* :1076 */
            c->ref[l][i8+0] = c->ref[l][i8+1] = s->ref[l][top_8x8+0];
            c->ref[l][i8+2] = c->ref[l][i8+3] = s->ref[l][top_8x8+1];
            memcpy(c->mv[l][i8], s->mv[l][top_4x4], 4*sizeof(int16_t[2]));
                                                         /* :1078-1082 */
        } else {
            memset(c->mv[l][i8], 0, 4*sizeof(int16_t[2]));
            for (int k = 0; k < 4; k++) c->ref[l][i8+k] = -2; /* :1086-1087 */
        }
        i8 = phx_scan8[0] + 3;                           /* :1090 */
        if (c->i_neighbour & PHX_MB_TOPRIGHT) {          /* :1091 */
            c->ref[l][i8] = s->ref[l][top_8x8+2];         /* :1095 */
            memcpy(c->mv[l][i8], s->mv[l][top_4x4+4], sizeof(int16_t[2]));
        } else c->ref[l][i8] = -2;                       /* :1099 */
        i8 = phx_scan8[0] - 1;                           /* :1101 */
        if (c->i_neighbour & PHX_MB_LEFT) {              /* :1102 */
            int ir = c->i_b8_xy - 1, iv = c->i_b4_xy - 1;/* :1118-1119 */
            for (int k = 0; k < 2; k++) {
                c->ref[l][i8+k*8] = c->ref[l][i8+1+k*8] = s->ref[l][ir+k*s8x8];
                memcpy(c->mv[l][i8+k*8], s->mv[l][iv+k*s4x4], 2*sizeof(int16_t[2]));
            }                                            /* :1120-1128 */
        } else {
            for (int k = 0; k < 4; k++) {
                c->ref[l][i8+k*8] = -2;
                memset(c->mv[l][i8+k*8], 0, sizeof(int16_t[2]));
            }                                            /* :1133-1137 */
        }
        if (s->b_cabac) {                                /* :1173 */
            int base = phx_scan8[0] - 8;
            if (c->i_neighbour & PHX_MB_TOP)
                memcpy(c->mvd[l][base], s->mvd[l][top][0], 4*sizeof(int16_t[2]));
            else memset(c->mvd[l][base], 0, 4*sizeof(int16_t[2]));
                                                         /* :1176-1179 */
            const int mvd_idx[] = {0,2,8,10};
            if (c->i_neighbour & PHX_MB_LEFT) {
                int lt_mb = c->left[0];
                for (int k = 0; k < 4; k++)
                    memcpy(c->mvd[l][phx_scan8[mvd_idx[k]]-1], s->mvd[l][lt_mb][lt->intra[k]],
                           sizeof(int16_t[2]));
            } else {
                for (int k = 0; k < 4; k++)
                    memset(c->mvd[l][phx_scan8[mvd_idx[k]]-1], 0, sizeof(int16_t[2]));
            }                                            /* :1181-1198 */
        }
    }
}

/* common/macroblock.c:1680 — macroblock_cache_save(), progressive branch. */
void phx_macroblock_cache_save(phx_mb_state_t *s, phx_mb_cache_t *c)
{
    const int xy = c->i_mb_xy;
    const int i_mb_4x4 = c->i_b4_xy, i_mb_8x8 = c->i_b8_xy;
    const int s8x8 = s->b8_stride, s4x4 = s->b4_stride;
    int8_t *i4x4 = s->intra4x4_pred_mode[xy];            /* :1691 */
    uint8_t *nnz = s->non_zero_count[xy];                /* :1692 */

    /* The caller must place the current MB type in state->type[xy] before
     * save; x264 uses h->mb.i_type which is not part of the neighbour cache. */
    int mb_type = s->type[xy];                          /* :1683 */
    s->slice_table[xy] = s->first_mb;                   /* :1722 */
    s->mb_transform_size[xy] = c->b_transform_8x8;      /* :1779 */

    if (mb_type == PHX_I_4x4) {                         /* :1727 */
        memcpy(&i4x4[0], &c->intra4x4_pred_mode[phx_scan8[10]], 4);
        uint32_t packed = (uint8_t)c->intra4x4_pred_mode[phx_scan8[5]]
                        | ((uint32_t)(uint8_t)c->intra4x4_pred_mode[phx_scan8[7]] << 8)
                        | ((uint32_t)(uint8_t)c->intra4x4_pred_mode[phx_scan8[13]] << 16);
                                                         /* :1730-1732 (last byte 0) */
        for (int k = 0; k < 4; k++) i4x4[4+k] = (int8_t)(packed >> (8*k));
    } else if (!s->b_constrained_intra || mb_type == PHX_I_4x4
               || mb_type == PHX_I_8x8 || mb_type == PHX_I_16x16
               || mb_type == PHX_I_PCM) {               /* :1734 */
        for (int k = 0; k < 8; k++) i4x4[k] = PHX_I_PRED_4x4_DC; /* :1735 */
    } else {
        for (int k = 0; k < 8; k++) i4x4[k] = -1;        /* :1737 */
    }

    if (mb_type == PHX_I_PCM) {                         /* :1740 */
        s->cbp[xy] = (int16_t)((c->i_cbp_chroma << 4) | c->i_cbp_luma | 0x1700);
        c->b_transform_8x8 = 0;
        for (int k = 0; k < 48; k++)
            c->non_zero_count[phx_scan8[k]] = s->b_cabac ? 1 : 16; /* :1749 */
    }

    /* :1761-1768 — save non_zero_count. */
    const int nz_idx[] = {0,2,8,10,16,18,32,34};
    const int nz_dst[] = {0,4,8,12,16,20,32,36};
    for (int k = 0; k < 8; k++)
        memcpy(&nnz[nz_dst[k]], &c->non_zero_count[phx_scan8[nz_idx[k]]], 4);

    if (c->i_cbp_luma == 0 && mb_type != PHX_I_8x8)
        c->b_transform_8x8 = 0;                         /* :1777-1778 */
    s->mb_transform_size[xy] = (uint8_t)c->b_transform_8x8;
    if (mb_type != PHX_I_PCM)                           /* encoder/macroblock.c cbp store */
        s->cbp[xy] = (int16_t)((c->i_cbp_chroma << 4) | c->i_cbp_luma | 0x1700);

    int intra = mb_type == PHX_I_4x4 || mb_type == PHX_I_8x8
             || mb_type == PHX_I_16x16 || mb_type == PHX_I_PCM;
    if (s->slice_type != 2) {                           /* :1781, SLICE_TYPE_I=2 */
        if (!intra) {                                   /* :1785 */
            for (int k = 0; k < 4; k++) {               /* :1787-1790 */
                int b = phx_scan8[4*k];
                s->ref[0][i_mb_8x8 + (k>>1)*s8x8 + (k&1)] = c->ref[0][b];
                memcpy(&s->mv[0][i_mb_4x4 + k*s4x4], &c->mv[0][b], sizeof(int16_t[2]));
            }
            for (int k = 0; k < 16; k++)
                memcpy(&s->mv[0][i_mb_4x4 + k], &c->mv[0][phx_scan8[k]],
                       sizeof(int16_t[2]));              /* :1791-1794 */
            if (s->slice_type == 1)                     /* B slice */
                memcpy(s->ref[1] + i_mb_8x8, c->ref[1], 4); /* :1799-1802 */
        } else {
            for (int k = 0; k < 4; k++)                 /* :1811-1816 */
                s->ref[0][i_mb_8x8 + (k>>1)*s8x8 + (k&1)] = -1;
            for (int k = 0; k < 16; k++)
                memset(&s->mv[0][i_mb_4x4 + k], 0, sizeof(int16_t[2]));
        }
    }
    if (s->b_cabac && !intra)                           /* CABAC mvd save */
        memcpy(s->mvd[0][xy], &c->mvd[0][phx_scan8[0]], 8*sizeof(int16_t[2]));
}
