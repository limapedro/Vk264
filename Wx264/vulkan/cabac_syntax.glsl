// SPDX-License-Identifier: GPL-2.0-or-later
// CABAC macroblock syntax shared by the direct arithmetic coder and the
// parallel bin generator. The includer must provide the Corpus buffer members
// residualBank and fixtures plus these sinks:
//   void emitDecision(int f, int index, int bit);
//   void emitBypass(int f, int bit);
//   void emitUeBypass(int f, int expBits, int value);
//   void emitTerminal(int f);
//   void emitFlush(int f, int frame);
// The syntax itself never observes coder state, so a bin sink reproduces the
// direct coder's bin sequence exactly.

const int chromaOffset = 4+16+16*15;
const int chromaPlaneWords = 2+4+4*15;
const int macroblockWords = chromaOffset+2+2*chromaPlaneWords;

void encodeIntraType(int f, int contexts, int flags, int frame) {
    int context0 = contexts & 63;
    int type = (flags >> 6) & 3;
    if (type <= 1) {
        emitDecision(f, context0, 0);
        return;
    }
    emitDecision(f, context0, 1);
    if (type == 3) {
        emitFlush(f, frame);
        return;
    }
    emitTerminal(f);

    int mode = (flags >> 8) & 7;
    if (mode >= 4) mode = 2;
    int chroma = (flags >> 12) & 3;
    emitDecision(f, (contexts >> 6) & 63, (flags >> 11) & 1);
    emitDecision(f, (contexts >> 12) & 63, int(chroma != 0));
    if (chroma != 0) emitDecision(f, (contexts >> 18) & 63, chroma >> 1);
    emitDecision(f, (contexts >> 24) & 63, mode >> 1);
    emitDecision(f, flags & 63, mode & 1);
}
void encodeIntra4x4Mode(int f, int predictor, int mode) {
    emitDecision(f, 68, int(predictor == mode));
    if (predictor == mode) return;
    if (mode > predictor) --mode;
    for (int i = 0; i < 3; ++i) emitDecision(f, 69, (mode >> i) & 1);
}
void encodeIntraChromaMode(int f, int mode, int neighbors) {
    int context = 64 + int((neighbors & 5) == 5) + int((neighbors & 10) == 10);
    if (mode >= 4) mode = 0;
    emitDecision(f, context, int(mode > 0));
    if (mode > 0) emitDecision(f, 67, int(mode > 1));
    if (mode > 1) emitDecision(f, 67, int(mode > 2));
}

void encodeLumaCbp(int f, int cbp, int left, int top) {
    emitDecision(f, 76-((left >> 1) & 1)-((top >> 1) & 2), cbp & 1);
    emitDecision(f, 76-(cbp & 1)-((top >> 2) & 2), (cbp >> 1) & 1);
    emitDecision(f, 76-((left >> 3) & 1)-((cbp << 1) & 2), (cbp >> 2) & 1);
    emitDecision(f, 76-((cbp >> 2) & 1)-(cbp & 2), (cbp >> 3) & 1);
}
void encodeChromaCbp(int f, int cbp, int left, int top) {
    int a = left & 48, b = top & 48;
    int context = int(a != 0 && left != -1)+2*int(b != 0 && top != -1);
    emitDecision(f, 77+context, int(cbp != 0));
    if (cbp != 0) {
        context = 4+int(a == 32)+2*int(b == 32);
        emitDecision(f, 77+context, cbp >> 1);
    }
}
int encodeQpDelta(int f, int qp, int lastQp, bool emptyIntra16, bool previousHadDelta) {
    if (emptyIntra16 && qp > lastQp) qp = lastQp;
    int delta = qp-lastQp;
    int context = int(previousHadDelta);
    if (delta != 0) {
        const int maxQp = 51;
        int value = delta > 0 ? 2*delta-1 : -2*delta;
        if (value >= maxQp && value != maxQp+1) value = 2*maxQp+1-value;
        do {
            emitDecision(f, 60+context, 1);
            context = 2+(context >> 1);
        } while (--value != 0);
    }
    emitDecision(f, 60+context, 0);
    return qp;
}
void encodeTransformSize(int f, int neighbors, int size8x8) {
    emitDecision(f, 399+neighbors, size8x8);
}
void encodeMacroblockSkip(int f, int neighbors, bool bidirectional, int skip) {
    emitDecision(f, 11+neighbors+(bidirectional ? 13 : 0), skip);
}
int encodeMvdComponent(int f, int axis, int delta, int context) {
    int base = axis == 0 ? 40 : 47;
    emitDecision(f, base+context, int(delta != 0));
    if (delta == 0) return 0;
    int magnitude = abs(delta);
    for (int i = 1; i < min(magnitude, 9); ++i)
        emitDecision(f, base+min(i+2, 6), 1);
    if (magnitude < 9)
        emitDecision(f, base+min(magnitude+2, 6), 0);
    else
        emitUeBypass(f, 3, magnitude-9);
    emitBypass(f, delta >> 31);
    return min(magnitude, 66);
}

const int level1Context[8] = int[8](1, 2, 3, 4, 0, 0, 0, 0);
const int levelGt1Context[8] = int[8](5, 5, 5, 5, 6, 7, 8, 9);
const int levelGt1Context422[8] = int[8](5, 5, 5, 5, 6, 7, 8, 8);
const int level1Transition[8] = int[8](1, 2, 3, 3, 4, 5, 6, 7);
const int levelGt1Transition[8] = int[8](4, 4, 4, 4, 5, 6, 7, 7);

int coefficient(int f, int externalOffset, int index) {
    return externalOffset < 0 ? fixtures[f].coefficients[index] : residualBank[externalOffset+index];
}
void encodeResidual(int f, int category, int field, bool chroma422dc, int externalOffset) {
    int countM1 = chroma422dc ? 7 : countMinusOne[category];
    int last = countM1;
    while (last >= 0 && coefficient(f, externalOffset, last) == 0) --last;

    int contextSig = significantBase[field][category];
    int contextLast = lastBase[field][category];
    int contextLevel = levelBase[category];
    for (int i = 0; i <= last && i < countM1; ++i) {
        int sigOffset = chroma422dc ? significance422[i] : countM1 == 63 ? significant8x8[field][i] : i;
        int lastOffset = chroma422dc ? significance422[i] : countM1 == 63 ? last8x8[i] : i;
        bool nonzero = coefficient(f, externalOffset, i) != 0;
        emitDecision(f, contextSig + sigOffset, int(nonzero));
        if (nonzero) emitDecision(f, contextLast + lastOffset, int(i == last));
    }
    int node = 0;
    for (int i = last; i >= 0; --i) {
        int level = coefficient(f, externalOffset, i);
        if (level == 0) continue;
        int magnitude = abs(level);
        emitDecision(f, contextLevel + level1Context[node], int(magnitude > 1));
        if (magnitude > 1) {
            int context = contextLevel + (chroma422dc ? levelGt1Context422[node] : levelGt1Context[node]);
            for (int repeat = min(magnitude, 15)-2; repeat > 0; --repeat)
                emitDecision(f, context, 1);
            if (magnitude < 15) emitDecision(f, context, 0);
            else emitUeBypass(f, 0, magnitude-15);
            node = levelGt1Transition[node];
        } else {
            node = level1Transition[node];
        }
        emitBypass(f, level >> 31);
    }
}

int lumaCodedFlag(int frameBase, int width, int x4, int y4, bool intra) {
    if (x4 < 0 || y4 < 0) return int(intra);
    int mb = (y4/4)*width+x4/4;
    int raster = (y4%4)*4+(x4%4);
    return (residualBank[frameBase+mb*macroblockWords+3] >> raster) & 1;
}
int chromaAcFlag(int frameBase, int width, int plane, int x4, int y4, bool intra) {
    if (x4 < 0 || y4 < 0) return int(intra);
    int mb = (y4/2)*width+x4/2;
    int block = 2*(y4%2)+x4%2;
    int offset = frameBase+mb*macroblockWords+chromaOffset+2+plane*chromaPlaneWords;
    return (residualBank[offset+1] >> block) & 1;
}
void encodeChroma420Residual(int f, int frameBase, int base, int width,
                             int x, int y, int cbp, bool intra) {
    if (cbp == 0) return;
    for (int plane = 0; plane < 2; ++plane) {
        int offset = base+chromaOffset+2+plane*chromaPlaneWords;
        int left = x == 0 ? int(intra) : residualBank[offset-macroblockWords];
        int top = y == 0 ? int(intra) : residualBank[offset-width*macroblockWords];
        int coded = residualBank[offset];
        emitDecision(f, 97+left+2*top, coded);
        if (coded != 0) encodeResidual(f, 3, 0, false, offset+2);
    }
    if (cbp != 2) return;
    for (int plane = 0; plane < 2; ++plane) {
        int offset = base+chromaOffset+2+plane*chromaPlaneWords;
        for (int b = 0; b < 4; ++b) {
            int x4 = 2*x+b%2, y4 = 2*y+b/2;
            int coded = chromaAcFlag(frameBase, width, plane, x4, y4, intra);
            int left = chromaAcFlag(frameBase, width, plane, x4-1, y4, intra);
            int top = chromaAcFlag(frameBase, width, plane, x4, y4-1, intra);
            emitDecision(f, 101+left+2*top, coded);
            if (coded != 0) encodeResidual(f, 4, 0, false, offset+6+b*15);
        }
    }
}
void encodeIntra16Macroblock(int f, int frameBase, int position, int frame) {
    int mb = position & 65535, width = position >> 16;
    int x = mb%width, y = mb/width;
    int base = frameBase+mb*macroblockWords;
    int mode = residualBank[base];
    int ac = residualBank[base+1], dc = residualBank[base+2];
    if (mb != 0) emitTerminal(f);
    int typeContext = 3 + int(x != 0) + int(y != 0);
    int contexts = typeContext | (6 << 6) | (7 << 12) | (8 << 18) | (9 << 24);
    int chromaCbp = residualBank[base+chromaOffset];
    encodeIntraType(f, contexts,
                    10 | (2 << 6) | (mode << 8) | (ac << 11) | (chromaCbp << 12), frame);
    // The integrated stream uses DC prediction for both chroma planes and constant slice QP.
    encodeIntraChromaMode(f, residualBank[base+chromaOffset+1], 0);
    encodeQpDelta(f, fixtures[f].qp, fixtures[f].qp, false, false);
    int leftDc = x == 0 ? 1 : residualBank[base-macroblockWords+2];
    int topDc = y == 0 ? 1 : residualBank[base-width*macroblockWords+2];
    emitDecision(f, 85 + leftDc + 2*topDc, dc);
    if (dc != 0) encodeResidual(f, 0, 0, false, base+4);
    for (int b = 0; ac != 0 && b < 16; ++b) {
        int raster = blockToRaster[b];
        int x4 = 4*x+raster%4, y4 = 4*y+raster/4;
        int coded = lumaCodedFlag(frameBase, width, x4, y4, true);
        int leftAc = lumaCodedFlag(frameBase, width, x4-1, y4, true);
        int topAc = lumaCodedFlag(frameBase, width, x4, y4-1, true);
        emitDecision(f, 89 + leftAc + 2*topAc, coded);
        if (coded != 0) encodeResidual(f, 1, 0, false, base+20+b*15);
    }
    encodeChroma420Residual(f, frameBase, base, width, x, y, chromaCbp, true);
}

int codedBlockPatternAt(int base) {
    return residualBank[base+1] | (residualBank[base+chromaOffset] << 4);
}

ivec2 interMotionAt(int frameBase, int mb) {
    int base = frameBase+mb*macroblockWords;
    return ivec2(residualBank[base], residualBank[base+2]);
}

ivec2 interMotionDelta(int frameBase, int width, int mb) {
    int x = mb%width, y = mb/width;
    bool hasLeft = x > 0, hasTop = y > 0;
    bool hasRightTop = hasTop && x+1 < width;
    bool hasLeftTop = hasTop && hasLeft;
    ivec2 left = hasLeft ? interMotionAt(frameBase, mb-1) : ivec2(0);
    ivec2 top = hasTop ? interMotionAt(frameBase, mb-width) : ivec2(0);
    ivec2 rightTop = hasRightTop ? interMotionAt(frameBase, mb-width+1) : ivec2(0);
    ivec2 leftTop = hasLeftTop ? interMotionAt(frameBase, mb-width-1) : ivec2(0);
    ivec4 refs = ivec4(hasLeft ? 0 : -2, hasTop ? 0 : -2, hasRightTop ? 0 : -2, hasLeftTop ? 0 : -2);
    ivec2 predictor = predictMotion16(0, refs, left, top, rightTop, leftTop);
    return interMotionAt(frameBase, mb)-predictor;
}

void encodeInter16Macroblock(int f, int frameBase, int position) {
    int mb = position & 65535, width = position >> 16;
    int x = mb%width, y = mb/width;
    if (mb != 0) emitTerminal(f);
    // P_L0 16x16 with one active reference.
    encodeMacroblockSkip(f, int(x != 0)+int(y != 0), false, 0);
    emitDecision(f, 14, 0);
    emitDecision(f, 15, 0);
    emitDecision(f, 16, 0);
    ivec2 delta = interMotionDelta(frameBase, width, mb);
    ivec2 leftMagnitude = x == 0 ? ivec2(0) : min(abs(interMotionDelta(frameBase, width, mb-1)), ivec2(66));
    ivec2 topMagnitude = y == 0 ? ivec2(0) : min(abs(interMotionDelta(frameBase, width, mb-width)), ivec2(66));
    ivec2 contexts = motionDeltaContexts(leftMagnitude, topMagnitude);
    for (int axis = 0; axis < 2; ++axis)
        encodeMvdComponent(f, axis, delta[axis], contexts[axis]);

    int base = frameBase+mb*macroblockWords;
    int lumaCbp = residualBank[base+1], chromaCbp = residualBank[base+chromaOffset];
    int left = x == 0 ? -1 : codedBlockPatternAt(base-macroblockWords);
    int top = y == 0 ? -1 : codedBlockPatternAt(base-width*macroblockWords);
    encodeLumaCbp(f, lumaCbp, left, top);
    encodeChromaCbp(f, chromaCbp, left, top);
    int size8x8 = (residualBank[base+3] >> 16) & 1;
    if (lumaCbp != 0 && (residualBank[base+3] & 131072) != 0) {
        int left8 = x == 0 ? 0 : (residualBank[base-macroblockWords+3] >> 16) & 1;
        int top8 = y == 0 ? 0 : (residualBank[base-width*macroblockWords+3] >> 16) & 1;
        encodeTransformSize(f, left8+top8, size8x8);
    }
    if ((lumaCbp | chromaCbp) != 0)
        encodeQpDelta(f, fixtures[f].qp, fixtures[f].qp, false, false);
    if (size8x8 != 0) {
        for (int b = 0; b < 4; ++b)
            if ((lumaCbp & (1 << b)) != 0)
                encodeResidual(f, 5, 0, false, base+4+b*64);
    }
    for (int b = 0; size8x8 == 0 && b < 16; ++b) {
        if ((lumaCbp & (1 << (b/4))) == 0) continue;
        int raster = blockToRaster[b];
        int x4 = 4*x+raster%4, y4 = 4*y+raster/4;
        int coded = lumaCodedFlag(frameBase, width, x4, y4, false);
        int leftCoded = lumaCodedFlag(frameBase, width, x4-1, y4, false);
        int topCoded = lumaCodedFlag(frameBase, width, x4, y4-1, false);
        emitDecision(f, 93+leftCoded+2*topCoded, coded);
        if (coded != 0) encodeResidual(f, 2, 0, false, base+4+b*16);
    }
    encodeChroma420Residual(f, frameBase, base, width, x, y, chromaCbp, false);
}
