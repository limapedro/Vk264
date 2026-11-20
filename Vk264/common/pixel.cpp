// Vk264 mirror of x264/common/pixel.c
// shaders: motion_metrics.comp
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace pixel {

// mirrors the x264 common/pixel.c metrics family
template<class... A> void metrics(A&&... a) { execute(std::forward<A>(a)...); }

} // namespace pixel
} // namespace vk264
