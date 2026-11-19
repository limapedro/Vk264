const int quantScale[18] = int[18](13107,8066,5243,11916,7490,4660,
    10082,6554,4194,9362,5825,3647,8192,5243,3355,7282,4559,2893);
const int inverseScale[18] = int[18](10,13,16,11,14,18,13,16,20,
    14,18,23,16,20,25,18,23,29);
const int chromaMap[22] = int[22](29,30,31,32,32,33,34,34,35,35,36,36,37,37,37,38,38,38,39,39,39,39);

int chromaQp(int qp) { return qp < 30 ? qp : chromaMap[qp - 30]; }
int category(int index) { return (index & 1) + ((index >> 2) & 1); }

int quantize(int value, int qp, int index, int extraShift, bool intra) {
    int shift = 15 + qp / 6 + extraShift;
    // Preserve small inter DC corrections to the predictor's block average.
    // AC coefficients keep their wider dead zone to limit residual bitrate.
    int divisor = intra ? 3 : (index == 0 ? 2 : 6);
    int rounding = (1 << shift) / divisor;
    int magnitude = (abs(value) * quantScale[3 * (qp % 6) + category(index)] + rounding) >> shift;
    return value < 0 ? -magnitude : magnitude;
}

int dequantize(int value, int qp, int index) {
    return value * inverseScale[3 * (qp % 6) + category(index)] * (1 << (qp / 6));
}

ivec4 forward4(ivec4 v) {
    int a = v.x + v.w, b = v.y + v.z;
    int c = v.y - v.z, d = v.x - v.w;
    return ivec4(a + b, 2 * d + c, a - b, d - 2 * c);
}

ivec4 inverse4(ivec4 v) {
    int a = v.x + v.z, b = v.x - v.z;
    int c = (v.y >> 1) - v.w, d = v.y + (v.w >> 1);
    return ivec4(a + d, b + c, b - c, a - d);
}

ivec4 hadamard4(ivec4 v) {
    int a = v.x + v.w, b = v.y + v.z;
    int c = v.y - v.z, d = v.x - v.w;
    return ivec4(a + b, d + c, a - b, d - c);
}

ivec4 hadamard2x2(ivec4 v) {
    int a = v.x + v.y, b = v.z + v.w;
    int c = v.x - v.y, d = v.z - v.w;
    return ivec4(a + b, c + d, a - b, c - d);
}

void inverseBlock(inout ivec4 rows[4]) {
    for (int y = 0; y < 4; ++y) rows[y] = inverse4(rows[y]);
    for (int x = 0; x < 4; ++x) {
        ivec4 v = inverse4(ivec4(rows[0][x], rows[1][x], rows[2][x], rows[3][x]));
        for (int y = 0; y < 4; ++y) rows[y][x] = (v[y] + 32) >> 6;
    }
}
