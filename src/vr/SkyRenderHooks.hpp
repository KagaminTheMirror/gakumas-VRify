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

} // namespace gakumas::vr
