#pragma once
#include <Windows.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>

namespace gakumas::vr::perf {
using HitchClock = std::chrono::steady_clock;
inline double HitchMs(HitchClock::time_point a, HitchClock::time_point b) {
    return std::chrono::duration<double, std::milli>(b-a).count();
}
struct ThreadCounters {
    bool timeKnown = false, cyclesKnown = false;
    std::uint64_t kernel = 0, user = 0, cycles = 0;
    static ThreadCounters Read() {
        ThreadCounters r;
        FILETIME created{}, exited{}, kernel{}, user{};
        r.timeKnown = GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != FALSE;
        r.kernel = (std::uint64_t(kernel.dwHighDateTime)<<32) | kernel.dwLowDateTime;
        r.user = (std::uint64_t(user.dwHighDateTime)<<32) | user.dwLowDateTime;
        r.cyclesKnown = QueryThreadCycleTime(GetCurrentThread(), &r.cycles) != FALSE;
        return r;
    }
};
struct HitchCall {
    bool enabled = false, stopped = false;
    HitchClock::time_point start{};
    ThreadCounters before{}, after{};
    double wallMs = 0;
    explicit HitchCall(bool active) : enabled(active) {
        if (enabled) { before = ThreadCounters::Read(); start = HitchClock::now(); }
    }
    void Stop() {
        if (!enabled || stopped) return;
        wallMs = HitchMs(start, HitchClock::now());
        after = ThreadCounters::Read();
        stopped = true;
    }
    template<class Sink>
    void Report(const char* phase, std::uint64_t frame, std::int64_t result, Sink sink,
                std::string_view extra = {},
                HitchClock::time_point now = HitchClock::now()) const {
        if (!enabled || !stopped) return;
        // Sink types identify the existing frame-stage call sites. Emit the
        // first hitch promptly, then aggregate repeats into ten-second windows.
        static thread_local HitchClock::time_point lastReport{};
        static thread_local std::uint64_t repeats = 0;
        static thread_local double peakMs = 0;
        if (wallMs >= 20.0) {
            ++repeats;
            if (wallMs > peakMs) peakMs = wallMs;
        }
        if (repeats == 0) return;
        if (lastReport != HitchClock::time_point{} &&
            now - lastReport < std::chrono::seconds(10)) return;
        lastReport = now;
        const bool timeOk = before.timeKnown && after.timeKnown && after.kernel >= before.kernel && after.user >= before.user;
        const bool cycleOk = before.cyclesKnown && after.cyclesKnown && after.cycles >= before.cycles;
        char line[1024]{};
        const int n = std::snprintf(line, sizeof(line),
            "[VR][perf] FRAME_HITCH phase=%s frameId=%llu tid=%lu wallMs=%.3f cpuKnown=%d kernelMs=%.3f userMs=%.3f cyclesKnown=%d cycles=%llu result=%lld count=%llu maxMs=%.3f %.*s",
            phase, static_cast<unsigned long long>(frame), GetCurrentThreadId(), wallMs, timeOk,
            timeOk ? (after.kernel-before.kernel)/10000.0 : -1.0,
            timeOk ? (after.user-before.user)/10000.0 : -1.0, cycleOk,
            static_cast<unsigned long long>(cycleOk ? after.cycles-before.cycles : 0),
            static_cast<long long>(result), static_cast<unsigned long long>(repeats),
            peakMs, static_cast<int>(extra.size()), extra.empty() ? "" : extra.data());
        repeats = 0;
        peakMs = 0;
        if (n > 0 && n < static_cast<int>(sizeof(line))) sink(std::string_view(line, n));
    }
};

} // namespace gakumas::vr::perf
