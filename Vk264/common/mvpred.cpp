// Vk264 mirror of x264/common/mvpred.c
// shaders: mv_predict.comp
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace mvpred {

// mirrors the x264 common/mvpred.c predict family
template<class... A> void predict(A&&... a) { execute(std::forward<A>(a)...); }

} // namespace mvpred
} // namespace vk264
