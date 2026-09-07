#pragma once

#include "pose/PoseMath.hpp"

#include <algorithm>
#include <cmath>

// Pose math for the adjustable desktop-mirror panel (A-button adjust mode).
// Everything is OpenXR-handed (right-handed, view forward -Z) and expressed
// with the pose:: value types so the offline test harness can compile this
// header together with PoseMath.cpp and no OpenXR/D3D dependency.
//
// The placement's source of truth is `offset`: the vector from the head (VIEW
// origin) to the panel centre. Unpinned it lives in VIEW space (panel turns
// with the head); pinned it is re-expressed once in the projection base space
// (STAGE/LOCAL) so the direction freezes while the position keeps following
// the head. The quad orientation is always derived from the offset so the
// panel faces the viewer roll-free.
namespace gakumas::vr::panel {

// stereo.221 hardware: 1.5 m dead ahead reads as a slight upward glance and
// tires the eyes; a bit more distance drops the angular height. This pair
// stays the photo-scene (head-locked centred) panel.
constexpr float kDefaultPanelDistanceMetres = 1.75F;
constexpr float kDefaultPanelWidthMetres = 1.2F;
// Default adjustable placement for the other (portrait / 2D) scenes: fresh
// config and the left-stick reset land here. Rounded from the user's tuned
// pose on stereo.222 hardware (0, -0.18, -1.96, w 1.17).
constexpr float kDefaultAdjustOffsetYMetres = -0.2F;
constexpr float kDefaultAdjustOffsetZMetres = -2.0F;
constexpr float kDefaultAdjustWidthMetres = 1.2F;
constexpr float kMinPanelDistanceMetres = 0.4F;
constexpr float kMaxPanelDistanceMetres = 8.0F;
constexpr float kMinPanelWidthMetres = 0.3F;
constexpr float kMaxPanelWidthMetres = 4.0F;

struct Placement {
    bool customized = false;
    bool pinned = false;
    // Head -> panel centre, VIEW space (persistence form).
    pose::Vector3 offset{
        0.0F, kDefaultAdjustOffsetYMetres, kDefaultAdjustOffsetZMetres};
    float width = kDefaultAdjustWidthMetres;
};

// A on the right controller is one action with two scene-dependent meanings.
enum class AButtonRoute {
    PhotoShutter,
    PanelAdjust,
};

[[nodiscard]] constexpr AButtonRoute RouteAButton(bool photoSceneActive) noexcept {
    return photoSceneActive ? AButtonRoute::PhotoShutter
                            : AButtonRoute::PanelAdjust;
}

[[nodiscard]] inline float VectorLength(const pose::Vector3& value) noexcept {
    return std::sqrt(
        value.x * value.x + value.y * value.y + value.z * value.z);
}

[[nodiscard]] inline pose::Vector3 VectorAdd(
    const pose::Vector3& left, const pose::Vector3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] inline pose::Vector3 VectorSubtract(
    const pose::Vector3& left, const pose::Vector3& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] inline pose::Vector3 VectorScale(
    const pose::Vector3& value, float scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}

[[nodiscard]] inline float VectorDot(
    const pose::Vector3& left, const pose::Vector3& right) noexcept {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

[[nodiscard]] inline float ClampPanelWidth(float width) noexcept {
    if (!std::isfinite(width)) {
        return kDefaultPanelWidthMetres;
    }
    return std::clamp(width, kMinPanelWidthMetres, kMaxPanelWidthMetres);
}

// Keeps the panel centre inside a usable radial shell around the head. A
// degenerate or non-finite offset falls back to the default front placement.
[[nodiscard]] inline pose::Vector3 ClampPanelOffset(
    const pose::Vector3& offset) noexcept {
    if (!pose::IsFinite(offset)) {
        return {0.0F, 0.0F, -kDefaultPanelDistanceMetres};
    }
    const float length = VectorLength(offset);
    if (length < 0.01F) {
        return {0.0F, 0.0F, -kDefaultPanelDistanceMetres};
    }
    const float clamped =
        std::clamp(length, kMinPanelDistanceMetres, kMaxPanelDistanceMetres);
    return VectorScale(offset, clamped / length);
}

// Roll-free orientation whose +Z (the quad's visible-face normal) points from
// the panel centre back to the viewer.
[[nodiscard]] inline pose::Quaternion PanelFacingOrientation(
    const pose::Vector3& offset) noexcept {
    const float length = VectorLength(offset);
    if (length < 0.0001F) {
        return {};
    }
    // d = unit vector panel -> viewer.
    const pose::Vector3 d = VectorScale(offset, -1.0F / length);
    const float yaw = std::atan2(d.x, d.z);
    const float pitch = std::asin(std::clamp(d.y, -1.0F, 1.0F));
    const pose::Quaternion yawRotation{
        0.0F, std::sin(yaw * 0.5F), 0.0F, std::cos(yaw * 0.5F)};
    const pose::Quaternion pitchRotation{
        std::sin(-pitch * 0.5F), 0.0F, 0.0F, std::cos(-pitch * 0.5F)};
    pose::Quaternion combined =
        pose::Multiply(yawRotation, pitchRotation);
    pose::Quaternion normalized{};
    if (!pose::TryNormalize(combined, normalized)) {
        return {};
    }
    return normalized;
}

[[nodiscard]] inline pose::Pose PanelPoseFromOffset(
    const pose::Vector3& viewerPosition, const pose::Vector3& offset) noexcept {
    pose::Pose result;
    result.position = VectorAdd(viewerPosition, offset);
    result.orientation = PanelFacingOrientation(offset);
    return result;
}

// Ray/quad hit with UV output. Front face only: the viewer must be on the
// quad's +Z side and the ray must approach it. Returns false outside the
// rectangle, matching the legacy fixed-plane behaviour.
[[nodiscard]] inline bool RayQuadUv(
    const pose::Pose& quadPose,
    float widthMetres,
    float heightMetres,
    const pose::Vector3& rayOrigin,
    const pose::Vector3& rayDirection,
    float& u,
    float& v) noexcept {
    if (widthMetres <= 0.0F || heightMetres <= 0.0F) {
        return false;
    }
    const pose::Quaternion inverse = pose::Conjugate(quadPose.orientation);
    const pose::Vector3 localOrigin =
        pose::Rotate(inverse, VectorSubtract(rayOrigin, quadPose.position));
    const pose::Vector3 localDirection = pose::Rotate(inverse, rayDirection);
    if (localOrigin.z <= 0.0F || localDirection.z >= -0.00001F) {
        return false;
    }
    const float distance = -localOrigin.z / localDirection.z;
    if (distance <= 0.0F) {
        return false;
    }
    const float hitX = localOrigin.x + localDirection.x * distance;
    const float hitY = localOrigin.y + localDirection.y * distance;
    if (std::abs(hitX) > widthMetres * 0.5F ||
        std::abs(hitY) > heightMetres * 0.5F) {
        return false;
    }
    u = std::clamp(hitX / widthMetres + 0.5F, 0.0F, 1.0F);
    v = std::clamp(0.5F - hitY / heightMetres, 0.0F, 1.0F);
    return true;
}

// VD-style "Height": pure vertical move of the panel centre.
[[nodiscard]] inline pose::Vector3 ApplyHeightDelta(
    const pose::Vector3& offset, float deltaMetres) noexcept {
    return ClampPanelOffset({offset.x, offset.y + deltaMetres, offset.z});
}

// VD-style "Distance": radial move along the current direction.
[[nodiscard]] inline pose::Vector3 ApplyDistanceDelta(
    const pose::Vector3& offset, float deltaMetres) noexcept {
    const float length = VectorLength(offset);
    if (length < 0.0001F) {
        return ClampPanelOffset(offset);
    }
    const float next = std::clamp(
        length + deltaMetres,
        kMinPanelDistanceMetres,
        kMaxPanelDistanceMetres);
    return VectorScale(offset, next / length);
}

[[nodiscard]] inline float ApplySizeDelta(
    float width, float deltaMetres) noexcept {
    return ClampPanelWidth(width + deltaMetres);
}

// Two-hand grip scale: width follows the ratio of the current hand
// separation to the separation captured when the second grip closed.
[[nodiscard]] inline float ScaledPanelWidth(
    float baselineWidth,
    float baselineSeparation,
    float separation) noexcept {
    if (baselineSeparation < 0.01F || separation < 0.001F) {
        return ClampPanelWidth(baselineWidth);
    }
    return ClampPanelWidth(baselineWidth * separation / baselineSeparation);
}

// Shortest-arc rotation taking `from` onto `to`. Degenerate or antiparallel
// inputs return identity so a grab never snaps the panel.
[[nodiscard]] inline pose::Quaternion RotationBetween(
    const pose::Vector3& from, const pose::Vector3& to) noexcept {
    const float fromLength = VectorLength(from);
    const float toLength = VectorLength(to);
    if (fromLength < 0.0001F || toLength < 0.0001F) {
        return {};
    }
    const pose::Vector3 a = VectorScale(from, 1.0F / fromLength);
    const pose::Vector3 b = VectorScale(to, 1.0F / toLength);
    const float dot = VectorDot(a, b);
    if (dot < -0.9999F) {
        return {};
    }
    pose::Quaternion result{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
        1.0F + dot,
    };
    pose::Quaternion normalized{};
    if (!pose::TryNormalize(result, normalized)) {
        return {};
    }
    return normalized;
}

// Pin conversions. headPoseInBase is the VIEW pose located in the projection
// base space (STAGE, or LOCAL as fallback).
[[nodiscard]] inline pose::Vector3 ViewOffsetToBase(
    const pose::Pose& headPoseInBase, const pose::Vector3& offsetView) noexcept {
    return pose::Rotate(headPoseInBase.orientation, offsetView);
}

[[nodiscard]] inline pose::Vector3 BaseOffsetToView(
    const pose::Pose& headPoseInBase, const pose::Vector3& offsetBase) noexcept {
    return pose::Rotate(
        pose::Conjugate(headPoseInBase.orientation), offsetBase);
}

// The exact world orientation of the panel as currently displayed in VIEW
// space. Pinning captures this instead of recomputing a roll-free facing so
// the panel freezes visually in place (no tilt jump on toggle).
[[nodiscard]] inline pose::Quaternion CapturedPanelOrientation(
    const pose::Pose& headPoseInBase, const pose::Vector3& offsetView) noexcept {
    return pose::Multiply(
        headPoseInBase.orientation, PanelFacingOrientation(offsetView));
}

// Pinned panel: direction and orientation frozen in base space, position
// anchored to the current head position so the panel walks with the user.
[[nodiscard]] inline pose::Pose PinnedPanelPose(
    const pose::Pose& headPoseInBase,
    const pose::Vector3& offsetBase,
    const pose::Quaternion& orientationBase) noexcept {
    pose::Pose result;
    result.position = VectorAdd(headPoseInBase.position, offsetBase);
    result.orientation = orientationBase;
    return result;
}

// Re-express a base-space pose in VIEW space (for the shared hit test).
[[nodiscard]] inline pose::Pose PoseInViewSpace(
    const pose::Pose& headPoseInBase, const pose::Pose& poseInBase) noexcept {
    const pose::Quaternion inverse = pose::Conjugate(headPoseInBase.orientation);
    pose::Pose result;
    result.position = pose::Rotate(
        inverse, VectorSubtract(poseInBase.position, headPoseInBase.position));
    result.orientation = pose::Multiply(inverse, poseInBase.orientation);
    return result;
}

// The adjust bar floats just above the panel's top edge, in the panel plane,
// nudged slightly toward the viewer so the layers never z-fight.
[[nodiscard]] inline pose::Pose AttachedBarPose(
    const pose::Pose& panelPose,
    float panelHeightMetres,
    float barHeightMetres) noexcept {
    const pose::Vector3 local{
        0.0F,
        panelHeightMetres * 0.5F + barHeightMetres * 0.5F + 0.035F,
        0.02F};
    pose::Pose result;
    result.position = VectorAdd(
        panelPose.position, pose::Rotate(panelPose.orientation, local));
    result.orientation = panelPose.orientation;
    return result;
}

} // namespace gakumas::vr::panel
