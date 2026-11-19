// Shared CPU/GPU ABI. Buffers use packed little-endian bytes where indicated.
layout(push_constant) uniform Frame {
    uint width, height, mbWidth, mbHeight;
    uint slicesPerRow, sliceCount, slotWords, frameIndex;
    uint gop, fps, initialQp, targetBits;
    uint motionRadius, subpel, partitions, deblock;
    uint sliceRows, satdCandidates;
    uint references, levelIdc, referenceStrideWords, previousWords, deblockWave, flatIntra;
    uint aq;
    uint fpsNumerator, fpsDenominator, videoSignal;
} pc;

layout(std430, binding=0) readonly buffer Source { uint source[]; };
layout(std430, binding=1) readonly buffer Previous { uint previous[]; };
#ifndef VK_RECONSTRUCTION_ACCESS
#define VK_RECONSTRUCTION_ACCESS
#endif
layout(std430, binding=2) VK_RECONSTRUCTION_ACCESS buffer Reconstruction { uint reconstructed[]; };
layout(std430, binding=3) buffer Residual { int coefficients[]; };
layout(std430, binding=4) buffer SliceStorage { uint scratchWords[]; };
layout(std430, binding=5) buffer SliceRecords { uvec2 slices[]; }; // words, offset
layout(std430, binding=6) buffer Output { uint outputWords[]; };  // 4 words of metadata, then Annex B
layout(std430, binding=7) buffer RateState { int interQpOffset, bitDebt, idrQpOffset; } rc;
layout(std430, binding=8) readonly buffer CodeTables { uint codes[]; };
struct MotionMatch { ivec2 mv; uint cost; uint mode; };
#ifndef VK_MOTION_ACCESS
#define VK_MOTION_ACCESS
#endif
layout(std430, binding=9) VK_MOTION_ACCESS buffer Motion { MotionMatch matches[]; };
layout(std430, binding=10) buffer HalfPel { uint halfPel[]; };
layout(std430, binding=11) buffer QuarterLuma { uint quarterLuma[]; };
layout(std430, binding=12) buffer MacroblockSyntax { uvec2 mbSyntax[]; }; // CBP/skip/QP, proposed QP then body bits
layout(std430, binding=13) buffer MacroblockBodies { uint mbBody[]; };

const uint sliceWidth = 64u;
const uint maxPairedSliceMbs = 512u; // two rows of up to 256 macroblocks
const uint mbCoefficients = 432u; // 384 AC/ordinary, 16 Y DC, 8 UV DC, 24 nonzero counts
const uint mbBodyWords = 768u;
const uint mbSkipFlag = 64u;
const uint mbIntraMode = 32u;

bool idr() { return (pc.frameIndex % pc.gop) == 0u; }
bool separateIdrBudget() { return idr() && pc.gop > 1u; }
int frameQp() {
    if (pc.targetBits == 0u) return int(pc.initialQp);
    int offset = separateIdrBudget() ? rc.idrQpOffset : rc.interQpOffset;
    return clamp(int(pc.initialQp) + offset, 0, 51);
}
bool pairedRows() { return pc.sliceRows == 2u && !idr() && pc.partitions == 0u; }
bool pictureAq() { return pc.aq != 0u && pairedRows(); }
int mbQp(uint mb) { return int(mbSyntax[mb].x >> 14u); }
int mbQpDelta(uint flags) { return bitfieldExtract(int(flags), 7, 7); }
uint firstMb(uint slice) {
    if (pairedRows()) return slice * 2u * pc.mbWidth;
    return (slice / pc.slicesPerRow) * pc.mbWidth + (slice % pc.slicesPerRow) * sliceWidth;
}
uint sliceLength(uint slice) {
    if (pairedRows()) return pc.mbWidth * min(2u, pc.mbHeight - 2u * slice);
    return min(sliceWidth, pc.mbWidth - (slice % pc.slicesPerRow) * sliceWidth);
}
bool leftAvailable(uint mb, uint inSlice) { return inSlice != 0u && mb % pc.mbWidth != 0u; }
bool topAvailable(uint inSlice) { return pairedRows() && inSlice >= pc.mbWidth; }
uint offsetInPair(uint mb) { return mb % (2u * pc.mbWidth); }
ivec2 medianMv(ivec2 a, ivec2 b, ivec2 c) { return min(max(a, b), max(min(a, b), c)); }
bool spatialMotion() { return pc.partitions == 0u && pc.subpel != 0u && pc.motionRadius >= 4u; }
int hierarchicalMotionLimit() { return int(pc.motionRadius) * (pc.satdCandidates >= 4u ? 16 : 8); }
uint motionSeedIndex(uint mb) { return 5u * pc.mbWidth * pc.mbHeight + mb; }
uint referenceIndex(uint mb) { return (matches[5u * mb].mode >> 1u) & 15u; }
bool isIntraMb(uint mb) { return (matches[5u * mb].mode & mbIntraMode) != 0u; }
uint availableReferences() { return min(pc.references, pc.frameIndex % pc.gop); }

uint referenceOffset(uint index) {
    uint slots = pc.references + 1u;
    uint previousSlot = pc.previousWords / pc.referenceStrideWords;
    return ((previousSlot + slots - index) % slots) * pc.referenceStrideWords;
}

int planeOffset(int plane, ivec2 size) {
    return plane == 0 ? 0 : (plane + 3) * size.x * size.y;
}

int srcPixel(int plane, ivec2 xy) {
    ivec2 size = ivec2(pc.width, pc.height) >> int(plane != 0);
    xy = clamp(xy, ivec2(0), size - 1);
    int offset = planeOffset(plane, size) + xy.y * size.x + xy.x;
    return int((source[offset >> 2] >> (8 * (offset & 3))) & 255u);
}

int refPixelAt(int plane, ivec2 xy, uint address) {
    ivec2 size = ivec2(pc.mbWidth, pc.mbHeight) << (plane == 0 ? 4 : 3);
    xy = clamp(xy, ivec2(0), size - 1);
    int offset = planeOffset(plane, size) + xy.y * size.x + xy.x;
    return int((previous[address + uint(offset >> 2)] >> (8 * (offset & 3))) & 255u);
}

int refPixel(int plane, ivec2 xy) { return refPixelAt(plane, xy, pc.previousWords); }

ivec2 quarterSize() { return ivec2(pc.mbWidth, pc.mbHeight) * 4; }

uint quarterAt(ivec2 xy) {
    ivec2 size = quarterSize();
    xy = clamp(xy, ivec2(0), size - 1);
    return quarterLuma[xy.y * size.x + xy.x];
}

ivec2 halfPelSize() { return (ivec2(pc.mbWidth, pc.mbHeight) << 4) + 8; }

int lumaHalf(ivec2 xy, int plane) {
    if (plane == 0) return refPixel(0, xy);
    ivec2 size = halfPelSize();
    ivec2 ij = clamp(xy + 4, ivec2(0), size - 1);
    return int((halfPel[ij.y * size.x + ij.x] >> (8 * (plane - 1))) & 255u);
}

int lumaPrediction(ivec2 xy, ivec2 mv) {
    ivec2 base = xy + (mv >> 2), frac = mv & 3;
    bool oddX = (frac.x & 1) != 0, oddY = (frac.y & 1) != 0;
    int horizontal = (frac.x >> 1), vertical = (frac.y >> 1) * 2;
    if (!oddX && !oddY) return lumaHalf(base, horizontal + vertical);
    int a, b;
    if (oddX && oddY) {
        a = lumaHalf(base + ivec2(0, int(frac.y == 3)), 1);
        b = lumaHalf(base + ivec2(int(frac.x == 3), 0), 2);
    } else if (oddX) {
        a = lumaHalf(base, horizontal + vertical);
        b = lumaHalf(base + ivec2(horizontal, 0), (1 - horizontal) + vertical);
    } else {
        a = lumaHalf(base, horizontal + vertical);
        b = lumaHalf(base + ivec2(0, vertical / 2), horizontal + (2 - vertical));
    }
    return (a + b + 1) >> 1;
}

ivec2 mbOrigin(uint mb, int plane) {
    return ivec2(mb % pc.mbWidth, mb / pc.mbWidth) << (plane == 0 ? 4 : 3);
}

int reconPixel(int plane, ivec2 xy) {
    ivec2 size = ivec2(pc.mbWidth, pc.mbHeight) << (plane == 0 ? 4 : 3);
    uint offset = uint(planeOffset(plane, size) + xy.y * size.x + xy.x);
    return int((reconstructed[offset >> 2u] >> (8u * (offset & 3u))) & 255u);
}

// Both luma and chroma receive a vector expressed in quarter luma pixels.
int prediction(int plane, ivec2 xy, ivec2 mv, uint address) {
    if (plane == 0) return address == pc.previousWords ? lumaPrediction(xy, mv)
                                                      : refPixelAt(0, xy + (mv >> 2), address);
    ivec2 base = xy + (mv >> 3);
    ivec2 frac = mv & 7;
    int a = refPixelAt(plane, base, address);
    if (all(equal(frac, ivec2(0)))) return a;
    int b = refPixelAt(plane, base + ivec2(1, 0), address);
    int c = refPixelAt(plane, base + ivec2(0, 1), address);
    int d = refPixelAt(plane, base + 1, address);
    return ((8 - frac.x) * ((8 - frac.y) * a + frac.y * c) +
            frac.x * ((8 - frac.y) * b + frac.y * d) + 32) >> 6;
}
