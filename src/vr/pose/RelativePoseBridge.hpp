#pragma once

#include "StereoPoseMailbox.hpp"

#include <cstdint>

namespace gakumas::vr::pose {

enum class BridgeUpdateResult {
    Unavailable,
    BaselineLatched,
    Applied,
};

struct StereoComposedPose {
    Pose center{};
    std::array<Pose, 2> eyes{};
};

class RelativePoseBridge final {
public:
    void Reset() noexcept;
    [[nodiscard]] BridgeUpdateResult Update(
        const Pose& gameRequested,
        const StereoPoseSample& sample,
        std::int64_t nowNanoseconds,
        std::int64_t maximumAgeNanoseconds,
        float worldScale,
        Pose& composed) noexcept;
    [[nodiscard]] BridgeUpdateResult UpdateStereo(
        const Pose& gameRequested,
        const StereoPoseSample& sample,
        std::int64_t nowNanoseconds,
        std::int64_t maximumAgeNanoseconds,
        float worldScale,
        StereoComposedPose& composed) noexcept;

private:
    bool baselineReady_ = false;
    std::uint64_t baselinePoseEpoch_ = 0;
    std::uint64_t baselineSessionGeneration_ = 0;
    std::int32_t baselineReferenceSpaceType_ = 0;
    Pose baseline_{};
};

} // namespace gakumas::vr::pose
