/* x264's macroblock cache load/save, line by line — progressive and MBAFF.
 *
 * SHADER: B — the cache arrays are the explicit boundary between the
 * per-MB analysis workgroup and the entropy/state writer; every neighbour
 * read below is ordered exactly as in reference/x264/common/macroblock.c.
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
    s->field = calloc(n, 1);                             /* MBAFF field flags */
    /* macroblock.c:357-362 — five row-pointer sets, each with a 16-byte
     * emulated left edge, sized to the widest MB row (+32 for safety). */
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 3; j++) {
            uint8_t *base = calloc((size_t)mb_width*16 + 48, 1);
            s->intra_border_backup_base[i][j] = base;
            s->intra_border_backup[i][j] = base ? base + 16 : NULL; /* :361 */
        }
    s->threadslice_start = 0;                            /* single-thread port */
    s->slice_type = 1;                                   /* P slice default */
    s->b_cabac = 1;
    s->b_interlaced = 0;                                 /* MB_INTERLACED */
    s->b_transform_8x8_mode = 1;                         /* pps default */
    return s;
}

void phx_mb_state_delete(phx_mb_state_t *s)
{
    if (!s) return;
    free(s->type); free(s->intra4x4_pred_mode); free(s->non_zero_count);
    free(s->cbp); free(s->mb_transform_size);
    free(s->ref[0]); free(s->ref[1]); free(s->mv[0]); free(s->mv[1]);
    free(s->mvd[0]); free(s->mvd[1]); free(s->slice_table); free(s->field);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 3; j++)
            free(s->intra_border_backup_base[i][j]);
    free(s);
}

/* common/macroblock.c:673-846 — macroblock_cache_load_neighbours().
 * MB_INTERLACED is the frame-level h->mb.b_interlaced flag (common.h:71),
 * not the per-MB field[] byte; the latter only steers field/frame pairing. */
void phx_macroblock_cache_load_neighbours(phx_mb_state_t *s, phx_mb_cache_t *c,
                                          int mb_x, int mb_y, int b_mbaff)
{
    int xy = mb_y*s->mb_stride + mb_x;                   /* macroblock.c:681 */
    int mb_interlaced = b_mbaff && s->b_interlaced;      /* :675 */
    int top_y = mb_y - (1 << mb_interlaced);             /* :676 */
    int top = top_y*s->mb_stride + mb_x;                 /* :677 */

    c->i_mb_x = mb_x; c->i_mb_y = mb_y;                  /* :679-680 */
    c->i_mb_xy = xy;                                     /* :681 */
    c->i_b8_xy = 2*(mb_y*s->b8_stride + mb_x);           /* :682 */
    c->i_b4_xy = 4*(mb_y*s->b4_stride + mb_x);           /* :683 */
    c->left_b8[0] = c->left_b8[1] = -1;                  /* :684-685 */
    c->left_b4[0] = c->left_b4[1] = -1;                  /* :686-687 */
    c->i_neighbour = c->i_neighbour_intra = 0;           /* :688-689 */
    c->i_neighbour_frame = 0;                            /* :690 */
    c->top = -1; c->top_y = -1;                          /* :691-692 */
    c->left[0] = c->left[1] = -1;                        /* :693 */
    c->topleft = -1; c->topright = -1;                   /* :694-695 */
    c->type_top = -1; c->type_left[0] = c->type_left[1] = -1;
    c->type_topleft = c->type_topright = -1;             /* :696-699 */
    c->left_index = &phx_left_indices[3];                /* :700 */
    c->topleft_partition = 0;                            /* :701 */

    int topleft_y = top_y, topright_y = top_y;           /* :703-704 */
    int left[2] = {xy-1, xy-1};                          /* :707 */
    c->left_b8[0] = c->left_b8[1] = c->i_b8_xy - 2;      /* :708 */
    c->left_b4[0] = c->left_b4[1] = c->i_b4_xy - 4;      /* :709 */

    if (b_mbaff) {                                       /* :711 */
        c->i_mb_top_mbpair_xy = xy - 2*s->mb_stride;     /* :713 */
        c->i_mb_topleft_y = -1; c->i_mb_topright_y = -1; /* :714-715 */
        if (mb_y & 1) {                                  /* :717 */
            if (mb_x && mb_interlaced != s->field[xy-1]) {/* :719 */
                left[0] = left[1] = xy - 1 - s->mb_stride; /* :721 */
                c->left_b8[0] = c->left_b8[1] = c->i_b8_xy - 2 - 2*s->b8_stride;
                c->left_b4[0] = c->left_b4[1] = c->i_b4_xy - 4 - 4*s->b4_stride;
                if (mb_interlaced) {                     /* :725 */
                    c->left_index = &phx_left_indices[2];
                    left[1] += s->mb_stride;
                    c->left_b8[1] += 2*s->b8_stride;
                    c->left_b4[1] += 4*s->b4_stride;
                } else {                                 /* :733-737 */
                    c->left_index = &phx_left_indices[1];
                    topleft_y++;
                    c->topleft_partition = 1;
                }
            }
            if (!mb_interlaced) topright_y = -1;         /* :739-740 */
        } else {                                         /* :742 */
            if (mb_interlaced && top >= 0) {             /* :744 */
                if (!s->field[top]) { top += s->mb_stride; top_y++; } /* :746-749 */
                if (mb_x) topleft_y += !s->field[s->mb_stride*topleft_y + mb_x-1];
                if (mb_x < s->mb_width-1)
                    topright_y += !s->field[s->mb_stride*topright_y + mb_x+1];
                                                         /* :752-754 */
            }
            if (mb_x && mb_interlaced != s->field[xy-1]) {/* :756 */
                if (mb_interlaced) {                     /* :758 */
                    c->left_index = &phx_left_indices[2];/* :760 */
                    left[1] += s->mb_stride;             /* :761 */
                    c->left_b8[1] += 2*s->b8_stride;     /* :762 */
                    c->left_b4[1] += 4*s->b4_stride;     /* :763 */
                } else c->left_index = &phx_left_indices[0]; /* :766 */
            }
        }
    }

    if (mb_x > 0) {                                      /* :771 */
        c->i_neighbour_frame |= PHX_MB_LEFT;             /* :773 */
        c->left[0] = left[0]; c->left[1] = left[1];      /* :774-775 */
        c->type_left[0] = s->type[c->left[0]];
        c->type_left[1] = s->type[c->left[1]];           /* :776-777 */
        if (s->slice_table[c->left[0]] == s->first_mb) { /* :778 */
            c->i_neighbour |= PHX_MB_LEFT;               /* :780 */
            int t = c->type_left[0];
            int intra = t == PHX_I_4x4 || t == PHX_I_8x8
                     || t == PHX_I_16x16 || t == PHX_I_PCM;
            if (!s->b_constrained_intra || intra)
                c->i_neighbour_intra |= PHX_MB_LEFT;     /* :783-784 */
        }
    }
    if ((s->threadslice_start >> mb_interlaced) != (mb_y >> mb_interlaced)) {
                                                         /* :789 */
      if (top >= 0) {                                    /* :791 */
        c->i_neighbour_frame |= PHX_MB_TOP;              /* :793 */
        c->top = top; c->top_y = top_y;                  /* :794-795 */
        c->type_top = s->type[top];                      /* :796 */
        if (s->slice_table[top] == s->first_mb) {        /* :797 */
            c->i_neighbour |= PHX_MB_TOP;                /* :799 */
            int t = c->type_top;
            int intra = t == PHX_I_4x4 || t == PHX_I_8x8
                     || t == PHX_I_16x16 || t == PHX_I_PCM;
            if (!s->b_constrained_intra || intra)
                c->i_neighbour_intra |= PHX_MB_TOP;      /* :801-802 */
        }
      }
      if (mb_x > 0 && topleft_y >= 0) {                  /* :816 */
        c->i_neighbour_frame |= PHX_MB_TOPLEFT;          /* :818 */
        c->topleft = s->mb_stride*topleft_y + mb_x - 1;  /* :819 */
        c->i_mb_topleft_y = topleft_y;                   /* :820 */
        c->type_topleft = s->type[c->topleft];           /* :821 */
        if (s->slice_table[c->topleft] == s->first_mb) {
            c->i_neighbour |= PHX_MB_TOPLEFT;            /* :824 */
            int t = c->type_topleft;
            int intra = t == PHX_I_4x4 || t == PHX_I_8x8
                     || t == PHX_I_16x16 || t == PHX_I_PCM;
            if (!s->b_constrained_intra || intra)
                c->i_neighbour_intra |= PHX_MB_TOPLEFT;  /* :826-827 */
        }
      }
      if (mb_x < s->mb_width-1 && topright_y >= 0) {     /* :831 */
        c->i_neighbour_frame |= PHX_MB_TOPRIGHT;         /* :833 */
        c->topright = s->mb_stride*topright_y + mb_x + 1;/* :834 */
        c->i_mb_topright_y = topright_y;                 /* :835 */
        c->type_topright = s->type[c->topright];         /* :836 */
        if (s->slice_table[c->topright] == s->first_mb) {
            c->i_neighbour |= PHX_MB_TOPRIGHT;           /* :839 */
            int t = c->type_topright;
            int intra = t == PHX_I_4x4 || t == PHX_I_8x8
                     || t == PHX_I_16x16 || t == PHX_I_PCM;
            if (!s->b_constrained_intra || intra)
                c->i_neighbour_intra |= PHX_MB_TOPRIGHT; /* :841-842 */
        }
      }
    }

    /* :1262-1267 ? field decoding flag is sampled at the start of a pair. */
    if (b_mbaff && mb_x == 0 && !(mb_y & 1))
        c->field_decoding_flag = (c->top >= s->first_mb) ? s->field[c->top] : 0;

    /* :1272-1278 ? skip would mispredict interlace mode at a field boundary. */
    c->b_allow_skip = 1;
    if (b_mbaff && mb_interlaced != c->field_decoding_flag && (mb_y & 1)) {
        int up = xy - s->mb_stride;
        if (up >= 0 && (s->type[up] == PHX_P_SKIP || s->type[up] == 18))
            c->b_allow_skip = 0;
    }

    /* :1280-1306 ? CABAC skip-neighbour count. */
    c->i_neighbour_skip = 0;
    if (s->b_cabac) {
        if (b_mbaff) {
            int mb_pair = mb_x + (mb_y & ~1)*s->mb_stride; /* :1286 */
            int left_xy = mb_pair - 1;                     /* :1287 */
            int top_xy;
            if ((mb_y & 1) && mb_x > 0 && c->field_decoding_flag == s->field[left_xy])
                left_xy += s->mb_stride;                   /* :1288-1289 */
            if (c->field_decoding_flag) {
                top_xy = mb_pair - s->mb_stride;           /* :1292 */
                if (!(mb_y & 1) && top_xy >= 0
                    && s->slice_table[top_xy] == s->first_mb && s->field[top_xy])
                    top_xy -= s->mb_stride;                /* :1293-1294 */
            } else top_xy = mb_x + (mb_y-1)*s->mb_stride;  /* :1297 */
            if (mb_x > 0 && s->slice_table[left_xy] == s->first_mb
                && s->type[left_xy] != PHX_P_SKIP && s->type[left_xy] != 18)
                c->i_neighbour_skip++;                     /* :1299 */
            if (top_xy >= 0 && s->slice_table[top_xy] == s->first_mb
                && s->type[top_xy] != PHX_P_SKIP && s->type[top_xy] != 18)
                c->i_neighbour_skip++;                     /* :1300 */
        } else {
            c->i_neighbour_skip = ((c->i_neighbour & PHX_MB_LEFT)
                                   && c->type_left[0] != PHX_P_SKIP
                                   && c->type_left[0] != 18)
                                + ((c->i_neighbour & PHX_MB_TOP)
                                   && c->type_top != PHX_P_SKIP
                                   && c->type_top != 18);  /* :1304-1305 */
        }
    }
}

/* common/macroblock.c:855 — macroblock_cache_load(), progressive branch. */
void phx_macroblock_cache_load(phx_mb_state_t *s, phx_mb_cache_t *c,
                               int mb_x, int mb_y, int b_mbaff)
{
    phx_macroblock_cache_load_neighbours(s, c, mb_x, mb_y, b_mbaff); /* :857 */
    int top = c->top, top_y = c->top_y;
    int mb_interlaced = b_mbaff && s->b_interlaced;      /* :675 (MB_INTERLACED) */
    int s8x8 = s->b8_stride, s4x4 = s->b4_stride;        /* :862-863 */
    int top_8x8 = (2*top_y+1)*s8x8 + 2*mb_x;            /* :864 */
    int top_4x4 = (4*top_y+3)*s4x4 + 4*mb_x;            /* :865 */
    int lists = (1 << s->slice_type) & 3;                /* :866 */
    const phx_left_table_t *lt = c->left_index;          /* :700/:766/:734 */

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
        int lbot = b_mbaff ? c->left[1] : ltop;          /* :918 */
        if (b_mbaff) {                                   /* :919-924 */
            int top_luma = (s->cbp[ltop] >> (lt->mv[0] & ~1)) & 2;
            int bot_luma = (s->cbp[lbot] >> (lt->mv[2] & ~1)) & 2;
            c->i_cbp_left = (s->cbp[ltop] & 0xfff0) | (bot_luma<<2) | top_luma;
        } else c->i_cbp_left = s->cbp[ltop];             /* :926 */
        c->intra4x4_pred_mode[phx_scan8[ 0]-1] = s->intra4x4_pred_mode[ltop][lt->intra[0]];
        c->intra4x4_pred_mode[phx_scan8[ 2]-1] = s->intra4x4_pred_mode[ltop][lt->intra[1]];
        c->intra4x4_pred_mode[phx_scan8[ 8]-1] = s->intra4x4_pred_mode[lbot][lt->intra[2]];
        c->intra4x4_pred_mode[phx_scan8[10]-1] = s->intra4x4_pred_mode[lbot][lt->intra[3]];
                                                         /* :929-932 */
        c->non_zero_count[phx_scan8[ 0]-1] = s->non_zero_count[ltop][lt->nnz[0]];
        c->non_zero_count[phx_scan8[ 2]-1] = s->non_zero_count[ltop][lt->nnz[1]];
        c->non_zero_count[phx_scan8[ 8]-1] = s->non_zero_count[lbot][lt->nnz[2]];
        c->non_zero_count[phx_scan8[10]-1] = s->non_zero_count[lbot][lt->nnz[3]];
        c->non_zero_count[phx_scan8[16+ 0]-1] = s->non_zero_count[ltop][lt->nnz_chroma[0]];
        c->non_zero_count[phx_scan8[16+ 2]-1] = s->non_zero_count[lbot][lt->nnz_chroma[1]];
        c->non_zero_count[phx_scan8[32+ 0]-1] = s->non_zero_count[ltop][lt->nnz_chroma[2]];
        c->non_zero_count[phx_scan8[32+ 2]-1] = s->non_zero_count[lbot][lt->nnz_chroma[3]];
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

    /* :987-992 — 8x8 transform neighbour flag (PPS gate at :987). */
    if (s->b_transform_8x8_mode)
        c->i_neighbour_transform_size =
            ((c->i_neighbour & PHX_MB_LEFT) && s->mb_transform_size[c->left[0]])
          + ((c->i_neighbour & PHX_MB_TOP) && s->mb_transform_size[top]);

    /* :1048-1198 — ref/mv/mvd caches, progressive path. */
    for (int l = 0; l < lists; l++) {
        int i8 = phx_scan8[0] - 1 - 8;                   /* :1054 */
        if (c->i_neighbour & PHX_MB_TOPLEFT) {           /* :1055 */
            int ir = b_mbaff ? 2*(s->b8_stride*c->i_mb_topleft_y + mb_x-1)+1+s->b8_stride
                             : top_8x8 - 1;            /* :1057 */
            int iv = b_mbaff ? 4*(s->b4_stride*c->i_mb_topleft_y + mb_x-1)+3+3*s->b4_stride
                             : top_4x4 - 1;            /* :1058 */
            if (b_mbaff && c->topleft_partition) {       /* :1059-1065 */
                iv -= 2*s->b4_stride;
                ir -= s->b8_stride;
            }
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
        i8 = phx_scan8[0] + 4 - 8;                       /* :1090 (scan8 row0 col8) */
        if (c->i_neighbour & PHX_MB_TOPRIGHT) {          /* :1091 */
            int ir = b_mbaff ? 2*(s->b8_stride*c->i_mb_topright_y + mb_x+1)+s->b8_stride
                             : top_8x8 + 2;             /* :1093 */
            int iv = b_mbaff ? 4*(s->b4_stride*c->i_mb_topright_y + mb_x+1)+3*s->b4_stride
                             : top_4x4 + 4;             /* :1094 */
            c->ref[l][i8] = s->ref[l][ir];         /* :1095 */
            memcpy(c->mv[l][i8], s->mv[l][iv], sizeof(int16_t[2]));
        } else c->ref[l][i8] = -2;                       /* :1099 */
        i8 = phx_scan8[0] - 1;                           /* :1101 */
        if (c->i_neighbour & PHX_MB_LEFT) {              /* :1102 */
            if (b_mbaff) {                               /* :1104-1114 */
                for (int k = 0; k < 4; k++) {
                    int b8 = k < 2 ? c->left_b8[0] : c->left_b8[1];
                    int b4 = k < 2 ? c->left_b4[0] : c->left_b4[1];
                    c->ref[l][i8+k*8] = s->ref[l][b8 + 1 + s->b8_stride*lt->ref[k]];
                    memcpy(c->mv[l][i8+k*8], s->mv[l][b4 + 3 + s->b4_stride*lt->mv[k]],
                           sizeof(int16_t[2]));
                }
            } else {
                int ir = c->i_b8_xy - 1, iv = c->i_b4_xy - 1;/* :1118-1119 */
                c->ref[l][i8+0*8] = c->ref[l][i8+1*8] = s->ref[l][ir+0*s8x8];
                c->ref[l][i8+2*8] = c->ref[l][i8+3*8] = s->ref[l][ir+1*s8x8];
                for (int k = 0; k < 4; k++)              /* :1125-1128 */
                    memcpy(c->mv[l][i8+k*8], s->mv[l][iv+k*s4x4], sizeof(int16_t[2]));
            }
        } else {
            for (int k = 0; k < 4; k++) {
                c->ref[l][i8+k*8] = -2;
                memset(c->mv[l][i8+k*8], 0, sizeof(int16_t[2]));
            }                                            /* :1133-1137 */
        }
        /* :1140-1171 — MBAFF top-right MV/ref come from the left MB when the
         * nominal top-right partition does not exist.  Untouched otherwise. */
        if (b_mbaff && (c->i_neighbour & PHX_MB_LEFT)) {
            if (mb_interlaced && !s->field[c->i_mb_xy-1]) { /* :1152 */
                c->topright_ref[l][0] = s->ref[l][c->left_b8[0] + 1];           /* :1154 */
                c->topright_ref[l][1] = s->ref[l][c->left_b8[0] + 1 + s->b8_stride]; /* :1155 */
                c->topright_ref[l][2] = s->ref[l][c->left_b8[1] + 1];           /* :1156 */
                for (int j = 0; j < 3; j++) {
                    int b8 = j == 2 ? c->left_b4[1] : c->left_b4[0]; /* :1157-1159 */
                    memcpy(c->topright_mv[l][j],
                           s->mv[l][b8 + 3 + s->b4_stride*(lt->mv[j]+1)],
                           sizeof(int16_t[2]));
                }
            } else if (!mb_interlaced && s->field[c->i_mb_xy-1]) { /* :1161 */
                for (int j = 0; j < 3; j++)
                    c->topright_ref[l][j] = s->ref[l][c->left_b8[0] + 1
                        + s->b8_stride*(2 + lt->ref[j])];            /* :1164-1166 */
                for (int j = 0; j < 3; j++)
                    memcpy(c->topright_mv[l][j],
                           s->mv[l][c->left_b4[0] + 3 + s->b4_stride*(4 + lt->mv[j])],
                           sizeof(int16_t[2]));                      /* :1167-1169 */
            }
        }

        if (s->b_cabac) {                                /* :1173 */
            int base = phx_scan8[0] - 8;
            if (c->i_neighbour & PHX_MB_TOP)
                memcpy(c->mvd[l][base], s->mvd[l][top][0], 4*sizeof(int16_t[2]));
            else memset(c->mvd[l][base], 0, 4*sizeof(int16_t[2]));
                                                         /* :1176-1179 */
            const int mvd_idx[] = {0,2,8,10};
            int lt_mb = c->left[0], lb_mb = b_mbaff ? c->left[1] : lt_mb;
            int i8left = phx_scan8[0] - 1;
            int top_ok = (c->i_neighbour & PHX_MB_LEFT)
                      && (!b_mbaff || c->ref[l][i8left] >= 0);    /* :1181 */
            int bot_ok = (c->i_neighbour & PHX_MB_LEFT)
                      && (!b_mbaff || c->ref[l][i8left + 2*8] >= 0); /* :1191 */
            for (int k = 0; k < 4; k++) {
                int idx = phx_scan8[mvd_idx[k]] - 1;
                int ok = k < 2 ? top_ok : bot_ok;
                int mb = k < 2 ? lt_mb : lb_mb;
                if (ok) memcpy(c->mvd[l][idx], s->mvd[l][mb][lt->intra[k]],
                               sizeof(int16_t[2]));
                else memset(c->mvd[l][idx], 0, sizeof(int16_t[2]));
            }                                            /* :1183-1200 */
        }
    }
    /* :1206-1259 ? MAP_MVS: convert MVs cached from opposite field type. */
    if (b_mbaff) {
#define PHX_FIELD_DIFFERENT(mb) ((mb) >= 0 && (mb_interlaced ? !s->field[mb] : s->field[mb]))
#define PHX_MAP_F2F(idx) do { \
        for (int l = 0; l < lists; l++) { \
            if (c->ref[l][idx] >= 0) { \
                c->ref[l][idx] <<= 1; c->mv[l][idx][1] /= 2; c->mvd[l][idx][1] >>= 1; \
            } \
        } \
    } while (0)
#define PHX_MAP_F2F_INV(idx) do { \
        for (int l = 0; l < lists; l++) { \
            if (c->ref[l][idx] >= 0) { \
                c->ref[l][idx] >>= 1; c->mv[l][idx][1] *= 2; c->mvd[l][idx][1] <<= 1; \
            } \
        } \
    } while (0)
/* :1226-1228 — the MBAFF top-right extras live in their own small arrays. */
#define PHX_MAP_F2F_TR(idx) do { \
        for (int l = 0; l < lists; l++) { \
            if (c->topright_ref[l][idx] >= 0) { \
                c->topright_ref[l][idx] <<= 1; \
                c->topright_mv[l][idx][1] /= 2; \
                c->mvd[l][idx][1] >>= 1; /* x264 MAP_F2F:1239 */ \
            } \
        } \
    } while (0)
#define PHX_MAP_F2F_TR_INV(idx) do { \
        for (int l = 0; l < lists; l++) { \
            if (c->topright_ref[l][idx] >= 0) { \
                c->topright_ref[l][idx] >>= 1; \
                c->topright_mv[l][idx][1] *= 2; \
                c->mvd[l][idx][1] <<= 1; /* x264 MAP_F2F:1253 */ \
            } \
        } \
    } while (0)
/* :1208-1229 — MAP_MVS: x264 tests only left[LTOP] for the whole left column
 * (including the three topright extras); left[LBOT] is not consulted here. */
#define PHX_MAP_MVS(F2F, TR) do { \
        if (PHX_FIELD_DIFFERENT(c->topleft)) F2F(tl); \
        if (PHX_FIELD_DIFFERENT(c->top)) for (int j=0;j<4;j++) F2F(tp+j); \
        if (PHX_FIELD_DIFFERENT(c->topright)) F2F(tr); \
        if (PHX_FIELD_DIFFERENT(c->left[0])) { \
            for (int j=0;j<4;j++) F2F(lf+j*8); \
            for (int j=0;j<3;j++) TR(j); \
        } \
    } while (0)
        int tl = phx_scan8[0] - 9;
        int tp = phx_scan8[0] - 8;
        int tr = phx_scan8[0] + 3;
        int lf = phx_scan8[0] - 1;
        if (mb_interlaced) PHX_MAP_MVS(PHX_MAP_F2F, PHX_MAP_F2F_TR);
        else               PHX_MAP_MVS(PHX_MAP_F2F_INV, PHX_MAP_F2F_TR_INV);
#undef PHX_MAP_MVS
#undef PHX_FIELD_DIFFERENT
#undef PHX_MAP_F2F
#undef PHX_MAP_F2F_INV
#undef PHX_MAP_F2F_TR
#undef PHX_MAP_F2F_TR_INV
    }
}

/* common/macroblock.c:1638-1678 — macroblock_backup_intra().  The row
 * pointers are allocated exactly as x264 macroblock.c:357-362 does. */
void phx_macroblock_backup_intra(phx_mb_state_t *s, int mb_x, int mb_y,
                                 int b_mbaff, const uint8_t *p_fdec, int stride)
{
    int mb_xy = mb_y*s->mb_stride + mb_x;                /* macroblock.c:1644 */
    int backup_dst = !b_mbaff ? (mb_y&1)
                   : (mb_y&1) ? 1 : (s->b_interlaced ? 0 : 2); /* :1644 */
    uint8_t *dst_y = s->intra_border_backup[backup_dst][0] + mb_x*16; /* :1645 */
    uint8_t *dst_c = s->intra_border_backup[backup_dst][1] + mb_x*16; /* :1654 */
    memcpy(dst_y, p_fdec + stride*15, 16);               /* :1645 */
    memcpy(dst_c,      p_fdec + (15>>1)*stride,      8);  /* :1653-1654 */
    memcpy(dst_c + 8,  p_fdec + (15>>1)*stride + 8,  8);  /* :1655 */
    if (b_mbaff && (mb_y & 1)) {                         /* :1657-1676 */
        int interlaced = s->b_interlaced;                /* MB_INTERLACED */
        int backup_src = (interlaced ? 7 : 14)*stride;   /* :1661 */
        backup_dst = interlaced ? 2 : 0;                 /* :1662 */
        dst_y = s->intra_border_backup[backup_dst][0] + mb_x*16;
        dst_c = s->intra_border_backup[backup_dst][1] + mb_x*16;
        memcpy(dst_y, p_fdec + backup_src, 16);          /* :1663 */
        int chroma_src = (interlaced ? 3 : 6)*stride;    /* :1672 */
        memcpy(dst_c,     p_fdec + chroma_src,      8);  /* :1673 */
        memcpy(dst_c + 8, p_fdec + chroma_src + 8,  8);  /* :1674 */
    }
}

/* common/macroblock.c:1680 — macroblock_cache_save(), progressive branch. */
void phx_macroblock_cache_save(phx_mb_state_t *s, phx_mb_cache_t *c, int b_mbaff)
{
    const int xy = c->i_mb_xy;
    const int i_mb_4x4 = c->i_b4_xy, i_mb_8x8 = c->i_b8_xy;
    const int s8x8 = s->b8_stride, s4x4 = s->b4_stride;
    int8_t *i4x4 = s->intra4x4_pred_mode[xy];            /* :1691 */
    uint8_t *nnz = s->non_zero_count[xy];                /* :1692 */

    /* :1694-1717 — MBAFF backs up two field rows; macroblock_store_pic()
     * (reconstructed-pixel write-back) is not part of the cache state. */
    if (b_mbaff && c->p_fdec)
        phx_macroblock_backup_intra(s, c->i_mb_x, c->i_mb_y, 1,
                                    c->p_fdec, c->fdec_stride);
    if (!b_mbaff && c->p_fdec)
        phx_macroblock_backup_intra(s, c->i_mb_x, c->i_mb_y, 0,
                                    c->p_fdec, c->fdec_stride);

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
        for (int l = 0; l < (s->slice_type == 1 ? 2 : 1); l++) {
            if (!intra) {                               /* :1785 */
                for (int k = 0; k < 4; k++) {           /* :1787-1790 / :1799-1802 */
                    int b = phx_scan8[4*k];
                    s->ref[l][i_mb_8x8 + (k>>1)*s8x8 + (k&1)] = c->ref[l][b];
                }
                for (int k = 0; k < 16; k++)            /* :1791-1794 / :1803-1806 */
                    memcpy(&s->mv[l][i_mb_4x4 + (k>>2)*s4x4 + (k&3)],
                           &c->mv[l][phx_scan8[0] + 8*(k>>2) + (k&3)],
                           sizeof(int16_t[2]));
            } else {                                    /* :1811-1827 */
                for (int k = 0; k < 4; k++)
                    s->ref[l][i_mb_8x8 + (k>>1)*s8x8 + (k&1)] = -1;
                for (int k = 0; k < 16; k++)             /* :1813-1816/:1823-1826 */
                    memset(&s->mv[l][i_mb_4x4 + (k>>2)*s4x4 + (k&3)], 0,
                           sizeof(int16_t[2]));
            }
        }
    }
    if (s->b_cabac) {                                   /* :1831-1862 */
        /* x264 :1839 — !INTRA && !SKIP && !DIRECT for mb types 0..18. */
        int store_mvd = (0x3FF30 >> mb_type) & 1;
        for (int l = 0; l < (s->slice_type == 1 ? 2 : 1); l++) {
            if (store_mvd) {
                memcpy(s->mvd[l][xy], &c->mvd[l][phx_scan8[10]], 4*sizeof(int16_t[2]));
                memcpy(s->mvd[l][xy] + 4, &c->mvd[l][phx_scan8[5]], sizeof(int16_t[2]));
                memcpy(s->mvd[l][xy] + 5, &c->mvd[l][phx_scan8[7]], sizeof(int16_t[2]));
                memcpy(s->mvd[l][xy] + 6, &c->mvd[l][phx_scan8[13]], sizeof(int16_t[2]));
            } else memset(s->mvd[l][xy], 0, 8*sizeof(int16_t[2]));
        }
    }
}
