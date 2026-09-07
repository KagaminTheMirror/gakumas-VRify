#pragma once

#include "pose/PoseMath.hpp"
#include "pose/StereoPoseMailbox.hpp"

namespace gakumas::vr {

// Publishes both controller poses for the mod-owned two-person Crowd draw.
// The legacy GameObject renderer stays hidden even if the Crowd draw fails,
// so failures remain visually distinct. Call after headsetPose_ is updated.
void TickHandGlowSticks(
    const pose::Pose& headsetPose,
    bool headsetValid,
    const pose::StereoPoseSample& trackingSample) noexcept;

// Advance both hands to the next unique official ColorTable color.
// No-op when the live palette has fewer than two uniques.
void CycleHandGlowStickColor() noexcept;

// Called after official MobAudiencePenlightController.UpdatePenlightParams
// so hand MaterialInfos receive the same per-camera write as nearby mobs.
void AfterOfficialPenlightCamera(void* controller) noexcept;

// Called after the real CrowdSystem.RenderCrowd. It records the current Live's
// mesh/material/buffer ABI in diagnostics mode, then records an independent
// two-person indirect draw using only mod-owned buffers.
void AfterOfficialCrowdRender(
    void* crowdSystem, void* commandBuffer, int eventType) noexcept;

} // namespace gakumas::vr
