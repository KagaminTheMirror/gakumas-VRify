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
    std::uint64_t revision = 0;
    std::uint64_t poseEpoch = 0;
    std::uint64_t sessionGeneration = 0;
    std::int32_t referenceSpaceType = 0;
    std::int64_t predictedDisplayTime = 0;
    std::int64_t hostPublishTimeNanoseconds = 0;
    std::uint64_t viewStateFlags = 0;
    std::uint32_t viewCount = 0;
    std::array<EyeSample, 2> eyes{};
};

[[nodiscard]] std::int64_t MonotonicNowNanoseconds() noexcept;

// A mutex is intentional here: OpenXR publishes once per frame and Unity takes
// a short copy. Unlike a raw seqlock over non-atomic floats, this has no C++ data
// race and never exposes a half-written stereo sample.
class StereoPoseMailbox final {
public:
    void Publish(StereoPoseSample sample) noexcept;
    void Invalidate(
        std::uint64_t sessionGeneration = 0,
        std::int32_t referenceSpaceType = 0) noexcept;
    [[nodiscard]] bool Read(StereoPoseSample& sample) const noexcept;

private:
    mutable std::mutex mutex_;
    StereoPoseSample sample_{};
    std::uint64_t nextRevision_ = 0;
    std::uint64_t nextPoseEpoch_ = 0;
};

} // namespace gakumas::vr::pose
