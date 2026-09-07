#pragma once

#include "PoseMath.hpp"

#include <array>
#include <cstdint>
#include <mutex>

namespace gakumas::vr::pose {

struct HandSample {
    bool valid = false;
    bool usedGrip = false;
    Pose pose{};
};

// OpenXR tracking-space hand poses (stage/local, same space as stereo eyes).
// Do not put these in StereoPoseSample — that type is viewCount==2 checked.
struct HandPoseSample {
    bool valid = false;
    std::uint64_t revision = 0;
    std::int64_t hostPublishTimeNanoseconds = 0;
    std::array<HandSample, 2> hands{};
};

class HandPoseMailbox final {
public:
    void Publish(HandPoseSample sample) noexcept;
    void Invalidate() noexcept;
    [[nodiscard]] bool Read(HandPoseSample& sample) const noexcept;

private:
    mutable std::mutex mutex_;
    HandPoseSample sample_{};
    std::uint64_t nextRevision_ = 0;
};

[[nodiscard]] HandPoseMailbox& HandTrackingMailbox() noexcept;

} // namespace gakumas::vr::pose
