#include "RelativePoseBridge.hpp"

#include <cmath>

namespace gakumas::vr::pose {

void RelativePoseBridge::Reset() noexcept {
    baselineReady_ = false;
    baselinePoseEpoch_ = 0;
    baselineSessionGeneration_ = 0;
    baselineReferenceSpaceType_ = 0;
    baseline_ = {};
}

BridgeUpdateResult RelativePoseBridge::Update(
    const Pose& gameRequested,
    const StereoPoseSample& sample,
    std::int64_t nowNanoseconds,
    std::int64_t maximumAgeNanoseconds,
    float worldScale,
    Pose& composed,
    const PoseAdmission* admission) noexcept {
    StereoComposedPose stereo{};
    const BridgeUpdateResult result = UpdateStereo(
        gameRequested,
        sample,
        nowNanoseconds,
        maximumAgeNanoseconds,
        worldScale,
        stereo,
        admission);
    composed = stereo.center;
    return result;
}

BridgeUpdateResult RelativePoseBridge::UpdateStereo(
    const Pose& gameRequested,
    const StereoPoseSample& sample,
    std::int64_t nowNanoseconds,
    std::int64_t maximumAgeNanoseconds,
    float worldScale,
    StereoComposedPose& composed,
    const PoseAdmission* admission) noexcept {
    const bool ticketValid = admission != nullptr &&
        SampleMatchesAdmission(sample, *admission);
    const bool timestampValid = admission == nullptr &&
        sample.hostPublishTimeNanoseconds > 0 &&
        nowNanoseconds >= sample.hostPublishTimeNanoseconds &&
        nowNanoseconds - sample.hostPublishTimeNanoseconds <= maximumAgeNanoseconds;
    if (!sample.valid || sample.viewCount != 2 ||
        (admission != nullptr ? !ticketValid : !timestampValid) ||
        (admission == nullptr && maximumAgeNanoseconds < 0) ||
        !std::isfinite(worldScale) || worldScale < 0.0F) {
        Reset();
        return BridgeUpdateResult::Unavailable;
    }

    std::array<Pose, 2> eyes{sample.eyes[0].pose, sample.eyes[1].pose};
    Pose currentCenter{};
    if (!TryCenterStereoPose(eyes, currentCenter)) {
        Reset();
        return BridgeUpdateResult::Unavailable;
    }

    const bool baselineChanged = !baselineReady_ ||
        baselinePoseEpoch_ != sample.poseEpoch ||
        baselineSessionGeneration_ != sample.sessionGeneration ||
        baselineReferenceSpaceType_ != sample.referenceSpaceType;
    if (baselineChanged) {
        baseline_ = currentCenter;
        baselineReady_ = true;
        baselinePoseEpoch_ = sample.poseEpoch;
        baselineSessionGeneration_ = sample.sessionGeneration;
        baselineReferenceSpaceType_ = sample.referenceSpaceType;
    }

    if (!TryComposeGamePose(
            gameRequested,
            baseline_,
            currentCenter,
            worldScale,
            composed.center)) {
        Reset();
        return BridgeUpdateResult::Unavailable;
    }

    for (std::size_t eyeIndex = 0; eyeIndex < composed.eyes.size(); ++eyeIndex) {
        if (!TryComposeGamePose(
                gameRequested,
                baseline_,
                sample.eyes[eyeIndex].pose,
                worldScale,
                composed.eyes[eyeIndex])) {
            Reset();
            return BridgeUpdateResult::Unavailable;
        }
    }
    return baselineChanged
        ? BridgeUpdateResult::BaselineLatched
        : BridgeUpdateResult::Applied;
}

} // namespace gakumas::vr::pose
