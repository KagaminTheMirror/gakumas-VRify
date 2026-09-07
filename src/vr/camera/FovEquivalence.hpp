#pragma once

#include <cmath>
#include <cstdint>

namespace gakumas::vr::camera {

struct ProjectionTerms {
    float m00 = 0.0F;
    float m02 = 0.0F;
    float m11 = 0.0F;
    float m12 = 0.0F;
};

struct ProjectionMap {
    float scaleX = 0.0F;
    float biasX = 0.0F;
    float scaleY = 0.0F;
    float biasY = 0.0F;
};

struct MotionLogDelta {
    float projection = 0.0F;
    float lookAtDistance = 0.0F;
    float residual = 0.0F;
};

inline bool TryBuildProjectionTerms(
    float verticalFovDegrees,
    float aspect,
    float lensShiftX,
    float lensShiftY,
    ProjectionTerms& result) noexcept {
    constexpr float kPi = 3.14159265358979323846F;
    if (!std::isfinite(verticalFovDegrees) ||
        !std::isfinite(aspect) || !std::isfinite(lensShiftX) ||
        !std::isfinite(lensShiftY) || verticalFovDegrees <= 0.0F ||
        verticalFovDegrees >= 179.0F || aspect <= 0.0F) {
        return false;
    }
    const float tangent = std::tan(verticalFovDegrees * kPi / 360.0F);
    if (!std::isfinite(tangent) || tangent <= 0.0F) {
        return false;
    }
    ProjectionTerms terms{};
    terms.m11 = 1.0F / tangent;
    terms.m00 = terms.m11 / aspect;
    terms.m02 = 2.0F * lensShiftX;
    terms.m12 = 2.0F * lensShiftY;
    if (!std::isfinite(terms.m00) || !std::isfinite(terms.m11) ||
        terms.m00 <= 0.0F || terms.m11 <= 0.0F) {
        return false;
    }
    result = terms;
    return true;
}

inline bool TryBuildProjectionMap(
    const ProjectionTerms& source,
    const ProjectionTerms& destination,
    ProjectionMap& result) noexcept {
    if (!std::isfinite(source.m00) || !std::isfinite(source.m02) ||
        !std::isfinite(source.m11) || !std::isfinite(source.m12) ||
        !std::isfinite(destination.m00) || !std::isfinite(destination.m02) ||
        !std::isfinite(destination.m11) || !std::isfinite(destination.m12) ||
        source.m00 <= 0.0F || source.m11 <= 0.0F ||
        destination.m00 <= 0.0F || destination.m11 <= 0.0F) {
        return false;
    }
    ProjectionMap map{};
    map.scaleX = destination.m00 / source.m00;
    map.biasX = destination.m02 - map.scaleX * source.m02;
    map.scaleY = destination.m11 / source.m11;
    map.biasY = destination.m12 - map.scaleY * source.m12;
    result = map;
    return std::isfinite(map.scaleX) && std::isfinite(map.biasX) &&
        std::isfinite(map.scaleY) && std::isfinite(map.biasY) &&
        map.scaleX > 0.0F && map.scaleY > 0.0F;
}

inline bool TryScaleViewportSize(
    float authoredX,
    float authoredY,
    float projectionScaleX,
    float projectionScaleY,
    float& writtenX,
    float& writtenY) noexcept {
    if (!std::isfinite(authoredX) || !std::isfinite(authoredY) ||
        !std::isfinite(projectionScaleX) ||
        !std::isfinite(projectionScaleY) || projectionScaleX <= 0.0F ||
        projectionScaleY <= 0.0F) {
        return false;
    }
    const float x = authoredX * projectionScaleX;
    const float y = authoredY * projectionScaleY;
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    writtenX = x;
    writtenY = y;
    return true;
}

inline bool TryBuildVerticalUniformProjectionScale(
    float projectionScaleY,
    float& scaleX,
    float& scaleY) noexcept {
    if (!std::isfinite(projectionScaleY) || projectionScaleY <= 0.0F) {
        return false;
    }
    scaleX = projectionScaleY;
    scaleY = projectionScaleY;
    return true;
}

inline bool ProjectionEquivalentProFlareReady(
    bool layoutReady,
    bool scheduleHookReady,
    bool elementHookReady) noexcept {
    return layoutReady && scheduleHookReady && elementHookReady;
}

inline bool TryComputeMotionLogDelta(
    float previousProjectionM11,
    float projectionM11,
    float previousLookAtDistance,
    float lookAtDistance,
    MotionLogDelta& result) noexcept {
    if (!std::isfinite(previousProjectionM11) ||
        !std::isfinite(projectionM11) ||
        !std::isfinite(previousLookAtDistance) ||
        !std::isfinite(lookAtDistance) || previousProjectionM11 <= 0.0F ||
        projectionM11 <= 0.0F || previousLookAtDistance <= 0.0F ||
        lookAtDistance <= 0.0F) {
        return false;
    }
    MotionLogDelta delta{};
    delta.projection = std::log(projectionM11 / previousProjectionM11);
    delta.lookAtDistance = std::log(
        lookAtDistance / previousLookAtDistance);
    delta.residual = delta.projection - delta.lookAtDistance;
    result = delta;
    return std::isfinite(delta.projection) &&
        std::isfinite(delta.lookAtDistance) &&
        std::isfinite(delta.residual);
}

inline float EquivalentVlBloomDiffusion(
    float authoredDiffusion,
    float projectionScale) noexcept {
    if (!std::isfinite(authoredDiffusion) ||
        !std::isfinite(projectionScale) || projectionScale <= 0.0F) {
        return authoredDiffusion;
    }
    return authoredDiffusion + std::log2(projectionScale);
}

inline bool TryBuildCenteredVerticalProjectionScale(
    float sourceVerticalFovDegrees,
    float eyeVerticalFovDegrees,
    float& scale) noexcept {
    ProjectionTerms source{};
    ProjectionTerms eye{};
    if (!TryBuildProjectionTerms(
            sourceVerticalFovDegrees, 1.0F, 0.0F, 0.0F, source) ||
        !TryBuildProjectionTerms(
            eyeVerticalFovDegrees, 1.0F, 0.0F, 0.0F, eye) ||
        source.m11 <= 0.0F) {
        return false;
    }
    const float value = eye.m11 / source.m11;
    if (!std::isfinite(value) || value <= 0.0F) {
        return false;
    }
    scale = value;
    return true;
}

inline bool TryRoundEquivalentVlBloomDiffusion(
    std::int32_t authoredDiffusion,
    float projectionScale,
    std::int32_t& written) noexcept {
    if (authoredDiffusion <= 0) {
        return false;
    }
    const float equivalent = EquivalentVlBloomDiffusion(
        static_cast<float>(authoredDiffusion), projectionScale);
    if (!std::isfinite(equivalent)) {
        return false;
    }
    const auto rounded = std::lround(equivalent);
    written = rounded < 1 ? 1 : static_cast<std::int32_t>(rounded);
    return true;
}

} // namespace gakumas::vr::camera
