#include "StereoPoseMailbox.hpp"

#include <chrono>

namespace gakumas::vr::pose {

std::int64_t MonotonicNowNanoseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int StereoPoseMailbox::SlotFor(std::uint64_t frameId) noexcept {
    if (frameId == 0) {
        return 0;
    }
    return static_cast<int>((frameId - 1U) % 2U);
}

void StereoPoseMailbox::Publish(StereoPoseSample sample) noexcept {
    std::lock_guard lock(mutex_);
    const int index = SlotFor(sample.frameId);
    const StereoPoseSample& previous = latestIndex_ >= 0
        ? slots_[static_cast<std::size_t>(latestIndex_)]
        : slots_[static_cast<std::size_t>(index)];
    const bool startsNewEpoch = !previous.valid ||
        previous.sessionGeneration != sample.sessionGeneration ||
        previous.referenceSpaceType != sample.referenceSpaceType;
    if (startsNewEpoch) {
        ++nextPoseEpoch_;
    }
    sample.valid = !sample.cancelled;
    sample.revision = ++nextRevision_;
    sample.poseEpoch = nextPoseEpoch_;
    sample.hostPublishTimeNanoseconds = MonotonicNowNanoseconds();
    slots_[static_cast<std::size_t>(index)] = sample;
    latestIndex_ = index;
}

void StereoPoseMailbox::Invalidate(
    std::uint64_t sessionGeneration,
    std::int32_t referenceSpaceType) noexcept {
    std::lock_guard lock(mutex_);
    ++nextRevision_;
    const std::int64_t now = MonotonicNowNanoseconds();
    for (StereoPoseSample& slot : slots_) {
        slot.valid = false;
        slot.cancelled = true;
        slot.revision = nextRevision_;
        slot.sessionGeneration = sessionGeneration;
        slot.referenceSpaceType = referenceSpaceType;
        slot.hostPublishTimeNanoseconds = now;
    }
    latestIndex_ = -1;
}

bool StereoPoseMailbox::Read(StereoPoseSample& sample) const noexcept {
    std::lock_guard lock(mutex_);
    const StereoPoseSample* best = nullptr;
    for (const StereoPoseSample& slot : slots_) {
        if (!slot.valid || slot.cancelled) {
            continue;
        }
        if (best == nullptr || slot.revision > best->revision) {
            best = &slot;
        }
    }
    if (best == nullptr) {
        sample = {};
        return false;
    }
    sample = *best;
    return true;
}

bool StereoPoseMailbox::ReadByFrameId(
    std::uint64_t frameId,
    StereoPoseSample& sample) const noexcept {
    std::lock_guard lock(mutex_);
    if (frameId == 0) {
        sample = {};
        return false;
    }
    const StereoPoseSample& slot = slots_[static_cast<std::size_t>(SlotFor(frameId))];
    if (!slot.valid || slot.cancelled || slot.frameId != frameId) {
        sample = {};
        return false;
    }
    sample = slot;
    return true;
}

bool StereoPoseMailbox::ReadAccepted(
    const PoseAdmission& admission,
    StereoPoseSample& sample) const noexcept {
    std::lock_guard lock(mutex_);
    if (admission.frameId == 0) {
        sample = {};
        return false;
    }
    const StereoPoseSample& slot =
        slots_[static_cast<std::size_t>(SlotFor(admission.frameId))];
    if (!SampleMatchesAdmission(slot, admission)) {
        sample = {};
        return false;
    }
    sample = slot;
    return true;
}

} // namespace gakumas::vr::pose
