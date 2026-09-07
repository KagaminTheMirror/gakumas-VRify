#pragma once

#include <array>

namespace gakumas::vr::pose {

struct Vector3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct Quaternion {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;
};

struct Pose {
    Vector3 position{};
    Quaternion orientation{};
};

[[nodiscard]] bool IsFinite(const Vector3& value) noexcept;
[[nodiscard]] bool IsFinite(const Quaternion& value) noexcept;
[[nodiscard]] bool TryNormalize(Quaternion value, Quaternion& normalized) noexcept;
[[nodiscard]] Quaternion Conjugate(const Quaternion& value) noexcept;
[[nodiscard]] Quaternion Multiply(
    const Quaternion& left,
    const Quaternion& right) noexcept;
[[nodiscard]] Vector3 Rotate(
    const Quaternion& rotation,
    const Vector3& value) noexcept;

// OpenXR is right-handed with forward along -Z. Unity world space is
// left-handed with forward along +Z. This reflection preserves X/Y and flips Z.
[[nodiscard]] Pose OpenXrToUnity(const Pose& openXrPose) noexcept;

[[nodiscard]] bool TryCenterStereoPose(
    const std::array<Pose, 2>& eyes,
    Pose& center) noexcept;

// Unity LookRotation: +Z forward, Y-up. Fails when `forward` is ~0.
[[nodiscard]] bool TryLookRotation(
    const Vector3& forward,
    Quaternion& out) noexcept;

// Yaw around +Y, then pitch around local +X, same as authoredVector
// (sin(yaw)·cos(pitch), sin(pitch), cos(yaw)·cos(pitch)).
[[nodiscard]] bool TryYawPitchOrientation(
    float yawRadians,
    float pitchRadians,
    Quaternion& out) noexcept;

// Unity camera local +X is right. out = center + R·(localX, 0, 0),
// same orientation. Used to place a lighting eye on the shot's local X.
[[nodiscard]] bool TryOffsetPoseOnLocalX(
    const Pose& center,
    float localX,
    Pose& out) noexcept;

// Compose the current OpenXR head delta in the local basis of the game camera.
// The gameRequested pose remains the anchor, so Cinemachine cuts and motion are
// preserved instead of being overwritten by an absolute tracking-space pose.
[[nodiscard]] bool TryComposeGamePose(
    const Pose& gameRequested,
    const Pose& openXrBaseline,
    const Pose& openXrCurrent,
    float worldScale,
    Pose& composed) noexcept;

} // namespace gakumas::vr::pose
