#include "../../src/vr/VrFreeCamera.hpp"
#include "../../src/vr/pose/PoseMath.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using gakumas::vr::camera::VrFreeCameraAnchor;
using gakumas::vr::camera::VrFreeCameraCommands;
using gakumas::vr::camera::VrFreeCameraMode;
using gakumas::vr::camera::VrFreeCameraRig;
using gakumas::vr::camera::VrFreeCameraTurnMode;
using gakumas::vr::pose::Pose;

constexpr float kPi = 3.14159265358979323846F;

[[noreturn]] void Fail(const char* message) {
    std::cerr << "free camera turn test failed: " << message << '\n';
    std::exit(1);
}

void Expect(bool condition, const char* message) {
    if (!condition) {
        Fail(message);
    }
}

void ExpectNear(float actual, float expected, float tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << message << ": expected " << expected << ", got " << actual
                  << " (diff=" << std::abs(actual - expected) << " > " << tolerance << ")\n";
        std::exit(1);
    }
}

float ExtractYawDegrees(const Pose& pose) {
    // Quaternion to yaw angle in degrees
    const float y = 2.0F * (pose.orientation.w * pose.orientation.y + pose.orientation.x * pose.orientation.z);
    const float x = 1.0F - 2.0F * (pose.orientation.y * pose.orientation.y + pose.orientation.z * pose.orientation.z);
    return std::atan2(y, x) * 180.0F / kPi;
}

void TestSnapTurnFreeMode() {
    VrFreeCameraRig rig;
    VrFreeCameraCommands commands;
    commands.hasModeRequest = true;
    commands.modeRequest = VrFreeCameraMode::Free;
    commands.turnMode = static_cast<int>(VrFreeCameraTurnMode::Snap);
    commands.dtSeconds = 0.016F;

    VrFreeCameraAnchor anchor{};
    Pose gamePose{{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}};

    auto result = rig.Update(commands, anchor, gamePose, false, {});
    Expect(result.mode == VrFreeCameraMode::Free, "Rig entered Free mode");
    commands.hasModeRequest = false;

    const float initialYaw = ExtractYawDegrees(result.rigPose);

    // 1. Flick right stick to 0.8 (above engage threshold 0.60)
    commands.rightStickX = 0.8F;
    commands.rightStickY = 0.0F;
    result = rig.Update(commands, anchor, gamePose, false, {});
    const float yawAfterFlick = ExtractYawDegrees(result.rigPose);
    ExpectNear(yawAfterFlick - initialYaw, 30.0F, 0.5F, "Snap turn 30 deg on first flick");

    // 2. Holding stick at 0.8 for multiple frames must NOT turn further
    for (int i = 0; i < 10; ++i) {
        result = rig.Update(commands, anchor, gamePose, false, {});
        ExpectNear(ExtractYawDegrees(result.rigPose), yawAfterFlick, 0.01F,
                   "Snap turn must not turn while held");
    }

    // 3. Release stick below release threshold 0.40
    commands.rightStickX = 0.0F;
    result = rig.Update(commands, anchor, gamePose, false, {});
    ExpectNear(ExtractYawDegrees(result.rigPose), yawAfterFlick, 0.01F,
               "Snap turn stays unchanged after stick release");

    // 4. Second flick turns another 30 degrees
    commands.rightStickX = 0.8F;
    result = rig.Update(commands, anchor, gamePose, false, {});
    ExpectNear(ExtractYawDegrees(result.rigPose) - initialYaw, 60.0F, 0.5F,
               "Snap turn second flick adds 30 deg");
}

void TestSmoothTurnFreeMode() {
    VrFreeCameraRig rig;
    VrFreeCameraCommands commands;
    commands.hasModeRequest = true;
    commands.modeRequest = VrFreeCameraMode::Free;
    commands.turnMode = static_cast<int>(VrFreeCameraTurnMode::Smooth);
    commands.turnSpeed = 120.0F; // 120 degrees per second
    commands.dtSeconds = 0.1F;   // 100ms per frame

    VrFreeCameraAnchor anchor{};
    Pose gamePose{{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}};

    auto result = rig.Update(commands, anchor, gamePose, false, {});
    commands.hasModeRequest = false;
    const float initialYaw = ExtractYawDegrees(result.rigPose);

    // 1. Center stick: yaw does not change
    commands.rightStickX = 0.0F;
    commands.rightStickY = 0.0F;
    result = rig.Update(commands, anchor, gamePose, false, {});
    ExpectNear(ExtractYawDegrees(result.rigPose), initialYaw, 0.01F, "Centered stick no turn");

    // 2. Deadzone: stick at 0.10 (below deadzone 0.15): yaw does not change
    commands.rightStickX = 0.10F;
    result = rig.Update(commands, anchor, gamePose, false, {});
    ExpectNear(ExtractYawDegrees(result.rigPose), initialYaw, 0.01F, "Deadzone stick no turn");

    // 3. Hold stick fully right (rightStickX = 1.0) for 5 frames of dt=0.1s (total 0.5s)
    // Full deflection -> 120 deg/s * 0.5s = 60 degrees.
    commands.rightStickX = 1.0F;
    for (int i = 0; i < 5; ++i) {
        result = rig.Update(commands, anchor, gamePose, false, {});
    }
    const float yawAfterTurn = ExtractYawDegrees(result.rigPose);
    ExpectNear(yawAfterTurn - initialYaw, 60.0F, 0.5F, "Smooth turn rotates continuously at 120 deg/s");

    // 4. Release stick: turning stops immediately
    commands.rightStickX = 0.0F;
    for (int i = 0; i < 5; ++i) {
        result = rig.Update(commands, anchor, gamePose, false, {});
        ExpectNear(ExtractYawDegrees(result.rigPose), yawAfterTurn, 0.01F,
                   "Yaw remains stationary after release");
    }

    // 5. Half deflection left: rightStickX = -0.5F
    // Scaled by ApplyDeadzone: (0.5 - 0.15) / (1.0 - 0.15) = 0.35 / 0.85 =~ 0.41176
    // In 0.1s: -0.41176 * 120 * 0.1 =~ -4.94 degrees
    commands.rightStickX = -0.5F;
    result = rig.Update(commands, anchor, gamePose, false, {});
    const float expectedDelta = -((0.5F - 0.15F) / (1.0F - 0.15F)) * 120.0F * 0.1F;
    ExpectNear(ExtractYawDegrees(result.rigPose) - yawAfterTurn, expectedDelta, 0.2F,
               "Partial stick deflection scales turn rate proportionally");
}

void TestVerticalDominanceSuppressesSmoothTurn() {
    VrFreeCameraRig rig;
    VrFreeCameraCommands commands;
    commands.hasModeRequest = true;
    commands.modeRequest = VrFreeCameraMode::Free;
    commands.turnMode = static_cast<int>(VrFreeCameraTurnMode::Smooth);
    commands.turnSpeed = 120.0F;
    commands.dtSeconds = 0.05F;

    VrFreeCameraAnchor anchor{};
    Pose gamePose{{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}};

    auto result = rig.Update(commands, anchor, gamePose, false, {});
    commands.hasModeRequest = false;
    const float initialYaw = ExtractYawDegrees(result.rigPose);
    const float initialY = result.rigPose.position.y;

    // Moving up (rightStickY = 0.8) with slight horizontal drift (rightStickX = 0.2)
    // |Y| = 0.8 >= 1.5 * |X| = 0.30 -> verticalDominant = true
    commands.rightStickY = 0.8F;
    commands.rightStickX = 0.2F;
    result = rig.Update(commands, anchor, gamePose, false, {});

    // Vertical position should increase, yaw should NOT change
    Expect(result.rigPose.position.y > initialY, "Camera height increased");
    ExpectNear(ExtractYawDegrees(result.rigPose), initialYaw, 0.01F,
               "Vertical dominant stick does not drift yaw");
}

void TestHeadPivotPreservation() {
    VrFreeCameraRig rig;
    VrFreeCameraCommands commands;
    commands.hasModeRequest = true;
    commands.modeRequest = VrFreeCameraMode::Free;
    commands.turnMode = static_cast<int>(VrFreeCameraTurnMode::Smooth);
    commands.turnSpeed = 120.0F;
    commands.dtSeconds = 0.1F;

    VrFreeCameraAnchor anchor{};
    Pose gamePose{{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}};
    // Head is 0.5m forward and 1.7m high from rig origin
    Pose headPose{{0.0F, 1.7F, 0.5F}, {0.0F, 0.0F, 0.0F, 1.0F}};

    auto result = rig.Update(commands, anchor, gamePose, true, headPose);
    commands.hasModeRequest = false;

    // Turn right smoothly
    commands.rightStickX = 1.0F;
    result = rig.Update(commands, anchor, gamePose, true, headPose);

    // After turning, the distance from rig position to head pivot must be preserved
    // (pivot was headPose.position)
    const float initialDist = std::hypot(headPose.position.x - 0.0F, headPose.position.z - 0.0F);
    const float newDist = std::hypot(headPose.position.x - result.rigPose.position.x,
                                     headPose.position.z - result.rigPose.position.z);
    ExpectNear(newDist, initialDist, 0.01F, "Pivot around user's head preserves radius");
}

} // namespace

int main() {
    std::cout << "Running free camera turn tests...\n";
    TestSnapTurnFreeMode();
    TestSmoothTurnFreeMode();
    TestVerticalDominanceSuppressesSmoothTurn();
    TestHeadPivotPreservation();
    std::cout << "All free camera turn tests passed.\n";
    return 0;
}
