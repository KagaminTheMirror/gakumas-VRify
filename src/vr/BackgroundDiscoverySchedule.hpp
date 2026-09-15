#pragma once
#include <cstdint>

namespace gakumas::vr {
// No Unity work here: deterministic retry policy for absent background objects.
struct BackgroundDiscoverySchedule {
    bool wasArmed = false;
    unsigned misses = 0;
    std::uint64_t due = 0;
    bool ShouldSearch(std::uint64_t now, bool armed, bool cached, bool retired) {
        const bool entering = armed && !wasArmed;
        wasArmed = armed;
        if (cached) { misses = 0; return false; }
        if (!armed) return false;
        if (entering || retired) { misses = 0; due = now; }
        return now >= due;
    }
    void Searched(std::uint64_t now, bool found) {
        if (found) { misses = 0; return; }
        // Cap at two seconds so a newly created background can still recover
        // promptly even when the renderer keeps the same armed state.
        const auto delay = misses == 0 ? 1000ULL : 2000ULL;
        if (misses == 0) ++misses;
        due = now + delay;
    }
};
}
