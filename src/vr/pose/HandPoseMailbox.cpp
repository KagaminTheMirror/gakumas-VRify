#include "HandPoseMailbox.hpp"

#include "StereoPoseMailbox.hpp"

namespace gakumas::vr::pose {
namespace {

HandPoseMailbox g_mailbox;

} // namespace

void HandPoseMailbox::Publish(HandPoseSample sample) noexcept {
    std::lock_guard lock(mutex_);
    sample.valid = true;
    sample.revision = ++nextRevision_;
    sample.hostPublishTimeNanoseconds = MonotonicNowNanoseconds();
    sample_ = sample;
}

void HandPoseMailbox::Invalidate() noexcept {
    std::lock_guard lock(mutex_);
    sample_ = {};
    sample_.revision = ++nextRevision_;
    sample_.hostPublishTimeNanoseconds = MonotonicNowNanoseconds();
}

bool HandPoseMailbox::Read(HandPoseSample& sample) const noexcept {
    std::lock_guard lock(mutex_);
    sample = sample_;
    return sample.valid;
}

HandPoseMailbox& HandTrackingMailbox() noexcept {
    return g_mailbox;
}

} // namespace gakumas::vr::pose
