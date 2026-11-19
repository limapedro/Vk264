// SPDX-License-Identifier: GPL-2.0-or-later
void inverseChromaDc(int coefficients[8], int scale, bool chroma422, out int restored[8]) {
    if (chroma422) {
        int transformed[8];
        chromaHadamard2x4(coefficients, transformed);
        for (int i = 0; i < 8; ++i)
            restored[i] = bitfieldExtract((transformed[i] * scale + 2080) >> 6, 0, 16);
    } else {
        ivec4 transformed = chromaHadamard2x2(ivec4(coefficients[0], coefficients[1],
                                                   coefficients[2], coefficients[3]));
        for (int i = 0; i < 4; ++i)
            restored[i] = bitfieldExtract(((transformed[i] * scale) >> 5) + 32, 0, 16);
    }
}

int optimizeChromaDc(inout int coefficients[8], int scale, bool chroma422) {
    int count = chroma422 ? 8 : 4;
    int original[8];
    inverseChromaDc(coefficients, scale, chroma422, original);
    int combined = 0;
    for (int i = 0; i < count; ++i) combined |= original[i];
    if ((combined >> 6) == 0) return 0;

    int nonzero = 0;
    for (int coefficient = count - 1; coefficient >= 0; --coefficient) {
        int level = coefficients[coefficient];
        int sign = (level >> 31) | 1;
        while (level != 0) {
            coefficients[coefficient] = level - sign;
            int restored[8];
            inverseChromaDc(coefficients, scale, chroma422, restored);
            int difference = 0;
            for (int i = 0; i < count; ++i) difference |= original[i] ^ restored[i];
            if ((difference >> 6) != 0) {
                nonzero = 1;
                coefficients[coefficient] = level;
                break;
            }
            level -= sign;
        }
    }
    return nonzero;
}
