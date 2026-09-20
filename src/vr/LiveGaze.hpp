#pragma once
#include <atomic>

namespace gakumas::vr {
inline std::atomic<bool> liveGazeRequested{false};
inline void SetLiveGazeRequested(bool value) noexcept { liveGazeRequested.store(value); }
// 0 stability, 1 accepted dev.474 standard, 2 gaze. Owner-thread menu/config
// publishes atomically; the running fade is never reset by a preference change.
inline std::atomic<int> liveGazePreset{1};
inline void SetLiveGazePreset(int value) noexcept { liveGazePreset.store(value>=0 && value<=2 ? value : 1); }
// 0 selected FOLLOW/focus idol, 1 every live actor. Owner-thread menu/config
// publishes atomically. All-character only applies while gaze is requested.
inline std::atomic<int> liveGazeScope{0};
inline void SetLiveGazeScope(int value) noexcept { liveGazeScope.store(value == 1 ? 1 : 0); }
inline bool LiveGazeWantsAllActors() noexcept {
    return liveGazeRequested.load() && liveGazeScope.load() == 1;
}
void TickLiveGaze() noexcept;
// Only ordinary-Live instances constructed by us are eligible. Panorama stays
// on its original update path. Called on Unity's owner thread.
bool RegisterRebuiltLiveGazeSmoothing(void* actor, void* effector) noexcept;
void ClearRebuiltLiveGazeSmoothing() noexcept;
void PublishNaturalGazeController(void* actor, void* effector, void* controller, bool requested) noexcept;
void TickNaturalLiveGaze(void* actor) noexcept;
void PublishLiveGazeCenter(float x, float y, float z) noexcept;
void InvalidateLiveGazeCenter() noexcept;
}
