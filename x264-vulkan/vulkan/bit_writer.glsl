// SPDX-License-Identifier: GPL-2.0-or-later
// Translated from common/bitstream.h. Copyright (C) 2003-2024 x264 project.
struct Writer {
    int start, pointer, left;
    uint low, high;
};

uint readBigEndian(uint f, int p) {
    return (fixtures[f].bytes[p] << 24) | (fixtures[f].bytes[p+1] << 16)
         | (fixtures[f].bytes[p+2] << 8) | fixtures[f].bytes[p+3];
}
void writeBigEndian(uint f, int p, uint word) {
    for (int i = 0; i < 4; ++i) fixtures[f].bytes[p+i] = (word >> (24-8*i)) & 255u;
}
void bsRealign(uint f, inout Writer writer) {
    int offset = writer.pointer & 3;
    if (offset == 0) return;
    writer.pointer -= offset;
    writer.left = 64 - 8*offset;
    writer.low = readBigEndian(f, writer.pointer) >> ((4-offset)*8);
    writer.high = 0u;
}
int bsPos(Writer writer) {
    return 8*(writer.pointer-writer.start) + 64 - writer.left;
}
void bsAppend(inout Writer writer, int count, uint bits) {
    // x264's 64-bit reservoir represented by a pair of 32-bit registers.
    if (count == 32) {
        writer.high = writer.low;
        writer.low = 0u;
    } else if (count != 0) {
        writer.high = (writer.high << count) | (writer.low >> (32-count));
        writer.low <<= count;
    }
    writer.low |= bits;
    writer.left -= count;
}
void bsWrite(uint f, inout Writer writer, int count, uint bits) {
    bsAppend(writer, count, bits);
    if (writer.left <= 32) {
        int shift = 32-writer.left;
        uint word = shift == 0 ? writer.low : (writer.low >> shift) | (writer.high << (32-shift));
        writeBigEndian(f, writer.pointer, word);
        writer.pointer += 4;
        writer.left += 32;
    }
}
void bsWrite1(uint f, inout Writer writer, uint bit) {
    bsAppend(writer, 1, bit);
    if (writer.left == 32) {
        writeBigEndian(f, writer.pointer, writer.low);
        writer.pointer += 4;
        writer.left = 64;
    }
}
void bsFlush(uint f, inout Writer writer) {
    writeBigEndian(f, writer.pointer, writer.low << (writer.left & 31));
    writer.pointer += 8 - (writer.left >> 3);
    writer.left = 64;
}
int golombSize(uint codeword) { return 2*findMSB(codeword)+1; }
uint signedCodeword(int value) { return uint(value <= 0 ? 1-2*value : 2*value); }

void bsWrite32(uint f, inout Writer writer, uint value) {
    bsWrite(f, writer, 16, value >> 16);
    bsWrite(f, writer, 16, value);
}
void bsWriteUeBig(uint f, inout Writer writer, uint value) {
    uint codeword = value+1u;
    int zeros = findMSB(codeword);
    bsWrite(f, writer, zeros, 0u);
    bsWrite(f, writer, zeros+1, codeword);
}
void bsWriteSe(uint f, inout Writer writer, int value) {
    uint codeword = signedCodeword(value);
    bsWrite(f, writer, golombSize(codeword), codeword);
}
void bsTrailing(uint f, inout Writer writer) {
    bsWrite1(f, writer, 1u);
    bsWrite(f, writer, writer.left & 7, 0u);
}
