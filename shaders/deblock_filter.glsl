const int tileSamples = 688;
shared int tile[tileSamples * WX_DEBLOCK_TILES];
shared uint thresholds[2];
shared uint edgeThresholds[WX_DEBLOCK_TILES * 6];

void prepareEdgeThreshold(uint mb, int edge, bool left, bool top) {
    int qp = mbQp(mb);
    int other = qp;
    if (edge == 1 && left) other = mbQp(mb - 1u);
    if (edge == 2 && top) other = mbQp(mb - pc.mbWidth);
    int index = 6 * int(gl_LocalInvocationIndex / 32u) + 2 * edge;
    edgeThresholds[index] = deblockThresholds[(qp + other + 1) >> 1];
    edgeThresholds[index + 1] = deblockThresholds[(chromaQp(qp) + chromaQp(other) + 1) >> 1];
}

int tileIndex(int plane, int x, int y) {
    int width = plane == 0 ? 20 : 12;
    int start = plane == 0 ? 0 : 400 + 144 * (plane - 1);
    int tileBase = int(gl_LocalInvocationIndex / 32u) * tileSamples;
    return tileBase + start + (y + 4) * width + x + 4;
}

int reconWord(uint mb, int plane, int x, int y) {
    ivec2 size = ivec2(pc.mbWidth, pc.mbHeight) << (plane == 0 ? 4 : 3);
    ivec2 xy = mbOrigin(mb, plane) + ivec2(x, y);
    return (planeOffset(plane, size) + xy.y * size.x + xy.x) >> 2;
}

ivec2 blockVector(uint mb, ivec2 block) {
    uint index = mb * 5u;
    if ((matches[index].mode & 1u) != 0u) index += uint(1 + (block.x >> 1) + 2 * (block.y >> 1));
    return matches[index].mv;
}

int nonzero(uint mb, ivec2 block) {
    return coefficients[mb * mbCoefficients + 408u + uint(block.y * 4 + block.x)];
}

int strength(uint mb, int along, int edge, bool vertical, bool intra) {
    if (intra) return edge == 0 ? 4 : 3;
    ivec2 q = vertical ? ivec2(edge, along) : ivec2(along, edge);
    ivec2 p = q - (vertical ? ivec2(1, 0) : ivec2(0, 1));
    uint neighbor = mb;
    if (p.x < 0) { p.x = 3; --neighbor; }
    if (p.y < 0) { p.y = 3; neighbor -= pc.mbWidth; }
    if (pc.flatIntra != 0u && (isIntraMb(mb) || isIntraMb(neighbor))) return edge == 0 ? 4 : 3;
    if (nonzero(mb, q) != 0 || nonzero(neighbor, p) != 0) return 2;
    if (referenceIndex(mb) != referenceIndex(neighbor)) return 1;
    ivec2 delta = abs(blockVector(mb, q) - blockVector(neighbor, p));
    return int(any(greaterThanEqual(delta, ivec2(4))));
}

// 8-bit progressive AVC deblocking, H.264 section 8.7.
void filterLine(int pos, int step, int bs, uint table, bool luma) {
    int p0 = tile[pos-step], p1 = tile[pos-2*step];
    int q0 = tile[pos], q1 = tile[pos+step];
    int alpha = int(table & 255u), beta = int((table >> 8u) & 31u);
    int edge = abs(p0 - q0);
    if (edge >= alpha || abs(p1 - p0) >= beta || abs(q1 - q0) >= beta) return;

    int p2 = luma ? tile[pos-3*step] : p0;
    int q2 = luma ? tile[pos+2*step] : q0;
    bool smoothP = luma && abs(p2 - p0) < beta;
    bool smoothQ = luma && abs(q2 - q0) < beta;
    if (bs == 4) {
        bool strong = edge < (alpha >> 2) + 2;
        tile[pos-step] = strong && smoothP ? (p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4) >> 3
                                         : (2*p1 + p0 + q1 + 2) >> 2;
        tile[pos] = strong && smoothQ ? (p1 + 2*p0 + 2*q0 + 2*q1 + q2 + 4) >> 3
                                      : (2*q1 + q0 + p1 + 2) >> 2;
        if (strong && smoothP) {
            tile[pos-2*step] = (p2 + p1 + p0 + q0 + 2) >> 2;
            tile[pos-3*step] = (2*tile[pos-4*step] + 3*p2 + p1 + p0 + q0 + 4) >> 3;
        }
        if (strong && smoothQ) {
            tile[pos+step] = (p0 + q0 + q1 + q2 + 2) >> 2;
            tile[pos+2*step] = (2*tile[pos+3*step] + 3*q2 + q1 + q0 + p0 + 4) >> 3;
        }
    } else {
        int tc0 = int((table >> (8 + 5 * bs)) & 31u);
        int tc = tc0 + (luma ? int(smoothP) + int(smoothQ) : 1);
        int average = (p0 + q0 + 1) >> 1;
        if (smoothP) tile[pos-2*step] = p1 + clamp((p2 + average - 2*p1) >> 1, -tc0, tc0);
        if (smoothQ) tile[pos+step] = q1 + clamp((q2 + average - 2*q1) >> 1, -tc0, tc0);
        int delta = clamp((4*(q0 - p0) + p1 - q1 + 4) >> 3, -tc, tc);
        tile[pos-step] = clamp(p0 + delta, 0, 255);
        tile[pos] = clamp(q0 - delta, 0, 255);
    }
}

void filterEdgeWithStrength(int plane, int along, int edge, bool vertical, int bs) {
    bool luma = plane == 0;
    if (!luma && (edge & 1) != 0) return;
    if (bs == 0) return;
    int start = edge * (luma ? 4 : 2);
    int pos = vertical ? tileIndex(plane, start, along) : tileIndex(plane, along, start);
    int step = vertical ? 1 : (luma ? 20 : 12);
    int component = int(!luma);
    uint table = thresholds[component];
    if (pictureAq()) {
        int side = edge != 0 ? 0 : (vertical ? 1 : 2);
        table = edgeThresholds[6 * int(gl_LocalInvocationIndex / 32u) + 2 * side + component];
    }
    filterLine(pos, step, bs, table, luma);
}

void filterEdge(uint mb, int plane, int along, int edge, bool vertical, bool intra) {
    bool luma = plane == 0;
    if (!luma && (edge & 1) != 0) return;
    int bs = strength(mb, along >> (luma ? 2 : 1), edge, vertical, intra);
    filterEdgeWithStrength(plane, along, edge, vertical, bs);
}

uint packWord(int index) {
    return uint(tile[index]) | (uint(tile[index+1]) << 8) | (uint(tile[index+2]) << 16) | (uint(tile[index+3]) << 24);
}

void loadWord(uint mb, int component, int x, int y) {
    uint original = reconstructed[reconWord(mb, component, x, y)];
    int pos = tileIndex(component, x, y);
    for (int p = 0; p < 4; ++p) tile[pos+p] = int((original >> (8*p)) & 255u);
}

