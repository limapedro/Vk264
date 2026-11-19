// SPDX-License-Identifier: GPL-2.0-or-later
// Translated from common/quant.c. Copyright (C) 2003-2024 x264 project.
int quantLevel(int coefficient, uint multiplier, uint bias) {
    uint magnitude = ((bias + uint(abs(coefficient))) * multiplier) >> 16;
    return bitfieldExtract(coefficient > 0 ? int(magnitude) : -int(magnitude), 0, 16);
}
