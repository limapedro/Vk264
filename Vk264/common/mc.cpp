// Vk264 mirror of x264/common/mc.c
// shaders: motion_compensation.comp, reference_frame.comp
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace mc {

// mirrors the x264 common/mc.c compensate family
template<class... A> void compensate(A&&... a) { execute(std::forward<A>(a)...); }

// mirrors the x264 common/mc.c reference family
template<class... A> void reference(A&&... a) { execute(std::forward<A>(a)...); }

} // namespace mc
} // namespace vk264
