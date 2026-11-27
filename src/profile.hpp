#pragma once
#include <cstdio>
#define RT(x) (void)0
// Optional host-side operation timing. Enabled only in a WX_PROFILE build.
#ifdef WX_PROFILE
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct HostProfile {
    using Clk = std::chrono::steady_clock;
    double copyInMs = 0, h264WriteMs = 0, reconWriteMs = 0;
    double resetMs = 0, recordMs = 0, submitMs = 0, waitMs = 0, outParseMs = 0, queryMs = 0;
    std::uint64_t frames = 0;
};
inline HostProfile g_hostProfile{};
inline double profileMs(HostProfile::Clk::time_point t) {
    return std::chrono::duration<double, std::milli>(HostProfile::Clk::now() - t).count();
}
inline void profileDump() {
    const char* path = std::getenv("WX_PROFILE_OUT");
    if (!path) return;
    if (std::FILE* f = std::fopen(path, "w")) {
        const auto& p = g_hostProfile;
        std::fprintf(f,
            "frames,%llu\n"
            "copyInMs,%.6f\nh264WriteMs,%.6f\nreconWriteMs,%.6f\n"
            "resetMs,%.6f\nrecordMs,%.6f\nsubmitMs,%.6f\nwaitMs,%.6f\n"
            "outParseMs,%.6f\nqueryMs,%.6f\n",
            (unsigned long long)p.frames,
            p.copyInMs / p.frames, p.h264WriteMs / p.frames, p.reconWriteMs / p.frames,
            p.resetMs / p.frames, p.recordMs / p.frames, p.submitMs / p.frames, p.waitMs / p.frames,
            p.outParseMs / p.frames, p.queryMs / p.frames);
        std::fclose(f);
    }
}
#define WX_PROF_START(name) auto prof_##name = HostProfile::Clk::now()
#define WX_PROF_ADD(name) g_hostProfile.name##Ms += profileMs(prof_##name)
#define WX_PROF_INC_FRAMES() ++g_hostProfile.frames
#else
inline void profileDump() {}
#define WX_PROF_START(name) (void)0
#define WX_PROF_ADD(name) (void)0
#define WX_PROF_INC_FRAMES() (void)0
#endif
