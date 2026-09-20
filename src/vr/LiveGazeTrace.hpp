#pragma once
namespace gakumas::vr {
void RegisterLiveGazeTrace(void* actor, void* effector) noexcept;
void ClearLiveGazeTrace() noexcept;
void TraceLiveGazeCurve(void* effector, unsigned index, float value) noexcept;
void TraceLiveGazeStage(void* actor, void* effector, const char* stage, void* caller = nullptr, void* controller = nullptr) noexcept;
}
