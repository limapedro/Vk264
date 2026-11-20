// Vk264 mirror of x264/common/me.c
// shaders: motion_search.comp, motion_bidir.comp, motion_integral.comp, motion_lowres.comp
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace me {

// mirrors the x264 common/me.c search family
template<class... A> void search(A&&... a) { execute(std::forward<A>(a)...); }

// mirrors the x264 common/me.c refine family
template<class... A> void refine(A&&... a) { execute(std::forward<A>(a)...); }

// mirrors the x264 common/me.c metrics family
template<class... A> void metrics(A&&... a) { execute(std::forward<A>(a)...); }

} // namespace me
} // namespace vk264
