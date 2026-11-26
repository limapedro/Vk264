// SPDX-License-Identifier: GPL-2.0-or-later
// Translated from common/deblock.c. Copyright (C) 2003-2024 x264 project.
int clip3(int value, int low, int high) {
    return value < low ? low : value > high ? high : value;
}

void filterLine(uint fixture, int center, int stride, int alpha, int beta,
                int tc0, bool intra, bool chroma) {
    int p1 = int(EDGE_PIXEL(center - 2*stride));
    int p0 = int(EDGE_PIXEL(center - stride));
    int q0 = int(EDGE_PIXEL(center));
    int q1 = int(EDGE_PIXEL(center + stride));
    if (abs(p0-q0) >= alpha || abs(p1-p0) >= beta || abs(q1-q0) >= beta) return;

    if (!intra) {
        int tc = tc0;
        if (!chroma) {
            int p2 = int(EDGE_PIXEL(center - 3*stride));
            int q2 = int(EDGE_PIXEL(center + 2*stride));
            int midpoint = (p0 + q0 + 1) >> 1;
            if (abs(p2-p0) < beta) {
                if (tc0 != 0)
                    EDGE_PIXEL(center - 2*stride) =
                        uint(p1 + clip3(((p2 + midpoint) >> 1) - p1, -tc0, tc0)) & 255u;
                ++tc;
            }
            if (abs(q2-q0) < beta) {
                if (tc0 != 0)
                    EDGE_PIXEL(center + stride) =
                        uint(q1 + clip3(((q2 + midpoint) >> 1) - q1, -tc0, tc0)) & 255u;
                ++tc;
            }
        }
        int delta = clip3((4*(q0 - p0) + p1 - q1 + 4) >> 3, -tc, tc);
        EDGE_PIXEL(center - stride) = uint(clamp(p0 + delta, 0, 255));
        EDGE_PIXEL(center) = uint(clamp(q0 - delta, 0, 255));
        return;
    }

    int nextP0 = (2*p1 + p0 + q1 + 2) >> 2;
    int nextQ0 = (2*q1 + q0 + p1 + 2) >> 2;
    if (!chroma && abs(p0-q0) < ((alpha >> 2) + 2)) {
        int p2 = int(EDGE_PIXEL(center - 3*stride));
        int q2 = int(EDGE_PIXEL(center + 2*stride));
        if (abs(p2-p0) < beta) {
            int p3 = int(EDGE_PIXEL(center - 4*stride));
            nextP0 = (p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4) >> 3;
            EDGE_PIXEL(center - 2*stride) = uint((p2 + p1 + p0 + q0 + 2) >> 2);
            EDGE_PIXEL(center - 3*stride) = uint((2*p3 + 3*p2 + p1 + p0 + q0 + 4) >> 3);
        }
        if (abs(q2-q0) < beta) {
            int q3 = int(EDGE_PIXEL(center + 3*stride));
            nextQ0 = (p1 + 2*p0 + 2*q0 + 2*q1 + q2 + 4) >> 3;
            EDGE_PIXEL(center + stride) = uint((p0 + q0 + q1 + q2 + 2) >> 2);
            EDGE_PIXEL(center + 2*stride) = uint((2*q3 + 3*q2 + q1 + q0 + p0 + 4) >> 3);
        }
    }
    EDGE_PIXEL(center - stride) = uint(nextP0);
    EDGE_PIXEL(center) = uint(nextQ0);
}
