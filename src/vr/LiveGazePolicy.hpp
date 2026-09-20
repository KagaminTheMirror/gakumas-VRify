#pragma once
#include <cmath>
#include <cstdint>
namespace gakumas::vr::gaze {
// A curve time step must not be consumed again by a second caller in the same
// Unity frame. A failed frame query passes through, rather than freezing gaze.
struct WeightFrame {
    int last = -1;
    bool Advance(int frame) noexcept {
        if (frame < 0) return true;
        if (last == frame) return false;
        last = frame;
        return true;
    }
};
struct Request {
    bool official = false;
    bool owned = false;
    bool Effective(bool vr) const noexcept { return vr || official; }
    void OfficialChanged(bool value) noexcept { official = value; }
};
inline bool UsableCenter(bool valid, bool sameScene, std::int64_t ageMs,
                         float x, float y, float z) noexcept {
    return valid && sameScene && ageMs >= 0 && ageMs <= 100 &&
        std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}
}
