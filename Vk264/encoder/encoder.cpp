// Vk264 mirror of x264/encoder/encoder.c
// shaders: slice_header.comp
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace encoder {

// mirrors the x264 encoder/encoder.c sliceHeader family
template<class... A> void sliceHeader(A&&... a) { execute(std::forward<A>(a)...); }

} // namespace encoder
} // namespace vk264
