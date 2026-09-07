#pragma once

namespace gakumas::vr {

class UnityStereoRenderer;

void EnsureSkyRenderHooks(UnityStereoRenderer& renderer) noexcept;
void ObserveSmaaT2xRenderPass(
    void* renderPass,
    void* renderContext,
    void* renderingData,
    int renderPassEvent,
    UnityStereoRenderer& renderer) noexcept;
void NoteBeginEyeSky(void* camera) noexcept;
void ResetSkyTaaSamples() noexcept;
void LogTaaForensics(
    void* camera,
    void* additionalData,
    const char* role,
    bool suppressed) noexcept;

} // namespace gakumas::vr
