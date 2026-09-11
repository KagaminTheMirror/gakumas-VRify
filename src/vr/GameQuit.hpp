#pragma once

#include <chrono>
#include <mutex>

namespace gakumas::vr {

// No XR or window calls under this lock. The clock is supplied by the caller.
class GameQuit final {
public:
    using Clock = std::chrono::steady_clock;
    enum class Result { None, Ended, NoSession, RuntimeLost, Timeout, TimerFailed };
    struct Snapshot {
        bool requested = false;
        bool exitAttempted = false;
        Result result = Result::None;
        bool CanClose() const { return result != Result::None; }
    };
    void Reset() {
        std::lock_guard lock(mutex_);
        state_ = {};
        deadline_ = {};
    }
    bool Request(Clock::time_point now) {
        std::lock_guard lock(mutex_);
        if (state_.requested) return false;
        deadline_ = now + std::chrono::seconds(2);
        state_.requested = true;
        return true;
    }
    bool BeginExitAttempt() {
        std::lock_guard lock(mutex_);
        if (!state_.requested || state_.CanClose() || state_.exitAttempted) return false;
        state_.exitAttempted = true;
        return true;
    }
    void Complete(Result result) {
        std::lock_guard lock(mutex_);
        if (state_.requested && !state_.CanClose()) state_.result = result;
    }
    Snapshot Poll(Clock::time_point now) {
        std::lock_guard lock(mutex_);
        if (state_.requested && !state_.CanClose() && now >= deadline_)
            state_.result = Result::Timeout;
        return state_;
    }
    Snapshot Read() const {
        std::lock_guard lock(mutex_);
        return state_;
    }
private:
    mutable std::mutex mutex_;
    Snapshot state_{};
    Clock::time_point deadline_{};
};

inline const char* GameQuitResultName(GameQuit::Result result) {
    switch (result) {
    case GameQuit::Result::None: return "pending";
    case GameQuit::Result::Ended: return "session-ended";
    case GameQuit::Result::NoSession: return "no-session";
    case GameQuit::Result::RuntimeLost: return "runtime-lost";
    case GameQuit::Result::Timeout: return "timeout";
    case GameQuit::Result::TimerFailed: return "timer-failed";
    }
    return "unknown";
}
} // namespace gakumas::vr
