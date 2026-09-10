#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>

namespace gakumas::vr::perf {

// Sampled owner-thread wall-time tree. No Unity calls, allocations, locks or
// logging while collecting. Exclusive times partition the measured outer call;
// they include scheduling/driver waits, and are NOT GPU execution times.
struct SrpTrace {
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr int Capacity = 2048;
    struct Node {
        const char* name = "unknown";
        const char* role = "none";
        std::uintptr_t object = 0;
        int event = -1;
        int parent = -1;
        Time begin{};
        double wallMs = 0, childrenMs = 0;
        bool closed = false;
    };
    std::array<Node, Capacity> nodes{};
    Time nextBurst{};
    int burstRemaining = 0, count = 0, top = -1, cameraNode = -1;
    unsigned dropped = 0, errors = 0, aliases = 0;
    bool active = false;
    std::uint64_t loop = 0, ticket = 0, session = 0, epoch = 0;

    static double Ms(Time a, Time b) noexcept {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }
    bool Select(bool enabled, Time now = Clock::now()) noexcept {
        if (!enabled) return false;
        ++loop;
        if (now >= nextBurst) {
            nextBurst = now + std::chrono::seconds(2);
            burstRemaining = 4;
        }
        if (burstRemaining == 0) return false;
        --burstRemaining;
        return true;
    }
    void Begin(std::uint64_t frame, std::uint64_t generation,
               std::uint64_t referenceEpoch, Time now = Clock::now()) noexcept {
        ticket = frame; session = generation; epoch = referenceEpoch;
        count = 0; top = cameraNode = -1;
        dropped = errors = aliases = 0;
        active = true;
        Push("outer", 0, "none", -1, now);
    }
    int Push(const char* name, std::uintptr_t object = 0,
             const char* role = nullptr, int event = -1,
             Time now = Clock::now()) noexcept {
        if (!active) return -1;
        if (count == Capacity) { ++dropped; return -1; }
        const int id = count++;
        nodes[id] = {};
        auto& n = nodes[id];
        n.name = name; n.object = object; n.event = event; n.parent = top;
        n.role = role ? role : (top >= 0 ? nodes[top].role : "none");
        n.begin = now;
        top = id;
        return id;
    }
    void Pop(int id, Time now = Clock::now()) noexcept {
        if (!active || id < 0) return;
        if (id != top) { ++errors; return; }
        auto& n = nodes[id];
        n.wallMs = Ms(n.begin, now); n.closed = true;
        top = n.parent;
        if (top >= 0) nodes[top].childrenMs += n.wallMs;
    }
    void BeginCamera(std::uintptr_t camera, const char* role,
                     Time now = Clock::now()) noexcept {
        if (!active) return;
        if (cameraNode >= 0) {
            // RenderPipeline wrapper may call RenderPipelineManager's wrapper.
            if (nodes[cameraNode].object == camera) { ++aliases; return; }
            ++errors; // unsupported nested camera: invalidate this sample
            return;
        }
        cameraNode = Push("camera", camera, role, -1, now);
    }
    void EndCamera(std::uintptr_t camera, Time now = Clock::now()) noexcept {
        if (!active) return;
        if (cameraNode < 0 || nodes[cameraNode].object != camera) {
            ++errors; return;
        }
        Pop(cameraNode, now);
        cameraNode = -1;
    }
    void End(Time now = Clock::now()) noexcept {
        if (!active) return;
        if (top != 0 || cameraNode >= 0) ++errors;
        // Close unfinished spans at the boundary, but mark sample invalid.
        while (top >= 0) Pop(top, now);
        active = false;
    }
    template<class Sink>
    void Emit(unsigned long tid, Sink sink, const char* prefix = "SRP_PERF") const noexcept {
        if (count == 0) return;
        char line[768]{};
        std::snprintf(line, sizeof(line),
            "[VR][perf] %s_LOOP tid=%lu loop=%llu ticket=%llu session=%llu epoch=%llu wallMs=%.6f nodes=%d dropped=%u errors=%u aliases=%u clock=cpu-wall",
            prefix, tid, static_cast<unsigned long long>(loop),
            static_cast<unsigned long long>(ticket),
            static_cast<unsigned long long>(session),
            static_cast<unsigned long long>(epoch), nodes[0].wallMs,
            count, dropped, errors, aliases);
        sink(std::string_view(line));
        for (int i = 0; i < count; ++i) {
            const auto& n = nodes[i];
            std::snprintf(line, sizeof(line),
                "[VR][perf] %s_NODE tid=%lu loop=%llu id=%d parent=%d name=%s role=%s object=0x%llx event=%d startMs=%.6f wallMs=%.6f selfMs=%.6f",
                prefix, tid, static_cast<unsigned long long>(loop), i, n.parent,
                n.name, n.role, static_cast<unsigned long long>(n.object), n.event,
                Ms(nodes[0].begin, n.begin), n.wallMs, n.wallMs - n.childrenMs);
            sink(std::string_view(line));
        }
    }
};

// One trace across translation units, so existing UI/blur hooks contribute
// children to the owning render thread's sampled tree. Inactive outside the
// frozen diagnostic startup gate; no hook here starts sampling on its own.
inline thread_local SrpTrace srpPerformance;

class SrpSpan {
public:
    explicit SrpSpan(SrpTrace& trace, const char* name,
                     std::uintptr_t object = 0) noexcept : trace_(trace) {
        if (trace.active) id_ = trace.Push(name, object);
    }
    ~SrpSpan() { Stop(); }
    SrpSpan(const SrpSpan&) = delete;
    SrpSpan& operator=(const SrpSpan&) = delete;
    void Stop() noexcept { if (id_ >= 0) { trace_.Pop(id_); id_ = -1; } }
    void Describe(const char* name, int event) noexcept {
        if (id_ >= 0) { trace_.nodes[id_].name = name; trace_.nodes[id_].event = event; }
    }
private:
    SrpTrace& trace_;
    int id_ = -1;
};

// Independent sampled root for existing non-SRP work. RAII closes early-return
// paths before emitting; prefix keeps its sequence separate from SRP loops.
template<class Sink>
class TraceCapture {
public:
    TraceCapture(SrpTrace& trace, bool enabled, unsigned long tid,
                 const char* prefix, Sink sink) noexcept
        : trace_(trace), tid_(tid), prefix_(prefix), sink_(std::move(sink)),
          selected_(!trace.active && trace.Select(enabled)) {
        if (selected_) trace_.Begin(0, 0, 0); // no XR ticket attribution available
    }
    ~TraceCapture() noexcept {
        if (!selected_) return;
        trace_.End();
        const auto begin = SrpTrace::Clock::now();
        trace_.Emit(tid_, sink_, prefix_);
        char line[192]{};
        std::snprintf(line, sizeof(line), "[VR][perf] %s_FLUSH tid=%lu loop=%llu wallMs=%.6f",
            prefix_, tid_, static_cast<unsigned long long>(trace_.loop),
            SrpTrace::Ms(begin, SrpTrace::Clock::now()));
        sink_(std::string_view(line));
    }
    TraceCapture(const TraceCapture&) = delete;
    TraceCapture& operator=(const TraceCapture&) = delete;
private:
    SrpTrace& trace_;
    unsigned long tid_;
    const char* prefix_;
    Sink sink_;
    bool selected_;
};

} // namespace gakumas::vr::perf
