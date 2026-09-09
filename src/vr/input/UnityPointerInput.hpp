#pragma once

#include <cstdint>

namespace gakumas::vr::input {
void InstallUnityPointerInput() noexcept;
bool UnityPointerInputAvailable() noexcept;
bool QueueUnityPointer(float u, float v, bool held, bool down, bool up) noexcept;
void RenewUnityPointer() noexcept;
void CancelUnityPointer() noexcept;
void RegisterUnityPointerWait(
    std::uint64_t frameId,
    std::uint64_t sessionGeneration) noexcept;
void RegisterUnityPointerEndQueued(
    std::uint64_t frameId,
    std::uint64_t sessionGeneration) noexcept;
void ClearUnityPointerLease() noexcept;
} // namespace gakumas::vr::input
