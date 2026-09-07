#include "ScrollAxisRouter.hpp"

#include <cmath>

namespace gakumas::vr::input {

ScrollVector RouteVrScrollDelta(
    ScrollVector eventDelta,
    ScrollVector injectedDelta,
    bool horizontal,
    bool vertical,
    float targetSensitivity) noexcept {
    ScrollVector routed{
        eventDelta.x - injectedDelta.x,
        eventDelta.y - injectedDelta.y,
    };
    const float scale = std::isfinite(targetSensitivity) &&
            std::abs(targetSensitivity) > 0.0001F
        ? kReferenceScrollSensitivity / targetSensitivity
        : 1.0F;
    if (horizontal) {
        routed.x += injectedDelta.x * scale;
    }
    if (vertical) {
        routed.y += injectedDelta.y * scale;
    }
    return routed;
}

} // namespace gakumas::vr::input
