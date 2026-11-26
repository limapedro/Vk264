// SPDX-License-Identifier: GPL-2.0-or-later
const int GROUP_PIXELS = GROUP_SIDE * GROUP_SIDE;
const int BLOCK_COEFFICIENTS = BLOCK_SIDE * BLOCK_SIDE;
const int BLOCKS_PER_GROUP = GROUP_PIXELS / BLOCK_COEFFICIENTS;

struct Block {
    uint sourcePixels[GROUP_PIXELS];
    uint predictedPixels[GROUP_PIXELS];
    int inverseCoefficients[GROUP_PIXELS];
    int transformed[GROUP_PIXELS];
    uint reconstructed[GROUP_PIXELS];
};
layout(std430, binding = 0) buffer Corpus { Block blocks[]; };

int pixelOffset(int blockIndex) {
    int x = blockIndex % 2;
    int y = (blockIndex / 2) % 2;
    // x264's 16x16 transform using 4x4 blocks groups its blocks by 8x8 quadrant.
    if (BLOCKS_PER_GROUP == 16) {
        x += 2 * ((blockIndex / 4) % 2);
        y += 2 * (blockIndex / 8);
    }
    return BLOCK_SIDE * (y * GROUP_SIDE + x);
}
