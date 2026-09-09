#pragma once

#include "FrameCoordinator.hpp"
#include <chrono>

namespace gakumas::vr::frame {

// Transfers the immutable ticket from the Unity thread to one render callback.
// The callback takes ownership before End; no shared-slot cleanup follows End.
class FrameEndDispatch final {
public:
    [[nodiscard]] bool Publish(int eventId, const FrameIdentity& ticket) noexcept {
        std::lock_guard lock(mutex_);
        if (eventId <= 0 || ticket.frameId == 0 || active_ || closed_) {
            return false;
        }
        ticket_ = ticket;
        eventId_ = eventId;
        active_ = true;
        submitted_ = false;
        return true;
    }

    [[nodiscard]] bool Take(int eventId, FrameIdentity& ticket) noexcept {
        std::lock_guard lock(mutex_);
        if (eventId <= 0 || eventId != eventId_) {
            return false;
        }
        ticket = ticket_;
        ticket_ = {};
        eventId_ = 0;
        return true;
    }

    // Optional handoff after Submit. Ordinary CPU Wait no longer uses this:
    // GPU submit parameters are frozen on the ticket. Reference-space frames
    // wait for the whole callback via WaitForIdle. No lock spans GPU/XR work.
    void CompleteSubmission() noexcept {
        std::lock_guard lock(mutex_);
        submitted_ = true;
        changed_.notify_all();
    }

    void CompleteCallback() noexcept {
        std::lock_guard lock(mutex_);
        active_ = false;
        submitted_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] bool WaitForSubmission(std::chrono::milliseconds timeout) noexcept {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return !active_ || submitted_; });
    }

    [[nodiscard]] bool WaitForIdle(std::chrono::milliseconds timeout) noexcept {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return !active_; });
    }

    [[nodiscard]] bool Reset() noexcept {
        std::lock_guard lock(mutex_);
        if (active_) return false;
        ticket_ = {};
        eventId_ = 0;
        closed_ = false;
        return true;
    }

    void Close() noexcept {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    FrameIdentity ticket_{};
    int eventId_ = 0;
    bool active_ = false;
    bool submitted_ = true;
    bool closed_ = false;
};

} // namespace gakumas::vr::frame
