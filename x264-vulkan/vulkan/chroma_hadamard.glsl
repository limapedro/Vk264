// SPDX-License-Identifier: GPL-2.0-or-later
// Translated from common/dct.c. Copyright (C) 2003-2024 x264 project.
ivec4 chromaHadamard2x2(ivec4 dc) {
    int d0 = dc.x + dc.y, d1 = dc.z + dc.w;
    int d2 = dc.x - dc.y, d3 = dc.z - dc.w;
    return ivec4(d0 + d1, d0 - d1, d2 + d3, d2 - d3);
}

void chromaHadamard2x4(int dc[8], out int result[8]) {
    int a0 = dc[0] + dc[1], a1 = dc[2] + dc[3];
    int a2 = dc[4] + dc[5], a3 = dc[6] + dc[7];
    int a4 = dc[0] - dc[1], a5 = dc[2] - dc[3];
    int a6 = dc[4] - dc[5], a7 = dc[6] - dc[7];
    int b0 = a0 + a1, b1 = a2 + a3;
    int b2 = a4 + a5, b3 = a6 + a7;
    int b4 = a0 - a1, b5 = a2 - a3;
    int b6 = a4 - a5, b7 = a6 - a7;
    result[0] = b0 + b1;
    result[1] = b2 + b3;
    result[2] = b0 - b1;
    result[3] = b2 - b3;
    result[4] = b4 - b5;
    result[5] = b6 - b7;
    result[6] = b4 + b5;
    result[7] = b6 + b7;
}
