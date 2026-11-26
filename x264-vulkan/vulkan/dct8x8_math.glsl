// SPDX-License-Identifier: GPL-2.0-or-later
void forwardPass(int src[8], out int dst[8]) {
    int s07 = src[0] + src[7];
    int s16 = src[1] + src[6];
    int s25 = src[2] + src[5];
    int s34 = src[3] + src[4];
    int a0 = s07 + s34;
    int a1 = s16 + s25;
    int a2 = s07 - s34;
    int a3 = s16 - s25;
    int d07 = src[0] - src[7];
    int d16 = src[1] - src[6];
    int d25 = src[2] - src[5];
    int d34 = src[3] - src[4];
    int a4 = d16 + d25 + (d07 + (d07>>1));
    int a5 = d07 - d34 - (d25 + (d25>>1));
    int a6 = d07 + d34 - (d16 + (d16>>1));
    int a7 = d16 - d25 + (d34 + (d34>>1));
    dst[0] = a0 + a1;
    dst[1] = a4 + (a7>>2);
    dst[2] = a2 + (a3>>1);
    dst[3] = a5 + (a6>>2);
    dst[4] = a0 - a1;
    dst[5] = a6 - (a5>>2);
    dst[6] = (a2>>1) - a3;
    dst[7] = (a4>>2) - a7;
}

void inversePass(int src[8], out int dst[8]) {
    int a0 =  src[0] + src[4];
    int a2 =  src[0] - src[4];
    int a4 = (src[2]>>1) - src[6];
    int a6 = (src[6]>>1) + src[2];
    int b0 = a0 + a6;
    int b2 = a2 + a4;
    int b4 = a2 - a4;
    int b6 = a0 - a6;
    int a1 = -src[3] + src[5] - src[7] - (src[7]>>1);
    int a3 =  src[1] + src[7] - src[3] - (src[3]>>1);
    int a5 = -src[1] + src[7] + src[5] + (src[5]>>1);
    int a7 =  src[3] + src[5] + src[1] + (src[1]>>1);
    int b1 = (a7>>2) + a1;
    int b3 =  a3 + (a5>>2);
    int b5 = (a3>>2) - a5;
    int b7 =  a7 - (a1>>2);
    dst[0] = b0 + b7;
    dst[1] = b2 + b5;
    dst[2] = b4 + b3;
    dst[3] = b6 + b1;
    dst[4] = b6 - b1;
    dst[5] = b4 - b3;
    dst[6] = b2 - b5;
    dst[7] = b0 - b7;
}

int signed16(int value) { return bitfieldExtract(value, 0, 16); }
