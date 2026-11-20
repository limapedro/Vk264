// Vk264 mirror of x264/common/quant.c
// ---------------------------------------------------------------------------
// x264 file : references/x264-master/common/quant.c
// shaders   : quant.comp, quant_group.comp (Vulkan compute)
//
// The x264 C functions (quant_4x4, quant_8x8, quant_4x4x4, dequant_4x4, ...)
// are the byte-exact oracle, extracted verbatim into upstream_quant.inc.
// This module exposes their GPU compute as ordinary function calls so the
// fixtures can compare the two implementations across QP 0..51.
//
// Textually included into the parity translation unit by ../src/dct_parity.cpp.
// ---------------------------------------------------------------------------

namespace vk264 {
namespace quant {

// Launch the quantizer shader for a batch of fixture records. Mirrors the
// quant_* / dequant_* family from x264 common/quant.c; the shader writes
// levels, nonzero flags and dequantized coefficients in one dispatch.
template<class Cases>
void quantize(const char* shader, Cases& cases) {
    execute(shader, cases);
}

} // namespace quant
} // namespace vk264
