#include "VrCameraInputMailbox.hpp"

namespace gakumas::vr::camera {

void VrCameraInputMailbox::Publish(const VrCameraInputSample& sample) noexcept {
    std::lock_guard lock(mutex_);
    sample_ = sample;
}

void VrCameraInputMailbox::Invalidate() noexcept {
    std::lock_guard lock(mutex_);
    // Keep the cumulative press counters so the consumer's diff stays
    // monotonic across session restarts; only the live stick data dies.
    sample_.valid = false;
    sample_.leftStickX = 0.0F;
    sample_.leftStickY = 0.0F;
    sample_.rightStickX = 0.0F;
    sample_.rightStickY = 0.0F;
    sample_.sprintHeld = false;
}

bool VrCameraInputMailbox::Read(VrCameraInputSample& sample) const noexcept {
    std::lock_guard lock(mutex_);
    sample = sample_;
    return sample_.valid;
}

} // namespace gakumas::vr::camera
