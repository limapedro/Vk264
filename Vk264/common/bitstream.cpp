// Vk264 mirror of x264/common/bitstream.c
// shaders: bitstream.comp, nal.comp
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace bitstream {

// mirrors the x264 common/bitstream.c write family
template<class... A> void write(A&&... a) { execute(std::forward<A>(a)...); }

// mirrors the x264 common/bitstream.c nal family
template<class... A> void nal(A&&... a) { execute(std::forward<A>(a)...); }

} // namespace bitstream
} // namespace vk264
