#pragma once
#include <algorithm>

namespace gakumas::vr {
struct GripBlurAabb { float minX, minY, maxX, maxY; };
struct GripBlurRect { float x, y, width, height; };

inline bool GripBlurCoversRoot(const GripBlurAabb& bounds, const GripBlurRect& root) noexcept {
    if (!(root.width > 0.0F && root.height > 0.0F)) return false;
    const float tolerance = (std::max)(2.0F, (std::max)(root.width, root.height) * 0.002F);
    return bounds.minX <= root.x + tolerance && bounds.minY <= root.y + tolerance &&
        bounds.maxX >= root.x + root.width - tolerance &&
        bounds.maxY >= root.y + root.height - tolerance;
}
} // namespace gakumas::vr
