#include "PointerSmoother.hpp"

#include <algorithm>
#include <cmath>

namespace gakumas::vr::input {
namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kDefaultDCutoff = 1.0F;
// A layout rebuild or a teleport-sized UV jump is not hand tremor.
constexpr float kDiscontinuityUv = 0.06F;

[[nodiscard]] float Alpha(float dtSeconds, float cutoffHz) noexcept {
    if (!(dtSeconds > 0.0F) || !(cutoffHz > 0.0F) || !std::isfinite(dtSeconds) ||
        !std::isfinite(cutoffHz)) {
        return 1.0F;
    }
    const float tau = 1.0F / (2.0F * kPi * cutoffHz);
    return 1.0F / (1.0F + tau / dtSeconds);
}

} // namespace

void OneEuroFilter::Reset() noexcept {
    initialized = false;
    hatX = 0.0F;
    hatDx = 0.0F;
}

float OneEuroFilter::Filter(
    float value,
    float dtSeconds,
    float minCutoff,
    float beta,
    float dCutoff) noexcept {
    if (!std::isfinite(value)) {
        return hatX;
    }
    if (!initialized || !(dtSeconds > 0.0F) || !std::isfinite(dtSeconds) ||
        !(minCutoff > 0.0F)) {
        initialized = true;
        hatX = value;
        hatDx = 0.0F;
        return value;
    }

    const float dx = (value - hatX) / dtSeconds;
    const float dxAlpha = Alpha(dtSeconds, dCutoff > 0.0F ? dCutoff : kDefaultDCutoff);
    hatDx = dxAlpha * dx + (1.0F - dxAlpha) * hatDx;
    const float cutoff = minCutoff + std::max(0.0F, beta) * std::abs(hatDx);
    const float alpha = Alpha(dtSeconds, cutoff);
    hatX = alpha * value + (1.0F - alpha) * hatX;
    return hatX;
}

void PointerUvSmoother::Reset() noexcept {
    u.Reset();
    v.Reset();
}

bool PointerUvSmoother::Filter(
    bool hovering,
    float rawU,
    float rawV,
    float dtSeconds,
    float minCutoff,
    float beta,
    float& outU,
    float& outV) noexcept {
    if (!hovering || !std::isfinite(rawU) || !std::isfinite(rawV)) {
        Reset();
        return false;
    }
    if (u.initialized && v.initialized) {
        const float jumpU = rawU - u.hatX;
        const float jumpV = rawV - v.hatX;
        if (jumpU * jumpU + jumpV * jumpV >= kDiscontinuityUv * kDiscontinuityUv) {
            Reset();
        }
    }
    outU = u.Filter(rawU, dtSeconds, minCutoff, beta, kDefaultDCutoff);
    outV = v.Filter(rawV, dtSeconds, minCutoff, beta, kDefaultDCutoff);
    return true;
}

} // namespace gakumas::vr::input
