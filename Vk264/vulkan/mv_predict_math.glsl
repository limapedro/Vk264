// SPDX-License-Identifier: GPL-2.0-or-later
// Translated from common/mvpred.c and common/base.h. Copyright (C) 2003-2024 x264 project.
ivec2 medianMotion(ivec2 a, ivec2 b, ivec2 c) {
    return a+b+c-min(a,min(b,c))-max(a,max(b,c));
}

ivec2 predictMotion16(int ref, ivec4 refs, ivec2 a, ivec2 b, ivec2 c, ivec2 d) {
    if (refs.z == -2) {
        refs.z = refs.w;
        c = d;
    }
    int matches = int(refs.x == ref)+int(refs.y == ref)+int(refs.z == ref);
    if (matches == 1) {
        if (refs.x == ref) return a;
        if (refs.y == ref) return b;
        return c;
    }
    if (matches == 0 && refs.y == -2 && refs.z == -2 && refs.x != -2) return a;
    return medianMotion(a, b, c);
}

ivec2 predictPSkip(ivec4 refs, ivec2 a, ivec2 b, ivec2 c, ivec2 d) {
    if (refs.x == -2 || refs.y == -2 ||
        (refs.x == 0 && all(equal(a, ivec2(0)))) ||
        (refs.y == 0 && all(equal(b, ivec2(0)))))
        return ivec2(0);
    return predictMotion16(0, refs, a, b, c, d);
}

ivec2 motionDeltaContexts(ivec2 a, ivec2 b) {
    ivec2 sum = a+b;
    return ivec2(greaterThan(sum, ivec2(2)))+ivec2(greaterThan(sum, ivec2(32)));
}
