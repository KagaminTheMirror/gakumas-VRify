#pragma once

#include "pose/PoseMath.hpp"

#include <cstddef>
#include <cstdint>

namespace gakumas::vr::camera {

// VR migration of the upstream free-camera modes. The rig produces the pose
// that replaces Cinemachine's requested pose in ApplyVrHeadPose; the headset
// 6DoF delta is composed on top by RelativePoseBridge exactly as in Off mode.
enum class VrFreeCameraMode : int {
    Off = 0,
    Free = 1,
    Follow = 2,
    FirstPerson = 3,
};

// First-person availability + direction follow (one persisted setting).
// Stored values keep their .216 meanings so existing configs stay valid:
//   0 = None (first person on, yaw manual)
//   1 = Turn (yaw follows the bone)
//   2 = Turn + tilt (full bone rotation)
//   3 = Disabled (first person hidden from the X cycle and the mode combo)
// Display order in the menu is Disabled / None / Turn / TurnAndTilt.
// 1 and 2 are motion-sickness hazards; the menu still confirms them.
enum class VrFpDirectionFollow : int {
    None = 0,
    Turn = 1,
    TurnAndTilt = 2,
    Disabled = 3,
};

inline constexpr int kVrFpFollowDisplayCount = 4;

[[nodiscard]] inline bool IsVrFirstPersonEnabled(int fpDirectionFollow) noexcept {
    return fpDirectionFollow != static_cast<int>(VrFpDirectionFollow::Disabled);
}

// Menu combo index (Disabled, None, Turn, TurnAndTilt) <-> stored value.
[[nodiscard]] inline int VrFpFollowToDisplay(int stored) noexcept {
    switch (static_cast<VrFpDirectionFollow>(stored)) {
    case VrFpDirectionFollow::None:
        return 1;
    case VrFpDirectionFollow::Turn:
        return 2;
    case VrFpDirectionFollow::TurnAndTilt:
        return 3;
    case VrFpDirectionFollow::Disabled:
    default:
        return 0;
    }
}

[[nodiscard]] inline int VrFpFollowFromDisplay(int display) noexcept {
    switch (display) {
    case 1:
        return static_cast<int>(VrFpDirectionFollow::None);
    case 2:
        return static_cast<int>(VrFpDirectionFollow::Turn);
    case 3:
        return static_cast<int>(VrFpDirectionFollow::TurnAndTilt);
    default:
        return static_cast<int>(VrFpDirectionFollow::Disabled);
    }
}

[[nodiscard]] inline bool IsVrFpAutoFollow(int fpDirectionFollow) noexcept {
    return fpDirectionFollow == static_cast<int>(VrFpDirectionFollow::Turn) ||
        fpDirectionFollow == static_cast<int>(VrFpDirectionFollow::TurnAndTilt);
}

// Left-trigger sprint: the trigger is also the UI click, so the worker
// must drop the hold while the left pointer is on a *visible* quad.
// Geometric hits on a hidden menu / panel must not starve sprint: the
// settings menu is head-locked in front of the user.
[[nodiscard]] inline bool FreeMoveSprintFromLeftTrigger(
    bool leftTriggerHeld,
    bool leftOnVisibleGamePanel,
    bool leftOnVisibleMenu,
    bool leftOnAdjustBar) noexcept {
    return leftTriggerHeld && !leftOnVisibleGamePanel &&
        !leftOnVisibleMenu && !leftOnAdjustBar;
}

[[nodiscard]] inline VrFreeCameraMode NextVrFreeCameraMode(
    VrFreeCameraMode current,
    bool firstPersonEnabled) noexcept {
    switch (current) {
    case VrFreeCameraMode::Off:
        return VrFreeCameraMode::Free;
    case VrFreeCameraMode::Free:
        return VrFreeCameraMode::Follow;
    case VrFreeCameraMode::Follow:
        return firstPersonEnabled
            ? VrFreeCameraMode::FirstPerson
            : VrFreeCameraMode::Off;
    case VrFreeCameraMode::FirstPerson:
    default:
        return VrFreeCameraMode::Off;
    }
}

struct VrFreeCameraCommands {
    float dtSeconds = 0.0F;
    // Sticks are pre-gated by the caller: zero while the VR menu is open or
    // the input sample is stale.
    float leftStickX = 0.0F;
    float leftStickY = 0.0F;
    float rightStickX = 0.0F;
    float rightStickY = 0.0F;
    // Hold-to-sprint. FREE: planar + climb. FOLLOW: distance only (orbit
    // and height stay at walk rate). The OpenXR worker already gates this
    // on left trigger held and the left pointer not sitting on a visible
    // UI quad (game panel / settings menu / adjust bar).
    bool sprintHeld = false;
    std::uint32_t modePresses = 0;
    std::uint32_t resetPresses = 0;
    // Direct mode request (menu). Applied before modePresses cycling.
    bool hasModeRequest = false;
    VrFreeCameraMode modeRequest = VrFreeCameraMode::Off;
    int fpDirectionFollow = 0;
};

// Character bone sample published by CampusActorController.LateUpdate on the
// Unity main thread. `valid` is false when the sample is stale (scene change,
// missing actor index) - the rig then freezes at its last pose instead of
// snapping to the origin.
struct VrFreeCameraAnchor {
    bool valid = false;
    pose::Vector3 position{};
    pose::Vector3 forward{};
    // Full bone rotation, sampled together with position/forward. Only used
    // by the first-person "turn + tilt" direction follow.
    bool rotationValid = false;
    pose::Quaternion rotation{};
};

// FOLLOW-mode anchor bones offered by the menu and cycled by the Y button
// when the Y-button setting is "switch bone". Values are Unity
// HumanBodyBones constants verified against dump.cs.
struct VrFreeCameraBoneOption {
    const char* i18nKey;
    int humanBodyBoneValue;
};
inline constexpr VrFreeCameraBoneOption kVrFreeCameraBoneOptions[] = {
    {"vr_bone_head", 10},
    {"vr_bone_neck", 9},
    {"vr_bone_chest", 8},
    {"vr_bone_spine", 7},
    {"vr_bone_hips", 0},
    {"vr_bone_left_hand", 17},
    {"vr_bone_right_hand", 18},
    {"vr_bone_left_foot", 5},
    {"vr_bone_right_foot", 6},
};
inline constexpr int kVrFreeCameraBoneOptionCount =
    static_cast<int>(
        sizeof(kVrFreeCameraBoneOptions) / sizeof(kVrFreeCameraBoneOptions[0]));

struct VrFreeCameraUpdateResult {
    bool poseValid = false;
    bool modeChanged = false;
    bool resetApplied = false;
    VrFreeCameraMode mode = VrFreeCameraMode::Off;
    pose::Pose rigPose{};
};

class VrFreeCameraRig final {
public:
    // Tuning constants (public so offline tests can assert against them).
    static constexpr float kSnapTurnDegrees = 30.0F;
    static constexpr float kSnapEngageThreshold = 0.60F;
    static constexpr float kSnapReleaseThreshold = 0.40F;
    static constexpr float kStickDeadzone = 0.15F;
    // Right-stick axis separation: vertical movement engages only while the
    // stick points mostly up/down (|y| >= ratio*|x|), and a snap turn only
    // while it points mostly sideways (|x| >= |y|). A diagonal snap flick
    // must not also drift the rig vertically.
    static constexpr float kVerticalDominanceRatio = 1.5F;
    static constexpr float kFreeMoveSpeed = 1.6F;        // m/s horizontal
    static constexpr float kFreeVerticalSpeed = 1.2F;    // m/s
    static constexpr float kFreeSprintMultiplier = 3.0F;
    static constexpr float kFollowOrbitDegreesPerSecond = 90.0F;
    static constexpr float kFollowDistanceSpeed = 1.2F;  // m/s
    static constexpr float kFollowHeightSpeed = 1.0F;    // m/s
    static constexpr float kFollowDistanceMinimum = 0.3F;
    static constexpr float kFollowDistanceMaximum = 8.0F;
    static constexpr float kFollowHeightMinimum = -2.0F;
    static constexpr float kFollowHeightMaximum = 3.0F;
    static constexpr float kFollowDistanceDefault = 1.5F;
    static constexpr float kFirstPersonOffsetSpeed = 0.25F;  // m/s
    static constexpr float kFirstPersonOffsetLimit = 0.5F;   // m per axis
    static constexpr float kFirstPersonOffsetDefaultY = 0.06F;
    static constexpr float kFollowSmoothingTau = 0.25F;      // seconds
    // FP position smoothing is off (tau 0 = raw tracking): lag detaches
    // the view from a running character (stereo.216). FOLLOW does not
    // have that problem and keeps its orbit smoothing.
    static constexpr float kFirstPersonSmoothingTau = 0.0F;  // seconds
    // Direction follow lags behind the bone on purpose: an instant hard lock
    // to a dancing head is the worst motion-sickness case.
    static constexpr float kFpDirectionFollowTau = 0.30F;    // seconds

    [[nodiscard]] VrFreeCameraMode Mode() const noexcept { return mode_; }

    // gamePose: the pose Cinemachine requested this frame. It seeds Free mode
    //   on entry / reset and stays untouched by the rig otherwise.
    // headPose: last composed HMD pose in game world (previous frame). Drives
    //   the Free-mode movement basis and the snap-turn pivot so a turn spins
    //   the view around the user's head, not around the rig origin.
    VrFreeCameraUpdateResult Update(
        const VrFreeCameraCommands& commands,
        const VrFreeCameraAnchor& anchor,
        const pose::Pose& gamePose,
        bool headPoseValid,
        const pose::Pose& headPose) noexcept;

    void ForceOff() noexcept;

private:
    void EnterMode(VrFreeCameraMode next, const pose::Pose& gamePose) noexcept;
    void ApplyReset(const pose::Pose& gamePose) noexcept;
    void UpdateAnchorSmoothing(
        const VrFreeCameraAnchor& anchor,
        float dtSeconds,
        float tauSeconds) noexcept;
    [[nodiscard]] bool ConsumeSnapTurn(
        float rightStickX,
        float rightStickY,
        float& degrees) noexcept;

    VrFreeCameraMode mode_ = VrFreeCameraMode::Off;

    // Free mode.
    pose::Vector3 freePosition_{};
    float freeYawRadians_ = 0.0F;

    // Follow mode.
    bool followSeeded_ = false;
    float followYawRadians_ = 0.0F;
    float followDistance_ = kFollowDistanceDefault;
    float followHeight_ = 0.0F;

    // First person mode. Offset is local to the rig yaw: x lateral, y up,
    // z forward.
    pose::Vector3 firstPersonOffset_{0.0F, kFirstPersonOffsetDefaultY, 0.0F};
    float firstPersonYawRadians_ = 0.0F;
    // Smoothed full-rotation state for the "turn + tilt" direction follow.
    bool fpOrientationValid_ = false;
    pose::Quaternion fpOrientation_{};

    // Shared anchor smoothing.
    bool anchorSmoothedValid_ = false;
    pose::Vector3 anchorSmoothedPosition_{};
    pose::Vector3 anchorForward_{0.0F, 0.0F, 1.0F};

    bool snapEngaged_ = false;
    bool lastPoseValid_ = false;
    pose::Pose lastPose_{};
};

// Yaw helpers shared with tests.
[[nodiscard]] pose::Quaternion QuaternionFromYaw(float yawRadians) noexcept;
[[nodiscard]] float YawFromOrientation(
    const pose::Quaternion& orientation,
    float fallbackYawRadians) noexcept;

// Cross-thread mode mirror. The Unity main thread updates it from the rig;
// the OpenXR worker reads it to suppress mirror-panel thumbstick scrolling
// while locomotion owns the sticks.
void PublishVrFreeCameraMode(VrFreeCameraMode mode) noexcept;
[[nodiscard]] VrFreeCameraMode ReadVrFreeCameraMode() noexcept;
[[nodiscard]] bool IsVrFreeCameraLocomotionActive() noexcept;
[[nodiscard]] bool IsVrFreeCameraBoneAnchorWanted() noexcept;
[[nodiscard]] bool IsVrFreeCameraFirstPerson() noexcept;

// Menu -> game-thread mode request. The OpenXR worker (menu) stores a target
// mode; the Unity main thread consumes it into VrFreeCameraCommands. Returns
// -1 when no request is pending.
void RequestVrFreeCameraMode(VrFreeCameraMode mode) noexcept;
[[nodiscard]] int ConsumeVrFreeCameraModeRequest() noexcept;

// FOLLOW anchor bone (HumanBodyBones value). Owned here as an atomic so the
// menu (OpenXR worker) and the bone sampler (Unity main thread) never touch
// GKCamera's CSEnum across threads. Defaults to Head (10).
void SetVrFreeCameraFollowBone(int humanBodyBoneValue) noexcept;
[[nodiscard]] int ReadVrFreeCameraFollowBone() noexcept;
// Advances through kVrFreeCameraBoneOptions; returns the new value.
int CycleVrFreeCameraFollowBone(std::uint32_t presses) noexcept;
// i18n key of the option matching `value`, or the Head key when unknown.
[[nodiscard]] const char* VrFreeCameraBoneKey(int humanBodyBoneValue) noexcept;

[[nodiscard]] const char* VrFreeCameraModeName(VrFreeCameraMode mode) noexcept;

} // namespace gakumas::vr::camera
