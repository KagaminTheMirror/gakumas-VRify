#pragma once

#include "../frame/SingleFrameLoopContracts.hpp"

#include <cstdint>
#include <deque>
#include <mutex>

namespace gakumas::vr::input {

struct NativePointerSample {
    float u = 0, v = 0;
    bool held = false, down = false, up = false, active = false;
    std::uint64_t serial = 0;
};

// Worker/Unity publish, Unity snapshots once per EventSystem.Update. A
// transition is removed only after CampusInputModule has actually read it.
// Ticket leases keep a consumed hold across slow frames. Disconnect is 2 s
// without Unity Ack on a live hold, or on a queue that has stayed unacked
// since the first Publish after the last Ack. A completed click's Ack does
// not cancel the next press after a quiet gap. Publish/Renew still cannot
// refresh a hold's Ack clock.
class NativePointerQueue {
public:
    static constexpr std::uint64_t kOrphanCancelMs = 250;
    static constexpr std::uint64_t kDisconnectMs = frame::kPointerDisconnectMs;

    bool Publish(float u, float v, bool held, bool down, bool up, std::uint64_t now) {
        std::lock_guard lock(mutex_);
        lastTick_ = now;
        if (unackedSince_ == 0) {
            unackedSince_ = now;
        }
        if (cancelLatched_) {
            return true;
        }
        if (queue_.size() >= 32) return false;
        NativePointerSample next{u, v, held, down, up, true, ++serial_};
        if (!queue_.empty() && !queue_.back().down && !queue_.back().up && !down && !up)
            queue_.back() = next;
        else queue_.push_back(next);
        return true;
    }
    void Renew(std::uint64_t now) {
        std::lock_guard lock(mutex_);
        lastTick_ = now;
    }
    void RegisterWait(std::uint64_t frameId, std::uint64_t sessionGeneration) {
        std::lock_guard lock(mutex_);
        if (sessionGeneration_ != 0 && sessionGeneration_ != sessionGeneration) {
            CancelLocked();
        }
        sessionGeneration_ = sessionGeneration;
        waitFrameId_ = frameId;
        hasWait_ = frameId != 0;
    }
    void RegisterEndQueued(std::uint64_t frameId, std::uint64_t sessionGeneration) {
        std::lock_guard lock(mutex_);
        if (sessionGeneration_ != 0 && sessionGeneration_ != sessionGeneration) {
            CancelLocked();
        }
        sessionGeneration_ = sessionGeneration;
        endFrameId_ = frameId;
        hasEnd_ = frameId != 0;
    }
    void ClearLease() {
        std::lock_guard lock(mutex_);
        hasWait_ = false;
        hasEnd_ = false;
        waitFrameId_ = 0;
        endFrameId_ = 0;
    }
    void NoteFocusLost() {
        std::lock_guard lock(mutex_);
        CancelLocked();
        hasWait_ = false;
        hasEnd_ = false;
        waitFrameId_ = 0;
        endFrameId_ = 0;
    }
    void Cancel() {
        std::lock_guard lock(mutex_);
        CancelLocked();
    }
    NativePointerSample Begin(std::uint64_t now) {
        std::lock_guard lock(mutex_);
        if (now < lastTick_) {
            CancelLocked();
        } else if (HasLeaseLocked()) {
            // A completed click's Ack must not poison the next press after a
            // quiet gap (stereo hides the panel, then the user brings it back).
            // Disconnect only if a live hold or unacked queue has gone 2 s
            // without Unity confirming a consume.
            const std::uint64_t consumeOrigin = current_.held && lastConsumeConfirmMs_ != 0
                ? lastConsumeConfirmMs_
                : unackedSince_;
            if (consumeOrigin != 0 && now - consumeOrigin > kDisconnectMs) {
                cancelLatched_ = true;
                CancelLocked();
            }
        } else if (now - lastTick_ > kOrphanCancelMs) {
            CancelLocked();
        }
        if (!queue_.empty()) {
            if (lastObservedMs_ == 0) {
                lastObservedMs_ = now;
            }
            return queue_.front();
        }
        auto result = current_;
        result.down = result.up = false;
        return result;
    }
    void Acknowledge(NativePointerSample sample) {
        Acknowledge(sample, 0);
    }
    void Acknowledge(NativePointerSample sample, std::uint64_t now) {
        std::lock_guard lock(mutex_);
        current_ = sample;
        current_.down = current_.up = false;
        unackedSince_ = 0;
        if (now != 0) {
            lastConsumeConfirmMs_ = now;
            lastObservedMs_ = now;
        }
        if (sample.serial < cancelSerial_) {
            // Cancellation raced a Unity read. Retire the now-observed press
            // before any newer action, without throwing away that newer action.
            if (sample.held) queue_.push_front({-1, -1, false, false, true, true, ++serial_});
            else current_ = {};
            return;
        }
        // An up owns the release frame only. Idle desktop input returns next frame.
        if (sample.up) {
            current_.active = false;
            if (cancelLatched_) {
                cancelLatched_ = false;
                lastConsumeConfirmMs_ = 0;
                lastObservedMs_ = 0;
            }
        }
        if (!queue_.empty() && queue_.front().serial == sample.serial) queue_.pop_front();
    }
private:
    [[nodiscard]] bool HasLeaseLocked() const noexcept {
        return hasWait_ || hasEnd_;
    }
    void CancelLocked() {
        cancelSerial_ = ++serial_;
        queue_.clear();
        unackedSince_ = 0;
        if (current_.held) {
            // Release outside every UI target: cancellation must never click.
            queue_.push_back({-1, -1, false, false, true, true, ++serial_});
        } else current_ = {};
    }
    std::mutex mutex_;
    std::deque<NativePointerSample> queue_;
    NativePointerSample current_{};
    std::uint64_t lastTick_ = 0, serial_ = 0, cancelSerial_ = 0;
    std::uint64_t lastConsumeConfirmMs_ = 0;
    std::uint64_t lastObservedMs_ = 0;
    std::uint64_t unackedSince_ = 0;
    std::uint64_t sessionGeneration_ = 0;
    std::uint64_t waitFrameId_ = 0;
    std::uint64_t endFrameId_ = 0;
    bool hasWait_ = false;
    bool hasEnd_ = false;
    bool cancelLatched_ = false;
};

} // namespace gakumas::vr::input
