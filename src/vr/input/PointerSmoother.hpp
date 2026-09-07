#pragma once

namespace gakumas::vr::input {

// One Euro filter (Casiez, Roussel, Vogel 2012). Low cutoff while the
// hand is still; cutoff rises with speed so a real swipe still follows.
struct OneEuroFilter {
    bool initialized = false;
    float hatX = 0.0F;
    float hatDx = 0.0F;

    void Reset() noexcept;
    [[nodiscard]] float Filter(
        float value,
        float dtSeconds,
        float minCutoff,
        float beta,
        float dCutoff) noexcept;
};

struct PointerUvSmoother {
    OneEuroFilter u{};
    OneEuroFilter v{};

    void Reset() noexcept;
    // First hover sample snaps. Lost hover resets. outU/outV are only
    // written when hovering is true and the raw sample is finite.
    bool Filter(
        bool hovering,
        float rawU,
        float rawV,
        float dtSeconds,
        float minCutoff,
        float beta,
        float& outU,
        float& outV) noexcept;
};

} // namespace gakumas::vr::input
