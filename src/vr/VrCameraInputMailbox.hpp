#pragma once

#include <cstdint>
#include <mutex>

namespace gakumas::vr::camera {

// One controller-input snapshot per OpenXR frame for the VR free camera.
// Press counts are cumulative rising-edge counters from the worker; the
// Unity-thread consumer diffs them against the last value it consumed, so a
// missed frame never drops a press.
struct VrCameraInputSample {
    bool valid = false;
    bool menuVisible = false;
    float leftStickX = 0.0F;
    float leftStickY = 0.0F;
    float rightStickX = 0.0F;
    float rightStickY = 0.0F;
    // Left trigger held and the left pointer is not on a visible UI quad.
    bool sprintHeld = false;
    std::uint32_t modePressCount = 0;
    std::uint32_t charaPressCount = 0;
    std::uint32_t resetPressCount = 0;
    // Right-A presses routed to the game's photo shutter (photo scenes only).
    std::uint32_t photoPressCount = 0;
    std::int64_t publishTimeNanoseconds = 0;
    std::uint64_t frameId = 0;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t inputEpoch = 0;
    bool cancelled = false;
};

// Same rationale as StereoPoseMailbox: a mutex over a tiny struct, published
// once per frame by the OpenXR worker and copied by the Unity thread.
class VrCameraInputMailbox final {
public:
    void Publish(const VrCameraInputSample& sample) noexcept;
    void Invalidate() noexcept;
    [[nodiscard]] bool Read(VrCameraInputSample& sample) const noexcept;

private:
    mutable std::mutex mutex_;
    VrCameraInputSample sample_{};
};

} // namespace gakumas::vr::camera
