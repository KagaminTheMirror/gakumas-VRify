#include "VrFreeCamera.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace gakumas::vr::camera {
namespace {

constexpr float kPi = 3.14159265358979323846F;

std::atomic<int> publishedMode{static_cast<int>(VrFreeCameraMode::Off)};
std::atomic<int> requestedMode{-1};
std::atomic<int> followBoneValue{10}; // HumanBodyBones.Head

[[nodiscard]] float WrapPi(float radians) noexcept {
    while (radians > kPi) {
        radians -= 2.0F * kPi;
    }
    while (radians < -kPi) {
        radians += 2.0F * kPi;
    }
    return radians;
}

// Normalized lerp: enough for frame-to-frame smoothing where the angular
// step per frame is small. Falls back to `to` when normalization fails.
[[nodiscard]] pose::Quaternion Nlerp(
    const pose::Quaternion& from,
    pose::Quaternion to,
    float alpha) noexcept {
    const float dot =
        from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
    if (dot < 0.0F) {
        to = {-to.x, -to.y, -to.z, -to.w};
    }
    const pose::Quaternion mixed{
        from.x + (to.x - from.x) * alpha,
        from.y + (to.y - from.y) * alpha,
        from.z + (to.z - from.z) * alpha,
        from.w + (to.w - from.w) * alpha,
    };
    pose::Quaternion normalized{};
    if (!pose::TryNormalize(mixed, normalized)) {
        return to;
    }
    return normalized;
}

[[nodiscard]] float ApplyDeadzone(float value, float deadzone) noexcept {
    if (!std::isfinite(value)) {
        return 0.0F;
    }
    const float magnitude = std::abs(value);
    if (magnitude <= deadzone) {
        return 0.0F;
    }
    const float scaled = (magnitude - deadzone) / (1.0F - deadzone);
    return std::copysign(std::min(scaled, 1.0F), value);
}

[[nodiscard]] pose::Vector3 Add(
    const pose::Vector3& left,
    const pose::Vector3& right) noexcept {
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

[[nodiscard]] pose::Vector3 Subtract(
    const pose::Vector3& left,
    const pose::Vector3& right) noexcept {
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

[[nodiscard]] pose::Vector3 Scale(const pose::Vector3& value, float scale) noexcept {
    return {value.x * scale, value.y * scale, value.z * scale};
}

[[nodiscard]] pose::Vector3 RotateAroundY(
    const pose::Vector3& value,
    float radians) noexcept {
    const float sine = std::sin(radians);
    const float cosine = std::cos(radians);
    // Unity is left-handed with +Y up: a positive yaw takes +Z toward +X.
    return {
        value.x * cosine + value.z * sine,
        value.y,
        value.z * cosine - value.x * sine,
    };
}

[[nodiscard]] float YawFromForward(
    const pose::Vector3& forward,
    float fallbackYawRadians) noexcept {
    const float horizontal = std::sqrt(forward.x * forward.x + forward.z * forward.z);
    if (!std::isfinite(horizontal) || horizontal < 1.0e-3F) {
        return fallbackYawRadians;
    }
    return std::atan2(forward.x, forward.z);
}

[[nodiscard]] float SmoothingAlpha(float dtSeconds, float tauSeconds) noexcept {
    if (dtSeconds <= 0.0F || tauSeconds <= 0.0F) {
        return 1.0F;
    }
    return 1.0F - std::exp(-dtSeconds / tauSeconds);
}

} // namespace

pose::Quaternion QuaternionFromYaw(float yawRadians) noexcept {
    const float half = yawRadians * 0.5F;
    return {0.0F, std::sin(half), 0.0F, std::cos(half)};
}

float YawFromOrientation(
    const pose::Quaternion& orientation,
    float fallbackYawRadians) noexcept {
    pose::Quaternion normalized{};
    if (!pose::TryNormalize(orientation, normalized)) {
        return fallbackYawRadians;
    }
    const pose::Vector3 forward = pose::Rotate(normalized, {0.0F, 0.0F, 1.0F});
    return YawFromForward(forward, fallbackYawRadians);
}

void PublishVrFreeCameraMode(VrFreeCameraMode mode) noexcept {
    publishedMode.store(static_cast<int>(mode), std::memory_order_release);
}

VrFreeCameraMode ReadVrFreeCameraMode() noexcept {
    return static_cast<VrFreeCameraMode>(
        publishedMode.load(std::memory_order_acquire));
}

bool IsVrFreeCameraLocomotionActive() noexcept {
    return ReadVrFreeCameraMode() != VrFreeCameraMode::Off;
}

bool IsVrFreeCameraBoneAnchorWanted() noexcept {
    const VrFreeCameraMode mode = ReadVrFreeCameraMode();
    return mode == VrFreeCameraMode::Follow ||
        mode == VrFreeCameraMode::FirstPerson;
}

bool IsVrFreeCameraFirstPerson() noexcept {
    return ReadVrFreeCameraMode() == VrFreeCameraMode::FirstPerson;
}

void RequestVrFreeCameraMode(VrFreeCameraMode mode) noexcept {
    requestedMode.store(static_cast<int>(mode), std::memory_order_release);
}

int ConsumeVrFreeCameraModeRequest() noexcept {
    return requestedMode.exchange(-1, std::memory_order_acq_rel);
}

void SetVrFreeCameraFollowBone(int humanBodyBoneValue) noexcept {
    followBoneValue.store(humanBodyBoneValue, std::memory_order_release);
}

int ReadVrFreeCameraFollowBone() noexcept {
    return followBoneValue.load(std::memory_order_acquire);
}

int CycleVrFreeCameraFollowBone(std::uint32_t presses) noexcept {
    int value = ReadVrFreeCameraFollowBone();
    int index = 0;
    for (int i = 0; i < kVrFreeCameraBoneOptionCount; ++i) {
        if (kVrFreeCameraBoneOptions[i].humanBodyBoneValue == value) {
            index = i;
            break;
        }
    }
    index = (index + static_cast<int>(presses % kVrFreeCameraBoneOptionCount)) %
        kVrFreeCameraBoneOptionCount;
    value = kVrFreeCameraBoneOptions[index].humanBodyBoneValue;
    SetVrFreeCameraFollowBone(value);
    return value;
}

const char* VrFreeCameraBoneKey(int humanBodyBoneValue) noexcept {
    for (const auto& option : kVrFreeCameraBoneOptions) {
        if (option.humanBodyBoneValue == humanBodyBoneValue) {
            return option.i18nKey;
        }
    }
    return kVrFreeCameraBoneOptions[0].i18nKey;
}

const char* VrFreeCameraModeName(VrFreeCameraMode mode) noexcept {
    switch (mode) {
    case VrFreeCameraMode::Off:
        return "OFF";
    case VrFreeCameraMode::Free:
        return "FREE";
    case VrFreeCameraMode::Follow:
        return "FOLLOW";
    case VrFreeCameraMode::FirstPerson:
        return "FIRST_PERSON";
    default:
        return "UNKNOWN";
    }
}

void VrFreeCameraRig::ForceOff() noexcept {
    mode_ = VrFreeCameraMode::Off;
    snapEngaged_ = false;
    lastPoseValid_ = false;
    anchorSmoothedValid_ = false;
    followSeeded_ = false;
    PublishVrFreeCameraMode(mode_);
}

void VrFreeCameraRig::EnterMode(
    VrFreeCameraMode next,
    const pose::Pose& gamePose) noexcept {
    switch (next) {
    case VrFreeCameraMode::Free:
        // Seed at the current game camera pose so the composed output does
        // not jump at the moment of activation.
        if (lastPoseValid_) {
            freePosition_ = lastPose_.position;
            freeYawRadians_ = YawFromOrientation(
                lastPose_.orientation, freeYawRadians_);
        } else {
            freePosition_ = gamePose.position;
            freeYawRadians_ = YawFromOrientation(
                gamePose.orientation, 0.0F);
        }
        break;
    case VrFreeCameraMode::Follow:
        // Real seeding happens on the first valid anchor so the transition
        // keeps the user where they currently stand.
        followSeeded_ = false;
        break;
    case VrFreeCameraMode::FirstPerson:
        firstPersonOffset_ = {0.0F, kFirstPersonOffsetDefaultY, 0.0F};
        firstPersonYawRadians_ = lastPoseValid_
            ? YawFromOrientation(lastPose_.orientation, followYawRadians_ + kPi)
            : followYawRadians_ + kPi;
        fpOrientationValid_ = false;
        break;
    case VrFreeCameraMode::Off:
    default:
        break;
    }
    mode_ = next;
    snapEngaged_ = false;
}

void VrFreeCameraRig::ApplyReset(const pose::Pose& gamePose) noexcept {
    switch (mode_) {
    case VrFreeCameraMode::Free:
        freePosition_ = gamePose.position;
        freeYawRadians_ = YawFromOrientation(gamePose.orientation, 0.0F);
        break;
    case VrFreeCameraMode::Follow:
        followDistance_ = kFollowDistanceDefault;
        followHeight_ = 0.0F;
        // Canonical front view: the offset points along the character's
        // forward vector, so the camera faces the character head-on.
        followYawRadians_ = YawFromForward(anchorForward_, followYawRadians_);
        followSeeded_ = true;
        break;
    case VrFreeCameraMode::FirstPerson:
        firstPersonOffset_ = {0.0F, kFirstPersonOffsetDefaultY, 0.0F};
        break;
    case VrFreeCameraMode::Off:
    default:
        break;
    }
}

void VrFreeCameraRig::UpdateAnchorSmoothing(
    const VrFreeCameraAnchor& anchor,
    float dtSeconds,
    float tauSeconds) noexcept {
    if (!anchor.valid || !pose::IsFinite(anchor.position)) {
        return;
    }
    if (!anchorSmoothedValid_) {
        anchorSmoothedPosition_ = anchor.position;
        anchorSmoothedValid_ = true;
    } else {
        const float alpha = SmoothingAlpha(dtSeconds, tauSeconds);
        anchorSmoothedPosition_ = Add(
            anchorSmoothedPosition_,
            Scale(Subtract(anchor.position, anchorSmoothedPosition_), alpha));
    }
    if (pose::IsFinite(anchor.forward)) {
        anchorForward_ = anchor.forward;
    }
}

bool VrFreeCameraRig::ConsumeSnapTurn(
    float rightStickX,
    float rightStickY,
    float& degrees) noexcept {
    const float magnitude = std::abs(rightStickX);
    if (snapEngaged_) {
        if (magnitude < kSnapReleaseThreshold) {
            snapEngaged_ = false;
        }
        return false;
    }
    if (magnitude >= kSnapEngageThreshold &&
        magnitude >= std::abs(rightStickY)) {
        snapEngaged_ = true;
        degrees = std::copysign(kSnapTurnDegrees, rightStickX);
        return true;
    }
    return false;
}

VrFreeCameraUpdateResult VrFreeCameraRig::Update(
    const VrFreeCameraCommands& commands,
    const VrFreeCameraAnchor& anchor,
    const pose::Pose& gamePose,
    bool headPoseValid,
    const pose::Pose& headPose) noexcept {
    VrFreeCameraUpdateResult result;

    const bool firstPersonEnabled =
        IsVrFirstPersonEnabled(commands.fpDirectionFollow);

    if (commands.hasModeRequest) {
        VrFreeCameraMode requested = commands.modeRequest;
        if (requested == VrFreeCameraMode::FirstPerson && !firstPersonEnabled) {
            requested = VrFreeCameraMode::Off;
        }
        if (requested != mode_) {
            EnterMode(requested, gamePose);
            result.modeChanged = true;
        }
    }

    if (mode_ == VrFreeCameraMode::FirstPerson && !firstPersonEnabled) {
        EnterMode(VrFreeCameraMode::Off, gamePose);
        result.modeChanged = true;
    }

    for (std::uint32_t press = 0; press < commands.modePresses; ++press) {
        EnterMode(NextVrFreeCameraMode(mode_, firstPersonEnabled), gamePose);
        result.modeChanged = true;
    }

    if (commands.resetPresses > 0 && mode_ != VrFreeCameraMode::Off) {
        ApplyReset(gamePose);
        result.resetApplied = true;
    }

    const float dt = std::clamp(commands.dtSeconds, 0.0F, 0.1F);
    const float leftX = ApplyDeadzone(commands.leftStickX, kStickDeadzone);
    const float leftY = ApplyDeadzone(commands.leftStickY, kStickDeadzone);
    // Vertical movement wants a mostly-vertical stick; a sideways snap flick
    // that drifts slightly off-axis must not also raise or lower the rig.
    const bool verticalDominant =
        std::abs(commands.rightStickY) >=
        std::abs(commands.rightStickX) * kVerticalDominanceRatio;
    const float rightY = verticalDominant
        ? ApplyDeadzone(commands.rightStickY, kStickDeadzone)
        : 0.0F;
    float snapDegrees = 0.0F;
    const bool snapTurned = ConsumeSnapTurn(
        commands.rightStickX, commands.rightStickY, snapDegrees);
    const float snapRadians = snapDegrees * kPi / 180.0F;

    switch (mode_) {
    case VrFreeCameraMode::Off:
        lastPoseValid_ = false;
        anchorSmoothedValid_ = false;
        break;

    case VrFreeCameraMode::Free: {
        if (snapTurned) {
            freeYawRadians_ += snapRadians;
            // Pivot around the user's head so the view spins in place. The
            // bridge composes head = rig + R(rigYaw)*delta, so rotating the
            // rig position around the head keeps the head position fixed.
            const pose::Vector3 pivot =
                headPoseValid ? headPose.position : freePosition_;
            freePosition_ = Add(
                pivot,
                RotateAroundY(Subtract(freePosition_, pivot), snapRadians));
        }
        if (leftX != 0.0F || leftY != 0.0F) {
            const pose::Quaternion basis = headPoseValid
                ? headPose.orientation
                : QuaternionFromYaw(freeYawRadians_);
            const float moveYaw = YawFromOrientation(basis, freeYawRadians_);
            const pose::Vector3 forward{
                std::sin(moveYaw), 0.0F, std::cos(moveYaw)};
            const pose::Vector3 right{
                std::cos(moveYaw), 0.0F, -std::sin(moveYaw)};
            const float moveSpeed = kFreeMoveSpeed *
                (commands.sprintHeld ? kFreeSprintMultiplier : 1.0F);
            freePosition_ = Add(
                freePosition_,
                Scale(
                    Add(Scale(forward, leftY), Scale(right, leftX)),
                    moveSpeed * dt));
        }
        const float verticalSpeed = kFreeVerticalSpeed *
            (commands.sprintHeld ? kFreeSprintMultiplier : 1.0F);
        freePosition_.y += rightY * verticalSpeed * dt;
        lastPose_ = {freePosition_, QuaternionFromYaw(freeYawRadians_)};
        lastPoseValid_ = true;
        result.poseValid = true;
        result.rigPose = lastPose_;
        break;
    }

    case VrFreeCameraMode::Follow: {
        UpdateAnchorSmoothing(anchor, dt, kFollowSmoothingTau);
        if (!followSeeded_ && anchorSmoothedValid_ && lastPoseValid_) {
            const pose::Vector3 delta =
                Subtract(lastPose_.position, anchorSmoothedPosition_);
            const float horizontal =
                std::sqrt(delta.x * delta.x + delta.z * delta.z);
            if (horizontal >= 1.0e-2F) {
                followYawRadians_ = std::atan2(delta.x, delta.z);
                followDistance_ = std::clamp(
                    horizontal, kFollowDistanceMinimum, kFollowDistanceMaximum);
            } else {
                followYawRadians_ =
                    YawFromForward(anchorForward_, followYawRadians_);
                followDistance_ = kFollowDistanceDefault;
            }
            followHeight_ = std::clamp(
                delta.y, kFollowHeightMinimum, kFollowHeightMaximum);
            followSeeded_ = true;
        } else if (!followSeeded_ && anchorSmoothedValid_) {
            // No previous rig pose (entered Follow as the first mode after a
            // rig reset): canonical front view.
            followYawRadians_ = YawFromForward(anchorForward_, 0.0F);
            followDistance_ = kFollowDistanceDefault;
            followHeight_ = 0.0F;
            followSeeded_ = true;
        }

        if (followSeeded_) {
            if (snapTurned) {
                followYawRadians_ += snapRadians;
            }
            // Stick right orbits the camera to the character's right side as
            // seen by the user (hardware feedback: += felt inverted).
            followYawRadians_ -=
                leftX * (kFollowOrbitDegreesPerSecond * kPi / 180.0F) * dt;
            const float distanceSpeed = kFollowDistanceSpeed *
                (commands.sprintHeld ? kFreeSprintMultiplier : 1.0F);
            followDistance_ = std::clamp(
                followDistance_ - leftY * distanceSpeed * dt,
                kFollowDistanceMinimum,
                kFollowDistanceMaximum);
            followHeight_ = std::clamp(
                followHeight_ + rightY * kFollowHeightSpeed * dt,
                kFollowHeightMinimum,
                kFollowHeightMaximum);
        }

        if (followSeeded_ && anchorSmoothedValid_) {
            const pose::Vector3 offset{
                std::sin(followYawRadians_) * followDistance_,
                followHeight_,
                std::cos(followYawRadians_) * followDistance_,
            };
            lastPose_ = {
                Add(anchorSmoothedPosition_, offset),
                QuaternionFromYaw(followYawRadians_ + kPi),
            };
            lastPoseValid_ = true;
        }
        // Anchor missing (scene change / bad chara index): hold the last
        // pose so the view freezes instead of snapping to the origin.
        result.poseValid = lastPoseValid_;
        result.rigPose = lastPose_;
        break;
    }

    case VrFreeCameraMode::FirstPerson: {
        UpdateAnchorSmoothing(anchor, dt, kFirstPersonSmoothingTau);
        const auto directionFollow =
            static_cast<VrFpDirectionFollow>(commands.fpDirectionFollow);
        // Manual snap turns only when the bone does not own the yaw.
        if (snapTurned && directionFollow == VrFpDirectionFollow::None) {
            firstPersonYawRadians_ += snapRadians;
        }
        firstPersonOffset_.x = std::clamp(
            firstPersonOffset_.x + leftX * kFirstPersonOffsetSpeed * dt,
            -kFirstPersonOffsetLimit,
            kFirstPersonOffsetLimit);
        firstPersonOffset_.z = std::clamp(
            firstPersonOffset_.z + leftY * kFirstPersonOffsetSpeed * dt,
            -kFirstPersonOffsetLimit,
            kFirstPersonOffsetLimit);
        firstPersonOffset_.y = std::clamp(
            firstPersonOffset_.y + rightY * kFirstPersonOffsetSpeed * dt,
            -kFirstPersonOffsetLimit,
            kFirstPersonOffsetLimit);

        if (anchorSmoothedValid_) {
            // Optional direction follow (menu setting, defaults to none):
            // "turn" tracks the bone's facing yaw, "turn + tilt" tracks the
            // full bone rotation. Both are smoothed toward the target so a
            // dancing head never hard-locks the view.
            const float followAlpha = SmoothingAlpha(dt, kFpDirectionFollowTau);
            if (directionFollow == VrFpDirectionFollow::Turn) {
                const float targetYaw =
                    YawFromForward(anchorForward_, firstPersonYawRadians_);
                firstPersonYawRadians_ += WrapPi(
                    targetYaw - firstPersonYawRadians_) * followAlpha;
                fpOrientationValid_ = false;
            } else if (directionFollow == VrFpDirectionFollow::TurnAndTilt &&
                       anchor.rotationValid &&
                       pose::IsFinite(anchor.rotation)) {
                if (!fpOrientationValid_) {
                    fpOrientation_ = anchor.rotation;
                    fpOrientationValid_ = true;
                } else {
                    fpOrientation_ =
                        Nlerp(fpOrientation_, anchor.rotation, followAlpha);
                }
                // Keep the yaw state coherent for the local offset basis and
                // for a clean handoff when the setting changes back.
                firstPersonYawRadians_ = YawFromOrientation(
                    fpOrientation_, firstPersonYawRadians_);
            } else {
                fpOrientationValid_ = false;
            }

            const pose::Vector3 localOffset = RotateAroundY(
                {firstPersonOffset_.x, 0.0F, firstPersonOffset_.z},
                firstPersonYawRadians_);
            pose::Vector3 position = Add(anchorSmoothedPosition_, localOffset);
            position.y += firstPersonOffset_.y;
            // Default rotation is rig yaw + HMD only; bone tilt is applied
            // solely through the opt-in "turn + tilt" setting above.
            const pose::Quaternion orientation = fpOrientationValid_
                ? fpOrientation_
                : QuaternionFromYaw(firstPersonYawRadians_);
            lastPose_ = {position, orientation};
            lastPoseValid_ = true;
        }
        result.poseValid = lastPoseValid_;
        result.rigPose = lastPose_;
        break;
    }

    default:
        break;
    }

    result.mode = mode_;
    PublishVrFreeCameraMode(mode_);
    return result;
}

} // namespace gakumas::vr::camera
