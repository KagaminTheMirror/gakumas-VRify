#include "ThumbstickScroll.hpp"

#include <algorithm>
#include <cmath>

namespace gakumas::vr::input {

ThumbstickScrollStep ThumbstickScrollIntegrator::Update(
    std::int64_t predictedDisplayTime,
    float verticalAxis) noexcept {
    ThumbstickScrollStep step;
    if (!std::isfinite(verticalAxis) ||
        std::abs(verticalAxis) <= kDeadzone) {
        step.stopped = active_;
        Reset();
        return step;
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
        step.started = true;
        return step;
    }

    if (direction != direction_) {
        direction_ = direction;
        lastSampleTime_ = predictedDisplayTime;
        fractionalWheelUnits_ = 0.0;
        step.directionChanged = true;
        return step;
    }

    const std::int64_t elapsedNanoseconds =
        predictedDisplayTime - lastSampleTime_;
    lastSampleTime_ = predictedDisplayTime;
    if (predictedDisplayTime <= 0 || elapsedNanoseconds <= 0 ||
        elapsedNanoseconds > kMaximumFrameGapNanoseconds) {
        fractionalWheelUnits_ = 0.0;
        step.timingReset = true;
        return step;
    }

    step.deltaSeconds = static_cast<float>(
        static_cast<double>(elapsedNanoseconds) / 1'000'000'000.0);
    step.unityDelta = step.unityUnitsPerSecond * step.deltaSeconds;
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
}

bool ThumbstickScrollIntegrator::Active() const noexcept {
    return active_;
}

} // namespace gakumas::vr::input
