// SPDX-License-Identifier: GPL-2.0-or-later
// Translated from common/mc.c and common/tables.c. Copyright (C) 2003-2024 x264 project.
const int reference0[16] = int[16](0,1,1,1,0,1,1,1,2,3,3,3,0,1,1,1);
const int reference1[16] = int[16](0,0,1,0,2,2,3,2,2,2,3,2,2,2,3,2);

struct Reference {
    int plane0, plane1, start0, start1;
};

Reference quarterReference(ivec2 mv, int stride, int origin) {
    int phase = ((mv.y & 3) << 2) | (mv.x & 3);
    int position = origin + (mv.y >> 2)*stride + (mv.x >> 2);
    return Reference(reference0[phase], (phase & 5) != 0 ? reference1[phase] : -1,
                     position + ((mv.y & 3) == 3 ? stride : 0), position + ((mv.x & 3) == 3 ? 1 : 0));
}

int weightedPixel(int pixel, int scale, int denom, int offset) {
    int value = pixel*scale;
    if (denom > 0) value = (value + (1 << (denom-1))) >> denom;
    return clamp(value + offset, 0, 255);
}

int implicitBiPixel(int a, int b, int weight) {
    return clamp((a*weight + b*(64-weight) + 32) >> 6, 0, 255);
}
