#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
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
                std::string_view extra = {}) const {
        if (!enabled || !stopped || wallMs < 20.0) return;
        const bool timeOk = before.timeKnown && after.timeKnown && after.kernel >= before.kernel && after.user >= before.user;
        const bool cycleOk = before.cyclesKnown && after.cyclesKnown && after.cycles >= before.cycles;
        char line[1024]{};
        const int n = std::snprintf(line, sizeof(line),
            "[VR][perf] FRAME_HITCH phase=%s frameId=%llu tid=%lu wallMs=%.3f cpuKnown=%d kernelMs=%.3f userMs=%.3f cyclesKnown=%d cycles=%llu result=%lld %.*s",
            phase, static_cast<unsigned long long>(frame), GetCurrentThreadId(), wallMs, timeOk,
            timeOk ? (after.kernel-before.kernel)/10000.0 : -1.0,
            timeOk ? (after.user-before.user)/10000.0 : -1.0, cycleOk,
            static_cast<unsigned long long>(cycleOk ? after.cycles-before.cycles : 0),
            static_cast<long long>(result), static_cast<int>(extra.size()), extra.empty() ? "" : extra.data());
        if (n > 0 && n < static_cast<int>(sizeof(line))) sink(std::string_view(line, n));
    }
};

// Graphics-owner thread only. Bounded markers of work BEFORE xrEndFrame;
// S_FALSE means not observed complete (including unflushed work), NOT GPU time.
// Never Flush, wait, spin, Map, touch a swapchain image, or reuse a pending query.
class EndGpuMarker {
    struct Entry {
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        bool pending = false;
        HitchClock::time_point issued{};
    };
    std::array<Entry, 8> entries_{};
    ID3D11Device* device_ = nullptr; // Session-owned; Reset before device release.
    bool failed_ = false;
public:
    struct Sample {
        int slot = -1, before = -1, after = -1;
        unsigned pending = 0;
        double oldestMs = 0, probeMs = 0;
        HRESULT status = S_OK;
    };
    void Reset() { for (auto& e : entries_) e = {}; device_ = nullptr; failed_ = false; }
    Sample Begin(bool enabled, ID3D11Device* device, ID3D11DeviceContext* context) {
        Sample s;
        if (!enabled || !device || !context) return s;
        const auto start = HitchClock::now();
        if (device_ != device) { Reset(); device_ = device; }
        if (failed_) { s.status = E_FAIL; return s; }
        if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { s.status = E_INVALIDARG; return s; }
        for (auto& e : entries_) {
            if (!e.query) continue;
            if (e.pending) {
                const HRESULT hr = context->GetData(e.query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (FAILED(hr)) { failed_ = true; s.status = hr; break; }
                e.pending = hr != S_OK;
                if (e.pending) {
                    ++s.pending;
                    const double age = HitchMs(e.issued, start);
                    if (age > s.oldestMs) s.oldestMs = age;
                }
            }
        }
        if (!failed_) for (std::size_t i = 0; i < entries_.size(); ++i) {
            auto& e = entries_[i];
            if (e.pending) continue;
            if (!e.query) {
                const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
                s.status = device->CreateQuery(&desc, &e.query);
                if (FAILED(s.status)) { failed_ = true; break; }
            }
            context->End(e.query.Get());
            e.pending = true; e.issued = HitchClock::now(); s.slot = static_cast<int>(i);
            s.status = context->GetData(e.query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            s.before = s.status == S_OK ? 1 : s.status == S_FALSE ? 0 : -1;
            if (FAILED(s.status)) failed_ = true;
            break;
        }
        s.probeMs = HitchMs(start, HitchClock::now());
        return s;
    }
    void Finish(ID3D11DeviceContext* context, Sample& s) {
        if (s.slot < 0 || !context) return;
        const auto start = HitchClock::now();
        auto& e = entries_[static_cast<std::size_t>(s.slot)];
        s.status = context->GetData(e.query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        s.after = s.status == S_OK ? 1 : s.status == S_FALSE ? 0 : -1;
        if (s.status == S_OK) e.pending = false;
        if (FAILED(s.status)) failed_ = true;
        s.probeMs += HitchMs(start, HitchClock::now());
    }
    static void Describe(const Sample& s, char (&out)[256]) {
        std::snprintf(out, sizeof(out),
            "gpuIssued=%d gpuBefore=%d gpuAfter=%d priorPending=%u oldestPendingMs=%.3f gpuProbeMs=%.3f gpuHr=%ld",
            s.slot >= 0, s.before, s.after, s.pending, s.oldestMs, s.probeMs, static_cast<long>(s.status));
    }
};
}
