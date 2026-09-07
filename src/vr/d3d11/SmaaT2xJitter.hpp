#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gakumas::vr::d3d11 {

struct SmaaT2xPhase final {
    float jitterX = 0.0F;
    float jitterY = 0.0F;
    std::array<float, 4> subsampleIndices{};
};

inline constexpr std::array<SmaaT2xPhase, 2> kSmaaT2xPhases{{
    {0.25F, -0.25F, {1.0F, 1.0F, 1.0F, 0.0F}},
    {-0.25F, 0.25F, {2.0F, 2.0F, 2.0F, 0.0F}},
}};

struct SmaaT2xClipOffset final {
    float x = 0.0F;
    float y = 0.0F;
};

[[nodiscard]] inline constexpr SmaaT2xClipOffset SmaaT2xClipOffsetForPhase(
    std::uint32_t phase,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    if (phase >= kSmaaT2xPhases.size() || width == 0U || height == 0U) {
        return {};
    }
    return {
        2.0F * kSmaaT2xPhases[phase].jitterX / static_cast<float>(width),
        2.0F * kSmaaT2xPhases[phase].jitterY / static_cast<float>(height),
    };
}

// Unity Matrix4x4 is column-major in memory. A left-multiplied clip-space
// translation changes only logical m02/m12 (floats 8/9); all asymmetric HMD
// frustum terms and depth coefficients remain untouched.
[[nodiscard]] inline bool ApplySmaaT2xProjectionJitter(
    float* columnMajorMatrix,
    std::uint32_t phase,
    std::uint32_t width,
    std::uint32_t height) noexcept {
    if (columnMajorMatrix == nullptr || phase >= kSmaaT2xPhases.size() ||
        width == 0U || height == 0U) {
        return false;
    }
    const SmaaT2xClipOffset offset =
        SmaaT2xClipOffsetForPhase(phase, width, height);
    columnMajorMatrix[8] -= offset.x;
    columnMajorMatrix[9] -= offset.y;
    return true;
}

// MotionVectorsPersistentData.Update receives CameraData by value. URP reads
// CameraData.m_ProjectionMatrix as its no-jitter projection, while an external
// camera projection jitter arrives with the phase already embedded there. At
// that one local call boundary, validate the live jittered matrix and replace
// it with the unjittered asymmetric HMD projection. The real render CameraData
// and Camera projection are untouched.
[[nodiscard]] inline bool PrepareSmaaT2xMotionHistoryProjection(
    float* liveProjection,
    const float* expectedJitteredProjection,
    const float* unjitteredProjection,
    float tolerance,
    float* incomingMaximumError = nullptr,
    float* correctedMaximumError = nullptr) noexcept {
    if (liveProjection == nullptr || expectedJitteredProjection == nullptr ||
        unjitteredProjection == nullptr || !std::isfinite(tolerance) ||
        tolerance < 0.0F) {
        return false;
    }
    float incomingError = 0.0F;
    for (std::size_t index = 0; index < 16U; ++index) {
        if (!std::isfinite(liveProjection[index]) ||
            !std::isfinite(expectedJitteredProjection[index]) ||
            !std::isfinite(unjitteredProjection[index])) {
            return false;
        }
        incomingError = (std::max)(
            incomingError,
            std::abs(liveProjection[index] - expectedJitteredProjection[index]));
    }
    if (incomingMaximumError != nullptr) {
        *incomingMaximumError = incomingError;
    }
    if (incomingError > tolerance) {
        return false;
    }
    float correctedError = 0.0F;
    for (std::size_t index = 0; index < 16U; ++index) {
        liveProjection[index] = unjitteredProjection[index];
        correctedError = (std::max)(
            correctedError,
            std::abs(liveProjection[index] - unjitteredProjection[index]));
    }
    if (correctedMaximumError != nullptr) {
        *correctedMaximumError = correctedError;
    }
    return true;
}

} // namespace gakumas::vr::d3d11
