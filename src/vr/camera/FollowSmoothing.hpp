#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace gakumas::vr::camera {

// Horizontal = world X/Z; height = world Y. Neither depends on HMD yaw.
// Keep custom's persisted ID 5; retired comparison presets migrate to auto 0.
inline constexpr int kFollowSmoothingPresetCount = 2;
struct FollowSmoothingSettings {
    int preset = 0;
    int horizontalMs = 0;
    int verticalMs = 50;
};

inline int ClampFollowSmoothingMs(float value, int fallback) noexcept {
    return std::isfinite(value)
        ? static_cast<int>(std::clamp(value, 0.0F, 500.0F) + 0.5F)
        : fallback;
}

inline FollowSmoothingSettings ResolveFollowSmoothing(
    int preset, float horizontalMs, float verticalMs) noexcept {
    switch (preset) {
    case 5: return {5, ClampFollowSmoothingMs(horizontalMs, 0),
        ClampFollowSmoothingMs(verticalMs, 100)};
    default: return {};
    }
}

// One coherent publication from the settings thread to the Unity thread.
inline std::atomic<std::uint64_t> followSmoothingMailbox{
    (std::uint64_t{50} << 32)};

inline void PublishFollowSmoothing(
    int preset, float horizontalMs, float verticalMs) noexcept {
    const auto value = ResolveFollowSmoothing(preset, horizontalMs, verticalMs);
    followSmoothingMailbox.store(static_cast<std::uint64_t>(value.preset) |
        (static_cast<std::uint64_t>(value.horizontalMs) << 16) |
        (static_cast<std::uint64_t>(value.verticalMs) << 32),
        std::memory_order_release);
}

inline FollowSmoothingSettings ReadFollowSmoothing() noexcept {
    const auto value = followSmoothingMailbox.load(std::memory_order_acquire);
    return {static_cast<int>(value & 0xffff),
        static_cast<int>((value >> 16) & 0xffff),
        static_cast<int>((value >> 32) & 0xffff)};
}

} // namespace gakumas::vr::camera
