#pragma once

namespace gakumas::vr::input {

struct ScrollVector {
    float x = 0.0F;
    float y = 0.0F;
};

// The measured game list uses sensitivity 80. Scaling only the VR component
// against the live target sensitivity keeps the requested content velocity
// consistent while leaving physical wheel input byte-for-byte unchanged.
inline constexpr float kReferenceScrollSensitivity = 80.0F;

[[nodiscard]] ScrollVector RouteVrScrollDelta(
    ScrollVector eventDelta,
    ScrollVector injectedDelta,
    bool horizontal,
    bool vertical,
    float targetSensitivity) noexcept;

} // namespace gakumas::vr::input
