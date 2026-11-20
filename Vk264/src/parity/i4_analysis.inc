// Vk264 parity module: i4_analysis.inc
// Host-side faithful port of x264's I4x4 mode decision for i_mbrd=0 presets.
// Reproduces x264's raw modes 16/16 for the flat case (build/i4_feedback2.py).
// Mirrors encoder/analyse.c:864-961, common/macroblock.h:225/420,
// common/base.h:180, common/x86/pixel-a.asm:2718-2975.

struct I4Analysis {
    std::int32_t modes[16];       // fixed modes (0..8) as CABAC expects
    dctcoef coeffs[16][16];       // quantised scanned coefficients (for CABAC)
    dctcoef recon[16][16];        // dequantised coefficients (internal)
    std::int32_t nnz[16];
    std::int32_t cbpLuma;
};

namespace i4detail {
// 8x8-grouped block order: block_idx_xy_fenc (macroblock.h:225).
constexpr int XY[16][2] = {
    {0,0},{1,0},{0,1},{1,1},{2,0},{3,0},{2,1},{3,1},
    {0,2},{1,2},{0,3},{1,3},{2,2},{3,2},{2,3},{3,3}};
// x264_scan8[0..15] (base.h:180) -> cache position.
constexpr int SCAN8[16] = {12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39};
constexpr int CACHE_OWNER[40] = {
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, 0,1,-1,-1, -1,-1,-1,-1,
    2,3,-1,-1, 4,5,-1,-1, 6,7,-1,-1, 8,9,-1,-1, 10,11,-1,-1};
// i4x4_mode_available[0][class] (analyse.c:512-528).
constexpr int MODE_AVAIL[5][10] = {
    {11,-1,-1,-1,-1,-1,-1,-1,-1},
    {9,1,8,-1,-1,-1,-1,-1,-1},
    {10,0,3,7,-1,-1,-1,-1,-1},
    {2,1,0,3,7,8,-1,-1,-1},
    {2,1,0,3,4,5,6,7,8,-1}};
// i_neighbour4 class for first MB of a frame (macroblock.c:489-498,1340-1351).
constexpr int NB4[16] = {0,1,2,4, 1,1,4,4, 2,4,2,4, 4,4,4,4};

inline int fix4(int m) { return m >= 0 && m <= 8 ? m : 2; }

inline void pred4(const int mode, pixel* p, int qp) {
    static void (*basic[9])(pixel*) = {
        x264_predict_4x4_v_c, x264_predict_4x4_h_c, x264_predict_4x4_dc_c,
        predict_4x4_ddl_c, predict_4x4_ddr_c, predict_4x4_vr_c,
        predict_4x4_hd_c, predict_4x4_vl_c, predict_4x4_hu_c};
    if (mode <= 8) { basic[mode](p); return; }
    if (mode == 11) { for (int i = 0; i < 16; ++i) p[i] = 128; return; }
    // DC_LEFT / DC_TOP: average of the 4 available samples.
    int v;
    if (mode == 9) {
        v = (p[-1] + p[FDEC_STRIDE-1] + p[2*FDEC_STRIDE-1] + p[3*FDEC_STRIDE-1] + 2) >> 2;
    } else { // 10
        v = (p[-FDEC_STRIDE] + p[-FDEC_STRIDE+1] + p[-FDEC_STRIDE+2] + p[-FDEC_STRIDE+3] + 2) >> 2;
    }
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) p[y*FDEC_STRIDE+x] = static_cast<pixel>(v);
}

inline int sad4(const pixel* fenc, int fencStride, const pixel* p) {
    return x264_pixel_sad_4x4(const_cast<pixel*>(fenc), fencStride, const_cast<pixel*>(p), FDEC_STRIDE);
}
} // namespace i4detail

inline void analyzeI4(const pixel* fenc, int fencStride, int qp, I4Analysis& out) {
    using namespace i4detail;
    int mf[16], bias[16], dequant[6][16]{};
    {
        constexpr int quant4[][3] = {{13107,8066,5243},{11916,7490,4660},{10082,6554,4194},
            {9362,5825,3647},{8192,5243,3355},{7282,4559,2893}};
        constexpr int dequant4[][3] = {{10,13,16},{11,14,18},{13,16,20},{14,18,23},{16,20,25},{18,23,29}};
        const int shift = qp/6-1;
        for (int i = 0; i < 16; ++i) {
            const int parity = (i&1)+((i>>2)&1);
            const int value = quant4[qp%6][parity];
            const int v = shift<=0 ? value<<-shift : (value+(1<<(shift-1)))>>shift;
            mf[i] = v; bias[i] = std::min((21*1024+v/2)/v, 32768/v);
            dequant[qp%6][i] = 16*dequant4[qp%6][parity];
        }
    }
    constexpr int origin = FDEC_STRIDE + 8;
    // origin = FDEC_STRIDE+8 puts 16 rows of content in rows 1..16, so we
    // need 17 rows plus the top border; allocate 32 to be safe.
    std::array<pixel, 32*FDEC_STRIDE> fdec{};
    for (auto& v : fdec) v = 128;
    std::array<int, 16> raw{};
    raw.fill(0);
    out.cbpLuma = 0;
    const int lambda = qp >= 0 && qp < 52 ? x264_lambda_tab[qp] : 4;
    for (int idx = 0; idx < 16; ++idx) {
        const int sx = XY[idx][0], sy = XY[idx][1];
        const int px = 4*sx, py = 4*sy;
        const int li = CACHE_OWNER[SCAN8[idx]-1], ti = CACHE_OWNER[SCAN8[idx]-8];
        const int ma = li >= 0 ? fix4(raw[li]) : -1;
        const int mb = ti >= 0 ? fix4(raw[ti]) : -1;
        int pred = std::min(ma, mb);
        if (pred < 0) pred = 2;
        const int cls = NB4[idx];
        const int* avail = MODE_AVAIL[cls];
        // x9 path when all 9 candidates are in 0..8 (class 4).
        const bool useX9 = cls == 4;
        int best = INT_MAX, bestMode = 2;
        for (int k = 0; avail[k] >= 0; ++k) {
            const int m = avail[k];
            pixel predBuf[32*FDEC_STRIDE];
            std::copy(fdec.begin(), fdec.end(), predBuf);
            pred4(m, predBuf+origin+py*FDEC_STRIDE+px, qp);
            int cost = sad4(fenc+py*fencStride+px, fencStride,
                            predBuf+origin+py*FDEC_STRIDE+px);
            if (useX9) {
                // cost_table[k]=3*lambda*(k!=8) passed as cost-pred: free at m=pred+8.
                if (m != pred+8) cost += 3*lambda;
                // INTRA_X9_END (pixel-a.asm:2841) adds tie-break constants
                // 0..7 in order [h,dc,ddl,ddr,vr,hd,vl,hu] (SAD path).
                // SSE4 phminposuw: first minimum wins (packed order
                // h,dc,ddl,ddr,vr,hd,vl,hu); no tie constants are added.
            } else if (fix4(m) == pred) {
                cost -= 3*lambda;
            }
            if (cost < best) { best = cost; bestMode = m; }
        }
        raw[idx] = bestMode;
        out.modes[idx] = fix4(bestMode);
        // predict -> DCT -> quant -> dequant -> IDCT -> write reconstruction
        pixel* block = fdec.data()+origin+py*FDEC_STRIDE+px;
        pred4(bestMode, block, qp);
        sub4x4_dct(out.coeffs[idx], const_cast<pixel*>(fenc+py*fencStride+px), block);
        out.nnz[idx] = quant_4x4(out.coeffs[idx], reinterpret_cast<udctcoef*>(mf),
                                 reinterpret_cast<udctcoef*>(bias));
        for (int j = 0; j < 16; ++j) out.recon[idx][j] = out.coeffs[idx][j];
        dequant_4x4(out.recon[idx], dequant, qp);
        add4x4_idct(block, out.recon[idx]);
        if (out.nnz[idx]) out.cbpLuma |= 1 << (idx>>2);
    }
}
