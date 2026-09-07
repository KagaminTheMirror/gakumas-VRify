#pragma once

#include <cstdint>
#include <deque>
#include <mutex>

namespace gakumas::vr::input {

struct NativePointerSample {
    float u = 0, v = 0;
    bool held = false, down = false, up = false, active = false;
    std::uint64_t serial = 0;
};

// Worker publishes, Unity snapshots once per EventSystem.Update. A transition
// is removed only after CampusInputModule has actually read it. Fast taps and
// module activation therefore cannot collapse down/up into one missed frame.
class NativePointerQueue {
public:
    bool Publish(float u, float v, bool held, bool down, bool up, std::uint64_t now) {
        std::lock_guard lock(mutex_);
        lastTick_ = now;
        if (queue_.size() >= 32) return false;
        NativePointerSample next{u, v, held, down, up, true, ++serial_};
        if (!queue_.empty() && !queue_.back().down && !queue_.back().up && !down && !up)
            queue_.back() = next;
        else queue_.push_back(next);
        return true;
    }
    void Renew(std::uint64_t now) { std::lock_guard lock(mutex_); lastTick_ = now; }
    void Cancel() { std::lock_guard lock(mutex_); CancelLocked(); }
    NativePointerSample Begin(std::uint64_t now) {
        std::lock_guard lock(mutex_);
        if (now < lastTick_ || now - lastTick_ > 250) CancelLocked();
        if (!queue_.empty()) return queue_.front();
        auto result = current_;
        result.down = result.up = false;
        return result;
    }
    void Acknowledge(NativePointerSample sample) {
        std::lock_guard lock(mutex_);
        current_ = sample;
        current_.down = current_.up = false;
        if (sample.serial < cancelSerial_) {
            // Cancellation raced a Unity read. Retire the now-observed press
            // before any newer action, without throwing away that newer action.
            if (sample.held) queue_.push_front({-1, -1, false, false, true, true, ++serial_});
            else current_ = {};
            return;
        }
        // An up owns the release frame only. Idle desktop input returns next frame.
        if (sample.up) current_.active = false;
        if (!queue_.empty() && queue_.front().serial == sample.serial) queue_.pop_front();
    }
private:
    void CancelLocked() {
        cancelSerial_ = ++serial_;
        queue_.clear();
        if (current_.held) {
            // Release outside every UI target: cancellation must never click.
            queue_.push_back({-1, -1, false, false, true, true, ++serial_});
        } else current_ = {};
    }
    std::mutex mutex_;
    std::deque<NativePointerSample> queue_;
    NativePointerSample current_{};
    std::uint64_t lastTick_ = 0, serial_ = 0, cancelSerial_ = 0;
};

} // namespace gakumas::vr::input
