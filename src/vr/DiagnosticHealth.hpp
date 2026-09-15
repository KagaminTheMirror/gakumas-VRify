#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace gakumas::vr {

// Only explicitly identified steady health events are coalesced. Unknown
// events and all action/failure messages retain their existing visibility.
inline bool IsDiagnosticStateEvent(std::string_view message) noexcept {
    constexpr std::string_view tokens[]{
        "APPLY", "APPLIED", "RESTORE", "FIGHT", "RECREATE", "SKIP", "IGNORE",
        "FAIL", "FAULT", "ERROR", "RECOVER", "FALLBACK", "REJECT", "INVALID",
        "apply", "restore", "fight", "recreate", "skip", "ignore", "fail",
        "fault", "error", "recover", "fallback", "reject", "invalid"};
    for (auto token : tokens) if (message.find(token) != message.npos) return true;
    return false;
}

class DiagnosticHealth final {
public:
    using Clock = std::chrono::steady_clock;
    struct Decision { bool emit = true; std::uint64_t suppressed = 0; };

    Decision Observe(std::string_view message, Clock::time_point now) noexcept {
        if (IsDiagnosticStateEvent(message)) return {};
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto at = message.find(events[i]);
            if (at == message.npos) continue;
            const auto end = at + events[i].size();
            if (end < message.size() && message[end] != ' ') continue;
            auto& slot = slots_[i];
            if (slot.seen && now - slot.last < std::chrono::seconds(10)) {
                ++slot.suppressed;
                return {false, 0};
            }
            const auto suppressed = slot.suppressed;
            slot = {now, 0, true};
            return {true, suppressed};
        }
        return {};
    }

private:
    static constexpr std::array<std::string_view, 23> events{
        "SMAA_T2X_FRAME", "TSCMAA_FRAME", "TEMPORAL_MV_ORDERED",
        "MAILBOX_PUBLISH_BEGIN", "MAILBOX_PUBLISH_OK", "NATIVE_POINTER_CACHE",
        "WAIT_ENTERED", "WAIT_RETURNED", "CPU_READY", "GRAPHICS_DISPATCH",
        "GRAPHICS_SUBMITTED", "BEGIN_ENTERED", "END_QUEUED", "END_ENTERED",
        "END_RETURNED", "TICKET_CONSUME", "TICKET_REPEAT", "FRAME_DRIVE",
        "COMPUTE_VIEW_DIR", "UI_INPUT_STATE", "GRIP_BLUR_SOURCE_SUBMIT_HEALTH",
        "SMAA_T2X_MV_MATRIX", "SMAA_T2X_MV_BOUNDARY"};
    struct Slot { Clock::time_point last{}; std::uint64_t suppressed = 0; bool seen = false; };
    std::array<Slot, events.size()> slots_{};
};

} // namespace gakumas::vr
