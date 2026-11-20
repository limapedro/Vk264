// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal ISO-BMFF (MP4) muxer for vk264 Annex B output.
// Mirrors the subset of x264 output/mp4.c needed for this encoder:
// CFR, no B-frames, one slice NAL per frame, SPS/PPS in avcC.
// No external dependencies (no GPAC/L-SMASH/ffmpeg).
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace vk264_mp4 {

inline void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back((x >> 8) & 0xff); v.push_back(x & 0xff);
}
inline void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back((x >> 24) & 0xff); v.push_back((x >> 16) & 0xff);
    v.push_back((x >> 8) & 0xff); v.push_back(x & 0xff);
}
inline void raw(std::vector<std::uint8_t>& v, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    v.insert(v.end(), b, b + n);
}
inline void tag(std::vector<std::uint8_t>& v, const char* t) { raw(v, t, 4); }

inline void box(std::vector<std::uint8_t>& out, const char* type,
                const std::vector<std::uint8_t>& payload) {
    put32(out, static_cast<std::uint32_t>(8 + payload.size()));
    tag(out, type);
    out.insert(out.end(), payload.begin(), payload.end());
}
inline void fullbox(std::vector<std::uint8_t>& out, const char* type,
                    std::uint8_t version, std::uint32_t flags,
                    const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> p;
    p.push_back(version);
    p.push_back((flags >> 16) & 0xff); p.push_back((flags >> 8) & 0xff); p.push_back(flags & 0xff);
    p.insert(p.end(), payload.begin(), payload.end());
    box(out, type, p);
}

struct Nal {
    std::uint8_t type;
    std::vector<std::uint8_t> bytes; // includes NAL header
};

inline std::vector<Nal> splitAnnexB(const std::vector<std::uint8_t>& data) {
    std::vector<Nal> nals;
    std::size_t i = 0, n = data.size();
    auto startCodeLen = [&](std::size_t p) -> std::size_t {
        if (p + 4 <= n && data[p] == 0 && data[p+1] == 0 && data[p+2] == 0 && data[p+3] == 1) return 4;
        if (p + 3 <= n && data[p] == 0 && data[p+1] == 0 && data[p+2] == 1) return 3;
        return 0;
    };
    // find first start code
    std::size_t pos = 0;
    while (pos < n && startCodeLen(pos) == 0) ++pos;
    while (pos < n) {
        std::size_t sc = startCodeLen(pos);
        if (!sc) break;
        std::size_t begin = pos + sc;
        std::size_t end = begin;
        while (end < n && startCodeLen(end) == 0) ++end;
        if (begin < end) {
            Nal nal;
            nal.type = data[begin] & 0x1f;
            nal.bytes.assign(data.begin() + begin, data.begin() + end);
            // trim trailing zero bytes that belong to the next start code prefix
            while (!nal.bytes.empty() && nal.bytes.back() == 0) nal.bytes.pop_back();
            if (!nal.bytes.empty()) nals.push_back(std::move(nal));
        }
        pos = end;
    }
    return nals;
}

inline std::vector<std::uint8_t> buildAvcC(const Nal& sps, const Nal& pps) {
    std::vector<std::uint8_t> p;
    p.push_back(1);                       // configurationVersion
    p.push_back(sps.bytes.size() > 1 ? sps.bytes[1] : 0x42);
    p.push_back(sps.bytes.size() > 2 ? sps.bytes[2] : 0xc0);
    p.push_back(sps.bytes.size() > 3 ? sps.bytes[3] : 0x1e);
    p.push_back(0xff);                    // lengthSizeMinusOne = 3
    p.push_back(0xe1);                    // numOfSPS = 1
    put16(p, static_cast<std::uint16_t>(sps.bytes.size()));
    raw(p, sps.bytes.data(), sps.bytes.size());
    p.push_back(0x01);                    // numOfPPS = 1
    put16(p, static_cast<std::uint16_t>(pps.bytes.size()));
    raw(p, pps.bytes.data(), pps.bytes.size());
    std::vector<std::uint8_t> out;
    box(out, "avcC", p);
    return out;
}

inline bool annexBToMp4(const std::filesystem::path& path,
                        std::uint32_t fpsNum, std::uint32_t fpsDen,
                        std::uint32_t frames, std::uint32_t gop,
                        std::uint32_t width, std::uint32_t height) {
    if (fpsNum == 0 || fpsDen == 0 || frames == 0) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    auto nals = splitAnnexB(data);
    const Nal* sps = nullptr; const Nal* pps = nullptr;
    for (const auto& n : nals) {
        if (n.type == 7 && !sps) sps = &n;
        if (n.type == 8 && !pps) pps = &n;
    }
    if (!sps || !pps) return false;

    // Build mdat: one sample per slice NAL (type 1 non-IDR or 5 IDR).
    std::vector<std::uint8_t> mdatPayload;
    std::vector<std::uint32_t> sampleSizes, sampleOffsets, syncSamples;
    for (const auto& n : nals) {
        if (n.type != 1 && n.type != 5) continue;
        put32(mdatPayload, static_cast<std::uint32_t>(n.bytes.size()));
        raw(mdatPayload, n.bytes.data(), n.bytes.size());
        sampleSizes.push_back(static_cast<std::uint32_t>(4 + n.bytes.size()));
        if (n.type == 5) syncSamples.push_back(static_cast<std::uint32_t>(sampleSizes.size()));
    }
    if (sampleSizes.empty()) return false;

    // Assembly: ftyp + free + mdat + moov. Compute mdat data offset.
    std::vector<std::uint8_t> ftyp;
    // ftyp payload: major_brand, minor_version, then compatible brands.
    tag(ftyp, "isom"); put32(ftyp, 0x200); tag(ftyp, "isom"); tag(ftyp, "iso2"); tag(ftyp, "avc1"); tag(ftyp, "mp41");
    std::vector<std::uint8_t> ftypBox;
    box(ftypBox, "ftyp", ftyp);

    std::vector<std::uint8_t> freeBox;
    box(freeBox, "free", std::vector<std::uint8_t>{});

    std::vector<std::uint8_t> mdatBox;
    box(mdatBox, "mdat", mdatPayload);

    // mdat data starts after ftypBox + freeBox + 8 (mdat header)
    std::uint64_t mdatDataOffset = ftypBox.size() + freeBox.size() + 8;
    std::uint64_t off = mdatDataOffset;
    for (auto s : sampleSizes) { sampleOffsets.push_back(static_cast<std::uint32_t>(off)); off += s; }

    // moov / mvhd (movie timescale 1000)
    std::uint32_t movieDuration = static_cast<std::uint32_t>(
        (std::uint64_t)frames * 1000 / fpsNum * fpsDen / std::max(1u, fpsDen) * fpsDen / std::max(1u, fpsNum));
    movieDuration = static_cast<std::uint32_t>((std::uint64_t)frames * 1000 * fpsDen / fpsNum);
    std::vector<std::uint8_t> mvhd;
    put32(mvhd, 0); put32(mvhd, 0); put32(mvhd, 1000); put32(mvhd, movieDuration);
    put32(mvhd, 0x00010000); put16(mvhd, 0x0100); put16(mvhd, 0);
    put32(mvhd, 0); put32(mvhd, 0); // reserved
    put32(mvhd, 0x00010000); put32(mvhd, 0); put32(mvhd, 0);
    put32(mvhd, 0); put32(mvhd, 0x00010000); put32(mvhd, 0);
    put32(mvhd, 0); put32(mvhd, 0); put32(mvhd, 0x40000000); // matrix
    for (int i = 0; i < 6; ++i) put32(mvhd, 0);              // pre_defined
    put32(mvhd, 2);                                          // next_track_ID
    std::vector<std::uint8_t> mvhdBox;
    fullbox(mvhdBox, "mvhd", 0, 0, mvhd);

    // tkhd
    std::vector<std::uint8_t> tkhd;
    put32(tkhd, 0); put32(tkhd, 0); put32(tkhd, 1); put32(tkhd, 0);
    put32(tkhd, movieDuration);
    put32(tkhd, 0); put32(tkhd, 0);
    put16(tkhd, 0); put16(tkhd, 0); put16(tkhd, 0); put16(tkhd, 0);
    put32(tkhd, 0x00010000); put32(tkhd, 0); put32(tkhd, 0);
    put32(tkhd, 0); put32(tkhd, 0x00010000); put32(tkhd, 0);
    put32(tkhd, 0); put32(tkhd, 0); put32(tkhd, 0x40000000);
    put32(tkhd, width << 16); put32(tkhd, height << 16);
    std::vector<std::uint8_t> tkhdBox;
    fullbox(tkhdBox, "tkhd", 0, 7, tkhd);

    // mdhd: media timescale = fpsNum, sample delta = fpsDen
    std::vector<std::uint8_t> mdhd;
    put32(mdhd, 0); put32(mdhd, 0); put32(mdhd, fpsNum);
    put32(mdhd, frames * fpsDen);
    put16(mdhd, 0x55c4); put16(mdhd, 0);
    std::vector<std::uint8_t> mdhdBox;
    fullbox(mdhdBox, "mdhd", 0, 0, mdhd);

    // hdlr
    std::vector<std::uint8_t> hdlr;
    put32(hdlr, 0); tag(hdlr, "vide");
    put32(hdlr, 0); put32(hdlr, 0); put32(hdlr, 0);
    const char name[] = "VideoHandler\0";
    raw(hdlr, name, sizeof(name));
    std::vector<std::uint8_t> hdlrBox;
    fullbox(hdlrBox, "hdlr", 0, 0, hdlr);

    // minf: vmhd
    std::vector<std::uint8_t> vmhd;
    put16(vmhd, 0); put16(vmhd, 0); put16(vmhd, 0); put16(vmhd, 0);
    std::vector<std::uint8_t> vmhdBox;
    fullbox(vmhdBox, "vmhd", 0, 1, vmhd);

    // dinf/dref/url
    std::vector<std::uint8_t> urlBox;
    fullbox(urlBox, "url ", 0, 1, std::vector<std::uint8_t>{});
    std::vector<std::uint8_t> dref;
    put32(dref, 1); dref.insert(dref.end(), urlBox.begin(), urlBox.end());
    std::vector<std::uint8_t> drefBox;
    fullbox(drefBox, "dref", 0, 0, dref);
    std::vector<std::uint8_t> dinf;
    dinf.insert(dinf.end(), drefBox.begin(), drefBox.end());
    std::vector<std::uint8_t> dinfBox;
    box(dinfBox, "dinf", dinf);

    // stbl/stsd/avc1/avcC
    std::vector<std::uint8_t> avcC = buildAvcC(*sps, *pps);
    std::vector<std::uint8_t> avc1;
    for (int i = 0; i < 6; ++i) avc1.push_back(0);
    put16(avc1, 1);                              // data_reference_index
    for (int i = 0; i < 16; ++i) avc1.push_back(0);
    put16(avc1, width); put16(avc1, height);
    put32(avc1, 0x00480000); put32(avc1, 0x00480000);
    put32(avc1, 0); put16(avc1, 1);
    for (int i = 0; i < 32; ++i) avc1.push_back(0); // compressorname
    put16(avc1, 0x0018); put16(avc1, 0xffff);
    avc1.insert(avc1.end(), avcC.begin(), avcC.end());
    std::vector<std::uint8_t> avc1Box;
    box(avc1Box, "avc1", avc1);
    std::vector<std::uint8_t> stsd;
    put32(stsd, 1); stsd.insert(stsd.end(), avc1Box.begin(), avc1Box.end());
    std::vector<std::uint8_t> stsdBox;
    fullbox(stsdBox, "stsd", 0, 0, stsd);

    std::vector<std::uint8_t> stts;
    put32(stts, 1); put32(stts, static_cast<std::uint32_t>(sampleSizes.size())); put32(stts, fpsDen);
    std::vector<std::uint8_t> sttsBox;
    fullbox(sttsBox, "stts", 0, 0, stts);

    std::vector<std::uint8_t> stsc;
    put32(stsc, 1); put32(stsc, 1); put32(stsc, 1); put32(stsc, 1);
    std::vector<std::uint8_t> stscBox;
    fullbox(stscBox, "stsc", 0, 0, stsc);

    std::vector<std::uint8_t> stsz;
    put32(stsz, 0); put32(stsz, static_cast<std::uint32_t>(sampleSizes.size()));
    for (auto s : sampleSizes) put32(stsz, s);
    std::vector<std::uint8_t> stszBox;
    fullbox(stszBox, "stsz", 0, 0, stsz);

    std::vector<std::uint8_t> stco;
    put32(stco, static_cast<std::uint32_t>(sampleOffsets.size()));
    for (auto o : sampleOffsets) put32(stco, o);
    std::vector<std::uint8_t> stcoBox;
    fullbox(stcoBox, "stco", 0, 0, stco);

    std::vector<std::uint8_t> stss;
    put32(stss, static_cast<std::uint32_t>(syncSamples.size()));
    for (auto s : syncSamples) put32(stss, s);
    std::vector<std::uint8_t> stssBox;
    fullbox(stssBox, "stss", 0, 0, stss);

    std::vector<std::uint8_t> stbl;
    stbl.insert(stbl.end(), stsdBox.begin(), stsdBox.end());
    stbl.insert(stbl.end(), sttsBox.begin(), sttsBox.end());
    stbl.insert(stbl.end(), stscBox.begin(), stscBox.end());
    stbl.insert(stbl.end(), stszBox.begin(), stszBox.end());
    stbl.insert(stbl.end(), stcoBox.begin(), stcoBox.end());
    stbl.insert(stbl.end(), stssBox.begin(), stssBox.end());
    std::vector<std::uint8_t> stblBox;
    box(stblBox, "stbl", stbl);

    std::vector<std::uint8_t> minf;
    minf.insert(minf.end(), vmhdBox.begin(), vmhdBox.end());
    minf.insert(minf.end(), dinfBox.begin(), dinfBox.end());
    minf.insert(minf.end(), stblBox.begin(), stblBox.end());
    std::vector<std::uint8_t> minfBox;
    box(minfBox, "minf", minf);

    std::vector<std::uint8_t> mdia;
    mdia.insert(mdia.end(), mdhdBox.begin(), mdhdBox.end());
    mdia.insert(mdia.end(), hdlrBox.begin(), hdlrBox.end());
    mdia.insert(mdia.end(), minfBox.begin(), minfBox.end());
    std::vector<std::uint8_t> mdiaBox;
    box(mdiaBox, "mdia", mdia);

    std::vector<std::uint8_t> trak;
    trak.insert(trak.end(), tkhdBox.begin(), tkhdBox.end());
    trak.insert(trak.end(), mdiaBox.begin(), mdiaBox.end());
    std::vector<std::uint8_t> trakBox;
    box(trakBox, "trak", trak);

    std::vector<std::uint8_t> moov;
    moov.insert(moov.end(), mvhdBox.begin(), mvhdBox.end());
    moov.insert(moov.end(), trakBox.begin(), trakBox.end());
    std::vector<std::uint8_t> moovBox;
    box(moovBox, "moov", moov);

    std::vector<std::uint8_t> file;
    file.insert(file.end(), ftypBox.begin(), ftypBox.end());
    file.insert(file.end(), freeBox.begin(), freeBox.end());
    file.insert(file.end(), mdatBox.begin(), mdatBox.end());
    file.insert(file.end(), moovBox.begin(), moovBox.end());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(file.data()), file.size());
    return true;
}

} // namespace vk264_mp4
