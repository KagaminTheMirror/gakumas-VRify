#pragma once

namespace gakumas::vr {

void NoteLiveScenePresenter(void* instance) noexcept;
void NoteLiveSceneModel(void* instance) noexcept;
// Drops the cached presenter when the scene finalizes it (OnFinalize hook).
void ForgetLiveScenePresenter(void* instance) noexcept;
void ToggleLivePause() noexcept;
// The cached LiveScenePresenter while its Unity object is alive. Lifecycle:
// noted by the Start hook at scene entry, dropped by the OnFinalize hook (or
// the m_CachedPtr alive check once the scene object dies), so this covers the
// whole Live scene including loading. stereo.220 hardware: requiring a living
// `_live`/`_livePlayer` target here made the photo-scene flag rise seconds
// late (panel stayed un-centred until a B press rescanned) and flicker during
// loading; presence-only is the correct scene signal. Cheap, no scene scans;
// shared with the photo shutter's scene routing.
[[nodiscard]] void* AliveLiveScenePresenter() noexcept;
// Install the live-resolved AutoPhotoAsync state-machine lifecycle hook.
void InstallLiveAutoPhotoProtection() noexcept;
// Unity thread refreshes task-owner liveness. If the hook is unavailable,
// retains the conservative whole-Produce-Live protection as a logged fallback.
void RefreshLiveSourcePhotoProtection() noexcept;
// Atomic capture-window snapshot: full source, authored view and no hand sticks.
// Also safe for the OpenXR worker's Right-A routing; never changes saved options.
[[nodiscard]] bool LiveSourcePhotoProtectionActive() noexcept;
// Cached presenter + `_livePlayer` only. No FindObjects / scene scan.
[[nodiscard]] bool IsOfficialLivePaused() noexcept;

} // namespace gakumas::vr
