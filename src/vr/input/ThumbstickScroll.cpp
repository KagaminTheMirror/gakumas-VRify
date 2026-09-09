#include "ThumbstickScroll.hpp"

#include <algorithm>
#include <cmath>

namespace gakumas::vr::input {

ThumbstickScrollStep ThumbstickScrollIntegrator::Update(
    std::int64_t predictedDisplayTime,
    float verticalAxis,
    std::uint64_t sessionGeneration,
    std::uint64_t inputEpoch) noexcept {
    ThumbstickScrollStep step;
    if (!std::isfinite(verticalAxis) ||
        std::abs(verticalAxis) <= kDeadzone) {
        step.stopped = active_;
        Reset();
        return step;
    }

    const bool epochChanged = epochReady_ &&
        (sessionGeneration_ != sessionGeneration ||
         inputEpoch_ != inputEpoch);
    if (epochChanged) {
        Reset();
    }

    const float clampedAxis = std::clamp(verticalAxis, -1.0F, 1.0F);
    const float magnitude = std::abs(clampedAxis);
    step.normalizedDeflection = std::clamp(
        (magnitude - kDeadzone) / (1.0F - kDeadzone),
        0.0F,
        1.0F);
    const float response = step.normalizedDeflection * step.normalizedDeflection;
    const int direction = clampedAxis > 0.0F ? 1 : -1;
    step.wheelUnitsPerSecond =
        static_cast<float>(direction) * kMaximumWheelUnitsPerSecond * response;
    step.unityUnitsPerSecond =
        static_cast<float>(direction) * kMaximumUnityUnitsPerSecond * response;

    if (!active_) {
        active_ = true;
        direction_ = direction;
        lastSampleTime_ = predictedDisplayTime;
        fractionalWheelUnits_ = 0.0;
        sessionGeneration_ = sessionGeneration;
        inputEpoch_ = inputEpoch;
        epochReady_ = true;
        step.started = true;
        return step;
    }

    if (direction != direction_) {
        direction_ = direction;
        lastSampleTime_ = predictedDisplayTime;
        fractionalWheelUnits_ = 0.0;
        sessionGeneration_ = sessionGeneration;
        inputEpoch_ = inputEpoch;
        epochReady_ = true;
        step.directionChanged = true;
        return step;
    }

    const std::int64_t elapsedNanoseconds =
        predictedDisplayTime - lastSampleTime_;
    lastSampleTime_ = predictedDisplayTime;
    sessionGeneration_ = sessionGeneration;
    inputEpoch_ = inputEpoch;
    epochReady_ = true;
    if (predictedDisplayTime <= 0 || elapsedNanoseconds <= 0) {
        fractionalWheelUnits_ = 0.0;
        step.timingReset = true;
        return step;
    }

    std::int64_t compensatedNanoseconds = elapsedNanoseconds;
    if (elapsedNanoseconds > kMaximumFrameGapNanoseconds) {
        compensatedNanoseconds = kMaximumFrameGapNanoseconds;
        step.timingCapped = true;
    }

    step.deltaSeconds = static_cast<float>(
        static_cast<double>(compensatedNanoseconds) / 1'000'000'000.0);
    step.unityDelta = std::clamp(
        step.unityUnitsPerSecond * step.deltaSeconds,
        -kMaximumUnityUnitsPerTicket,
        kMaximumUnityUnitsPerTicket);
    fractionalWheelUnits_ +=
        static_cast<double>(step.wheelUnitsPerSecond) * step.deltaSeconds;
    step.wheelDelta = static_cast<int>(fractionalWheelUnits_);
    fractionalWheelUnits_ -= static_cast<double>(step.wheelDelta);
    return step;
}

void ThumbstickScrollIntegrator::Reset() noexcept {
    active_ = false;
    direction_ = 0;
    lastSampleTime_ = 0;
    fractionalWheelUnits_ = 0.0;
    sessionGeneration_ = 0;
    inputEpoch_ = 0;
    epochReady_ = false;
}

bool ThumbstickScrollIntegrator::Active() const noexcept {
    return active_;
}

} // namespace gakumas::vr::input
