#pragma once

#include <cstdint>

namespace gakumas::vr {

// Portrait fallback while immersive stereo is withheld. OpenXR reads this
// atomically from the frame worker; all writes stay on the Unity thread.
// Every LoadingManager rising edge opens a generic transition and parks the
// full-resolution eyes. Every official loading frame is captured while still
// being presented live; Hold is reserved for a proven incoming 3D wait:
// loading that started without a live source, or an epoch that later changed
// SceneManager identity. Once LoadingManager reports completion, the portrait
// mirror follows the live desktop while source/left/right boundaries prove
// that the destination can be published in stereo. A SceneManager
// identity flap by itself does not start an epoch: Idol-path / contest replaces
// scenes without LoadingManager and must not freeze the last portrait frame.
// Idle LoadingManager samples are throttled; an open epoch or a live
// loading-active edge still samples every call.
enum class PortraitLatchPolicy : std::uint8_t {
    FollowDesktop = 0,
    CaptureLoading = 1,
    HoldFrozen = 2,
};

// Plain-data, Unity-thread diagnostic state. Reading this never invokes a
// managed method; the stereo lifetime probe uses it only at transition edges.
struct SceneReadyDiagnosticState {
    std::uint64_t epoch = 0;
    std::uint64_t revokeSerial = 0;
    std::uint64_t releaseSerial = 0;
    bool transitionActive = false;
    bool contentReady = false;
    bool loadingKnown = false;
    bool loadingActive = false;
    bool stereoEligible = false;
    bool sourcePresent = false;
    bool epochSawIdentity = false;
    bool renderAllowed = true;
    bool publishAllowed = true;
};

void SampleSceneReady(
    const char* where,
    bool stereoEligible,
    bool sourcePresent) noexcept;
void NoteSceneReadyIdentityChanged(const char* where) noexcept;
void NoteSceneReadySourceCameraChanged() noexcept;
void NoteSceneReadyCameraBoundary(const char* role, bool begin) noexcept;

[[nodiscard]] bool SceneReadyAllowsStereoPublish() noexcept;
// Full-resolution eye cameras stay parked while the destination content is
// still loading. Once the official completion state is true, one source/eye
// sequence may render to prove that the new scene is publishable.
[[nodiscard]] bool SceneReadyAllowsStereoRender() noexcept;
// One-shot edge for work that must run after content completion but before the
// first visible stereo frame (for example late actor-material discovery).
[[nodiscard]] bool SceneReadyConsumeContentReadyEdge() noexcept;
[[nodiscard]] bool SceneReadyConsumePublishHoldLog() noexcept;
[[nodiscard]] PortraitLatchPolicy CurrentPortraitLatchPolicy() noexcept;
[[nodiscard]] SceneReadyDiagnosticState CurrentSceneReadyDiagnosticState() noexcept;

} // namespace gakumas::vr
