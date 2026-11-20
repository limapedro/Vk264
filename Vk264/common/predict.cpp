// Vk264 mirror of x264/common/predict.c
// shaders: predict_intra.comp, predict_directional.comp
// x264 C functions (x264_predict_4x4_*, x264_predict_8x8*, ...) are the
// byte-exact oracle in upstream_predict.inc. Textually included into the
// parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace predict {

template<class Cases>
void intra(const char* shader, Cases& cases) { execute(shader, cases); }

template<class Cases>
void directional(const char* shader, Cases& cases) { execute(shader, cases); }

} // namespace predict
} // namespace vk264
