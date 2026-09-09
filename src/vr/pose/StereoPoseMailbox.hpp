#pragma once

#include "PoseMath.hpp"

#include <array>
#include <cstdint>
#include <mutex>

namespace gakumas::vr::pose {

struct EyeFov {
    float angleLeft = 0.0F;
    float angleRight = 0.0F;
    float angleUp = 0.0F;
    float angleDown = 0.0F;
};

struct EyeSample {
    Pose pose{};
    EyeFov fov{};
};

struct StereoPoseSample {
    bool valid = false;
    bool cancelled = false;
    std::uint64_t revision = 0;
    std::uint64_t poseEpoch = 0;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t frameId = 0;
    std::uint64_t inputEpoch = 0;
    std::int32_t referenceSpaceType = 0;
    std::int64_t predictedDisplayTime = 0;
    std::int64_t hostPublishTimeNanoseconds = 0;
    std::uint64_t viewStateFlags = 0;
    std::uint32_t viewCount = 0;
    std::array<EyeSample, 2> eyes{};
};

struct PoseAdmission {
    std::uint64_t frameId = 0;
    std::uint64_t sessionGeneration = 0;
    std::uint64_t inputEpoch = 0;
    std::int32_t referenceSpaceType = 0;
    bool requireTracked = false;
};

[[nodiscard]] std::int64_t MonotonicNowNanoseconds() noexcept;

[[nodiscard]] inline bool SampleMatchesAdmission(
    const StereoPoseSample& sample,
    const PoseAdmission& admission) noexcept {
    if (!sample.valid || sample.cancelled || sample.viewCount != 2 ||
        sample.frameId == 0 || sample.frameId != admission.frameId ||
        sample.sessionGeneration != admission.sessionGeneration ||
        sample.referenceSpaceType != admission.referenceSpaceType) {
        return false;
    }
    if (admission.inputEpoch != 0 && sample.inputEpoch != admission.inputEpoch) {
        return false;
    }
    if (admission.requireTracked) {
        constexpr std::uint64_t kRequired =
            0x1 | 0x2 | 0x4 | 0x8; // orientation/position valid+tracked
        if ((sample.viewStateFlags & kRequired) != kRequired) {
            return false;
        }
    }
    return true;
}

// Two slots so N and N+1 can stay readable while both are in flight. Publish
// keys by frameId; Read() returns the newest valid sample for diagnostics.
class StereoPoseMailbox final {
public:
    void Publish(StereoPoseSample sample) noexcept;
    void Invalidate(
        std::uint64_t sessionGeneration = 0,
        std::int32_t referenceSpaceType = 0) noexcept;
    [[nodiscard]] bool Read(StereoPoseSample& sample) const noexcept;
    [[nodiscard]] bool ReadByFrameId(
        std::uint64_t frameId,
        StereoPoseSample& sample) const noexcept;
    [[nodiscard]] bool ReadAccepted(
        const PoseAdmission& admission,
        StereoPoseSample& sample) const noexcept;

private:
    [[nodiscard]] static int SlotFor(std::uint64_t frameId) noexcept;

    mutable std::mutex mutex_;
    std::array<StereoPoseSample, 2> slots_{};
    int latestIndex_ = -1;
    std::uint64_t nextRevision_ = 0;
    std::uint64_t nextPoseEpoch_ = 0;
};

} // namespace gakumas::vr::pose
