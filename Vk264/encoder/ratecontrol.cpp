// Vk264 mirror of x264/encoder/ratecontrol.c
// shaders: aq.comp
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace ratecontrol {

// mirrors the x264 encoder/ratecontrol.c aq family
template<class... A> void aq(A&&... a) { execute(std::forward<A>(a)...); }

} // namespace ratecontrol
} // namespace vk264
