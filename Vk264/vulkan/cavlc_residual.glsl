void vlc(uint index) {
    uint entry = codes[index];
    if ((entry >> 16) == 0u) overflow = true;
    bits(entry & 65535u, int(entry >> 16));
}

void encodeLevel(int code, int suffix) {
    int prefixLength = code >> suffix;
    if (prefixLength < 14 || (suffix != 0 && prefixLength == 14)) {
        bits(1u, prefixLength + 1);
        bits(uint(code), suffix);
    } else if (suffix == 0 && code < 30) {
        bits(1u, 15);
        bits(uint(code - 14), 4);
    } else {
        uint escape = uint(code - (15 << suffix) - (suffix == 0 ? 15 : 0));
        int tail = 12;
        while (escape >= (1u << tail)) {
            escape -= 1u << tail;
            ++tail;
        }
        bits(1u, tail + 4);
        bits(escape, tail);
    }
}

void writeCavlcResidual(int samples[16], int nc, int count, bool chromaDc) {
    int level[16], gap[16];
    int total = 0, last = -1;
    for (int i = 0; i < count; ++i) {
        int v = samples[i];
        if (v == 0) continue;
        level[total] = v;
        gap[total++] = i - last - 1;
        last = i;
    }

    int trailing = 0;
    for (int i = total - 1; i >= 0 && trailing < 3; --i) {
        if (abs(level[i]) != 1) break;
        ++trailing;
    }
    int table = chromaDc ? 4 : (nc < 2 ? 0 : nc < 4 ? 1 : nc < 8 ? 2 : 3);
    vlc(uint((table * 17 + total) * 4 + trailing));
    if (total == 0) return;

    for (int i = 0; i < trailing; ++i) bits(uint(level[total - 1 - i] < 0), 1);
    int suffix = int(total > 10 && trailing < 3);
    for (int i = trailing; i < total; ++i) {
        int value = level[total - 1 - i];
        int code = 2 * abs(value) - (value > 0 ? 2 : 1);
        if (i == trailing && trailing < 3) code -= 2;
        encodeLevel(code, suffix);
        suffix = max(suffix, 1);
        if (suffix < 6 && abs(value) > (3 << (suffix - 1))) ++suffix;
    }

    int zeros = last + 1 - total;
    if (total != count) {
        int index = chromaDc ? 596 + 4 * total : 340 + 16 * total;
        vlc(uint(index + zeros));
    }
    for (int i = total - 1; i > 0 && zeros != 0; --i) {
        vlc(uint(612 + 15 * min(zeros, 7) + gap[i]));
        zeros -= gap[i];
    }
}
