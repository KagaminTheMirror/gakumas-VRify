#include "../../src/vr/input/PointerSmoother.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using gakumas::vr::input::PointerUvSmoother;

[[noreturn]] void Fail(const char* message) {
    std::cerr << "pointer smoother test failed: " << message << '\n';
    std::exit(1);
}

void Expect(bool condition, const char* message) {
    if (!condition) {
        Fail(message);
    }
}

void ExpectNear(float actual, float expected, float tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void TestSnapAndHold() {
    PointerUvSmoother smoother;
    float u = 0.0F;
    float v = 0.0F;
    Expect(smoother.Filter(true, 0.25F, 0.40F, 0.0F, 0.80F, 3.00F, u, v), "first snap");
    ExpectNear(u, 0.25F, 1.0e-6F, "first U");
    ExpectNear(v, 0.40F, 1.0e-6F, "first V");

    Expect(smoother.Filter(true, 0.25F, 0.40F, 1.0F / 90.0F, 0.80F, 3.00F, u, v), "hold");
    ExpectNear(u, 0.25F, 1.0e-5F, "held U");
    ExpectNear(v, 0.40F, 1.0e-5F, "held V");
}

void TestHighFrequencyTremorIsAttenuated() {
    PointerUvSmoother smoother;
    float u = 0.0F;
    float v = 0.0F;
    constexpr float kDt = 1.0F / 90.0F;
    constexpr float kCenter = 0.50F;
    constexpr float kAmp = 0.012F;
    float minU = 1.0F;
    float maxU = 0.0F;
    Expect(smoother.Filter(true, kCenter, 0.50F, 0.0F, 0.80F, 3.00F, u, v), "seed");
    for (int i = 0; i < 180; ++i) {
        const float t = static_cast<float>(i) * kDt;
        const float raw = kCenter + kAmp * std::sin(2.0F * 3.14159265F * 18.0F * t);
        Expect(smoother.Filter(true, raw, 0.50F, kDt, 0.80F, 3.00F, u, v), "tremor sample");
        if (i >= 60) {
            minU = std::min(minU, u);
            maxU = std::max(maxU, u);
        }
    }
    const float outAmp = 0.5F * (maxU - minU);
    Expect(outAmp < kAmp * 0.35F, "18 Hz tremor should be well below the raw amplitude");
}

void TestIntentionalStepFollows() {
    PointerUvSmoother smoother;
    float u = 0.0F;
    float v = 0.0F;
    constexpr float kDt = 1.0F / 90.0F;
    Expect(smoother.Filter(true, 0.20F, 0.50F, 0.0F, 0.80F, 3.00F, u, v), "step seed");
    for (int i = 0; i < 36; ++i) {
        Expect(
            smoother.Filter(true, 0.55F, 0.50F, kDt, 0.80F, 3.00F, u, v),
            "step sample");
    }
    Expect(u > 0.48F, "a 0.4 s swipe should mostly reach the new aim");
    ExpectNear(v, 0.50F, 0.01F, "V stays put during a U swipe");
}

void TestShortHarnessDragClearsClickThreshold() {
    PointerUvSmoother smoother;
    float u = 0.0F;
    float v = 0.0F;
    constexpr float kDt = 0.011111111F;
    Expect(smoother.Filter(true, 0.75F, 0.65F, 0.0F, 0.80F, 3.00F, u, v), "drag seed");
    for (int i = 1; i <= 8; ++i) {
        const float raw = 0.75F + (static_cast<float>(i) / 8.0F) * 0.125F;
        Expect(
            smoother.Filter(true, raw, 0.65F, kDt, 0.80F, 3.00F, u, v),
            "harness drag sample");
    }
    Expect(u - 0.75F > 0.04F, "a hover swipe should still move, not freeze");
}

void TestDiscontinuitySnaps() {
    PointerUvSmoother smoother;
    float u = 0.0F;
    float v = 0.0F;
    Expect(smoother.Filter(true, 0.75F, 0.65F, 0.0F, 0.80F, 3.00F, u, v), "before jump");
    Expect(
        smoother.Filter(true, 0.80F, 0.30F, 0.011111111F, 0.80F, 3.00F, u, v),
        "layout jump");
    ExpectNear(u, 0.80F, 1.0e-6F, "discontinuity snaps U");
    ExpectNear(v, 0.30F, 1.0e-6F, "discontinuity snaps V");
}

void TestLostHoverResets() {
    PointerUvSmoother smoother;
    float u = 0.0F;
    float v = 0.0F;
    Expect(smoother.Filter(true, 0.10F, 0.20F, 0.0F, 0.80F, 3.00F, u, v), "before lose");
    Expect(!smoother.Filter(false, 0.90F, 0.90F, 1.0F / 90.0F, 0.80F, 3.00F, u, v), "lost");
    Expect(smoother.Filter(true, 0.70F, 0.15F, 1.0F / 90.0F, 0.80F, 3.00F, u, v), "reacquire");
    ExpectNear(u, 0.70F, 1.0e-6F, "reacquire snaps U");
    ExpectNear(v, 0.15F, 1.0e-6F, "reacquire snaps V");
}

} // namespace

int main() {
    TestSnapAndHold();
    TestHighFrequencyTremorIsAttenuated();
    TestIntentionalStepFollows();
    TestShortHarnessDragClearsClickThreshold();
    TestDiscontinuitySnaps();
    TestLostHoverResets();
    return 0;
}
