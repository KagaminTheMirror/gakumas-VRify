#pragma once

#include "PoseMath.hpp"

namespace gakumas::vr::pose {

// Exponential pose follow. First sample and large jumps snap; otherwise
// position lerps and rotation nlerps with `1 - exp(-dt / tau)`.
struct PoseSmoother {
    bool initialized = false;
    Pose pose{};

    void Reset() noexcept;

    // Writes `out` only when `target` is finite and normalizable.
    bool Filter(
        const Pose& target,
        float dtSeconds,
        float tauSeconds,
        float snapMeters,
        float snapAbsDot,
        Pose& out) noexcept;
};

[[nodiscard]] float SmoothingAlpha(float dtSeconds, float tauSeconds) noexcept;
[[nodiscard]] Quaternion Nlerp(
    const Quaternion& from,
    const Quaternion& to,
    float alpha) noexcept;

} // namespace gakumas::vr::pose
