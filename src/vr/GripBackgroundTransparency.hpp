#pragma once
namespace gakumas::vr {
// Unity owner thread: apply, restore and retire the accepted background fix.
void SyncGripBackgroundTransparency(bool armed) noexcept;
}
