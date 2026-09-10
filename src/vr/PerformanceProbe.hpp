#pragma once
#include "PerformanceTiming.hpp"
#include "config/VrifyConfig.hpp"
#include <Windows.h>
#include <string>

// Existing call sites only: inclusive wall time, one summary/second per
// site/thread. Sinks run after Stop, never on every frame. Nested totals overlap.
#define VR_PERF_SCOPE(variable, label, sink) \
    static thread_local gakumas::vr::perf::Accumulator variable##Timing; \
    gakumas::vr::perf::Scope variable(variable##Timing, \
        GakumasLocal::Config::vrDiagnosticsStartupEnabled, label, \
        [&](std::string_view perfLine) noexcept { \
            (sink)(std::string(perfLine) + " tid=" + std::to_string(GetCurrentThreadId())); \
        })
