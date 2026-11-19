// SPDX-License-Identifier: GPL-2.0-or-later
ivec4 hadamard4(ivec4 d) {
    int a = d.x+d.y, b = d.x-d.y, c = d.z+d.w, e = d.z-d.w;
    return ivec4(a+c, b+e, a-c, b-e);
}

int satd4x4(ivec4 rows[4]) {
    for (int i = 0; i < 4; ++i) rows[i] = hadamard4(rows[i]);
    ivec4 totals = ivec4(0);
    for (int c = 0; c < 4; ++c)
        totals += abs(hadamard4(ivec4(rows[0][c], rows[1][c], rows[2][c], rows[3][c])));
    return (totals.x+totals.y+totals.z+totals.w) >> 1;
}
