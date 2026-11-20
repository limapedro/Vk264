// Vk264 mirror of x264/common/set.c
// shaders: parameters.comp
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace set {

// mirrors the x264 common/set.c parameters family
template<class... A> void parameters(A&&... a) { execute(std::forward<A>(a)...); }

} // namespace set
} // namespace vk264
