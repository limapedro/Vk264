// Vk264 mirror of x264/encoder/analyse.c — I16x16 mode decision
// ---------------------------------------------------------------------------
// x264 oracle: mb_analyse_intra(), i16x16_mode_available[5][5],
// x264_mb_pred_mode16x16_fix[], x264_lambda_tab[], bs_size_ue().
// Vk264 GPU logic: inter_frame.comp::selectIntra16Mode().
//
// This fixture compares the two algorithms on identical randomly generated
// source/neighbour data, isolating mode selection from the rest of the
// pipeline. No shader is launched here: it validates the algorithm transcribed
// into the shader, not the GPU dispatch (that is covered by the frame gates).
// Textually included into the parity TU by ../src/dct_parity.cpp.
// ---------------------------------------------------------------------------

static constexpr int origin8 = FDEC_STRIDE + 8;

namespace vk264 {
namespace analyse {

// Transcription of inter_frame.comp::selectIntra16Mode cost rules.
// Returns the x264 mode number (V=0,H=1,DC=2,P=3, DC_LEFT=4,DC_TOP=5,DC128=6).
inline int selectIntra16Mode(int satd[7], int qp, bool left, bool top, bool topleft) {
    // x264 analyse.c: idx = neighbour & (TOP|LEFT|TOPLEFT); 4 iff all three,
    // else idx & (TOP|LEFT). 3 means top+left but no top-left (slice edge).
    int idx = (left && top) ? (topleft ? 4 : 3) : (top ? 2 : (left ? 1 : 0));
    static const int candidates[5][5] = {
        {6, -1, -1, -1, -1},
        {4,  1, -1, -1, -1},
        {5,  0, -1, -1, -1},
        {0,  1,  2, -1, -1},
        {0,  1,  2,  3, -1},
    };
    const int lambda = qp >= 0 && qp < 52 ? x264_lambda_tab[qp] : 4;
    int best = INT_MAX, mode = candidates[idx][0];
    for (int i = 0; candidates[idx][i] >= 0; ++i) {
        const int m = candidates[idx][i];
        const int fix = x264_mb_pred_mode16x16_fix[m];
        const int cost = satd[m] + lambda * (2 * (31 - x264_clz(fix + 1)) + 1);
        if (cost < best) { best = cost; mode = m; }
    }
    return mode;
}

} // namespace analyse
} // namespace vk264

void verifyIntra16Mode() {
    static constexpr int origin = FDEC_STRIDE + 8;
    std::mt19937 random(0x264F16);
    const auto predictors = std::array{x264_predict_16x16_v_c, x264_predict_16x16_h_c,
        x264_predict_16x16_dc_c, x264_predict_16x16_p_c, predict_16x16_dc_left_c,
        predict_16x16_dc_top_c, predict_16x16_dc_128_c};
    constexpr int cases = 52 * 5 * 64;
    int checked = 0;
    for (int c = 0; c < cases; ++c) {
        const int qp = c % 52, idxCase = (c / 52) % 5;
        const bool topleft = idxCase == 4;
        const bool left = idxCase == 1 || idxCase == 3 || idxCase == 4;
        const bool top = idxCase == 2 || idxCase == 3 || idxCase == 4;
        pixel src[256], pred[17 * FDEC_STRIDE];
        for (auto& v : src) v = static_cast<pixel>(random() & 255);
        for (auto& v : pred) v = 0;
        // stage neighbours (the values x264 reads from p_fdec)
        for (int i = 0; i < 16; ++i) {
            pred[origin - FDEC_STRIDE + i] = static_cast<pixel>(random() & 255);
            pred[origin - 1 + i * FDEC_STRIDE] = static_cast<pixel>(random() & 255);
        }
        // x264 oracle SATD for every mode
        int satd[7]{};
        for (int m = 0; m < 7; ++m) {
            pixel work[17 * FDEC_STRIDE];
            std::copy(std::begin(pred), std::end(pred), work);
            predictors[m](work + origin);
            satd[m] = x264_pixel_satd_16x16(src, FENC_STRIDE, work + origin, FDEC_STRIDE);
        }
        // x264 mode selection: iterate available modes in table order, strict <.
        static const int available[5][5] = {
            {6, -1, -1, -1, -1}, {4, 1, -1, -1, -1}, {5, 0, -1, -1, -1},
            {0, 1, 2, -1, -1}, {0, 1, 2, 3, -1},
        };
        const int xidx = topleft && left && top ? 4 : (left && top ? 3 : (top ? 2 : (left ? 1 : 0)));
        const int lambda = x264_lambda_tab[qp];
        int xbest = INT_MAX, xmode = available[xidx][0];
        for (int i = 0; available[xidx][i] >= 0; ++i) {
            const int m = available[xidx][i];
            const int fix = x264_mb_pred_mode16x16_fix[m];
            const int ue = 2 * (31 - x264_clz(fix + 1)) + 1;
            const int cost = satd[m] + lambda * ue;
            if (cost < xbest) { xbest = cost; xmode = m; }
        }
        const int vm = vk264::analyse::selectIntra16Mode(satd, qp, left, top, topleft);
        if (vm != xmode)
            throw std::runtime_error("I16x16 mode mismatch: case " + std::to_string(c) +
                                     " qp=" + std::to_string(qp) + " idx=" + std::to_string(xidx) +
                                     " x264=" + std::to_string(xmode) + " vk264=" + std::to_string(vm));
        ++checked;
    }
    std::cout << checked << " I16x16 mode selections matched x264 across all 5 neighbour cases and QP 0..51.\n";
}


namespace vk264 {
namespace analyse {

// Transcription of inter_frame.comp::selectChromaMode() cost rules.
// Chroma mode numbering: DC=0, H=1, V=2, P=3, DC_LEFT=4, DC_TOP=5, DC128=6.
inline int selectChromaMode(int satd[7], int qp, bool left, bool top, bool topleft) {
    int idx = (left && top) ? (topleft ? 4 : 3) : (top ? 2 : (left ? 1 : 0));
    static const int available[5][5] = {
        {6, -1, -1, -1, -1}, {4, 1, -1, -1, -1}, {5, 2, -1, -1, -1},
        {2, 1, 0, -1, -1}, {2, 1, 0, 3, -1},
    };
    const int lambda = qp >= 0 && qp < 52 ? x264_lambda_tab[qp] : 4;
    int best = INT_MAX, mode = available[idx][0];
    for (int i = 0; available[idx][i] >= 0; ++i) {
        const int m = available[idx][i];
        // x264 uses bs_size_ue(m) directly for chroma modes 0..3.
        const int ue = 2 * (31 - x264_clz(m + 1)) + 1;
        const int cost = satd[m] + lambda * ue;
        if (cost < best) { best = cost; mode = m; }
    }
    return mode;
}

} // namespace analyse
} // namespace vk264

void verifyChromaMode() {
    std::mt19937 random(0x264C40);
    const auto predictors = std::array{x264_predict_8x8c_dc_c, x264_predict_8x8c_h_c,
        x264_predict_8x8c_v_c, x264_predict_8x8c_p_c, predict_8x8c_dc_left_c,
        predict_8x8c_dc_top_c, predict_8x8c_dc_128_c};
    constexpr int cases = 52 * 5 * 64;
    int checked = 0;
    for (int c = 0; c < cases; ++c) {
        const int qp = c % 52, idxCase = (c / 52) % 5;
        const bool topleft = idxCase == 4;
        const bool left = idxCase == 1 || idxCase == 3 || idxCase == 4;
        const bool top = idxCase == 2 || idxCase == 3 || idxCase == 4;
        pixel src[64], pred[17 * FDEC_STRIDE];
        for (auto& v : src) v = static_cast<pixel>(random() & 255);
        for (auto& v : pred) v = 0;
        for (int i = 0; i < 8; ++i) {
            pred[origin8 - FDEC_STRIDE + i] = static_cast<pixel>(random() & 255);
            pred[origin8 - 1 + i * FDEC_STRIDE] = static_cast<pixel>(random() & 255);
        }
        int satd[7]{};
        for (int m = 0; m < 7; ++m) {
            pixel work[17 * FDEC_STRIDE];
            std::copy(std::begin(pred), std::end(pred), work);
            predictors[m](work + origin8);
            satd[m] = x264_pixel_satd_8x8(src, 8, work + origin8, FDEC_STRIDE);
        }
        int xidx = topleft && left && top ? 4 : (left && top ? 3 : (top ? 2 : (left ? 1 : 0)));
        static const int available[5][5] = {
            {6, -1, -1, -1, -1}, {4, 1, -1, -1, -1}, {5, 2, -1, -1, -1},
            {2, 1, 0, -1, -1}, {2, 1, 0, 3, -1},
        };
        const int lambda = x264_lambda_tab[qp];
        int xbest = INT_MAX, xmode = available[xidx][0];
        for (int i = 0; available[xidx][i] >= 0; ++i) {
            const int m = available[xidx][i];
            const int ue = 2 * (31 - x264_clz(m + 1)) + 1;
            const int cost = satd[m] + lambda * ue;
            if (cost < xbest) { xbest = cost; xmode = m; }
        }
        const int vm = vk264::analyse::selectChromaMode(satd, qp, left, top, topleft);
        if (vm != xmode)
            throw std::runtime_error("Chroma mode mismatch: case " + std::to_string(c) +
                                     " qp=" + std::to_string(qp) + " idx=" + std::to_string(xidx) +
                                     " x264=" + std::to_string(xmode) + " vk264=" + std::to_string(vm));
        ++checked;
    }
    std::cout << checked << " chroma mode selections matched x264 across all 5 neighbour cases and QP 0..51.\n";
}


// Replays x264's exact typed bin sequence for the superfast/flat I4x4 MB.
void verifyI4BinReplay() {
    x264_t h{}; h.i_frame = 0; h.sh.i_type = SLICE_TYPE_I;
    x264_cabac_init(&h);
    std::uint8_t bytes[256]{};
    bytes[0] = 0x88; bytes[1] = 0x84; bytes[2] = 0x3f;
    x264_cabac_t cb{};
    x264_cabac_context_init(&h, &cb, SLICE_TYPE_I, 25, 0);
    x264_cabac_encode_init(&cb, bytes+3, bytes+sizeof(bytes));
    x264_cabac_encode_decision_c(&cb, 3, 0);
    x264_cabac_encode_decision_c(&cb, 399, 0);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 0);
    x264_cabac_encode_decision_c(&cb, 69, 0);
    x264_cabac_encode_decision_c(&cb, 69, 1);
    x264_cabac_encode_decision_c(&cb, 69, 0);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 68, 1);
    x264_cabac_encode_decision_c(&cb, 64, 0);
    x264_cabac_encode_decision_c(&cb, 73, 1);
    x264_cabac_encode_decision_c(&cb, 73, 0);
    x264_cabac_encode_decision_c(&cb, 73, 0);
    x264_cabac_encode_decision_c(&cb, 76, 0);
    x264_cabac_encode_decision_c(&cb, 77, 1);
    x264_cabac_encode_decision_c(&cb, 81, 0);
    x264_cabac_encode_decision_c(&cb, 60, 0);
    x264_cabac_encode_decision_c(&cb, 96, 1);
    x264_cabac_encode_decision_c(&cb, 134, 1);
    x264_cabac_encode_decision_c(&cb, 195, 1);
    x264_cabac_encode_decision_c(&cb, 248, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_decision_c(&cb, 252, 1);
    x264_cabac_encode_ue_bypass(&cb, 0, 31);
    x264_cabac_encode_bypass_c(&cb, -1);
    x264_cabac_encode_decision_c(&cb, 96, 1);
    x264_cabac_encode_decision_c(&cb, 134, 1);
    x264_cabac_encode_decision_c(&cb, 195, 1);
    x264_cabac_encode_decision_c(&cb, 248, 0);
    x264_cabac_encode_bypass_c(&cb, -1);
    x264_cabac_encode_decision_c(&cb, 96, 0);
    x264_cabac_encode_decision_c(&cb, 95, 0);
    x264_cabac_encode_decision_c(&cb, 100, 1);
    x264_cabac_encode_decision_c(&cb, 149, 1);
    x264_cabac_encode_decision_c(&cb, 210, 1);
    x264_cabac_encode_decision_c(&cb, 258, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_ue_bypass(&cb, 0, 74);
    x264_cabac_encode_bypass_c(&cb, 0);
    x264_cabac_encode_decision_c(&cb, 100, 1);
    x264_cabac_encode_decision_c(&cb, 149, 1);
    x264_cabac_encode_decision_c(&cb, 210, 1);
    x264_cabac_encode_decision_c(&cb, 258, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_decision_c(&cb, 262, 1);
    x264_cabac_encode_ue_bypass(&cb, 0, 56);
    x264_cabac_encode_bypass_c(&cb, -1);
    x264_cabac_encode_flush(&h, &cb);
    std::cout << "REPLAY len=" << (cb.p-bytes) << " bytes:";
    for (std::uint8_t* p = bytes; p < cb.p; ++p)
        std::cout << " " << std::hex << (int)*p << std::dec;
    std::cout << "\n";
}
