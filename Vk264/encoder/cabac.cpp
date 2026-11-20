// Vk264 mirror of x264/encoder/cabac.c
// shader: cabac.comp  (plus cabac_bins.comp for bin generation)
// x264 C CABAC coder is the byte-exact oracle in upstream_cabac*.inc.
// One shader launch per MB/slice: CABAC arithmetic state is serial, so it
// cannot be parallelized per bin. Textually included into the parity TU.
namespace vk264 {
namespace cabac {

template<class Cases, class Tables>
void encode(const char* shader, Cases& cases, const Tables& tables) {
    execute(shader, cases, 1, tables);
}

} // namespace cabac
} // namespace vk264
