#include "PoseMath.hpp"

#include <cmath>

namespace gakumas::vr::pose {
namespace {

Vector3 Add(const Vector3& left, const Vector3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vector3 Subtract(const Vector3& left, const Vector3& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vector3 Scale(const Vector3& value, float scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}

float Dot(const Quaternion& left, const Quaternion& right) noexcept {
    return left.x * right.x + left.y * right.y +
        left.z * right.z + left.w * right.w;
}

} // namespace

bool IsFinite(const Vector3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z);
}

bool IsFinite(const Quaternion& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
        std::isfinite(value.z) && std::isfinite(value.w);
}

bool TryNormalize(Quaternion value, Quaternion& normalized) noexcept {
    if (!IsFinite(value)) {
        return false;
    }
    const float normSquared = Dot(value, value);
    if (!std::isfinite(normSquared) || normSquared < 1.0e-8F) {
        return false;
    }
    const float inverseNorm = 1.0F / std::sqrt(normSquared);
    normalized = {
        value.x * inverseNorm,
        value.y * inverseNorm,
        value.z * inverseNorm,
        value.w * inverseNorm,
    };
    return IsFinite(normalized);
}

Quaternion Conjugate(const Quaternion& value) noexcept {
    return {-value.x, -value.y, -value.z, value.w};
}

Quaternion Multiply(const Quaternion& left, const Quaternion& right) noexcept {
    return {
        left.w * right.x + left.x * right.w +
            left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z +
            left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y -
            left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x -
            left.y * right.y - left.z * right.z,
    };
}

Vector3 Rotate(const Quaternion& rotation, const Vector3& value) noexcept {
    Quaternion normalized{};
    if (!TryNormalize(rotation, normalized) || !IsFinite(value)) {
        return {};
    }
    const Quaternion vector{value.x, value.y, value.z, 0.0F};
    const Quaternion rotated = Multiply(
        Multiply(normalized, vector),
        Conjugate(normalized));
    return {rotated.x, rotated.y, rotated.z};
}

Pose OpenXrToUnity(const Pose& openXrPose) noexcept {
    return {
        {
            openXrPose.position.x,
            openXrPose.position.y,
            -openXrPose.position.z,
        },
        {
            -openXrPose.orientation.x,
            -openXrPose.orientation.y,
            openXrPose.orientation.z,
            openXrPose.orientation.w,
        },
    };
}

bool TryCenterStereoPose(
    const std::array<Pose, 2>& eyes,
    Pose& center) noexcept {
    if (!IsFinite(eyes[0].position) || !IsFinite(eyes[1].position)) {
        return false;
    }
    Quaternion left{};
    Quaternion right{};
    if (!TryNormalize(eyes[0].orientation, left) ||
        !TryNormalize(eyes[1].orientation, right)) {
        return false;
    }
    if (Dot(left, right) < 0.0F) {
        right = {-right.x, -right.y, -right.z, -right.w};
    }
    Quaternion averaged{
        left.x + right.x,
        left.y + right.y,
        left.z + right.z,
        left.w + right.w,
    };
    if (!TryNormalize(averaged, center.orientation)) {
        return false;
    }
    center.position = Scale(Add(eyes[0].position, eyes[1].position), 0.5F);
    return true;
}

bool TryLookRotation(
    const Vector3& forward,
    Quaternion& out) noexcept {
    if (!IsFinite(forward)) {
        return false;
    }
    const float mag = std::sqrt(
        forward.x * forward.x + forward.y * forward.y +
        forward.z * forward.z);
    if (!std::isfinite(mag) || mag < 1.0e-6F) {
        return false;
    }
    const Vector3 f{
        forward.x / mag,
        forward.y / mag,
        forward.z / mag,
    };
    Vector3 worldUp{0.0F, 1.0F, 0.0F};
    Vector3 right{
        worldUp.y * f.z - worldUp.z * f.y,
        worldUp.z * f.x - worldUp.x * f.z,
        worldUp.x * f.y - worldUp.y * f.x,
    };
    float rightMag = std::sqrt(
        right.x * right.x + right.y * right.y + right.z * right.z);
    if (!std::isfinite(rightMag) || rightMag < 1.0e-5F) {
        worldUp = {0.0F, 0.0F, f.y >= 0.0F ? -1.0F : 1.0F};
        right = {
            worldUp.y * f.z - worldUp.z * f.y,
            worldUp.z * f.x - worldUp.x * f.z,
            worldUp.x * f.y - worldUp.y * f.x,
        };
        rightMag = std::sqrt(
            right.x * right.x + right.y * right.y + right.z * right.z);
        if (!std::isfinite(rightMag) || rightMag < 1.0e-5F) {
            return false;
        }
    }
    right = {
        right.x / rightMag,
        right.y / rightMag,
        right.z / rightMag,
    };
    const Vector3 up{
        f.y * right.z - f.z * right.y,
        f.z * right.x - f.x * right.z,
        f.x * right.y - f.y * right.x,
    };
    const float m00 = right.x;
    const float m01 = up.x;
    const float m02 = f.x;
    const float m10 = right.y;
    const float m11 = up.y;
    const float m12 = f.y;
    const float m20 = right.z;
    const float m21 = up.z;
    const float m22 = f.z;
    const float trace = m00 + m11 + m22;
    Quaternion raw{};
    if (trace > 0.0F) {
        const float s = 0.5F / std::sqrt(trace + 1.0F);
        raw.w = 0.25F / s;
        raw.x = (m21 - m12) * s;
        raw.y = (m02 - m20) * s;
        raw.z = (m10 - m01) * s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = 2.0F * std::sqrt(1.0F + m00 - m11 - m22);
        raw.w = (m21 - m12) / s;
        raw.x = 0.25F * s;
        raw.y = (m01 + m10) / s;
        raw.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = 2.0F * std::sqrt(1.0F + m11 - m00 - m22);
        raw.w = (m02 - m20) / s;
        raw.x = (m01 + m10) / s;
        raw.y = 0.25F * s;
        raw.z = (m12 + m21) / s;
    } else {
        const float s = 2.0F * std::sqrt(1.0F + m22 - m00 - m11);
        raw.w = (m10 - m01) / s;
        raw.x = (m02 + m20) / s;
        raw.y = (m12 + m21) / s;
        raw.z = 0.25F * s;
    }
    return TryNormalize(raw, out);
}

bool TryYawPitchOrientation(
    float yawRadians,
    float pitchRadians,
    Quaternion& out) noexcept {
    if (!std::isfinite(yawRadians) || !std::isfinite(pitchRadians)) {
        return false;
    }
    const float cp = std::cos(pitchRadians);
    const float sp = std::sin(pitchRadians);
    return TryLookRotation(
        {std::sin(yawRadians) * cp, sp, std::cos(yawRadians) * cp},
        out);
}

bool TryOffsetPoseOnLocalX(
    const Pose& center,
    float localX,
    Pose& out) noexcept {
    if (!IsFinite(center.position) || !std::isfinite(localX)) {
        return false;
    }
    Quaternion orientation{};
    if (!TryNormalize(center.orientation, orientation)) {
        return false;
    }
    const Vector3 offset = Rotate(orientation, {localX, 0.0F, 0.0F});
    if (!IsFinite(offset)) {
        return false;
    }
    out.orientation = orientation;
    out.position = Add(center.position, offset);
    return IsFinite(out.position);
}

bool TryComposeGamePose(
    const Pose& gameRequested,
    const Pose& openXrBaseline,
    const Pose& openXrCurrent,
    float worldScale,
    Pose& composed) noexcept {
    if (!IsFinite(gameRequested.position) || !IsFinite(openXrBaseline.position) ||
        !IsFinite(openXrCurrent.position) || !std::isfinite(worldScale) ||
        worldScale < 0.0F) {
        return false;
    }

    Quaternion gameRotation{};
    Quaternion baselineRotation{};
    Quaternion currentRotation{};
    if (!TryNormalize(gameRequested.orientation, gameRotation) ||
        !TryNormalize(openXrBaseline.orientation, baselineRotation) ||
        !TryNormalize(openXrCurrent.orientation, currentRotation)) {
        return false;
    }

    const Quaternion inverseBaseline = Conjugate(baselineRotation);
    const Quaternion relativeOpenXrRotation = Multiply(
        inverseBaseline,
        currentRotation);
    const Vector3 relativeOpenXrPosition = Rotate(
        inverseBaseline,
        Subtract(openXrCurrent.position, openXrBaseline.position));
    const Pose relativeUnity = OpenXrToUnity(
        {relativeOpenXrPosition, relativeOpenXrRotation});

    Quaternion composedRotation{};
    if (!TryNormalize(
            Multiply(gameRotation, relativeUnity.orientation),
            composedRotation)) {
        return false;
    }
    const Vector3 cameraLocalOffset = Scale(relativeUnity.position, worldScale);
    const Vector3 worldOffset = Rotate(gameRotation, cameraLocalOffset);
    composed = {
        Add(gameRequested.position, worldOffset),
        composedRotation,
    };
    return IsFinite(composed.position) && IsFinite(composed.orientation);
}

} // namespace gakumas::vr::pose
