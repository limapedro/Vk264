// Vk264 mirror of x264/encoder/cavlc.c
// shaders: cavlc_frame.comp, cavlc_packet.comp, cavlc_residual_test.comp
// x264 C VLC logic is the byte-exact oracle (upstream tables + x264_cavlc_codes.hpp).
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace cavlc {

template<class Cases, class Tables>
void encode(const char* shader, Cases& cases, const Tables& tables) {
    execute(shader, cases, 1, tables);
}

// Transcription of cavlc_frame.comp::context() (neighbour nonzero prediction).
inline int neighbourContext(int a, int b, bool left, bool top) {
    int la = left ? a : 0, tb = top ? b : 0;
    return left && top ? (la + tb + 1) / 2 : la + tb;
}

} // namespace cavlc
} // namespace vk264

void verifyCavlcContext() {
    // x264 common/macroblock.h::x264_mb_predict_non_zero_code() +
    // encoder/cavlc.c::ct_index[]. Unavailable neighbours read 0x80 from the
    // cache edge, so the sum is not averaged (keeps the raw neighbour count).
    static constexpr std::uint8_t ct_index[17] = {
        0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,3};
    static constexpr int counts[] = {0,1,2,3,4,7,8,15,16,0x80};
    int checked = 0;
    for (int la : counts) for (int tb : counts) for (int avail = 0; avail < 4; ++avail) {
        const bool left = (avail & 1) != 0, top = (avail & 2) != 0;
        const int za = left ? (la & 0x7f) : 0x80, zb = top ? (tb & 0x7f) : 0x80;
        int pred = za + zb;
        if (pred < 0x80) pred = (pred + 1) >> 1;
        pred &= 0x7f;
        const int x264table = ct_index[pred];
        // Vk264 maps the predicted count directly in cavlc_residual.glsl.
        const int vkcount = vk264::cavlc::neighbourContext(la & 0x7f, tb & 0x7f, left, top);
        const int vktable = vkcount < 2 ? 0 : vkcount < 4 ? 1 : vkcount < 8 ? 2 : 3;
        if (vktable != x264table)
            throw std::runtime_error("CAVLC neighbour context mismatch: la=" +
                std::to_string(la) + " tb=" + std::to_string(tb) +
                " avail=" + std::to_string(avail) + " x264=" + std::to_string(x264table) +
                " vk264=" + std::to_string(vktable));
        ++checked;
    }
    std::cout << checked << " CAVLC neighbour-context cases matched x264.\n";
}
