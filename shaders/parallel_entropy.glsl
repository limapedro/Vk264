// Shared helpers for the parallel CAVLC slice-assembly path.
// The Slices buffer is laid out (all offsets in uint words):
//   [0, slotCount*slotWords)                       final NAL words (region 0, assembled by assemble.comp)
//   [slotCount*slotWords, 2*slotCount*slotWords)   raw RBSP words (no emulation prevention)
//   [2*slotCount*slotWords, ...)                   one 10-bit summary per 64-byte chunk, per slot
//   ...                                            12 metadata words per slot
//   ...                                            one bit offset per macroblock
// All regions are zeroed with vkCmdFillBuffer before the frame's entropy passes.

const uint parallelMetaWords = 12u;
const uint parallelChunkBytes = 64u;
const uint parallelFlagActive = 1u;
const uint parallelFlagIntra = 2u;
const uint parallelFlagOverflow = 4u;

// Must match Impl::parallelCapable in gpu.cpp: bodies are only allocated for
// the two-row 16x16 mode.  Other modes keep the original inline entropy path.
bool parallelCapable() { return pc.sliceRows >= 2u && pc.partitions == 0u && pc.gop > 1u; }

uint parallelSlotCount() { return pc.sliceCount + 1u; }
uint parallelSummaryWords() { return (pc.slotWords + 15u) / 16u; }
uint parallelRawBase(uint slot) { return parallelSlotCount() * pc.slotWords + slot * pc.slotWords; }
uint parallelSummaryBase(uint slot) { return 2u * parallelSlotCount() * pc.slotWords + slot * parallelSummaryWords(); }
uint parallelMetaBase(uint slot) {
    return 2u * parallelSlotCount() * pc.slotWords +
           parallelSlotCount() * parallelSummaryWords() + slot * parallelMetaWords;
}
uint parallelOffsetBase() {
    return 2u * parallelSlotCount() * pc.slotWords +
           parallelSlotCount() * parallelSummaryWords() +
           parallelSlotCount() * parallelMetaWords;
}

uint parallelSlotForMb(uint mb) {
    if (idrWholeFrame()) return 1u;
    if (pairedRows()) return mb / (pc.sliceRows * pc.mbWidth) + 1u;
    return 0u;
}
bool parallelActive(uint slot) { return slot != 0u && (scratchWords[parallelMetaBase(slot) + 4u] & parallelFlagActive) != 0u; }
bool parallelOverflow(uint slot) { return (scratchWords[parallelMetaBase(slot) + 4u] & parallelFlagOverflow) != 0u; }

// MSB-first bit writer into the raw RBSP byte array.  bit position 0 is the
// most significant bit of word 0.  Atomics let independent macroblocks share
// the boundary word without mutual exclusion.
uint rawWordBase, rawPos;

void rawEmit(uint value, int count) {
    while (count > 0) {
        uint within = rawPos & 31u;
        int take = int(min(uint(count), 32u - within));
        uint shifted = take == 32 ? value : ((value >> (count - take)) & ((1u << take) - 1u));
        atomicOr(scratchWords[rawWordBase + (rawPos >> 5u)], shifted << (32u - within - uint(take)));
        rawPos += uint(take);
        count -= take;
    }
}

void rawUe(uint x) {
    uint value = x + 1u;
    int order = findMSB(value);
    rawEmit(0u, order);
    rawEmit(value, order + 1);
}
void rawSe(int x) { rawUe(uint(x <= 0 ? -2 * x : 2 * x - 1)); }

// Mirrors bitstream.glsl::sliceHeader without the start code / NAL header,
// which the emulation-prevention pass emits separately.
void rawSliceHeader(uint first, bool intra, uint activeRefs) {
    rawUe(first); rawUe(intra ? 7u : 5u); rawUe(0u);
    rawEmit(pc.frameIndex % pc.gop, pc.gop > 256u ? 16 : 8);
    if (intra) {
        rawUe((pc.frameIndex / pc.gop) & 1u);
        rawEmit(0u, 2);
    } else {
        rawEmit(uint(activeRefs > 1u), 1);
        if (activeRefs > 1u) rawUe(activeRefs - 1u);
        rawEmit(0u, 2);
    }
    rawSe(frameQp() - 26);
    rawUe(pc.deblock == 2u ? 0u : (pc.deblock == 1u ? 2u : 1u));
    if (pc.deblock != 0u) { rawSe(0); rawSe(0); }
}

void rawFinish() {
    rawEmit(1u, 1);
    rawEmit(0u, int((8u - (rawPos & 7u)) & 7u));
}
