uint writeSlot, wordCount, packedWord, packedBytes, accumulator, pendingBits, zeroBytes;
bool overflow;

void flushWord() {
    if (wordCount < pc.slotWords) scratchWords[writeSlot * pc.slotWords + wordCount] = packedWord;
    else overflow = true;
    ++wordCount;
    packedWord = 0u;
    packedBytes = 0u;
}

void byteOut(uint value) {
    packedWord |= (value & 255u) << (8u * packedBytes++);
    if (packedBytes == 4u) flushWord();
}

void rbspByte(uint value) {
    if (zeroBytes == 2u && value <= 3u) {
        byteOut(3u);
        zeroBytes = 0u;
    }
    byteOut(value);
    zeroBytes = value == 0u ? zeroBytes + 1u : 0u;
}

void bits(uint value, int count) {
#ifdef VK_MACROBLOCK_BODY
    while (count > 0) {
        uint take = min(32u - pendingBits, uint(count));
        count -= int(take);
        uint mask = take == 32u ? ~0u : (1u << take) - 1u;
        accumulator |= ((value >> count) & mask) << (32u - pendingBits - take);
        pendingBits += take;
        if (pendingBits == 32u) {
            if (wordCount < mbBodyWords) mbBody[writeSlot * mbBodyWords + wordCount] = accumulator;
            else overflow = true;
            ++wordCount;
            pendingBits = accumulator = 0u;
        }
    }
#else
    while (count > 0) {
        uint take = min(8u - pendingBits, uint(count));
        count -= int(take);
        accumulator = (accumulator << take) | ((value >> count) & ((1u << take) - 1u));
        pendingBits += take;
        if (pendingBits == 8u) {
            rbspByte(accumulator);
            pendingBits = 0u;
            accumulator = 0u;
        }
    }
#endif
}

void ue(uint x) {
    uint value = x + 1u;
    int order = findMSB(value);
    bits(0u, order);
    bits(value, order + 1);
}

void se(int x) { ue(uint(x <= 0 ? -2 * x : 2 * x - 1)); }

void nal(uint header) {
    byteOut(0u); byteOut(0u); byteOut(0u); byteOut(1u); byteOut(header);
    zeroBytes = 0u;
}

void finishRbsp() {
    bits(1u, 1);
    bits(0u, int((8u - pendingBits) & 7u));
}

void prefix(bool intra) {
    nal(0x09u);
    bits(intra ? 0u : 1u, 3);
    finishRbsp();
    if (!intra) return;

    nal(0x67u);
    bits(100u, 8); bits(0u, 8); bits(pc.levelIdc, 8);
    ue(0u); ue(1u); ue(0u); ue(0u); bits(0u, 2); // 4:2:0 8-bit, default scaling
    ue(pc.gop > 256u ? 12u : 4u); ue(2u); ue(pc.references); bits(0u, 1); // frame_num, POC type 2
    ue(pc.mbWidth - 1u); ue(pc.mbHeight - 1u);
    bits(1u, 1); bits(1u, 1); // progressive, direct_8x8_inference
    uint right = pc.mbWidth * 16u - pc.width, bottom = pc.mbHeight * 16u - pc.height;
    bits(uint((right | bottom) != 0u), 1);
    if ((right | bottom) != 0u) { ue(0u); ue(right / 2u); ue(0u); ue(bottom / 2u); }
    bits(1u, 1); // VUI present
    bits(0u, 2); // aspect ratio and overscan absent
    bits(uint(pc.videoSignal != 0u), 1);
    if (pc.videoSignal != 0u) {
        bits(5u, 3); bits(pc.videoSignal & 1u, 1); // unspecified video format, full range flag
        bits(1u, 1);
        bits((pc.videoSignal >> 8u) & 255u, 8);
        bits((pc.videoSignal >> 16u) & 255u, 8);
        bits(pc.videoSignal >> 24u, 8);
    }
    bits(0u, 1); // chroma location absent
    bits(1u, 1); bits(pc.fpsDenominator, 32); bits(2u * pc.fpsNumerator, 32); bits(1u, 1); // fixed rate
    bits(0u, 4); // HRD, pic_struct and bitstream restrictions absent
    finishRbsp();

    nal(0x68u);
    ue(0u); ue(0u); bits(0u, 2); // PPS and SPS IDs, CAVLC, no bottom field order
    ue(0u); ue(0u); ue(0u); bits(0u, 3); // one slice group, one reference, no weighting
    se(0); se(0); se(0);
    bits(1u, 1); bits(pc.flatIntra, 1); bits(0u, 1); // deblock control, constrained intra, no redundancy
    finishRbsp();
}

void sliceHeader(uint first, bool intra, uint activeRefs) {
    nal(intra ? 0x65u : 0x41u);
    ue(first); ue(intra ? 7u : 5u); ue(0u);
    bits(pc.frameIndex % pc.gop, pc.gop > 256u ? 16 : 8);
    if (intra) {
        ue((pc.frameIndex / pc.gop) & 1u);
        bits(0u, 2); // keep prior output, short-term reference
    } else {
        bits(uint(activeRefs > 1u), 1);
        if (activeRefs > 1u) ue(activeRefs - 1u);
        bits(0u, 2); // default reference order, sliding-window marking
    }
    se(frameQp() - 26);
    ue(pc.deblock == 2u ? 0u : (pc.deblock == 1u ? 2u : 1u));
    if (pc.deblock != 0u) { se(0); se(0); }
}

const int zigzag[16] = int[16](0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15);

#include "../vulkan/cavlc_residual.glsl"

void writeResidual(uint base, int nc, int first, int count, bool chromaDc) {
    int samples[16];
    for (int i = 0; i < count; ++i)
        samples[i] = coefficients[base + uint(chromaDc ? i : zigzag[first + i])];
    writeCavlcResidual(samples, nc, count, chromaDc);
}
