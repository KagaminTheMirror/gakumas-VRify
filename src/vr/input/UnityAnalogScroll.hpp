#pragma once

#include <cstdint>

namespace gakumas::vr::input {

// Installs the production worker-to-main-thread bridge at Unity's proven
// Input.mouseScrollDelta entry. Must be called under the frozen VR runtime gate.
// Physical mouse-wheel input is preserved and the queued VR delta is added to it.
void InstallUnityAnalogScrollHook() noexcept;
[[nodiscard]] bool UnityAnalogScrollAvailable() noexcept;
[[nodiscard]] bool QueueUnityAnalogScrollDelta(float x, float y) noexcept;
void ResetUnityAnalogScroll() noexcept;

} // namespace gakumas::vr::input
