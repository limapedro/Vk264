// SPDX-License-Identifier: GPL-2.0-or-later
int top(uint fixture, int x) { return int(fixtures[fixture].pixels[origin - stride + x]); }
int left(uint fixture, int y) { return int(fixtures[fixture].pixels[origin - 1 + stride*y]); }

// x264 numbers vertical and DC differently in its chroma and 16x16 luma tables.
const int dc = 0, horizontal = 1, vertical = 2, plane = 3;
const int dcLeft = 4, dcTop = 5, dcMidpoint = 6;

void predictIntra(uint fixture, int width, int height, int mode) {
    bool luma = width == 16;
    if (luma && (mode == 0 || mode == 2)) mode = 2 - mode;

    int topSum[4] = int[4](0, 0, 0, 0);
    int leftSum[4] = int[4](0, 0, 0, 0);
    if (mode == dc || mode == dcTop)
        for (int x = 0; x < width; ++x) topSum[luma ? 0 : x/4] += top(fixture, x);
    if (mode == dc || mode == dcLeft)
        for (int y = 0; y < height; ++y) leftSum[luma ? 0 : y/4] += left(fixture, y);

    int start = 0, slopeX = 0, slopeY = 0;
    if (mode == plane) {
        int gradientX = 0, gradientY = 0;
        int halfWidth = width / 2, halfHeight = height / 2;
        for (int i = 0; i < halfWidth; ++i)
            gradientX += (i + 1) * (top(fixture, halfWidth+i) - top(fixture, halfWidth-2-i));
        for (int i = 0; i < halfHeight; ++i)
            gradientY += (i + 1) * (left(fixture, halfHeight+i) - left(fixture, halfHeight-2-i));
        slopeX = width == 16 ? (5*gradientX + 32) >> 6 : (17*gradientX + 16) >> 5;
        slopeY = height == 16 ? (5*gradientY + 32) >> 6 : (17*gradientY + 16) >> 5;
        start = 16*(left(fixture, height-1) + top(fixture, width-1))
                - (halfWidth-1)*slopeX - (halfHeight-1)*slopeY + 16;
    }

    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            int value = 128;
            int to = luma ? 0 : x/4;
            int le = luma ? 0 : y/4;
            if (mode == vertical) value = top(fixture, x);
            else if (mode == horizontal) value = left(fixture, y);
            else if (mode == plane) value = clamp((start + x*slopeX + y*slopeY) >> 5, 0, 255);
            else if (mode == dcLeft) value = luma ? (leftSum[0] + 8) >> 4 : (leftSum[le] + 2) >> 2;
            else if (mode == dcTop) value = luma ? (topSum[0] + 8) >> 4 : (topSum[to] + 2) >> 2;
            else if (mode == dc) {
                if (luma) value = (topSum[0] + leftSum[0] + 16) >> 5;
                else if (to == 1 && le == 0) value = (topSum[to] + 2) >> 2;
                else if (to == 0 && le > 0) value = (leftSum[le] + 2) >> 2;
                else value = (topSum[to] + leftSum[le] + 4) >> 3;
            }
            fixtures[fixture].pixels[origin + y*stride + x] = uint(value);
        }
}
