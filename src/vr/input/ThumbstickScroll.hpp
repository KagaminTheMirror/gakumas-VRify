#pragma once

#include "../frame/SingleFrameLoopContracts.hpp"

#include <cstdint>

namespace gakumas::vr::input {

struct ThumbstickScrollStep {
    int wheelDelta = 0;
    float normalizedDeflection = 0.0F;
    float wheelUnitsPerSecond = 0.0F;
    float unityUnitsPerSecond = 0.0F;
    float unityDelta = 0.0F;
    float deltaSeconds = 0.0F;
    bool started = false;
    bool stopped = false;
    bool directionChanged = false;
    bool timingReset = false;
    bool timingCapped = false;
};

// Converts a sampled OpenXR thumbstick axis into high-resolution Windows
// wheel units. The fractional accumulator makes total travel independent of
// headset refresh rate while each individual message remains small.
class ThumbstickScrollIntegrator {
public:
    static constexpr float kDeadzone = 0.35F;
    static constexpr float kMaximumWheelUnitsPerSecond = 240.0F;
    // The live game multiplies CampusScrollRect event deltas by 80. 18.75 Unity
    // input units per second therefore targets 1500 UI units/s at full
    // deflection while retaining truly fractional frame steps.
    static constexpr float kMaximumUnityUnitsPerSecond =
        frame::kScrollMaxUnityUnitsPerSecond;
    static constexpr std::int64_t kMaximumFrameGapNanoseconds =
        frame::kScrollMaxCompensationNanoseconds;
    static constexpr float kMaximumUnityUnitsPerTicket =
        frame::kScrollMaxUnityUnitsPerTicket;

    [[nodiscard]] ThumbstickScrollStep Update(
        std::int64_t predictedDisplayTime,
        float verticalAxis,
        std::uint64_t sessionGeneration = 0,
        std::uint64_t inputEpoch = 0) noexcept;
    void Reset() noexcept;
    [[nodiscard]] bool Active() const noexcept;

private:
    bool active_ = false;
    int direction_ = 0;
    std::int64_t lastSampleTime_ = 0;
    double fractionalWheelUnits_ = 0.0;
    std::uint64_t sessionGeneration_ = 0;
    std::uint64_t inputEpoch_ = 0;
    bool epochReady_ = false;
};

} // namespace gakumas::vr::input
