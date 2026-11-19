// SPDX-License-Identifier: GPL-2.0-or-later
ivec4 forwardPass(ivec4 d) {
    int s03 = d.x + d.w, s12 = d.y + d.z;
    int d03 = d.x - d.w, d12 = d.y - d.z;
    return ivec4(s03 + s12, 2*d03 + d12, s03 - s12, d03 - 2*d12);
}

ivec4 inversePass(ivec4 d) {
    int s02 = d.x + d.z, d02 = d.x - d.z;
    int s13 = d.y + (d.w >> 1), d13 = (d.y >> 1) - d.w;
    return ivec4(s02 + s13, d02 + d13, d02 - d13, s02 - s13);
}
