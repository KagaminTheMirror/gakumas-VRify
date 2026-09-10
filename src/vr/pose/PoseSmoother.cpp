#include "PoseSmoother.hpp"

#include <cmath>

namespace gakumas::vr::pose {
namespace {

Vector3 Lerp(const Vector3& from, const Vector3& to, float alpha) noexcept {
    return {
        from.x + (to.x - from.x) * alpha,
        from.y + (to.y - from.y) * alpha,
        from.z + (to.z - from.z) * alpha,
    };
}

float Distance(const Vector3& from, const Vector3& to) noexcept {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float dz = to.z - from.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float AbsDot(const Quaternion& left, const Quaternion& right) noexcept {
    const float dot =
        left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w;
    return std::abs(dot);
}

} // namespace

float SmoothingAlpha(float dtSeconds, float tauSeconds) noexcept {
    if (dtSeconds <= 0.0F || tauSeconds <= 0.0F) {
        return 1.0F;
    }
    return 1.0F - std::exp(-dtSeconds / tauSeconds);
}

Quaternion Nlerp(
    const Quaternion& from,
    const Quaternion& to,
    float alpha) noexcept {
    Quaternion toward = to;
    const float dot =
        from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
    if (dot < 0.0F) {
        toward = {-to.x, -to.y, -to.z, -to.w};
    }
    const Quaternion mixed{
        from.x + (toward.x - from.x) * alpha,
        from.y + (toward.y - from.y) * alpha,
        from.z + (toward.z - from.z) * alpha,
        from.w + (toward.w - from.w) * alpha,
    };
    Quaternion normalized{};
    if (!TryNormalize(mixed, normalized)) {
        return toward;
    }
    return normalized;
}

void PoseSmoother::Reset() noexcept {
    initialized = false;
    pose = {};
}

bool PoseSmoother::Filter(
    const Pose& target,
    float dtSeconds,
    float tauSeconds,
    float snapMeters,
    float snapAbsDot,
    Pose& out) noexcept {
    if (!IsFinite(target.position) || !IsFinite(target.orientation)) {
        return false;
    }
    Quaternion targetRotation{};
    if (!TryNormalize(target.orientation, targetRotation)) {
        return false;
    }
    const Pose normalized{target.position, targetRotation};
    if (!initialized || dtSeconds <= 0.0F || tauSeconds <= 0.0F ||
        !std::isfinite(snapMeters) || snapMeters < 0.0F ||
        !std::isfinite(snapAbsDot) ||
        !IsFinite(pose.position) || !IsFinite(pose.orientation)) {
        pose = normalized;
        initialized = true;
        out = pose;
        return true;
    }

    Quaternion currentRotation{};
    if (!TryNormalize(pose.orientation, currentRotation)) {
        pose = normalized;
        out = pose;
        return true;
    }
    if (Distance(pose.position, normalized.position) >= snapMeters ||
        AbsDot(currentRotation, targetRotation) < snapAbsDot) {
        pose = normalized;
        out = pose;
        return true;
    }

    const float alpha = SmoothingAlpha(dtSeconds, tauSeconds);
    pose.position = Lerp(pose.position, normalized.position, alpha);
    pose.orientation = Nlerp(currentRotation, targetRotation, alpha);
    if (!IsFinite(pose.position) || !IsFinite(pose.orientation)) {
        pose = normalized;
    }
    out = pose;
    return true;
}

} // namespace gakumas::vr::pose
