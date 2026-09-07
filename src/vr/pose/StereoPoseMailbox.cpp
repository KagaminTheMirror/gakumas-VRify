#include "StereoPoseMailbox.hpp"

#include <chrono>

namespace gakumas::vr::pose {

std::int64_t MonotonicNowNanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void StereoPoseMailbox::Publish(StereoPoseSample sample) noexcept {
    std::lock_guard lock(mutex_);
    const bool startsNewEpoch = !sample_.valid ||
        sample_.sessionGeneration != sample.sessionGeneration ||
        sample_.referenceSpaceType != sample.referenceSpaceType;
    if (startsNewEpoch) {
        ++nextPoseEpoch_;
    }
    sample.valid = true;
    sample.revision = ++nextRevision_;
    sample.poseEpoch = nextPoseEpoch_;
    sample.hostPublishTimeNanoseconds = MonotonicNowNanoseconds();
    sample_ = sample;
}

void StereoPoseMailbox::Invalidate(
    std::uint64_t sessionGeneration,
    std::int32_t referenceSpaceType) noexcept {
    std::lock_guard lock(mutex_);
    sample_.valid = false;
    sample_.revision = ++nextRevision_;
    sample_.sessionGeneration = sessionGeneration;
    sample_.referenceSpaceType = referenceSpaceType;
    sample_.hostPublishTimeNanoseconds = MonotonicNowNanoseconds();
}

bool StereoPoseMailbox::Read(StereoPoseSample& sample) const noexcept {
    std::lock_guard lock(mutex_);
    sample = sample_;
    return sample.valid;
}

} // namespace gakumas::vr::pose
