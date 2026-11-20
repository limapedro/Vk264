// Vk264 mirror of x264/encoder/macroblock.c
// shaders: intra16.comp, intra_chroma.comp, inter8x8.comp
// Textually included into the parity TU by ../src/dct_parity.cpp.
namespace vk264 {
namespace macroblock {

// mirrors the x264 encoder/macroblock.c encode family
template<class... A> void encode(A&&... a) { execute(std::forward<A>(a)...); }

// mirrors the x264 encoder/macroblock.c inter family
template<class... A> void inter(A&&... a) { execute(std::forward<A>(a)...); }

// mirrors the x264 encoder/macroblock.c intraChroma family
template<class... A> void intraChroma(A&&... a) { execute(std::forward<A>(a)...); }

} // namespace macroblock
} // namespace vk264
