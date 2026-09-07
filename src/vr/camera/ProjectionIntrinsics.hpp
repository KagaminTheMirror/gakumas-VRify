#pragma once

#include <cmath>

namespace gakumas::vr::camera {

struct ProjectionIntrinsics {
    float verticalFieldOfViewDegrees = 0.0F;
    float aspect = 0.0F;
    float lensShiftX = 0.0F;
    float lensShiftY = 0.0F;
    float projectionM00 = 0.0F;
    float projectionM02 = 0.0F;
    float projectionM11 = 0.0F;
    float projectionM12 = 0.0F;
};

struct PhysicalCameraIntrinsics {
    float sensorWidthMillimeters = 0.0F;
    float sensorHeightMillimeters = 0.0F;
    float focalLengthMillimeters = 0.0F;
};

// Unity permits a custom off-centre projection matrix while retaining stale
// scalar Camera intrinsics. VLSRP's sky/cloud path reconstructs view rays from
// fieldOfView, aspect, and lensShift, so derive scalar values that describe the
// same OpenXR tangent-space frustum before installing the exact matrix.
inline bool TryDeriveProjectionIntrinsics(
    float angleLeft,
    float angleRight,
    float angleDown,
    float angleUp,
    ProjectionIntrinsics& result) noexcept {
    constexpr float kPi = 3.14159265358979323846F;
    constexpr float kRadiansToDegrees = 180.0F / kPi;
    constexpr float kMinimumSpan = 1.0e-5F;

    if (!std::isfinite(angleLeft) || !std::isfinite(angleRight) ||
        !std::isfinite(angleDown) || !std::isfinite(angleUp) ||
        angleLeft <= -kPi * 0.5F || angleRight >= kPi * 0.5F ||
        angleDown <= -kPi * 0.5F || angleUp >= kPi * 0.5F) {
        return false;
    }

    const float left = std::tan(angleLeft);
    const float right = std::tan(angleRight);
    const float bottom = std::tan(angleDown);
    const float top = std::tan(angleUp);
    const float horizontalSpan = right - left;
    const float verticalSpan = top - bottom;
    if (!std::isfinite(horizontalSpan) || !std::isfinite(verticalSpan) ||
        horizontalSpan <= kMinimumSpan || verticalSpan <= kMinimumSpan) {
        return false;
    }

    const float halfHeight = verticalSpan * 0.5F;
    ProjectionIntrinsics derived{};
    derived.verticalFieldOfViewDegrees =
        2.0F * std::atan(halfHeight) * kRadiansToDegrees;
    derived.aspect = horizontalSpan / verticalSpan;
    derived.lensShiftX = (left + right) / (2.0F * horizontalSpan);
    derived.lensShiftY = (bottom + top) / (2.0F * verticalSpan);
    derived.projectionM00 = 2.0F / horizontalSpan;
    derived.projectionM02 = (left + right) / horizontalSpan;
    derived.projectionM11 = 2.0F / verticalSpan;
    derived.projectionM12 = (bottom + top) / verticalSpan;

    if (!std::isfinite(derived.verticalFieldOfViewDegrees) ||
        !std::isfinite(derived.aspect) ||
        !std::isfinite(derived.lensShiftX) ||
        !std::isfinite(derived.lensShiftY) ||
        derived.verticalFieldOfViewDegrees <= 0.0F ||
        derived.verticalFieldOfViewDegrees >= 179.0F ||
        derived.aspect <= 0.0F) {
        return false;
    }
    result = derived;
    return true;
}

// Keep the source camera's physical sensor height as the scale anchor, then
// derive a film gate and focal length whose normalized projection matches the
// OpenXR eye frustum.  Sensor width follows the eye aspect so Unity's Gate Fit
// modes all converge on the same fitted intrinsics.  Aperture and focus
// distance deliberately remain untouched on the Camera.
inline bool TryDerivePhysicalCameraIntrinsics(
    const ProjectionIntrinsics& projection,
    float sourceSensorHeightMillimeters,
    PhysicalCameraIntrinsics& result) noexcept {
    constexpr float kMinimumPhysicalValue = 1.0e-4F;

    if (!std::isfinite(sourceSensorHeightMillimeters) ||
        sourceSensorHeightMillimeters <= kMinimumPhysicalValue ||
        !std::isfinite(projection.verticalFieldOfViewDegrees) ||
        !std::isfinite(projection.aspect) ||
        !std::isfinite(projection.projectionM11) ||
        projection.verticalFieldOfViewDegrees <= 0.0F ||
        projection.verticalFieldOfViewDegrees >= 179.0F ||
        projection.aspect <= kMinimumPhysicalValue ||
        projection.projectionM11 <= kMinimumPhysicalValue) {
        return false;
    }

    PhysicalCameraIntrinsics derived{};
    derived.sensorHeightMillimeters = sourceSensorHeightMillimeters;
    derived.sensorWidthMillimeters =
        sourceSensorHeightMillimeters * projection.aspect;
    derived.focalLengthMillimeters =
        0.5F * sourceSensorHeightMillimeters * projection.projectionM11;
    if (!std::isfinite(derived.sensorWidthMillimeters) ||
        !std::isfinite(derived.focalLengthMillimeters) ||
        derived.sensorWidthMillimeters <= kMinimumPhysicalValue ||
        derived.focalLengthMillimeters <= kMinimumPhysicalValue) {
        return false;
    }

    result = derived;
    return true;
}

} // namespace gakumas::vr::camera
