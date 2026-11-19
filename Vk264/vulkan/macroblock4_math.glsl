// SPDX-License-Identifier: GPL-2.0-or-later
int shortValue(int value) { return bitfieldExtract(value, 0, 16); }
int dequantize(int level, int matrix, int shift) {
    int product = level*matrix;
    return shortValue(shift >= 0 ? product*(1 << shift) : (product+(1 << (-shift-1))) >> -shift);
}
void reconstruct(uint f, int pixelBase, int coefficients[16]) {
    ivec4 tmp[4];
    for (int y = 0; y < 4; ++y) {
        ivec4 c = ivec4(coefficients[y], coefficients[4+y], coefficients[8+y], coefficients[12+y]);
        tmp[y] = bitfieldExtract(inversePass(c), 0, 16);
    }
    for (int x = 0; x < 4; ++x) {
        ivec4 delta = (inversePass(ivec4(tmp[0][x], tmp[1][x], tmp[2][x], tmp[3][x]))+32) >> 6;
        for (int y = 0; y < 4; ++y) {
            int pos = pixelBase+y*stride+x;
            fixtures[f].pixels[pos] = uint(clamp(int(fixtures[f].pixels[pos])+delta[y], 0, 255));
        }
    }
}
