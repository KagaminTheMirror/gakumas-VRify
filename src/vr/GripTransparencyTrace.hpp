#pragma once
#include <cstdint>
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace gakumas::vr {
class UnityStereoRenderer;
void InstallGripTransparencyTrace() noexcept;
void GripTraceBeginCamera(void* camera, const UnityStereoRenderer& renderer) noexcept;
void GripTraceEndCamera(void* camera) noexcept;
void GripTracePass(void* renderer, void* pass, void* context, int event, bool after) noexcept;
void GripTraceCanvasTexture(void* canvas, void* texture) noexcept;
void GripTraceFullscreen(void* material, int shaderPass) noexcept;
void GripTraceUiPolicy(void* pass, int index, bool drawFb, bool needsClear,
                       const float* clearColor) noexcept;
// Called only for the captured Unity swapchain; no Unity calls on this thread.
void GripTracePresent(ID3D11DeviceContext*, ID3D11Texture2D*) noexcept;
// Disk I/O and completion logging on the ordinary VR worker.
void PumpGripTraceOutput() noexcept;
} // namespace gakumas::vr
