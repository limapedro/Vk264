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

} // namespace cavlc
} // namespace vk264
