// SPDX-License-Identifier: GPL-2.0-or-later
ivec4 dcPass(ivec4 value) {
    int s01 = value.x + value.y, d01 = value.x - value.y;
    int s23 = value.z + value.w, d23 = value.z - value.w;
    return ivec4(s01 + s23, s01 - s23, d01 - d23, d01 + d23);
}
