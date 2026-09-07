#pragma once

namespace gakumas::vr::input {
void InstallUnityPointerInput() noexcept;
bool UnityPointerInputAvailable() noexcept;
bool QueueUnityPointer(float u, float v, bool held, bool down, bool up) noexcept;
void RenewUnityPointer() noexcept;
void CancelUnityPointer() noexcept;
} // namespace gakumas::vr::input
