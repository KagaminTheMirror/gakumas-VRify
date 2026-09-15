#include "TscmaaPass.hpp"

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

#pragma comment(lib, "d3dcompiler.lib")

namespace gakumas::vr::d3d11 {
namespace {

constexpr int kCmaa2HlslResource = 3101;
constexpr UINT kCmaa2InputKernelSize = 16U;
constexpr UINT kCmaa2OutputKernelSize = kCmaa2InputKernelSize - 2U;

// Temporal resolve is deliberately separate from the pinned CMAA2 source. It
// consumes CMAA2's packed edge nibbles so temporal work is restricted to edge
// candidates; since stereo.207 every candidate blends every frame (the
// stereo.206 checkerboard half-selection is removed).
constexpr char kFullscreenHlsl[] = R"TSCMAA(
Texture2D<float4> inputColor : register(t0);
Texture2D<float4> historyColor : register(t1);
Texture2D<float4> motionVectors : register(t2);
Texture2D<uint> workingEdges : register(t3);
Texture2D<float4> sourceColor : register(t4);
Texture2D<float4> resolvedColor : register(t5);
SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

cbuffer TscmaaConstants : register(b0) {
    float4 tscmaaMetrics;  // 1/w, 1/h, w, h
    uint4 tscmaaFrame;     // eye index, history valid, reserved, reserved
};

struct FullscreenOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

FullscreenOutput TscmaaFullscreenVS(uint vertexId : SV_VertexID) {
    FullscreenOutput output;
    output.texcoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(
        output.texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0),
        0.0, 1.0);
    return output;
}

float4 TscmaaCopyPS(FullscreenOutput input) : SV_Target {
    return inputColor.SampleLevel(PointSampler, input.texcoord, 0);
}

// Still pixels keep 0.7 through the inclusive 2px deadzone. Above that,
// only the official x30 UV attenuation remains: stereo.209 hardware saw
// no ghosting with the speed-cap removed, so the extra Hermite reject is
// gone. The 1.5-sigma variance clip is the other anti-ghost bound.
float TscmaaTemporalHistoryWeight(float2 motionUv) {
    const float speedPx = length(motionUv * tscmaaMetrics.zw);
    if (speedPx <= 2.0) return 0.7;
    return 0.7 * saturate(1.0 - length(motionUv) * 30.0);
}

float3 TscmaaRgbToYCoCg(float3 rgb) {
    return float3(
        dot(rgb, float3(0.25, 0.5, 0.25)),
        dot(rgb, float3(0.5, 0.0, -0.5)),
        dot(rgb, float3(-0.25, 0.5, -0.25)));
}

float3 TscmaaYCoCgToRgb(float3 value) {
    const float temp = value.x - value.z;
    return float3(temp + value.y, value.x + value.z, temp - value.y);
}

// Five-filter-tap Catmull-Rom reconstruction. The separable center row and
// column retain the four cubic side lobes while avoiding the four diagonal
// taps of the conventional optimized 9-tap form.
float4 TscmaaSampleHistoryCatmullRom5Tap(float2 uv) {
    const float2 samplePosition = uv * tscmaaMetrics.zw - 0.5;
    const float2 texelPosition1 = floor(samplePosition) + 0.5;
    const float2 f = frac(samplePosition);
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / max(abs(w12), 1.0e-5);
    const float2 p0 = texelPosition1 - 1.0;
    const float2 p3 = texelPosition1 + 2.0;
    const float2 p12 = texelPosition1 + offset12;

    const float centerWeight = w12.x * w12.y;
    const float leftWeight = w0.x * w12.y;
    const float rightWeight = w3.x * w12.y;
    const float topWeight = w12.x * w0.y;
    const float bottomWeight = w12.x * w3.y;
    const float totalWeight = centerWeight + leftWeight + rightWeight +
        topWeight + bottomWeight;
    float4 result =
        historyColor.SampleLevel(LinearSampler, p12 * tscmaaMetrics.xy, 0) *
            centerWeight;
    result += historyColor.SampleLevel(
        LinearSampler, float2(p0.x, p12.y) * tscmaaMetrics.xy, 0) * leftWeight;
    result += historyColor.SampleLevel(
        LinearSampler, float2(p3.x, p12.y) * tscmaaMetrics.xy, 0) * rightWeight;
    result += historyColor.SampleLevel(
        LinearSampler, float2(p12.x, p0.y) * tscmaaMetrics.xy, 0) * topWeight;
    result += historyColor.SampleLevel(
        LinearSampler, float2(p12.x, p3.y) * tscmaaMetrics.xy, 0) * bottomWeight;
    return result / max(abs(totalWeight), 1.0e-5);
}

float3 TscmaaVarianceClipYCoCg(float2 uv, float3 historyRgb) {
    const int2 center = int2(uv * tscmaaMetrics.zw);
    float3 firstMoment = 0.0;
    float3 secondMoment = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 pixel = clamp(
                center + int2(x, y), int2(0, 0),
                int2(tscmaaMetrics.zw) - 1);
            const float3 sampleValue = TscmaaRgbToYCoCg(
                inputColor.Load(int3(pixel, 0)).rgb);
            firstMoment += sampleValue;
            secondMoment += sampleValue * sampleValue;
        }
    }
    const float3 mean = firstMoment / 9.0;
    const float3 variance = max(secondMoment / 9.0 - mean * mean, 0.0);
    const float3 sigma = sqrt(variance);
    const float3 historyYCoCg = TscmaaRgbToYCoCg(historyRgb);
    return TscmaaYCoCgToRgb(clamp(
        historyYCoCg, mean - 1.5 * sigma, mean + 1.5 * sigma));
}

float4 TscmaaTemporalPS(FullscreenOutput input) : SV_Target {
    const int2 pixel = int2(input.position.xy);
    const float4 current = inputColor.Load(int3(pixel, 0));
    const float alpha = sourceColor.Load(int3(pixel, 0)).a;
    if (tscmaaFrame.y == 0) return float4(current.rgb, alpha);

    const uint packedEdges = workingEdges.Load(
        int3(uint2(uint(pixel.x) >> 1, uint(pixel.y)), 0));
    const uint edgeNibble =
        (packedEdges >> ((uint(pixel.x) & 1U) * 4U)) & 0x0fU;
    if (edgeNibble == 0U) return float4(current.rgb, alpha);

    const float2 motionUv = motionVectors.Load(int3(pixel, 0)).xy;
    if (any(isnan(motionUv)) || any(isinf(motionUv)))
        return float4(current.rgb, alpha);
    const float historyWeight = TscmaaTemporalHistoryWeight(motionUv);
    const float2 historyUv = input.texcoord - motionUv;
    if (historyWeight <= 0.0 || any(historyUv < 0.0) || any(historyUv > 1.0))
        return float4(current.rgb, alpha);

    const float3 history = TscmaaSampleHistoryCatmullRom5Tap(historyUv).rgb;
    const float3 clippedHistory =
        TscmaaVarianceClipYCoCg(input.texcoord, history);
    return float4(lerp(current.rgb, clippedHistory, historyWeight), alpha);
}

// Diagnostic-only mirror of the TscmaaTemporalPS admission gates. It counts,
// per eye, CMAA edge candidates, pixels whose history blend actually applied,
// the applied weight sum and the resolved-vs-current output delta so hardware
// logs can distinguish "correctly subtle" from "temporal work too small".
// Dispatched only on sparse diagnostic pairs; counters are 1/1000 fixed point.

)TSCMAA";

template <typename T>
void ReleaseObject(T*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

struct ResourceView {
    const std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
};

ResourceView LoadEmbeddedResource(int id) noexcept {
    HMODULE module = nullptr;
    const auto address = reinterpret_cast<LPCWSTR>(&LoadEmbeddedResource);
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            address, &module) ||
        module == nullptr) {
        return {};
    }
    HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (resource == nullptr) return {};
    HGLOBAL loaded = LoadResource(module, resource);
    const DWORD size = SizeofResource(module, resource);
    const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (bytes == nullptr || size == 0) return {};
    return {static_cast<const std::uint8_t*>(bytes), size};
}

int FormatFamily(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 2;
    default: return 0;
    }
}

DXGI_FORMAT TypelessFormat(int family) noexcept {
    return family == 1 ? DXGI_FORMAT_R8G8B8A8_TYPELESS
                       : family == 2 ? DXGI_FORMAT_B8G8R8A8_TYPELESS
                                     : DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT TypedFormat(int family, bool srgb) noexcept {
    if (family == 1) {
        return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                    : DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (family == 2) {
        return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
                    : DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT LinearFormat(int family) noexcept {
    return family == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM
                       : family == 2 ? DXGI_FORMAT_B8G8R8A8_UNORM
                                     : DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT MotionVectorViewFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

UINT MotionVectorBytesPerPixel(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16_FLOAT: return 4U;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8U;
    default: return 0U;
    }
}



std::uint64_t TextureBytes(
    UINT width, UINT height, UINT bytesPerPixel, UINT count = 1U) noexcept {
    return static_cast<std::uint64_t>(width) * height * bytesPerPixel * count;
}

} // namespace

TscmaaPass::~TscmaaPass() {
    Reset();
}

bool TscmaaPass::Prepare(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC& colorDescription,
    bool srgb,
    PrepareDiagnostics* diagnostics) noexcept {
    const auto started = std::chrono::steady_clock::now();
    if (device == nullptr || colorDescription.Width == 0U ||
        colorDescription.Height == 0U) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    const int family = FormatFamily(colorDescription.Format);
    if (family == 0 || colorDescription.MipLevels != 1U ||
        colorDescription.ArraySize != 1U || colorDescription.SampleDesc.Count != 1U) {
        SetFailure(Status::UnsupportedFormat, DXGI_ERROR_UNSUPPORTED);
        return false;
    }
    if (device_ != nullptr && device_ != device) Reset();
    if (device_ == nullptr) {
        device->AddRef();
        device_ = device;
    }
    const bool partialCore = deferredContext_ != nullptr &&
        (fullscreenVs_ == nullptr || copyPs_ == nullptr || temporalPs_ == nullptr ||
         rasterizerState_ == nullptr || depthStencilState_ == nullptr ||
         linearSampler_ == nullptr || pointSampler_ == nullptr || constants_ == nullptr);
    if (partialCore) {
        Reset();
        device->AddRef();
        device_ = device;
    }
    if (deferredContext_ == nullptr &&
        (!LoadAndCompileShaders() || !CreateFixedResources())) {
        return false;
    }
    if (!CreateSizeResources(colorDescription, srgb)) return false;
    lastStatus_ = Status::Ready;
    lastHresult_ = S_OK;
    if (diagnostics != nullptr) {
        diagnostics->allocationBytes = allocationBytes_;
        diagnostics->allocationMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
        diagnostics->width = colorDescription_.Width;
        diagnostics->height = colorDescription_.Height;
        diagnostics->resourceFormat = colorDescription_.Format;
        diagnostics->viewFormat = colorViewFormat_;
    }
    return true;
}

bool TscmaaPass::CaptureMotionVector(
    ID3D11DeviceContext* immediateContext,
    std::size_t eye,
    ID3D11Texture2D* source,
    std::uint64_t pairToken) noexcept {
    if (immediateContext == nullptr || source == nullptr ||
        eye >= motion_.size() || pairToken == 0U) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    ID3D11Device* sourceDevice = nullptr;
    source->GetDevice(&sourceDevice);
    if (sourceDevice == nullptr) {
        SetFailure(Status::InvalidArgument, E_NOINTERFACE);
        return false;
    }
    if (device_ != nullptr && device_ != sourceDevice) {
        sourceDevice->Release();
        SetFailure(Status::DescriptionMismatch, E_INVALIDARG);
        return false;
    }
    if (device_ == nullptr) {
        device_ = sourceDevice;
        sourceDevice = nullptr;
    }
    ReleaseObject(sourceDevice);
    if (!EnsureMotionSnapshot(eye, source)) return false;
    immediateContext->CopyResource(motion_[eye].texture, source);
    motion_[eye].pairToken = pairToken;
    lastStatus_ = Status::Ready;
    lastHresult_ = S_OK;
    return true;
}

bool TscmaaPass::HasFreshMotionVectors(std::uint64_t pairToken) const noexcept {
    return pairToken != 0U && motion_[0].texture != nullptr &&
           motion_[1].texture != nullptr && motion_[0].pairToken == pairToken &&
           motion_[1].pairToken == pairToken;
}



bool TscmaaPass::ResolveStereo(
    ID3D11DeviceContext* immediateContext,
    const std::array<ID3D11Texture2D*, 2>& colors,
    bool srgb,
    int quality,
    std::uint64_t pairToken,
    std::array<ID3D11Texture2D*, 2>& outputs) noexcept {
    outputs.fill(nullptr);
    if (!IsPrepared() || immediateContext == nullptr || colors[0] == nullptr ||
        colors[1] == nullptr || quality < 0 || quality > 2) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    if (!HasFreshMotionVectors(pairToken)) {
        SetFailure(Status::StaleMotionVectors, E_PENDING);
        return false;
    }
    for (std::size_t eye = 0; eye < colors.size(); ++eye) {
        D3D11_TEXTURE2D_DESC description{};
        colors[eye]->GetDesc(&description);
        if (description.Width != colorDescription_.Width ||
            description.Height != colorDescription_.Height ||
            FormatFamily(description.Format) != FormatFamily(colorDescription_.Format) ||
            description.MipLevels != 1U || description.ArraySize != 1U ||
            description.SampleDesc.Count != 1U || srgb != srgb_ ||
            TypedFormat(FormatFamily(description.Format), srgb) != colorViewFormat_ ||
            !EnsureColorSourceView(eye, colors[eye])) {
            SetFailure(Status::DescriptionMismatch, E_INVALIDARG);
            return false;
        }
    }

    const auto originalNextIndices = nextResolvedWriteIndex_;
    const auto originalOutputIndices = lastResolvedOutputIndex_;
    const auto originalHistory = historyValid_;
    deferredContext_->ClearState();
    BindFullscreenState();

    for (std::size_t eye = 0; eye < colors.size(); ++eye) {
        if (!RecordEye(eye, quality)) {
            nextResolvedWriteIndex_ = originalNextIndices;
            lastResolvedOutputIndex_ = originalOutputIndices;
            historyValid_ = originalHistory;
            deferredContext_->ClearState();
            return false;
        }
    }
    deferredContext_->OMSetRenderTargets(0, nullptr, nullptr);
    UnbindGraphicsResources();
    UnbindComputeResources();
    ID3D11CommandList* commandList = nullptr;
    const HRESULT result = deferredContext_->FinishCommandList(FALSE, &commandList);
    if (FAILED(result) || commandList == nullptr) {
        ReleaseObject(commandList);
        nextResolvedWriteIndex_ = originalNextIndices;
        lastResolvedOutputIndex_ = originalOutputIndices;
        historyValid_ = originalHistory;
        SetFailure(Status::CommandRecordingFailed, result);
        return false;
    }
    immediateContext->ExecuteCommandList(commandList, TRUE);
    commandList->Release();
    for (std::size_t eye = 0; eye < outputs.size(); ++eye) {
        outputs[eye] = resolved_[eye][lastResolvedOutputIndex_[eye]].texture;
    }

    lastStatus_ = Status::Ready;
    lastHresult_ = S_OK;
    return true;
}

float TscmaaPass::TemporalHistoryWeight(
    float motionXUv,
    float motionYUv,
    UINT width,
    UINT height) noexcept {
    if (width == 0U || height == 0U || !std::isfinite(motionXUv) ||
        !std::isfinite(motionYUv)) {
        return 0.0F;
    }
    const float speedPixels = std::hypot(
        motionXUv * static_cast<float>(width),
        motionYUv * static_cast<float>(height));
    if (speedPixels <= 2.0F) return 0.7F;
    const float motionUv = std::hypot(motionXUv, motionYUv);
    return 0.7F * std::clamp(1.0F - motionUv * 30.0F, 0.0F, 1.0F);
}

void TscmaaPass::ResetHistory() noexcept {
    historyValid_.fill(false);
    nextResolvedWriteIndex_.fill(0U);
    lastResolvedOutputIndex_.fill(0U);
    for (auto& snapshot : motion_) snapshot.pairToken = 0U;
}

void TscmaaPass::Reset() noexcept {
    ReleaseSizeResources();
    ReleaseObject(constants_);
    ReleaseObject(pointSampler_);
    ReleaseObject(linearSampler_);
    ReleaseObject(depthStencilState_);
    ReleaseObject(rasterizerState_);
    ReleaseShaders();
    ReleaseObject(deferredContext_);
    ReleaseObject(device_);
    colorDescription_ = {};
    colorViewFormat_ = DXGI_FORMAT_UNKNOWN;
    colorLinearFormat_ = DXGI_FORMAT_UNKNOWN;
    srgb_ = false;
    allocationBytes_ = 0U;
    lastCompilerError_.clear();
    lastStatus_ = Status::NotInitialized;
    lastHresult_ = S_OK;
    lastMotionVectorDiagnostics_ = {};
}

bool TscmaaPass::IsPrepared() const noexcept {
    const bool allSpatial = std::all_of(
        spatial_.begin(), spatial_.end(), [](const ColorTarget& target) {
            return target.texture != nullptr && target.srv != nullptr &&
                   target.rtv != nullptr && target.uav != nullptr;
        });
    const bool allResolved = std::all_of(
        resolved_.begin(), resolved_.end(), [](const auto& eyeTargets) {
            return std::all_of(
                eyeTargets.begin(), eyeTargets.end(), [](const ColorTarget& target) {
                    return target.texture != nullptr && target.srv != nullptr &&
                           target.rtv != nullptr;
                });
        });
    return device_ != nullptr && deferredContext_ != nullptr &&
           fullscreenVs_ != nullptr && copyPs_ != nullptr && temporalPs_ != nullptr &&
           rasterizerState_ != nullptr && depthStencilState_ != nullptr &&
           linearSampler_ != nullptr && pointSampler_ != nullptr && constants_ != nullptr &&
           workingEdges_ != nullptr && workingEdgesSrv_ != nullptr &&
           workingEdgesUav_ != nullptr && workingDeferredHeads_ != nullptr &&
           workingDeferredHeadsUav_ != nullptr &&
           workingShapeCandidates_.uav != nullptr &&
           workingDeferredLocations_.uav != nullptr &&
           workingDeferredItems_.uav != nullptr && workingControl_.uav != nullptr &&
           workingIndirect_.uav != nullptr && allSpatial && allResolved;
}

bool TscmaaPass::HasHistory() const noexcept {
    return historyValid_[0] && historyValid_[1];
}

TscmaaPass::Status TscmaaPass::LastStatus() const noexcept {
    return lastStatus_;
}

HRESULT TscmaaPass::LastHresult() const noexcept {
    return lastHresult_;
}

TscmaaPass::MotionVectorDiagnostics
TscmaaPass::LastMotionVectorDiagnostics() const noexcept {
    return lastMotionVectorDiagnostics_;
}

const std::string& TscmaaPass::LastCompilerError() const noexcept {
    return lastCompilerError_;
}

std::uint64_t TscmaaPass::AllocationBytes() const noexcept {
    return allocationBytes_;
}

TscmaaPass::ResourceDiagnostics TscmaaPass::GetResourceDiagnostics() const noexcept {
    ResourceDiagnostics diagnostics{};
    diagnostics.workingEdges =
        reinterpret_cast<std::uintptr_t>(workingEdges_);
    diagnostics.workingShapeCandidates =
        reinterpret_cast<std::uintptr_t>(workingShapeCandidates_.buffer);
    diagnostics.workingDeferredItems =
        reinterpret_cast<std::uintptr_t>(workingDeferredItems_.buffer);
    for (std::size_t eye = 0; eye < diagnostics.eyes.size(); ++eye) {
        auto& output = diagnostics.eyes[eye];
        output.motion = reinterpret_cast<std::uintptr_t>(motion_[eye].texture);
        output.spatial = reinterpret_cast<std::uintptr_t>(spatial_[eye].texture);
        output.resolvedHistory = historyValid_[eye]
            ? reinterpret_cast<std::uintptr_t>(
                  resolved_[eye][nextResolvedWriteIndex_[eye] ^ 1U].texture)
            : 0U;
        output.resolvedOutput = historyValid_[eye]
            ? reinterpret_cast<std::uintptr_t>(
                  resolved_[eye][lastResolvedOutputIndex_[eye]].texture)
            : 0U;
        output.motionPairToken = motion_[eye].pairToken;
        output.nextResolvedWriteIndex = nextResolvedWriteIndex_[eye];
        output.historyValid = historyValid_[eye];
    }
    return diagnostics;
}

const char* TscmaaPass::StatusName(Status status) noexcept {
    switch (status) {
    case Status::Ready: return "ready";
    case Status::NotInitialized: return "not-initialized";
    case Status::InvalidArgument: return "invalid-argument";
    case Status::DescriptionMismatch: return "description-mismatch";
    case Status::UnsupportedFormat: return "unsupported-format";
    case Status::UnsupportedMotionVectors: return "unsupported-motion-vectors";
    case Status::EmbeddedResourceMissing: return "embedded-resource-missing";
    case Status::ShaderCompilationFailed: return "shader-compilation-failed";
    case Status::DeviceResourceFailed: return "device-resource-failed";
    case Status::ViewCreationFailed: return "view-creation-failed";
    case Status::StaleMotionVectors: return "stale-motion-vectors";
    case Status::CommandRecordingFailed: return "command-recording-failed";
    }
    return "unknown";
}

bool TscmaaPass::LoadAndCompileShaders() noexcept {
    // A failed compile can leave a prefix of the variant table populated.
    // Retrying must release that prefix before replacing its COM pointers.
    ReleaseShaders();
    const ResourceView resource = LoadEmbeddedResource(kCmaa2HlslResource);
    if (resource.bytes == nullptr || resource.size == 0U) {
        SetFailure(Status::EmbeddedResourceMissing,
                   HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND));
        return false;
    }
    const std::string source(
        reinterpret_cast<const char*>(resource.bytes), resource.size);
    for (int quality = 0; quality < 3; ++quality) {
        for (int srgb = 0; srgb < 2; ++srgb) {
            if (!CompileVariant(quality, srgb != 0, source)) {
                ReleaseShaders();
                return false;
            }
        }
    }
    if (!CompileFullscreenShaders()) {
        ReleaseShaders();
        return false;
    }

    return true;
}

bool TscmaaPass::CompileVariant(
    int quality,
    bool srgb,
    const std::string& source) noexcept {
    // The UI's 0/1/2 quality scale deliberately maps to CMAA2's medium/high/
    // ultra presets 1/2/3. The high UI default therefore uses .05, close to
    // Intel TSCMAA's documented 1/22 edge threshold.
    std::ostringstream prefix;
    prefix << "#define CMAA2_STATIC_QUALITY_PRESET " << (quality + 1) << "\n"
           << "#define CMAA2_EXTRA_SHARPNESS 0\n"
           << "#define CMAA2_UAV_STORE_TYPED 1\n"
           << "#define CMAA2_UAV_STORE_TYPED_UNORM_FLOAT 1\n"
           << "#define CMAA2_UAV_STORE_CONVERT_TO_SRGB " << (srgb ? 1 : 0)
           << "\n#define CMAA2_SUPPORT_HDR_COLOR_RANGE 0\n"
           << "#define CMAA2_USE_HALF_FLOAT_PRECISION 0\n";
    const std::string compiledSource = prefix.str() + source;
    auto& variant = shaders_[static_cast<std::size_t>(quality)][srgb ? 1U : 0U];
    return CompileComputeShader(compiledSource, "EdgesColor2x2CS", &variant.edges) &&
           CompileComputeShader(
               compiledSource, "ComputeDispatchArgsCS",
               &variant.computeDispatchArgs) &&
           CompileComputeShader(
               compiledSource, "ProcessCandidatesCS",
               &variant.processCandidates) &&
           CompileComputeShader(
               compiledSource, "DeferredColorApply2x2CS",
               &variant.deferredApply);
}

bool TscmaaPass::CompileComputeShader(
    const std::string& source,
    const char* entry,
    ID3D11ComputeShader** shader) noexcept {
    if (shader == nullptr || entry == nullptr) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    *shader = nullptr;
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT compileResult = D3DCompile(
        source.data(), source.size(), "embedded/CMAA2.hlsl", nullptr, nullptr,
        entry, "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &blob, &errors);
    if (errors != nullptr) {
        lastCompilerError_.assign(
            static_cast<const char*>(errors->GetBufferPointer()),
            errors->GetBufferSize());
        errors->Release();
    }
    if (FAILED(compileResult) || blob == nullptr) {
        ReleaseObject(blob);
        SetFailure(Status::ShaderCompilationFailed, compileResult);
        return false;
    }
    const HRESULT createResult = device_->CreateComputeShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader);
    blob->Release();
    if (FAILED(createResult) || *shader == nullptr) {
        SetFailure(Status::ShaderCompilationFailed, createResult);
        return false;
    }
    return true;
}

bool TscmaaPass::CompileFullscreenShaders() noexcept {
    struct ShaderRequest {
        const char* entry;
        const char* profile;
        bool vertex;
        void** output;
    };
    const std::array<ShaderRequest, 3> requests{{
        {"TscmaaFullscreenVS", "vs_4_0", true,
         reinterpret_cast<void**>(&fullscreenVs_)},
        {"TscmaaCopyPS", "ps_4_0", false,
         reinterpret_cast<void**>(&copyPs_)},
        {"TscmaaTemporalPS", "ps_4_0", false,
         reinterpret_cast<void**>(&temporalPs_)},
    }};
    for (const auto& request : requests) {
        ID3DBlob* blob = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT compileResult = D3DCompile(
            kFullscreenHlsl, std::strlen(kFullscreenHlsl),
            "embedded/TSCMAAResolve.hlsl", nullptr, nullptr,
            request.entry, request.profile,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &blob, &errors);
        if (errors != nullptr) {
            lastCompilerError_.assign(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
            errors->Release();
        }
        if (FAILED(compileResult) || blob == nullptr) {
            ReleaseObject(blob);
            SetFailure(Status::ShaderCompilationFailed, compileResult);
            return false;
        }
        HRESULT createResult = request.vertex
            ? device_->CreateVertexShader(
                  blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                  reinterpret_cast<ID3D11VertexShader**>(request.output))
            : device_->CreatePixelShader(
                  blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                  reinterpret_cast<ID3D11PixelShader**>(request.output));
        blob->Release();
        if (FAILED(createResult) || *request.output == nullptr) {
            SetFailure(Status::ShaderCompilationFailed, createResult);
            return false;
        }
    }
    return true;
}

bool TscmaaPass::CreateFixedResources() noexcept {
    HRESULT result = device_->CreateDeferredContext(0, &deferredContext_);
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    if (SUCCEEDED(result)) {
        result = device_->CreateRasterizerState(&rasterizer, &rasterizerState_);
    }
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;
    if (SUCCEEDED(result)) {
        result = device_->CreateDepthStencilState(&depth, &depthStencilState_);
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(result)) {
        result = device_->CreateSamplerState(&sampler, &linearSampler_);
    }
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (SUCCEEDED(result)) {
        result = device_->CreateSamplerState(&sampler, &pointSampler_);
    }
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = 32U;
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result)) {
        result = device_->CreateBuffer(&constants, nullptr, &constants_);
    }
    if (FAILED(result) || deferredContext_ == nullptr ||
        rasterizerState_ == nullptr || depthStencilState_ == nullptr ||
        linearSampler_ == nullptr || pointSampler_ == nullptr || constants_ == nullptr) {
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    return true;
}

bool TscmaaPass::CreateSizeResources(
    const D3D11_TEXTURE2D_DESC& colorDescription,
    bool srgb) noexcept {
    const int family = FormatFamily(colorDescription.Format);
    const DXGI_FORMAT viewFormat = TypedFormat(family, srgb);
    const DXGI_FORMAT linearFormat = LinearFormat(family);
    if (workingEdges_ != nullptr &&
        colorDescription_.Width == colorDescription.Width &&
        colorDescription_.Height == colorDescription.Height &&
        FormatFamily(colorDescription_.Format) == family &&
        colorViewFormat_ == viewFormat && colorLinearFormat_ == linearFormat &&
        srgb_ == srgb) {
        return true;
    }

    ReleaseSizeResources();
    colorDescription_ = colorDescription;
    colorDescription_.Format = TypelessFormat(family);
    colorDescription_.Usage = D3D11_USAGE_DEFAULT;
    colorDescription_.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    colorDescription_.CPUAccessFlags = 0U;
    colorDescription_.MiscFlags = 0U;
    colorViewFormat_ = viewFormat;
    colorLinearFormat_ = linearFormat;
    srgb_ = srgb;

    const UINT width = colorDescription_.Width;
    const UINT height = colorDescription_.Height;
    const UINT halfWidth = (width + 1U) / 2U;
    const UINT halfHeight = (height + 1U) / 2U;
    if (!CreateWorkingTexture(
            halfWidth, height, DXGI_FORMAT_R8_UINT, &workingEdges_,
            &workingEdgesSrv_, &workingEdgesUav_) ||
        !CreateWorkingTexture(
            halfWidth, halfHeight, DXGI_FORMAT_R32_UINT,
            &workingDeferredHeads_, nullptr, &workingDeferredHeadsUav_)) {
        ReleaseSizeResources();
        return false;
    }

    const std::uint64_t pixelCount64 =
        static_cast<std::uint64_t>(width) * height;
    if (pixelCount64 > static_cast<std::uint64_t>(UINT_MAX)) {
        ReleaseSizeResources();
        SetFailure(Status::UnsupportedFormat, DXGI_ERROR_UNSUPPORTED);
        return false;
    }
    const UINT pixelCount = static_cast<UINT>(pixelCount64);
    const UINT candidateCount = (std::max)(1U, pixelCount / 4U);
    const UINT deferredItemCount = (std::max)(1U, pixelCount / 2U);
    const UINT deferredLocationCount =
        (std::max)(1U, (pixelCount + 3U) / 6U);
    std::array<UINT, 16> zeroControl{};
    if (!CreateStructuredBuffer(
            candidateCount, sizeof(UINT), workingShapeCandidates_) ||
        !CreateStructuredBuffer(
            deferredLocationCount, sizeof(UINT), workingDeferredLocations_) ||
        !CreateStructuredBuffer(
            deferredItemCount, sizeof(UINT) * 2U, workingDeferredItems_) ||
        !CreateRawBuffer(
            sizeof(UINT) * 16U, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,
            zeroControl.data(), workingControl_) ||
        !CreateRawBuffer(
            sizeof(UINT) * 4U,
            D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS |
                D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS,
            nullptr, workingIndirect_)) {
        ReleaseSizeResources();
        return false;
    }

    for (auto& target : spatial_) {
        if (!CreateColorTarget(target, true)) {
            ReleaseSizeResources();
            return false;
        }
    }
    for (auto& eyeTargets : resolved_) {
        for (auto& target : eyeTargets) {
            if (!CreateColorTarget(target, false)) {
                ReleaseSizeResources();
                return false;
            }
        }
    }
    UpdateAllocationBytes();
    ResetHistory();
    return true;
}

bool TscmaaPass::CreateColorTarget(
    ColorTarget& target,
    bool unorderedAccess) noexcept {
    D3D11_TEXTURE2D_DESC description = colorDescription_;
    description.BindFlags =
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET |
        (unorderedAccess ? D3D11_BIND_UNORDERED_ACCESS : 0U);
    HRESULT result = device_->CreateTexture2D(
        &description, nullptr, &target.texture);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = colorViewFormat_;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1U;
    if (SUCCEEDED(result)) {
        result = device_->CreateShaderResourceView(
            target.texture, &srv, &target.srv);
    }
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = colorViewFormat_;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(result)) {
        result = device_->CreateRenderTargetView(
            target.texture, &rtv, &target.rtv);
    }
    if (SUCCEEDED(result) && unorderedAccess) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = colorLinearFormat_;
        uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        result = device_->CreateUnorderedAccessView(
            target.texture, &uav, &target.uav);
    }
    if (FAILED(result) || target.texture == nullptr || target.srv == nullptr ||
        target.rtv == nullptr || (unorderedAccess && target.uav == nullptr)) {
        ReleaseObject(target.uav);
        ReleaseObject(target.rtv);
        ReleaseObject(target.srv);
        ReleaseObject(target.texture);
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    return true;
}

bool TscmaaPass::CreateWorkingTexture(
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    ID3D11Texture2D** texture,
    ID3D11ShaderResourceView** srv,
    ID3D11UnorderedAccessView** uav) noexcept {
    if (texture == nullptr || uav == nullptr) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1U;
    description.ArraySize = 1U;
    description.Format = format;
    description.SampleDesc.Count = 1U;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS |
        (srv != nullptr ? D3D11_BIND_SHADER_RESOURCE : 0U);
    HRESULT result = device_->CreateTexture2D(
        &description, nullptr, texture);
    if (SUCCEEDED(result) && srv != nullptr) {
        result = device_->CreateShaderResourceView(*texture, nullptr, srv);
    }
    if (SUCCEEDED(result)) {
        result = device_->CreateUnorderedAccessView(*texture, nullptr, uav);
    }
    if (FAILED(result) || *texture == nullptr || *uav == nullptr ||
        (srv != nullptr && *srv == nullptr)) {
        if (srv != nullptr) ReleaseObject(*srv);
        ReleaseObject(*uav);
        ReleaseObject(*texture);
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    return true;
}

bool TscmaaPass::CreateStructuredBuffer(
    UINT elementCount,
    UINT stride,
    BufferTarget& target) noexcept {
    if (elementCount == 0U || stride == 0U ||
        static_cast<std::uint64_t>(elementCount) * stride > UINT_MAX) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = elementCount * stride;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = stride;
    HRESULT result = device_->CreateBuffer(
        &description, nullptr, &target.buffer);
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = elementCount;
    if (SUCCEEDED(result)) {
        result = device_->CreateUnorderedAccessView(
            target.buffer, &view, &target.uav);
    }
    if (FAILED(result) || target.buffer == nullptr || target.uav == nullptr) {
        ReleaseObject(target.uav);
        ReleaseObject(target.buffer);
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    target.byteWidth = description.ByteWidth;
    return true;
}

bool TscmaaPass::CreateRawBuffer(
    UINT byteWidth,
    UINT miscFlags,
    const void* initialData,
    BufferTarget& target) noexcept {
    if (byteWidth == 0U || byteWidth % 16U != 0U) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = byteWidth;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    description.MiscFlags = miscFlags;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = initialData;
    HRESULT result = device_->CreateBuffer(
        &description, initialData != nullptr ? &data : nullptr, &target.buffer);
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    view.Buffer.NumElements = byteWidth / sizeof(UINT);
    view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (SUCCEEDED(result)) {
        result = device_->CreateUnorderedAccessView(
            target.buffer, &view, &target.uav);
    }
    if (FAILED(result) || target.buffer == nullptr || target.uav == nullptr) {
        ReleaseObject(target.uav);
        ReleaseObject(target.buffer);
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    target.byteWidth = description.ByteWidth;
    return true;
}

bool TscmaaPass::EnsureColorSourceView(
    std::size_t eye,
    ID3D11Texture2D* source) noexcept {
    if (colorSourceIdentity_[eye] == source && colorSourceSrv_[eye] != nullptr) {
        return true;
    }
    ReleaseObject(colorSourceSrv_[eye]);
    colorSourceIdentity_[eye] = nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = colorViewFormat_;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1U;
    const HRESULT result = device_->CreateShaderResourceView(
        source, &view, &colorSourceSrv_[eye]);
    if (FAILED(result) || colorSourceSrv_[eye] == nullptr) {
        SetFailure(Status::ViewCreationFailed, result);
        return false;
    }
    colorSourceIdentity_[eye] = source;
    return true;
}

bool TscmaaPass::EnsureMotionSnapshot(
    std::size_t eye,
    ID3D11Texture2D* source) noexcept {
    D3D11_TEXTURE2D_DESC sourceDescription{};
    source->GetDesc(&sourceDescription);
    lastMotionVectorDiagnostics_ = {};
    lastMotionVectorDiagnostics_.source = sourceDescription;
    lastMotionVectorDiagnostics_.expectedWidth = colorDescription_.Width;
    lastMotionVectorDiagnostics_.expectedHeight = colorDescription_.Height;
    const DXGI_FORMAT viewFormat = MotionVectorViewFormat(sourceDescription.Format);
    const UINT bytesPerPixel = MotionVectorBytesPerPixel(sourceDescription.Format);
    lastMotionVectorDiagnostics_.viewFormat = viewFormat;
    lastMotionVectorDiagnostics_.bytesPerPixel = bytesPerPixel;
    if (viewFormat == DXGI_FORMAT_UNKNOWN || bytesPerPixel == 0U) {
        lastMotionVectorDiagnostics_.mismatchMask |= MotionVectorMismatchFormat;
    }
    if (sourceDescription.MipLevels != 1U) {
        lastMotionVectorDiagnostics_.mismatchMask |= MotionVectorMismatchMipLevels;
    }
    if (sourceDescription.ArraySize != 1U) {
        lastMotionVectorDiagnostics_.mismatchMask |= MotionVectorMismatchArraySize;
    }
    if (sourceDescription.SampleDesc.Count != 1U) {
        lastMotionVectorDiagnostics_.mismatchMask |= MotionVectorMismatchSampleCount;
    }
    if (colorDescription_.Width != 0U &&
        (sourceDescription.Width != colorDescription_.Width ||
         sourceDescription.Height != colorDescription_.Height)) {
        lastMotionVectorDiagnostics_.mismatchMask |= MotionVectorMismatchDimensions;
    }
    if (lastMotionVectorDiagnostics_.mismatchMask != MotionVectorMismatchNone) {
        SetFailure(Status::UnsupportedMotionVectors, DXGI_ERROR_UNSUPPORTED);
        return false;
    }

    auto& snapshot = motion_[eye];
    if (snapshot.texture != nullptr) {
        D3D11_TEXTURE2D_DESC existing{};
        snapshot.texture->GetDesc(&existing);
        if (existing.Width == sourceDescription.Width &&
            existing.Height == sourceDescription.Height &&
            existing.Format == sourceDescription.Format) {
            motionBytesPerPixel_[eye] = bytesPerPixel;
            UpdateAllocationBytes();
            return true;
        }
        ReleaseObject(snapshot.srv);
        ReleaseObject(snapshot.texture);
        snapshot.pairToken = 0U;
    }
    sourceDescription.Usage = D3D11_USAGE_DEFAULT;
    sourceDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sourceDescription.CPUAccessFlags = 0U;
    sourceDescription.MiscFlags = 0U;
    HRESULT result = device_->CreateTexture2D(
        &sourceDescription, nullptr, &snapshot.texture);
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = viewFormat;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1U;
    if (SUCCEEDED(result)) {
        result = device_->CreateShaderResourceView(
            snapshot.texture, &view, &snapshot.srv);
    }
    if (FAILED(result) || snapshot.texture == nullptr || snapshot.srv == nullptr) {
        ReleaseObject(snapshot.srv);
        ReleaseObject(snapshot.texture);
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    motionBytesPerPixel_[eye] = bytesPerPixel;
    UpdateAllocationBytes();
    return true;
}

void TscmaaPass::UpdateAllocationBytes() noexcept {
    if (colorDescription_.Width == 0U || colorDescription_.Height == 0U) {
        allocationBytes_ = 0U;
        return;
    }
    const UINT width = colorDescription_.Width;
    const UINT height = colorDescription_.Height;
    const UINT halfWidth = (width + 1U) / 2U;
    const UINT halfHeight = (height + 1U) / 2U;
    allocationBytes_ =
        TextureBytes(width, height, 4U, 6U) +
        TextureBytes(halfWidth, height, 1U) +
        TextureBytes(halfWidth, halfHeight, 4U) +
        workingShapeCandidates_.byteWidth +
        workingDeferredLocations_.byteWidth +
        workingDeferredItems_.byteWidth +
        workingControl_.byteWidth + workingIndirect_.byteWidth +
        TextureBytes(
            width, height, motionBytesPerPixel_[0] + motionBytesPerPixel_[1]);
}

bool TscmaaPass::RecordEye(
    std::size_t eye,
    int quality) noexcept {
    auto& spatial = spatial_[eye];

    // Unity color surfaces are never assumed to expose UAV_BIND. Copy through
    // a typed RTV into the owned typeless SRV/RTV/UAV surface first.
    deferredContext_->VSSetShader(fullscreenVs_, nullptr, 0);
    deferredContext_->PSSetShader(copyPs_, nullptr, 0);
    deferredContext_->OMSetRenderTargets(1, &spatial.rtv, nullptr);
    deferredContext_->PSSetShaderResources(0, 1, &colorSourceSrv_[eye]);
    deferredContext_->Draw(3, 0);
    UnbindGraphicsResources();

    auto& shader = shaders_[static_cast<std::size_t>(quality)][srgb_ ? 1U : 0U];
    const UINT zeros[4]{};
    deferredContext_->ClearUnorderedAccessViewUint(workingEdgesUav_, zeros);
    std::array<ID3D11UnorderedAccessView*, 8> uavs{
        nullptr,
        workingEdgesUav_,
        workingShapeCandidates_.uav,
        workingDeferredLocations_.uav,
        workingDeferredItems_.uav,
        workingDeferredHeadsUav_,
        workingControl_.uav,
        workingIndirect_.uav,
    };
    std::array<ID3D11ShaderResourceView*, 4> srvs{
        spatial.srv, nullptr, nullptr, nullptr};
    deferredContext_->CSSetSamplers(0, 1, &pointSampler_);
    deferredContext_->CSSetUnorderedAccessViews(
        0, static_cast<UINT>(uavs.size()), uavs.data(), nullptr);
    deferredContext_->CSSetShaderResources(
        0, static_cast<UINT>(srvs.size()), srvs.data());

    const UINT groupCountX =
        (colorDescription_.Width + kCmaa2OutputKernelSize * 2U - 1U) /
        (kCmaa2OutputKernelSize * 2U);
    const UINT groupCountY =
        (colorDescription_.Height + kCmaa2OutputKernelSize * 2U - 1U) /
        (kCmaa2OutputKernelSize * 2U);
    deferredContext_->CSSetShader(shader.edges, nullptr, 0);
    deferredContext_->Dispatch(groupCountX, groupCountY, 1U);

    deferredContext_->CSSetShader(shader.computeDispatchArgs, nullptr, 0);
    deferredContext_->Dispatch(2U, 1U, 1U);
    deferredContext_->CSSetShader(shader.processCandidates, nullptr, 0);
    deferredContext_->DispatchIndirect(workingIndirect_.buffer, 0U);

    deferredContext_->CSSetShader(shader.computeDispatchArgs, nullptr, 0);
    deferredContext_->Dispatch(1U, 2U, 1U);
    srvs[0] = nullptr;
    deferredContext_->CSSetShaderResources(
        0, static_cast<UINT>(srvs.size()), srvs.data());
    uavs[0] = spatial.uav;
    deferredContext_->CSSetUnorderedAccessViews(
        0, static_cast<UINT>(uavs.size()), uavs.data(), nullptr);
    deferredContext_->CSSetShader(shader.deferredApply, nullptr, 0);
    deferredContext_->DispatchIndirect(workingIndirect_.buffer, 0U);
    UnbindComputeResources();

    struct Constants {
        float metrics[4];
        UINT frame[4];
    } constants{{
                    1.0F / static_cast<float>(colorDescription_.Width),
                    1.0F / static_cast<float>(colorDescription_.Height),
                    static_cast<float>(colorDescription_.Width),
                    static_cast<float>(colorDescription_.Height),
                },
                {
                    static_cast<UINT>(eye),
                    historyValid_[eye] ? 1U : 0U,
                    0U,
                    0U,
                }};
    deferredContext_->UpdateSubresource(constants_, 0, nullptr, &constants, 0, 0);
    const std::uint32_t writeIndex = nextResolvedWriteIndex_[eye];
    const std::uint32_t historyIndex = writeIndex ^ 1U;
    auto& output = resolved_[eye][writeIndex];
    deferredContext_->VSSetShader(fullscreenVs_, nullptr, 0);
    deferredContext_->PSSetShader(temporalPs_, nullptr, 0);
    deferredContext_->OMSetRenderTargets(1, &output.rtv, nullptr);
    const std::array<ID3D11ShaderResourceView*, 5> temporalInputs{
        spatial.srv,
        historyValid_[eye] ? resolved_[eye][historyIndex].srv : nullptr,
        motion_[eye].srv,
        workingEdgesSrv_,
        colorSourceSrv_[eye],
    };
    deferredContext_->PSSetShaderResources(
        0, static_cast<UINT>(temporalInputs.size()), temporalInputs.data());
    deferredContext_->Draw(3, 0);
    UnbindGraphicsResources();



    lastResolvedOutputIndex_[eye] = writeIndex;
    nextResolvedWriteIndex_[eye] = writeIndex ^ 1U;
    historyValid_[eye] = true;
    return true;
}

void TscmaaPass::BindFullscreenState() noexcept {
    deferredContext_->IASetInputLayout(nullptr);
    deferredContext_->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferredContext_->RSSetState(rasterizerState_);
    deferredContext_->OMSetDepthStencilState(depthStencilState_, 0U);
    deferredContext_->VSSetConstantBuffers(0, 1, &constants_);
    deferredContext_->PSSetConstantBuffers(0, 1, &constants_);
    const std::array<ID3D11SamplerState*, 2> samplers{
        linearSampler_, pointSampler_};
    deferredContext_->PSSetSamplers(
        0, static_cast<UINT>(samplers.size()), samplers.data());
    SetViewport(colorDescription_.Width, colorDescription_.Height);
}

void TscmaaPass::SetViewport(UINT width, UINT height) noexcept {
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;
    deferredContext_->RSSetViewports(1, &viewport);
}

void TscmaaPass::UnbindGraphicsResources() noexcept {
    const std::array<ID3D11ShaderResourceView*, 5> empty{};
    deferredContext_->PSSetShaderResources(
        0, static_cast<UINT>(empty.size()), empty.data());
    deferredContext_->OMSetRenderTargets(0, nullptr, nullptr);
}

void TscmaaPass::UnbindComputeResources() noexcept {
    const std::array<ID3D11UnorderedAccessView*, 8> emptyUavs{};
    const std::array<ID3D11ShaderResourceView*, 6> emptySrvs{};
    ID3D11SamplerState* emptySampler = nullptr;
    deferredContext_->CSSetShader(nullptr, nullptr, 0);
    deferredContext_->CSSetUnorderedAccessViews(
        0, static_cast<UINT>(emptyUavs.size()), emptyUavs.data(), nullptr);
    deferredContext_->CSSetShaderResources(
        0, static_cast<UINT>(emptySrvs.size()), emptySrvs.data());
    deferredContext_->CSSetSamplers(0, 1, &emptySampler);
}

void TscmaaPass::ReleaseSizeResources() noexcept {
    for (auto*& view : colorSourceSrv_) ReleaseObject(view);
    colorSourceIdentity_.fill(nullptr);
    for (auto& snapshot : motion_) {
        ReleaseObject(snapshot.srv);
        ReleaseObject(snapshot.texture);
        snapshot.pairToken = 0U;
    }
    motionBytesPerPixel_.fill(0U);
    for (auto& target : spatial_) {
        ReleaseObject(target.uav);
        ReleaseObject(target.rtv);
        ReleaseObject(target.srv);
        ReleaseObject(target.texture);
    }
    for (auto& eyeTargets : resolved_) {
        for (auto& target : eyeTargets) {
            ReleaseObject(target.uav);
            ReleaseObject(target.rtv);
            ReleaseObject(target.srv);
            ReleaseObject(target.texture);
        }
    }
    auto releaseBuffer = [](BufferTarget& target) {
        ReleaseObject(target.uav);
        ReleaseObject(target.buffer);
        target.byteWidth = 0U;
    };
    releaseBuffer(workingIndirect_);
    releaseBuffer(workingControl_);
    releaseBuffer(workingDeferredItems_);
    releaseBuffer(workingDeferredLocations_);
    releaseBuffer(workingShapeCandidates_);
    ReleaseObject(workingDeferredHeadsUav_);
    ReleaseObject(workingDeferredHeads_);
    ReleaseObject(workingEdgesUav_);
    ReleaseObject(workingEdgesSrv_);
    ReleaseObject(workingEdges_);
    ResetHistory();
    allocationBytes_ = 0U;
}

void TscmaaPass::ReleaseShaders() noexcept {
    ReleaseObject(temporalPs_);
    ReleaseObject(copyPs_);
    ReleaseObject(fullscreenVs_);
    for (auto& qualityVariants : shaders_) {
        for (auto& shader : qualityVariants) {
            ReleaseObject(shader.deferredApply);
            ReleaseObject(shader.processCandidates);
            ReleaseObject(shader.computeDispatchArgs);
            ReleaseObject(shader.edges);
        }
    }
}

void TscmaaPass::SetFailure(Status status, HRESULT result) noexcept {
    lastStatus_ = status;
    lastHresult_ = result;
}

} // namespace gakumas::vr::d3d11
