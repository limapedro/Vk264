const int lumaOrder[16] = int[16](0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15);
const uint interPattern[48] = uint[48](0,2,3,7,4,8,17,13,5,18,9,14,10,15,16,11,
    1,32,33,36,34,37,44,40,35,45,38,41,39,42,43,19,6,24,25,20,26,21,46,28,
    27,47,22,29,23,30,31,12);

int nz(uint base, int block) { return coefficients[base + 408u + uint(block)]; }

int context(uint base, int block, bool hasLeftMb, bool hasTopMb) {
    int wide = block < 16 ? 4 : 2;
    int x = block % wide, y = (block % (wide * wide)) / wide;
    int left = 0, top = 0, count = 0;
    if (x != 0) {
        left = nz(base, block - 1);
        ++count;
    } else if (hasLeftMb) {
        left = nz(base - mbCoefficients, block + wide - 1);
        ++count;
    }
    if (y != 0) {
        top = nz(base, block - wide);
        ++count;
    } else if (hasTopMb) {
        top = nz(base - pc.mbWidth * mbCoefficients, block + wide * (wide - 1));
        ++count;
    }
    return count == 2 ? (left + top + 1) >> 1 : left + top;
}

uint patterns(uint base) {
    uint pattern = 0u;
    for (int i = 0; i < 16; ++i) if (nz(base, lumaOrder[i]) != 0) pattern |= 1u << (i / 4);
    for (int i = 16; i < 24; ++i) if (nz(base, i) != 0) return pattern | 32u;
    for (uint i = 400u; i < 408u; ++i) if (coefficients[base + i] != 0) return pattern | 16u;
    return pattern;
}

void writeMvd(ivec2 mv, ivec2 predictor) {
    ivec2 delta = mv - predictor;
    se(delta.x);
    se(delta.y);
}

void writeMacroblock(uint base, uint pattern, bool intra, bool left, bool top, uint typeOffset, int qpDelta) {
    uint chroma = pattern >> 4;
    uint luma = pattern & 15u;
    if (intra) {
        // SHADER: H.264 table 7-11 and x264 encoder/cavlc.c:310-311:
        // mb_type = offset + 1 + I16_pred_mode + 4*cbp_chroma + 12*(cbp_luma!=0).
        // Previously the mode was hard-coded to DC (1 + 2 = 3).
        uint mode = intra16Mode(base / mbCoefficients);
        ue(typeOffset + 1u + mode + 4u * chroma + (luma != 0u ? 12u : 0u));
        ue(0u); // chroma DC prediction
    } else {
        ue(interPattern[pattern]);
    }
    if (intra || pattern != 0u) se(qpDelta);
    if (intra) writeResidual(base + 384u, context(base, 0, left, top), 0, 16, false);
    for (int i = 0; i < 16; ++i) {
        if ((luma & (1u << (i / 4))) == 0u && (!intra || luma == 0u)) continue;
        int block = lumaOrder[i];
        int dc = int(intra);
        writeResidual(base + uint(16 * block), context(base, block, left, top), dc, 16 - dc, false);
    }
    if (chroma != 0u) {
        writeResidual(base + 400u, -1, 0, 4, true);
        writeResidual(base + 404u, -1, 0, 4, true);
    }
    if (chroma == 2u) for (int block = 16; block < 24; ++block)
        writeResidual(base + uint(16 * block), context(base, block, left, top), 1, 15, false);
}

ivec2 interPredictor(uint mb, bool top, bool left, ivec2 a, out ivec2 skipVector) {
    skipVector = ivec2(0);
    if (!top) return a;
    uint above = mb - pc.mbWidth;
    bool topRight = mb % pc.mbWidth + 1u < pc.mbWidth;
    ivec2 b = matches[above * 5u].mv;
    if (pc.mbWidth == 1u) return b;
    ivec2 c = topRight ? matches[(above + 1u) * 5u].mv :
        (left ? matches[(above - 1u) * 5u].mv : ivec2(0));
    ivec2 median = medianMv(a, b, c);
    if (left && any(notEqual(a, ivec2(0))) && any(notEqual(b, ivec2(0)))) skipVector = median;
    return median;
}

// In the two-row P path all partitions are 16x16. In the default list,
// reference zero is the newest picture and greater indexes are older.
struct Neighbor { ivec2 mv; int ref; };

Neighbor neighborAt(uint mb) {
    if (isIntraMb(mb)) return Neighbor(ivec2(0), -1);
    return Neighbor(matches[mb * 5u].mv, int(referenceIndex(mb)));
}

ivec2 pairedPredictor(uint mb, uint local, int ref, bool skip) {
    bool left = leftAvailable(mb, local), top = topAvailable(local);
    if (skip && (!left || !top)) return ivec2(0);
    Neighbor absent = Neighbor(ivec2(0), -1);
    Neighbor a = left ? neighborAt(mb - 1u) : absent;
    if (!top) return a.mv;
    uint above = mb - pc.mbWidth;
    Neighbor b = neighborAt(above);
    if (skip && ((a.ref == 0 && all(equal(a.mv, ivec2(0)))) ||
                 (b.ref == 0 && all(equal(b.mv, ivec2(0)))))) return ivec2(0);
    bool topRight = mb % pc.mbWidth + 1u < pc.mbWidth;
    Neighbor c = topRight ? neighborAt(above + 1u) : (left ? neighborAt(above - 1u) : absent);
    bool matchA = a.ref == ref, matchB = b.ref == ref, matchC = c.ref == ref;
    if (int(matchA) + int(matchB) + int(matchC) == 1) return matchA ? a.mv : (matchB ? b.mv : c.mv);
    return medianMv(a.mv, b.mv, c.mv);
}

