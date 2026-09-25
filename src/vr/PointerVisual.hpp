#pragma once

#include "pose/PoseMath.hpp"
#include <algorithm>
#include <cmath>

namespace gakumas::vr::pointer {

// VD 1.34.22.0 Contact -> DrawContact: quad side = distance * 2 / 250.
// Its 32-texel image has a 30-texel visible diameter (including outline).
// Reproduce the measured geometry, without shipping VD's texture or code.
inline constexpr float kOuterDiameterPerMetre = 0.008F * (30.0F / 32.0F);
inline constexpr float kInnerRadiusRatio = 26.0F / 30.0F;
inline float ClampSizeScale(float value) noexcept {
    return std::isfinite(value) ? std::clamp(value, 0.25F, 4.0F) : 1.0F;
}

struct RadiusUv {
    float x = 0.0F;
    float y = 0.0F;
};

// VIEW-space head origin; use the final filtered UV and the same frame's
// panel pose. Separate UV radii preserve a physical circle on any aspect.
inline RadiusUv VisibleRadiusUv(
    const pose::Pose& panelInView, float width, float height,
    float u, float v, float sizeScale) noexcept {
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0F ||
        height <= 0.0F || !std::isfinite(u) || !std::isfinite(v) ||
        !pose::IsFinite(panelInView.position) || !pose::IsFinite(panelInView.orientation)) {
        return {};
    }
    const auto offset = pose::Rotate(panelInView.orientation,
        {(u - 0.5F) * width, (0.5F - v) * height, 0.0F});
    const auto& p = panelInView.position;
    const float x = p.x + offset.x, y = p.y + offset.y, z = p.z + offset.z;
    const float distance = std::sqrt(x*x + y*y + z*z);
    if (!std::isfinite(distance)) return {};
    const float radius = 0.5F * kOuterDiameterPerMetre * distance * ClampSizeScale(sizeScale);
    return {radius / width, radius / height};
}

} // namespace gakumas::vr::pointer
