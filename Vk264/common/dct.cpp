// Vk264 mirror of x264/common/dct.c
// ---------------------------------------------------------------------------
// x264 file : references/x264-master/common/dct.c
// shaders   : dct4x4.comp, dct8x8.comp (Vulkan compute)
//
// The x264 C functions (sub4x4_dct, add4x4_idct, sub8x8_dct8, ...) are the
// byte-exact oracle, extracted verbatim into upstream_dct.inc. This module
// exposes the equivalent GPU compute as ordinary C++ functions with the same
// intent, so a fixture can call the x264 function and the Vulkan function with
// identical inputs and compare the outputs byte for byte.
//
// This file is textually included into the parity translation unit by
// ../src/dct_parity.cpp (the extracted x264 reference uses file-static
// functions), so it can call execute()/fixtures()/Block directly.
// ---------------------------------------------------------------------------

namespace vk264 {
namespace dct {

// Launch the DCT shader for a batch of Group x Group blocks. Mirrors the
// family of x264 common/dct.c entry points; the shader computes both the
// forward transform and the inverse reconstruction in one dispatch.
template<int Side, int Group>
void transform(const char* shader, std::vector<Block<Group>>& blocks) {
    constexpr int blocksPerGroup = Group * Group / (Side * Side);
    execute(shader, blocks, blocksPerGroup);
}

} // namespace dct
} // namespace vk264
