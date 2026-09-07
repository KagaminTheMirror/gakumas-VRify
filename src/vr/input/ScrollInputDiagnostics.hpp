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

// Installs read-only managed IL2CPP hooks. This function is an optional probe
// and must only be called under Config::vrDiagnosticsStartupEnabled.
void InstallScrollInputDiagnosticHooks() noexcept;

} // namespace gakumas::vr::input
