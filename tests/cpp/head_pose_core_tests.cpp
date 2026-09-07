#include "../../src/vr/pose/PoseMath.hpp"
#include "../../src/vr/pose/RelativePoseBridge.hpp"
#include "../../src/vr/pose/StereoPoseMailbox.hpp"
#include "../../src/vr/camera/ProjectionIntrinsics.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

using gakumas::vr::pose::BridgeUpdateResult;
using gakumas::vr::pose::Pose;
using gakumas::vr::pose::Quaternion;
using gakumas::vr::pose::StereoPoseMailbox;
using gakumas::vr::pose::StereoPoseSample;
using gakumas::vr::pose::Vector3;

constexpr float kPi = 3.14159265358979323846F;

[[noreturn]] void Fail(const char* message) {
    std::cerr << "head-pose core test failed: " << message << '\n';
    std::exit(1);
}

void Expect(bool condition, const char* message) {
    if (!condition) {
        Fail(message);
    }
}

void ExpectNear(float actual, float expected, const char* message) {
    if (std::abs(actual - expected) > 1.0e-4F) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

Quaternion AxisAngleY(float degrees) {
    const float halfRadians = degrees * kPi / 360.0F;
    return {0.0F, std::sin(halfRadians), 0.0F, std::cos(halfRadians)};
}

StereoPoseSample MakeStereoSample(
    std::uint64_t sessionGeneration,
    std::uint64_t poseEpoch,
    std::int64_t publishTime,
    Vector3 center,
    Quaternion orientation = {}) {
    StereoPoseSample sample;
    sample.valid = true;
    sample.sessionGeneration = sessionGeneration;
    sample.poseEpoch = poseEpoch;
    sample.referenceSpaceType = 3;
    sample.hostPublishTimeNanoseconds = publishTime;
    sample.viewCount = 2;
    sample.eyes[0].pose = {
        {center.x - 0.032F, center.y, center.z}, orientation};
    sample.eyes[1].pose = {
        {center.x + 0.032F, center.y, center.z}, orientation};
    return sample;
}

void TestCoordinateConversionAndComposition() {
    const Pose xr{{1.0F, 2.0F, -3.0F}, {0.1F, 0.2F, 0.3F, 0.9F}};
    const Pose unity = gakumas::vr::pose::OpenXrToUnity(xr);
    ExpectNear(unity.position.x, 1.0F, "OpenXR X conversion");
    ExpectNear(unity.position.y, 2.0F, "OpenXR Y conversion");
    ExpectNear(unity.position.z, 3.0F, "OpenXR Z conversion");
    ExpectNear(unity.orientation.x, -0.1F, "OpenXR quaternion X conversion");
    ExpectNear(unity.orientation.y, -0.2F, "OpenXR quaternion Y conversion");
    ExpectNear(unity.orientation.z, 0.3F, "OpenXR quaternion Z conversion");

    Pose composed{};
    const Pose game{{10.0F, 20.0F, 30.0F}, {}};
    const Pose baseline{{0.0F, 0.0F, 0.0F}, {}};
    const Pose moved{{0.0F, 0.0F, -0.25F}, {}};
    Expect(
        gakumas::vr::pose::TryComposeGamePose(
            game, baseline, moved, 2.0F, composed),
        "identity pose composition");
    ExpectNear(composed.position.x, 10.0F, "scaled local X");
    ExpectNear(composed.position.y, 20.0F, "scaled local Y");
    ExpectNear(composed.position.z, 30.5F, "scaled local Z");

    const Pose turnedGame{{1.0F, 2.0F, 3.0F}, AxisAngleY(90.0F)};
    Expect(
        gakumas::vr::pose::TryComposeGamePose(
            turnedGame, baseline, moved, 1.0F, composed),
        "camera-local translation composition");
    ExpectNear(composed.position.x, 1.25F, "camera-local offset follows cut rotation");
    ExpectNear(composed.position.y, 2.0F, "camera-local rotated Y");
    ExpectNear(composed.position.z, 3.0F, "camera-local rotated Z");

    const Pose rotatedHead{{}, AxisAngleY(30.0F)};
    Expect(
        gakumas::vr::pose::TryComposeGamePose(
            game, baseline, rotatedHead, 1.0F, composed),
        "head rotation composition");
    const Vector3 forward = gakumas::vr::pose::Rotate(
        composed.orientation,
        {0.0F, 0.0F, 1.0F});
    ExpectNear(forward.x, -0.5F, "OpenXR yaw handedness");
    ExpectNear(forward.z, std::cos(30.0F * kPi / 180.0F), "OpenXR yaw magnitude");

    const Pose invalid{{}, {0.0F, 0.0F, 0.0F, 0.0F}};
    Expect(
        !gakumas::vr::pose::TryComposeGamePose(
            game, invalid, moved, 1.0F, composed),
        "zero quaternion must fail closed");

    const Pose headset{{10.0F, 20.0F, 30.0F}, {}};
    const Pose hmdOpenXr{{0.0F, 0.0F, 0.0F}, {}};
    const Pose handOpenXr{{0.20F, -0.10F, -0.30F}, {}};
    Expect(
        gakumas::vr::pose::TryComposeGamePose(
            headset, hmdOpenXr, handOpenXr, 1.0F, composed),
        "tracking-space hand composes into Unity world");
    ExpectNear(composed.position.x, 10.20F, "hand world X follows OpenXR +X");
    ExpectNear(composed.position.y, 19.90F, "hand world Y follows OpenXR +Y");
    ExpectNear(composed.position.z, 30.30F, "hand world Z flips OpenXR -Z");

    const Pose yawedHeadset{{1.0F, 2.0F, 3.0F}, AxisAngleY(90.0F)};
    Expect(
        gakumas::vr::pose::TryComposeGamePose(
            yawedHeadset, hmdOpenXr, handOpenXr, 1.0F, composed),
        "hand offset follows headset yaw");
    ExpectNear(composed.position.x, 1.30F, "yawed hand uses camera-local +X from OpenXR -Z");
    ExpectNear(composed.position.y, 1.90F, "yawed hand Y");
    ExpectNear(composed.position.z, 2.80F, "yawed hand uses camera-local -Z from OpenXR +X");
}

void TestStereoCenterAndMailboxEpochs() {
    std::array<Pose, 2> eyes{
        Pose{{-0.032F, 1.6F, 0.0F}, AxisAngleY(10.0F)},
        Pose{{0.032F, 1.6F, 0.0F}, AxisAngleY(10.0F)},
    };
    eyes[1].orientation = {
        -eyes[1].orientation.x,
        -eyes[1].orientation.y,
        -eyes[1].orientation.z,
        -eyes[1].orientation.w,
    };
    Pose center{};
    Expect(
        gakumas::vr::pose::TryCenterStereoPose(eyes, center),
        "stereo center with equivalent quaternion signs");
    ExpectNear(center.position.x, 0.0F, "stereo center X");
    ExpectNear(center.position.y, 1.6F, "stereo center Y");

    StereoPoseMailbox mailbox;
    StereoPoseSample published = MakeStereoSample(4, 0, 0, {});
    mailbox.Publish(published);
    StereoPoseSample read{};
    Expect(mailbox.Read(read), "mailbox first publish");
    const std::uint64_t firstEpoch = read.poseEpoch;
    const std::uint64_t firstRevision = read.revision;
    Expect(firstEpoch != 0, "mailbox assigns pose epoch");
    Expect(read.hostPublishTimeNanoseconds > 0, "mailbox assigns host timestamp");

    mailbox.Publish(published);
    Expect(mailbox.Read(read), "mailbox repeated publish");
    Expect(read.poseEpoch == firstEpoch, "continuous tracking keeps epoch");
    Expect(read.revision > firstRevision, "publish increments revision");

    mailbox.Invalidate(4, 3);
    Expect(!mailbox.Read(read), "mailbox invalidation");
    mailbox.Publish(published);
    Expect(mailbox.Read(read), "mailbox tracking reacquisition");
    Expect(read.poseEpoch > firstEpoch, "reacquisition starts a new epoch");
    const std::uint64_t reacquiredEpoch = read.poseEpoch;

    published.sessionGeneration = 5;
    mailbox.Publish(published);
    Expect(mailbox.Read(read), "mailbox new session publish");
    Expect(read.poseEpoch > reacquiredEpoch, "new session starts a new epoch");
}

void TestRelativeBridgeLifecycleAndCuts() {
    constexpr std::int64_t now = 10'000'000'000LL;
    constexpr std::int64_t maximumAge = 250'000'000LL;
    gakumas::vr::pose::RelativePoseBridge bridge;
    Pose output{};
    const Pose gameA{{1.0F, 2.0F, 3.0F}, {}};
    const Pose gameB{{10.0F, 20.0F, 30.0F}, AxisAngleY(90.0F)};

    auto sample = MakeStereoSample(7, 11, now - 1'000'000LL, {});
    Expect(
        bridge.Update(gameA, sample, now, maximumAge, 1.0F, output) ==
            BridgeUpdateResult::BaselineLatched,
        "first tracked pose latches baseline");
    ExpectNear(output.position.x, gameA.position.x, "baseline preserves game pose");

    gakumas::vr::pose::RelativePoseBridge stereoBridge;
    gakumas::vr::pose::StereoComposedPose stereoOutput{};
    Expect(
        stereoBridge.UpdateStereo(
            gameA, sample, now, maximumAge, 1.0F, stereoOutput) ==
            BridgeUpdateResult::BaselineLatched,
        "stereo baseline latches");
    ExpectNear(
        stereoOutput.eyes[0].position.x,
        gameA.position.x - 0.032F,
        "baseline keeps left-eye IPD offset");
    ExpectNear(
        stereoOutput.eyes[1].position.x,
        gameA.position.x + 0.032F,
        "baseline keeps right-eye IPD offset");

    sample.eyes[0].pose.position.z = -0.20F;
    sample.eyes[1].pose.position.z = -0.20F;
    Expect(
        bridge.Update(gameA, sample, now, maximumAge, 1.0F, output) ==
            BridgeUpdateResult::Applied,
        "tracked motion applies");
    ExpectNear(output.position.z, 3.20F, "tracked forward motion");

    Expect(
        bridge.Update(gameB, sample, now, maximumAge, 1.0F, output) ==
            BridgeUpdateResult::Applied,
        "game camera cut remains live");
    ExpectNear(output.position.x, 10.20F, "head delta follows new game camera basis");
    ExpectNear(output.position.z, 30.0F, "cut anchor is not frozen");

    StereoPoseSample stale = sample;
    stale.hostPublishTimeNanoseconds = now - maximumAge - 1;
    Expect(
        bridge.Update(gameA, stale, now, maximumAge, 1.0F, output) ==
            BridgeUpdateResult::Unavailable,
        "stale pose fails closed");

    sample.hostPublishTimeNanoseconds = now;
    Expect(
        bridge.Update(gameA, sample, now, maximumAge, 1.0F, output) ==
            BridgeUpdateResult::BaselineLatched,
        "fresh pose after staleness re-centers");

    sample.poseEpoch = 12;
    Expect(
        bridge.Update(gameB, sample, now, maximumAge, 1.0F, output) ==
            BridgeUpdateResult::BaselineLatched,
        "tracking epoch change re-centers");
    ExpectNear(output.position.x, gameB.position.x, "epoch reset preserves cut pose");

    sample.valid = false;
    Expect(
        bridge.Update(gameA, sample, now, maximumAge, 1.0F, output) ==
            BridgeUpdateResult::Unavailable,
        "invalid pose stops bridge writes");

    sample = MakeStereoSample(8, 13, now, {});
    Expect(
        bridge.Update(gameA, sample, now, maximumAge, 1.0F, output) ==
            BridgeUpdateResult::BaselineLatched,
        "session change re-centers");

    sample.eyes[0].pose.position.x = std::numeric_limits<float>::quiet_NaN();
    Expect(
        bridge.Update(gameA, sample, now, maximumAge, 1.0F, output) ==
            BridgeUpdateResult::Unavailable,
        "non-finite tracking data fails closed");
}

void TestProjectionScalarIntrinsics() {
    using gakumas::vr::camera::PhysicalCameraIntrinsics;
    using gakumas::vr::camera::ProjectionIntrinsics;
    using gakumas::vr::camera::TryDerivePhysicalCameraIntrinsics;
    using gakumas::vr::camera::TryDeriveProjectionIntrinsics;

    ProjectionIntrinsics symmetric{};
    Expect(
        TryDeriveProjectionIntrinsics(
            -kPi * 0.25F, kPi * 0.25F,
            -kPi * 0.25F, kPi * 0.25F, symmetric),
        "symmetric projection intrinsics");
    ExpectNear(symmetric.verticalFieldOfViewDegrees, 90.0F,
               "symmetric vertical FOV");
    ExpectNear(symmetric.aspect, 1.0F, "symmetric aspect");
    ExpectNear(symmetric.lensShiftX, 0.0F, "symmetric horizontal lens shift");
    ExpectNear(symmetric.lensShiftY, 0.0F, "symmetric vertical lens shift");
    ExpectNear(symmetric.projectionM00, 1.0F, "symmetric projection m00");
    ExpectNear(symmetric.projectionM11, 1.0F, "symmetric projection m11");

    PhysicalCameraIntrinsics symmetricPhysical{};
    Expect(
        TryDerivePhysicalCameraIntrinsics(
            symmetric, 24.0F, symmetricPhysical),
        "symmetric physical camera intrinsics");
    ExpectNear(symmetricPhysical.sensorWidthMillimeters, 24.0F,
               "symmetric physical sensor width");
    ExpectNear(symmetricPhysical.sensorHeightMillimeters, 24.0F,
               "symmetric physical sensor height");
    ExpectNear(symmetricPhysical.focalLengthMillimeters, 12.0F,
               "symmetric physical focal length");

    constexpr float left = -1.2F;
    constexpr float right = 0.8F;
    constexpr float bottom = -1.1F;
    constexpr float top = 0.9F;
    ProjectionIntrinsics asymmetric{};
    Expect(
        TryDeriveProjectionIntrinsics(
            std::atan(left), std::atan(right),
            std::atan(bottom), std::atan(top), asymmetric),
        "asymmetric projection intrinsics");
    const float halfHeight = std::tan(
        asymmetric.verticalFieldOfViewDegrees * kPi / 360.0F);
    const float halfWidth = halfHeight * asymmetric.aspect;
    const float centerX = 2.0F * halfWidth * asymmetric.lensShiftX;
    const float centerY = 2.0F * halfHeight * asymmetric.lensShiftY;
    ExpectNear(centerX - halfWidth, left, "reconstructed left tangent");
    ExpectNear(centerX + halfWidth, right, "reconstructed right tangent");
    ExpectNear(centerY - halfHeight, bottom, "reconstructed bottom tangent");
    ExpectNear(centerY + halfHeight, top, "reconstructed top tangent");
    ExpectNear(asymmetric.projectionM02,
               (left + right) / (right - left),
               "asymmetric projection m02");
    ExpectNear(asymmetric.projectionM12,
               (bottom + top) / (top - bottom),
               "asymmetric projection m12");

    PhysicalCameraIntrinsics asymmetricPhysical{};
    Expect(
        TryDerivePhysicalCameraIntrinsics(
            asymmetric, 24.0F, asymmetricPhysical),
        "asymmetric physical camera intrinsics");
    ExpectNear(asymmetricPhysical.sensorWidthMillimeters,
               24.0F * asymmetric.aspect,
               "asymmetric physical sensor width");
    ExpectNear(asymmetricPhysical.sensorHeightMillimeters, 24.0F,
               "asymmetric physical sensor height");
    ExpectNear(asymmetricPhysical.focalLengthMillimeters,
               12.0F * asymmetric.projectionM11,
               "asymmetric physical focal length");

    ProjectionIntrinsics invalid{};
    Expect(
        !TryDeriveProjectionIntrinsics(
            0.1F, -0.1F, -0.5F, 0.5F, invalid),
        "inverted projection fails closed");
    Expect(
        !TryDerivePhysicalCameraIntrinsics(
            symmetric, 0.0F, symmetricPhysical),
        "invalid physical sensor height fails closed");
}

} // namespace

void TestOffsetPoseOnLocalX() {
    Pose out{};
    const Pose identity{{1.0F, 2.0F, 3.0F}, {}};
    Expect(
        gakumas::vr::pose::TryOffsetPoseOnLocalX(identity, 0.032F, out),
        "identity local-X offset");
    ExpectNear(out.position.x, 1.032F, "identity local +X");
    ExpectNear(out.position.y, 2.0F, "identity local Y unchanged");
    ExpectNear(out.position.z, 3.0F, "identity local Z unchanged");

    const Pose yaw180{{0.0F, 0.0F, 0.0F}, AxisAngleY(180.0F)};
    Expect(
        gakumas::vr::pose::TryOffsetPoseOnLocalX(yaw180, 0.032F, out),
        "yaw-180 local-X offset");
    ExpectNear(out.position.x, -0.032F, "yaw-180 local +X is world -X");
    ExpectNear(out.position.y, 0.0F, "yaw-180 local Y unchanged");
    ExpectNear(out.position.z, 0.0F, "yaw-180 local Z unchanged");

    Expect(
        !gakumas::vr::pose::TryOffsetPoseOnLocalX(
            identity, std::numeric_limits<float>::quiet_NaN(), out),
        "NaN local-X fails closed");
}

int main() {
    TestCoordinateConversionAndComposition();
    TestOffsetPoseOnLocalX();
    TestStereoCenterAndMailboxEpochs();
    TestRelativeBridgeLifecycleAndCuts();
    TestProjectionScalarIntrinsics();
    std::cout << "Head-pose core tests passed.\n";
    return 0;
}
