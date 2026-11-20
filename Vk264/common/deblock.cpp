// Vk264 mirror of x264/common/deblock.c
// shaders: deblock_filter.comp, deblock_strength.comp, deblock_frame.comp
// x264 C deblocking functions are the byte-exact oracle in upstream_deblock.inc.
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace deblock {

template<class Cases>
void filter(const char* shader, Cases& cases) { execute(shader, cases); }

template<class Cases>
void strength(const char* shader, Cases& cases) { execute(shader, cases); }

template<class... Args>
void frame(Args&&... args) { execute(std::forward<Args>(args)...); }

} // namespace deblock
} // namespace vk264
