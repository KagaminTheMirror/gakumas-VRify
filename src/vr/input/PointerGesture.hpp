#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gakumas::vr::input {

// Radius is relative to the panel's short side, independent of desktop DPI.
class PointerGesture {
public:
    static int Radius(int width, int height) noexcept {
        return (std::max)(1, static_cast<int>(std::lround((std::min)(width, height) * 0.06)));
    }
    void Reset() noexcept { outsideSince_ = 0; maximum_ = 0; dragging_ = false; }
    bool Update(double dx, double dy, int radius, std::int64_t timeNs) noexcept {
        const double distance = std::hypot(dx, dy);
        maximum_ = (std::max)(maximum_, distance);
        if (dragging_) return true;
        if (distance < radius || timeNs <= 0) { outsideSince_ = 0; return false; }
        if (outsideSince_ == 0 || timeNs < outsideSince_) outsideSince_ = timeNs;
        dragging_ = distance >= radius * 2.0 || timeNs - outsideSince_ >= 80'000'000;
        return dragging_;
    }
    double MaximumDistance() const noexcept { return maximum_; }
private:
    std::int64_t outsideSince_ = 0;
    double maximum_ = 0;
    bool dragging_ = false;
};

} // namespace gakumas::vr::input
