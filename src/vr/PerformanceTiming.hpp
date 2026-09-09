#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>

namespace gakumas::vr::perf {

// One accumulator per call site AND thread. No locks, GPU queries, allocation,
// or logging on the hot path; the sink runs at most once per second per site.
// These are inclusive CPU wall times, not GPU execution times. Nested scopes
// overlap and must not be added together. A stalled call reports after return.
struct Accumulator {
    using Clock = std::chrono::steady_clock;
    Clock::time_point window{};
    std::uint64_t count = 0;
    std::uint64_t over16 = 0;
    std::uint64_t over50 = 0;
    double totalMs = 0;
    double maxMs = 0;

    template<class Sink>
    void Add(Clock::time_point begin, Clock::time_point end,
             const char* name, Sink& sink, std::uint64_t a, std::uint64_t b) noexcept {
        if (count == 0) window = begin;
        const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
        ++count;
        totalMs += ms;
        if (ms > maxMs) maxMs = ms;
        if (ms >= 16.0) ++over16;
        if (ms >= 50.0) ++over50;
        const double windowMs = std::chrono::duration<double, std::milli>(end - window).count();
        if (windowMs < 1000.0) return;
        char line[512]{};
        const int length = std::snprintf(line, sizeof(line),
            "[VR][perf] PERF_TIMING stage=%s samples=%llu windowMs=%.3f hz=%.2f avgMs=%.3f maxMs=%.3f over16=%llu over50=%llu a=%llu b=%llu",
            name, static_cast<unsigned long long>(count), windowMs,
            1000.0 * static_cast<double>(count) / windowMs,
            totalMs / static_cast<double>(count), maxMs,
            static_cast<unsigned long long>(over16), static_cast<unsigned long long>(over50),
            static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
        *this = {};
        if (length > 0 && static_cast<std::size_t>(length) < sizeof(line)) {
            sink(std::string_view(line, static_cast<std::size_t>(length)));
        }
    }
};

template<class Sink>
class Scope {
public:
    Scope(Accumulator& accumulator, bool enabled, const char* name, Sink sink,
          std::uint64_t a = 0, std::uint64_t b = 0) noexcept
        : accumulator_(accumulator), enabled_(enabled), name_(name), sink_(std::move(sink)), a_(a), b_(b) {
        if (enabled_) begin_ = Accumulator::Clock::now();
    }
    ~Scope() noexcept { Stop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Stop() noexcept {
        if (!enabled_) return;
        enabled_ = false;
        accumulator_.Add(begin_, Accumulator::Clock::now(), name_, sink_, a_, b_);
    }
private:
    Accumulator& accumulator_;
    bool enabled_;
    const char* name_;
    Sink sink_;
    std::uint64_t a_, b_;
    Accumulator::Clock::time_point begin_{};
};

} // namespace gakumas::vr::perf
