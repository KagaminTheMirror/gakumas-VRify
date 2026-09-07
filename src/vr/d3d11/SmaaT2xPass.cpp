#include "SmaaT2xPass.hpp"
#include "SmaaT2xJitter.hpp"

#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>

#pragma comment(lib, "d3dcompiler.lib")

namespace gakumas::vr::d3d11 {
namespace {

constexpr int kSmaaHlslResource = 3001;
constexpr int kAreaTextureResource = 3002;
constexpr int kSearchTextureResource = 3003;
constexpr UINT kAreaWidth = 160;
constexpr UINT kAreaHeight = 560;
constexpr UINT kAreaPitch = kAreaWidth * 2;
constexpr UINT kSearchWidth = 64;
constexpr UINT kSearchHeight = 16;
constexpr UINT kSearchPitch = kSearchWidth;
constexpr UINT kAlignmentWidth = 32;
constexpr UINT kAlignmentHeight = 32;
// `.203` hardware probes: static-head velocity-magnitude deltas average
// ~1.1px (HMD micro-motion + 8-bit alpha quantization), collapsing the
// official resolve weight to a 0.21 mean. The deadzone is only applied when
// the *current* pixel is itself still; `.204` hardware showed that applying
// it to every 1px |Δv| also ghosts walking characters (2–3 trails/min).
constexpr float kVrResolveDeltaDeadzonePixels = 2.0F;
constexpr std::size_t kDdsHeaderBytes = 128;
constexpr std::size_t kDdsDx10HeaderBytes = 148;

constexpr char kSmaaWrapper[] = R"SMAA(
Texture2D<float4> input0 : register(t0);
Texture2D<float4> input1 : register(t1);
Texture2D<float4> input2 : register(t2);
Texture2D<float4> input3 : register(t3);
Texture2D<float4> input4 : register(t4);
Texture2D<float4> input5 : register(t5);

struct EdgeVertexOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 offset0 : TEXCOORD1;
    float4 offset1 : TEXCOORD2;
    float4 offset2 : TEXCOORD3;
};

struct BlendVertexOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float2 pixcoord : TEXCOORD1;
    float4 offset0 : TEXCOORD2;
    float4 offset1 : TEXCOORD3;
    float4 offset2 : TEXCOORD4;
};

struct NeighborhoodVertexOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 offset : TEXCOORD1;
};

void SmaaFullscreenVertex(uint vertexId, out float4 position, out float2 texcoord) {
    texcoord = float2((vertexId << 1) & 2, vertexId & 2);
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

EdgeVertexOutput SmaaEdgeVS(uint vertexId : SV_VertexID) {
    EdgeVertexOutput output;
    SmaaFullscreenVertex(vertexId, output.position, output.texcoord);
    float4 offsets[3];
    SMAAEdgeDetectionVS(output.texcoord, offsets);
    output.offset0 = offsets[0];
    output.offset1 = offsets[1];
    output.offset2 = offsets[2];
    return output;
}

float2 SmaaEdgePS(EdgeVertexOutput input) : SV_Target {
    float4 offsets[3] = { input.offset0, input.offset1, input.offset2 };
    return SMAAColorEdgeDetectionPS(input.texcoord, offsets, input0);
}

BlendVertexOutput SmaaBlendVS(uint vertexId : SV_VertexID) {
    BlendVertexOutput output;
    SmaaFullscreenVertex(vertexId, output.position, output.texcoord);
    float4 offsets[3];
    SMAABlendingWeightCalculationVS(output.texcoord, output.pixcoord, offsets);
    output.offset0 = offsets[0];
    output.offset1 = offsets[1];
    output.offset2 = offsets[2];
    return output;
}

float4 SmaaBlendPS(BlendVertexOutput input) : SV_Target {
    float4 offsets[3] = { input.offset0, input.offset1, input.offset2 };
    return SMAABlendingWeightCalculationPS(
        input.texcoord, input.pixcoord, offsets, input0, input1, input2,
        smaaSubsampleIndices);
}

NeighborhoodVertexOutput SmaaNeighborhoodVS(uint vertexId : SV_VertexID) {
    NeighborhoodVertexOutput output;
    SmaaFullscreenVertex(vertexId, output.position, output.texcoord);
    SMAANeighborhoodBlendingVS(output.texcoord, output.offset);
    return output;
}

float4 SmaaNeighborhoodPS(NeighborhoodVertexOutput input) : SV_Target {
    return SMAANeighborhoodBlendingPS(
        input.texcoord, input.offset, input0, input1, input2);
}

float SmaaResolveWeight(float currentAlpha, float historyAlpha) {
    // Official SMAAResolvePS math with one VR adaptation. `.203` measured
    // mean history weight 0.21 of the ideal 0.5 because HMD micro-motion
    // keeps |Δv| near 1px on still pixels. `.204` then proved an ungated
    // 2px deadzone also keeps the 0.5 blend on walking characters whose
    // |v| is large but |Δv| is 1px (2–3 ghost trails per Live minute).
    // Gate the deadzone: only still pixels (current |v| ≤ floor) get it.
    float delta = abs(
        currentAlpha * currentAlpha - historyAlpha * historyAlpha) / 5.0;
    float currentSpeed = currentAlpha * currentAlpha / 5.0;
    float deadzone = currentSpeed <= smaaProbe.z ? smaaProbe.z : 0.0;
    delta = max(delta - deadzone, 0.0);
    return 0.5 * saturate(
        1.0 - sqrt(delta) * SMAA_REPROJECTION_WEIGHT_SCALE);
}

float4 SmaaResolvePSMain(NeighborhoodVertexOutput input) : SV_Target {
    float2 velocity = -SMAA_DECODE_VELOCITY(
        input2.SampleLevel(PointSampler, input.texcoord, 0).rg);
    float4 current = input0.SampleLevel(PointSampler, input.texcoord, 0);
    float4 previous = input1.SampleLevel(
        PointSampler, input.texcoord + velocity, 0);
    return lerp(current, previous, SmaaResolveWeight(current.a, previous.a));
}

float SmaaAlignmentError(float2 uv, float2 historyUv) {
    float3 currentColor = input0.SampleLevel(PointSampler, uv, 0).rgb;
    float3 historyColor = input1.SampleLevel(PointSampler, historyUv, 0).rgb;
    return dot(abs(currentColor - historyColor), float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
}

bool SmaaProbeUvValid(float2 uv) {
    return all(uv >= 0.0) && all(uv <= 1.0);
}

float4 SmaaProbeErrors(float2 uv, float2 uv0, float2 uv1, float2 uv2, float2 uv3) {
    if (!SmaaProbeUvValid(uv0) || !SmaaProbeUvValid(uv1) ||
        !SmaaProbeUvValid(uv2) || !SmaaProbeUvValid(uv3)) return -1.0;
    return float4(
        SmaaAlignmentError(uv, uv0), SmaaAlignmentError(uv, uv1),
        SmaaAlignmentError(uv, uv2), SmaaAlignmentError(uv, uv3));
}

float4 SmaaSourceProbePS(NeighborhoodVertexOutput input) : SV_Target {
    float2 own = SMAA_DECODE_VELOCITY(input2.SampleLevel(PointSampler, input.texcoord, 0));
    float2 other = SMAA_DECODE_VELOCITY(input3.SampleLevel(PointSampler, input.texcoord, 0));
    return SmaaProbeErrors(
        input.texcoord,
        input.texcoord - own,
        input.texcoord - other,
        input.texcoord - 0.5 * (own + other),
        input.texcoord);
}

float4 SmaaAgeProbePS(NeighborhoodVertexOutput input) : SV_Target {
    float2 ownCurrent = SMAA_DECODE_VELOCITY(input2.SampleLevel(PointSampler, input.texcoord, 0));
    float2 ownPrevious = input4.SampleLevel(PointSampler, input.texcoord, 0).xy;
    float2 otherPrevious = input5.SampleLevel(PointSampler, input.texcoord, 0).xy;
    return SmaaProbeErrors(
        input.texcoord,
        input.texcoord - ownCurrent,
        input.texcoord - ownPrevious,
        input.texcoord - otherPrevious,
        input.texcoord - 0.5 * (ownPrevious + otherPrevious));
}

float4 SmaaJitterProbePS(NeighborhoodVertexOutput input) : SV_Target {
    float2 own = SMAA_DECODE_VELOCITY(input2.SampleLevel(PointSampler, input.texcoord, 0));
    float2 jitter = smaaProbe.xy;
    return SmaaProbeErrors(
        input.texcoord,
        input.texcoord - own,
        input.texcoord - own + jitter,
        input.texcoord - own - jitter,
        input.texcoord + jitter);
}

float4 SmaaResolveProbePS(NeighborhoodVertexOutput input) : SV_Target {
    float2 own = SMAA_DECODE_VELOCITY(input2.SampleLevel(PointSampler, input.texcoord, 0));
    float2 historyUv = input.texcoord - own;
    if (!SmaaProbeUvValid(historyUv)) return -1.0;
    float4 currentColor = input0.SampleLevel(PointSampler, input.texcoord, 0);
    float4 historyColor = input1.SampleLevel(PointSampler, historyUv, 0);
    float weight = SmaaResolveWeight(currentColor.a, historyColor.a);
    return float4(
        currentColor.a, historyColor.a, weight,
        dot(abs(currentColor.rgb - historyColor.rgb),
            float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)));
}

float4 SmaaOutputProbePS(NeighborhoodVertexOutput input) : SV_Target {
    float4 currentColor = input0.SampleLevel(PointSampler, input.texcoord, 0);
    float4 resolvedColor = input1.SampleLevel(PointSampler, input.texcoord, 0);
    float3 luma = float3(0.2126, 0.7152, 0.0722);
    return float4(
        dot(abs(currentColor.rgb - resolvedColor.rgb),
            float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)),
        dot(currentColor.rgb, luma), dot(resolvedColor.rgb, luma),
        abs(currentColor.a - resolvedColor.a));
}

float2 SmaaMotionProbePS(NeighborhoodVertexOutput input) : SV_Target {
    return SMAA_DECODE_VELOCITY(input2.SampleLevel(PointSampler, input.texcoord, 0));
}
)SMAA";

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
            address,
            &module) ||
        module == nullptr) {
        return {};
    }
    HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (resource == nullptr) {
        return {};
    }
    HGLOBAL loaded = LoadResource(module, resource);
    const DWORD size = SizeofResource(module, resource);
    const void* bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (bytes == nullptr || size == 0) {
        return {};
    }
    return {static_cast<const std::uint8_t*>(bytes), size};
}

int FormatFamily(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return 1;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return 2;
    default:
        return 0;
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

DXGI_FORMAT MotionVectorViewFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16_FLOAT:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    // The mod-owned Unity copy target may allocate the typed RGBA16 float
    // resource for the same logical GraphicsFormat the live typeless RTHandle
    // uses. Both view the same half4 payload.
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

UINT MotionVectorBytesPerPixel(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16_FLOAT:
        return 4U;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8U;
    default:
        return 0U;
    }
}

float HalfToFloat(std::uint16_t bits) noexcept {
    const bool negative = (bits & 0x8000U) != 0U;
    const unsigned int exponent = (bits >> 10U) & 0x1fU;
    const unsigned int mantissa = bits & 0x3ffU;
    float value = 0.0F;
    if (exponent == 0U) {
        value = mantissa == 0U
            ? 0.0F
            : std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 0x1fU) {
        value = mantissa == 0U
            ? std::numeric_limits<float>::infinity()
            : std::numeric_limits<float>::quiet_NaN();
    } else {
        value = std::ldexp(
            1.0F + static_cast<float>(mantissa) / 1024.0F,
            static_cast<int>(exponent) - 15);
    }
    return negative ? -value : value;
}

std::string ShaderPrefix(int quality) {
    std::ostringstream stream;
    stream << "#define SMAA_HLSL_4 1\n"
           << "#define SMAA_REPROJECTION 1\n"
           << "#define SMAA_INCLUDE_VS 1\n"
           << "#define SMAA_INCLUDE_PS 1\n"
           << "#define SMAA_RT_METRICS smaaMetrics\n";
    switch (quality) {
    case 0: stream << "#define SMAA_PRESET_LOW 1\n"; break;
    case 1: stream << "#define SMAA_PRESET_MEDIUM 1\n"; break;
    default: stream << "#define SMAA_PRESET_HIGH 1\n"; break;
    }
    stream << "cbuffer SmaaConstants : register(b0) {\n"
           << "    float4 smaaMetrics;\n"
           << "    float4 smaaSubsampleIndices;\n"
           << "    float4 smaaProbe;\n"
           << "};\n";
    return stream.str();
}

std::uint64_t TextureBytes(UINT width, UINT height, UINT bytesPerPixel, UINT count) noexcept {
    return static_cast<std::uint64_t>(width) * height * bytesPerPixel * count;
}

} // namespace

SmaaT2xPass::~SmaaT2xPass() {
    Reset();
}

bool SmaaT2xPass::Prepare(
    ID3D11Device* device,
    const D3D11_TEXTURE2D_DESC& colorDescription,
    bool srgb,
    PrepareDiagnostics* diagnostics) noexcept {
    const auto started = std::chrono::steady_clock::now();
    if (device == nullptr || colorDescription.Width == 0 ||
        colorDescription.Height == 0) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    const int family = FormatFamily(colorDescription.Format);
    if (family == 0 || colorDescription.MipLevels != 1 ||
        colorDescription.ArraySize != 1 || colorDescription.SampleDesc.Count != 1) {
        SetFailure(Status::UnsupportedFormat, DXGI_ERROR_UNSUPPORTED);
        return false;
    }
    if (device_ != nullptr && device_ != device) {
        Reset();
    }
    if (device_ == nullptr) {
        device->AddRef();
        device_ = device;
    }
    const bool hasAllAlignmentStaging = std::all_of(
        alignmentStaging_.begin(), alignmentStaging_.end(),
        [](const auto& eyeStaging) {
            return std::all_of(
                eyeStaging.begin(), eyeStaging.end(),
                [](ID3D11Texture2D* texture) { return texture != nullptr; });
        });
    const bool hasAllMotionProbes = std::all_of(
        motionProbe_.begin(), motionProbe_.end(), [](const auto& eyeTargets) {
            return std::all_of(
                eyeTargets.begin(), eyeTargets.end(), [](const ColorTarget& target) {
                    return target.texture != nullptr && target.srv != nullptr &&
                           target.rtv != nullptr;
                });
        });
    const bool partialCore = deferredContext_ != nullptr &&
        (rasterizerState_ == nullptr || depthStencilState_ == nullptr ||
         linearSampler_ == nullptr || pointSampler_ == nullptr ||
         constants_ == nullptr || areaSrv_ == nullptr || searchSrv_ == nullptr ||
         alignmentTexture_[0] == nullptr || alignmentTexture_[1] == nullptr ||
         alignmentRtv_[0] == nullptr || alignmentRtv_[1] == nullptr ||
         !hasAllAlignmentStaging || !hasAllMotionProbes);
    if (partialCore) {
        // A previous allocation failure must be retryable and must never leave
        // ResolveStereo observing half-created state.
        Reset();
        device->AddRef();
        device_ = device;
    }
    if (deferredContext_ == nullptr &&
        (!LoadAndCompileShaders() || !CreateFixedResources())) {
        return false;
    }
    if (!CreateSizeResources(colorDescription, srgb)) {
        return false;
    }
    lastStatus_ = Status::Ready;
    lastHresult_ = S_OK;
    if (diagnostics != nullptr) {
        diagnostics->allocationBytes = allocationBytes_;
        diagnostics->allocationMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
        diagnostics->width = colorDescription_.Width;
        diagnostics->height = colorDescription_.Height;
        diagnostics->resourceFormat = colorDescription_.Format;
        diagnostics->viewFormat = colorViewFormat_;
    }
    return true;
}

bool SmaaT2xPass::CaptureMotionVector(
    ID3D11DeviceContext* immediateContext,
    std::size_t eye,
    ID3D11Texture2D* source,
    std::uint64_t pairToken) noexcept {
    if (immediateContext == nullptr || source == nullptr ||
        eye >= motion_.size() || pairToken == 0) {
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
    if (!EnsureMotionSnapshot(eye, source)) {
        return false;
    }
    immediateContext->CopyResource(motion_[eye].texture, source);
    motion_[eye].pairToken = pairToken;
    lastStatus_ = Status::Ready;
    lastHresult_ = S_OK;
    return true;
}

bool SmaaT2xPass::HasFreshMotionVectors(std::uint64_t pairToken) const noexcept {
    return pairToken != 0 && motion_[0].texture != nullptr &&
           motion_[1].texture != nullptr && motion_[0].pairToken == pairToken &&
           motion_[1].pairToken == pairToken;
}

bool SmaaT2xPass::ReadMotionVectorValues(
    ID3D11DeviceContext* immediateContext,
    std::size_t eye,
    MotionVectorValueDiagnostics& diagnostics) const noexcept {
    diagnostics = {};
    if (immediateContext == nullptr || eye >= motion_.size() ||
        motion_[eye].texture == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC source{};
    motion_[eye].texture->GetDesc(&source);
    const UINT componentCount =
        source.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ||
                source.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
            ? 4U
            : source.Format == DXGI_FORMAT_R16G16_FLOAT ? 2U : 0U;
    if (componentCount == 0U || source.Width == 0U || source.Height == 0U ||
        source.MipLevels != 1U || source.ArraySize != 1U ||
        source.SampleDesc.Count != 1U) {
        return false;
    }

    constexpr UINT kMaximumSampleWidth = 64U;
    constexpr UINT kMaximumSampleRows = 8U;
    const UINT sampleWidth = (std::min)(source.Width, kMaximumSampleWidth);
    const UINT sampleRows = (std::min)(source.Height, kMaximumSampleRows);
    D3D11_TEXTURE2D_DESC stagingDescription{};
    stagingDescription.Width = sampleWidth;
    stagingDescription.Height = sampleRows;
    stagingDescription.MipLevels = 1;
    stagingDescription.ArraySize = 1;
    stagingDescription.Format = source.Format;
    stagingDescription.SampleDesc.Count = 1;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device* contextDevice = nullptr;
    immediateContext->GetDevice(&contextDevice);
    if (contextDevice == nullptr) {
        return false;
    }
    ID3D11Texture2D* staging = nullptr;
    const HRESULT createResult = contextDevice->CreateTexture2D(
        &stagingDescription, nullptr, &staging);
    contextDevice->Release();
    if (FAILED(createResult) || staging == nullptr) {
        return false;
    }
    for (UINT row = 0; row < sampleRows; ++row) {
        const UINT sourceY = sampleRows == 1U
            ? source.Height / 2U
            : static_cast<UINT>(
                  (static_cast<std::uint64_t>(source.Height - 1U) * row) /
                  (sampleRows - 1U));
        const UINT maximumX = source.Width - sampleWidth;
        const UINT sourceX = sampleRows == 1U
            ? maximumX / 2U
            : static_cast<UINT>(
                  (static_cast<std::uint64_t>(maximumX) * row) /
                  (sampleRows - 1U));
        const D3D11_BOX box{
            sourceX,
            sourceY,
            0U,
            sourceX + sampleWidth,
            sourceY + 1U,
            1U,
        };
        immediateContext->CopySubresourceRegion(
            staging, 0, 0, row, 0, motion_[eye].texture, 0, &box);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapResult = immediateContext->Map(
        staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapResult) || mapped.pData == nullptr) {
        staging->Release();
        return false;
    }

    diagnostics.sampleCount = sampleWidth * sampleRows;
    float minimumX = std::numeric_limits<float>::infinity();
    float maximumX = -std::numeric_limits<float>::infinity();
    float minimumY = std::numeric_limits<float>::infinity();
    float maximumY = -std::numeric_limits<float>::infinity();
    float minimumZ = std::numeric_limits<float>::infinity();
    float maximumZ = -std::numeric_limits<float>::infinity();
    float minimumW = std::numeric_limits<float>::infinity();
    float maximumW = -std::numeric_limits<float>::infinity();
    double magnitudeSum = 0.0;
    for (UINT row = 0; row < sampleRows; ++row) {
        const auto* rowBytes = static_cast<const std::uint8_t*>(mapped.pData) +
            static_cast<std::size_t>(mapped.RowPitch) * row;
        for (UINT column = 0; column < sampleWidth; ++column) {
            const auto* components = reinterpret_cast<const std::uint16_t*>(
                rowBytes + static_cast<std::size_t>(column) * componentCount * 2U);
            const float x = HalfToFloat(components[0]);
            const float y = HalfToFloat(components[1]);
            if (!std::isfinite(x) || !std::isfinite(y)) {
                ++diagnostics.nonFiniteSampleCount;
                continue;
            }
            ++diagnostics.finiteSampleCount;
            minimumX = (std::min)(minimumX, x);
            maximumX = (std::max)(maximumX, x);
            minimumY = (std::min)(minimumY, y);
            maximumY = (std::max)(maximumY, y);
            const float magnitude = std::hypot(x, y);
            magnitudeSum += magnitude;
            diagnostics.maximumMagnitude =
                (std::max)(diagnostics.maximumMagnitude, magnitude);
            if (componentCount == 4U) {
                const float z = HalfToFloat(components[2]);
                const float w = HalfToFloat(components[3]);
                if (std::isfinite(z)) {
                    minimumZ = (std::min)(minimumZ, z);
                    maximumZ = (std::max)(maximumZ, z);
                }
                if (std::isfinite(w)) {
                    minimumW = (std::min)(minimumW, w);
                    maximumW = (std::max)(maximumW, w);
                }
            }
        }
    }
    immediateContext->Unmap(staging, 0);
    staging->Release();
    if (diagnostics.finiteSampleCount == 0U) {
        return false;
    }
    diagnostics.minimumX = minimumX;
    diagnostics.maximumX = maximumX;
    diagnostics.minimumY = minimumY;
    diagnostics.maximumY = maximumY;
    diagnostics.meanMagnitude = static_cast<float>(
        magnitudeSum / diagnostics.finiteSampleCount);
    if (componentCount == 4U) {
        diagnostics.minimumZ = std::isfinite(minimumZ) ? minimumZ : 0.0F;
        diagnostics.maximumZ = std::isfinite(maximumZ) ? maximumZ : 0.0F;
        diagnostics.minimumW = std::isfinite(minimumW) ? minimumW : 0.0F;
        diagnostics.maximumW = std::isfinite(maximumW) ? maximumW : 0.0F;
    }
    return true;
}

bool SmaaT2xPass::ResolveStereo(
    ID3D11DeviceContext* immediateContext,
    const std::array<ID3D11Texture2D*, 2>& colors,
    bool srgb,
    int quality,
    std::uint32_t phase,
    std::uint64_t pairToken,
    bool collectReprojectionAlignment,
    std::array<ID3D11Texture2D*, 2>& outputs) noexcept {
    outputs.fill(nullptr);
    alignmentPending_.fill(false);
    if (!IsPrepared() || immediateContext == nullptr || colors[0] == nullptr ||
        colors[1] == nullptr || quality < 0 || quality > 2 || phase > 1) {
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
            description.MipLevels != 1 || description.ArraySize != 1 ||
            description.SampleDesc.Count != 1 ||
            TypedFormat(FormatFamily(description.Format), srgb) != colorViewFormat_ ||
            !EnsureColorSourceView(eye, colors[eye])) {
            SetFailure(Status::DescriptionMismatch, E_INVALIDARG);
            return false;
        }
    }

    deferredContext_->ClearState();
    BindFullscreenState();
    for (std::size_t eye = 0; eye < colors.size(); ++eye) {
        if (!RecordEye(
                eye, colors[eye], quality, phase,
                collectReprojectionAlignment)) {
            deferredContext_->ClearState();
            return false;
        }
    }
    for (std::size_t eye = 0; eye < colors.size(); ++eye) {
        RecordMotionProbe(
            eye, shaders_[static_cast<std::size_t>(quality)].motionProbePs);
    }
    for (std::size_t eye = 0; eye < colors.size(); ++eye) {
        const std::uint32_t writeIndex = motionProbeValid_[eye]
            ? motionProbeCurrentIndex_[eye] ^ 1U
            : motionProbeCurrentIndex_[eye];
        motionProbeCurrentIndex_[eye] = writeIndex;
        motionProbeValid_[eye] = true;
        motionProbePairTokens_[eye][writeIndex] = pairToken;
    }
    deferredContext_->OMSetRenderTargets(0, nullptr, nullptr);
    UnbindShaderResources();
    ID3D11CommandList* commandList = nullptr;
    const HRESULT result = deferredContext_->FinishCommandList(FALSE, &commandList);
    if (FAILED(result) || commandList == nullptr) {
        ReleaseObject(commandList);
        SetFailure(Status::CommandRecordingFailed, result);
        return false;
    }
    immediateContext->ExecuteCommandList(commandList, TRUE);
    commandList->Release();
    for (std::size_t eye = 0; eye < outputs.size(); ++eye) {
        outputs[eye] = resolved_[eye].texture;
    }
    lastStatus_ = Status::Ready;
    lastHresult_ = S_OK;
    return true;
}

bool SmaaT2xPass::ReadReprojectionAlignment(
    ID3D11DeviceContext* immediateContext,
    ReprojectionAlignmentDiagnostics& diagnostics) noexcept {
    diagnostics = {};
    if (immediateContext == nullptr || !alignmentPending_[0] ||
        !alignmentPending_[1]) {
        return false;
    }
    alignmentPending_.fill(false);
    for (std::size_t group = 0; group < ProbeGroupCount; ++group) {
        for (std::size_t eye = 0; eye < alignmentStaging_.size(); ++eye) {
            auto& output = diagnostics.groups[group][eye];
            output.minimumValue.fill(std::numeric_limits<float>::max());
            output.maximumValue.fill(std::numeric_limits<float>::lowest());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT result = immediateContext->Map(
                alignmentStaging_[eye][group], 0, D3D11_MAP_READ, 0, &mapped);
            if (FAILED(result)) {
                return false;
            }
            std::array<double, ReprojectionCandidateCount> sums{};
            for (UINT y = 0; y < kAlignmentHeight; ++y) {
                const auto* row = reinterpret_cast<const float*>(
                    static_cast<const std::uint8_t*>(mapped.pData) +
                    static_cast<std::size_t>(y) * mapped.RowPitch);
                for (UINT x = 0; x < kAlignmentWidth; ++x) {
                    for (std::size_t candidate = 0;
                         candidate < ReprojectionCandidateCount; ++candidate) {
                        const float value =
                            row[x * ReprojectionCandidateCount + candidate];
                        if (std::isfinite(value) && value >= 0.0F) {
                            sums[candidate] += value;
                            ++output.validSampleCount[candidate];
                            output.minimumValue[candidate] = std::min(
                                output.minimumValue[candidate], value);
                            output.maximumValue[candidate] = std::max(
                                output.maximumValue[candidate], value);
                        }
                    }
                }
            }
            immediateContext->Unmap(alignmentStaging_[eye][group], 0);
            for (std::size_t candidate = 0;
                 candidate < ReprojectionCandidateCount; ++candidate) {
                const UINT count = output.validSampleCount[candidate];
                output.meanValue[candidate] = count != 0U
                    ? static_cast<float>(sums[candidate] / count)
                    : 0.0F;
                if (count == 0U) {
                    output.minimumValue[candidate] = 0.0F;
                    output.maximumValue[candidate] = 0.0F;
                }
            }
        }
    }
    return true;
}

SmaaT2xPass::ResourceDiagnostics SmaaT2xPass::GetResourceDiagnostics() const noexcept {
    ResourceDiagnostics diagnostics{};
    for (std::size_t eye = 0; eye < diagnostics.eyes.size(); ++eye) {
        auto& output = diagnostics.eyes[eye];
        const std::uint32_t next = currentSpatialIndex_[eye];
        output.motion = reinterpret_cast<std::uintptr_t>(motion_[eye].texture);
        const std::uint32_t currentMotionIndex = motionProbeCurrentIndex_[eye];
        const std::uint32_t previousMotionIndex = currentMotionIndex ^ 1U;
        output.currentMotionProbe = motionProbeValid_[eye]
            ? reinterpret_cast<std::uintptr_t>(
                  motionProbe_[eye][currentMotionIndex].texture)
            : 0;
        output.previousMotionProbe =
            motionProbePairTokens_[eye][previousMotionIndex] != 0U
            ? reinterpret_cast<std::uintptr_t>(
                  motionProbe_[eye][previousMotionIndex].texture)
            : 0;
        output.currentSpatial = reinterpret_cast<std::uintptr_t>(
            spatial_[eye][next ^ 1U].texture);
        output.previousSpatial = reinterpret_cast<std::uintptr_t>(
            spatial_[eye][next].texture);
        output.resolved = reinterpret_cast<std::uintptr_t>(resolved_[eye].texture);
        output.motionPairToken = motion_[eye].pairToken;
        output.currentMotionProbePairToken =
            motionProbePairTokens_[eye][currentMotionIndex];
        output.previousMotionProbePairToken =
            motionProbePairTokens_[eye][previousMotionIndex];
        output.nextSpatialWriteIndex = next;
        output.historyValid = historyValid_[eye];
    }
    return diagnostics;
}

const char* SmaaT2xPass::ProbeGroupName(ProbeGroup group) noexcept {
    switch (group) {
    case ProbeGroup::Source: return "source";
    case ProbeGroup::Age: return "age";
    case ProbeGroup::Jitter: return "jitter";
    case ProbeGroup::Resolve: return "resolve";
    case ProbeGroup::Output: return "output";
    case ProbeGroup::Count: break;
    }
    return "unknown";
}

void SmaaT2xPass::ResetHistory() noexcept {
    historyValid_.fill(false);
    currentSpatialIndex_.fill(0);
    motionProbeCurrentIndex_.fill(0);
    motionProbeValid_.fill(false);
    for (auto& tokens : motionProbePairTokens_) {
        tokens.fill(0);
    }
    for (auto& snapshot : motion_) {
        snapshot.pairToken = 0;
    }
    alignmentPending_.fill(false);
}

void SmaaT2xPass::Reset() noexcept {
    ReleaseSizeResources();
    ReleaseObject(searchSrv_);
    ReleaseObject(searchTexture_);
    for (auto& eyeStaging : alignmentStaging_) {
        for (auto*& texture : eyeStaging) {
            ReleaseObject(texture);
        }
    }
    for (auto& eyeTargets : motionProbe_) {
        for (auto& target : eyeTargets) {
            ReleaseObject(target.rtv);
            ReleaseObject(target.srv);
            ReleaseObject(target.texture);
        }
    }
    for (auto*& view : alignmentRtv_) {
        ReleaseObject(view);
    }
    for (auto*& texture : alignmentTexture_) {
        ReleaseObject(texture);
    }
    ReleaseObject(areaSrv_);
    ReleaseObject(areaTexture_);
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
    allocationBytes_ = 0;
    lastCompilerError_.clear();
    lastStatus_ = Status::NotInitialized;
    lastHresult_ = S_OK;
    lastMotionVectorDiagnostics_ = {};
}

bool SmaaT2xPass::IsPrepared() const noexcept {
    const bool hasAllAlignmentStaging = std::all_of(
        alignmentStaging_.begin(), alignmentStaging_.end(),
        [](const auto& eyeStaging) {
            return std::all_of(
                eyeStaging.begin(), eyeStaging.end(),
                [](ID3D11Texture2D* texture) { return texture != nullptr; });
        });
    const bool hasAllMotionProbes = std::all_of(
        motionProbe_.begin(), motionProbe_.end(), [](const auto& eyeTargets) {
            return std::all_of(
                eyeTargets.begin(), eyeTargets.end(), [](const ColorTarget& target) {
                    return target.texture != nullptr && target.srv != nullptr &&
                           target.rtv != nullptr;
                });
        });
    return device_ != nullptr && deferredContext_ != nullptr &&
           rasterizerState_ != nullptr && depthStencilState_ != nullptr &&
           linearSampler_ != nullptr && pointSampler_ != nullptr &&
           constants_ != nullptr && areaSrv_ != nullptr && searchSrv_ != nullptr &&
           alignmentTexture_[0] != nullptr && alignmentTexture_[1] != nullptr &&
           alignmentRtv_[0] != nullptr && alignmentRtv_[1] != nullptr &&
           hasAllAlignmentStaging && hasAllMotionProbes &&
           edgeTexture_ != nullptr && blendTexture_ != nullptr &&
           resolved_[0].texture != nullptr && resolved_[1].texture != nullptr;
}

bool SmaaT2xPass::HasHistory() const noexcept {
    return historyValid_[0] && historyValid_[1];
}

SmaaT2xPass::Status SmaaT2xPass::LastStatus() const noexcept {
    return lastStatus_;
}

HRESULT SmaaT2xPass::LastHresult() const noexcept {
    return lastHresult_;
}

SmaaT2xPass::MotionVectorDiagnostics
SmaaT2xPass::LastMotionVectorDiagnostics() const noexcept {
    return lastMotionVectorDiagnostics_;
}

const std::string& SmaaT2xPass::LastCompilerError() const noexcept {
    return lastCompilerError_;
}

std::uint64_t SmaaT2xPass::AllocationBytes() const noexcept {
    return allocationBytes_;
}

const char* SmaaT2xPass::StatusName(Status status) noexcept {
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

bool SmaaT2xPass::LoadAndCompileShaders() noexcept {
    const ResourceView resource = LoadEmbeddedResource(kSmaaHlslResource);
    if (resource.bytes == nullptr || resource.size == 0) {
        SetFailure(Status::EmbeddedResourceMissing, HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND));
        return false;
    }
    const std::string officialSource(
        reinterpret_cast<const char*>(resource.bytes), resource.size);
    for (int quality = 0; quality < 3; ++quality) {
        if (!CompileVariant(quality, officialSource)) {
            ReleaseShaders();
            return false;
        }
    }
    HRESULT result = device_->CreateDeferredContext(0, &deferredContext_);
    if (FAILED(result) || deferredContext_ == nullptr) {
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    return true;
}

bool SmaaT2xPass::CompileVariant(int quality, const std::string& officialSource) noexcept {
    std::string source = ShaderPrefix(quality);
    source += officialSource;
    source += kSmaaWrapper;
    struct ShaderRequest {
        const char* entry;
        const char* profile;
        bool vertex;
        void** output;
    };
    auto& variant = shaders_[static_cast<std::size_t>(quality)];
    const std::array<ShaderRequest, 13> requests{{
        {"SmaaEdgeVS", "vs_4_0", true, reinterpret_cast<void**>(&variant.edgeVs)},
        {"SmaaEdgePS", "ps_4_0", false, reinterpret_cast<void**>(&variant.edgePs)},
        {"SmaaBlendVS", "vs_4_0", true, reinterpret_cast<void**>(&variant.blendVs)},
        {"SmaaBlendPS", "ps_4_0", false, reinterpret_cast<void**>(&variant.blendPs)},
        {"SmaaNeighborhoodVS", "vs_4_0", true, reinterpret_cast<void**>(&variant.neighborhoodVs)},
        {"SmaaNeighborhoodPS", "ps_4_0", false, reinterpret_cast<void**>(&variant.neighborhoodPs)},
        {"SmaaResolvePSMain", "ps_4_0", false, reinterpret_cast<void**>(&variant.resolvePs)},
        {"SmaaSourceProbePS", "ps_4_0", false, reinterpret_cast<void**>(&variant.sourceProbePs)},
        {"SmaaAgeProbePS", "ps_4_0", false, reinterpret_cast<void**>(&variant.ageProbePs)},
        {"SmaaJitterProbePS", "ps_4_0", false, reinterpret_cast<void**>(&variant.jitterProbePs)},
        {"SmaaResolveProbePS", "ps_4_0", false, reinterpret_cast<void**>(&variant.resolveProbePs)},
        {"SmaaOutputProbePS", "ps_4_0", false, reinterpret_cast<void**>(&variant.outputProbePs)},
        {"SmaaMotionProbePS", "ps_4_0", false, reinterpret_cast<void**>(&variant.motionProbePs)},
    }};
    for (const auto& request : requests) {
        ID3DBlob* blob = nullptr;
        if (!CompileShader(source, request.entry, request.profile, &blob)) {
            ReleaseObject(blob);
            return false;
        }
        const HRESULT result = request.vertex
            ? device_->CreateVertexShader(
                  blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                  reinterpret_cast<ID3D11VertexShader**>(request.output))
            : device_->CreatePixelShader(
                  blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                  reinterpret_cast<ID3D11PixelShader**>(request.output));
        blob->Release();
        if (FAILED(result) || *request.output == nullptr) {
            SetFailure(Status::DeviceResourceFailed, result);
            return false;
        }
    }
    return true;
}

bool SmaaT2xPass::CompileShader(
    const std::string& source,
    const char* entry,
    const char* profile,
    ID3DBlob** blob) noexcept {
    if (blob == nullptr) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    *blob = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT result = D3DCompile(
        source.data(), source.size(), "embedded/SMAA.hlsl", nullptr, nullptr,
        entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, blob, &errors);
    if (errors != nullptr) {
        lastCompilerError_.assign(
            static_cast<const char*>(errors->GetBufferPointer()),
            errors->GetBufferSize());
        errors->Release();
    }
    if (FAILED(result) || *blob == nullptr) {
        ReleaseObject(*blob);
        SetFailure(Status::ShaderCompilationFailed, result);
        return false;
    }
    return true;
}

bool SmaaT2xPass::CreateFixedResources() noexcept {
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    HRESULT result = device_->CreateRasterizerState(&rasterizer, &rasterizerState_);

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
    constants.ByteWidth = sizeof(float) * 12;
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result)) {
        result = device_->CreateBuffer(&constants, nullptr, &constants_);
    }
    if (FAILED(result) || constants_ == nullptr) {
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    if (!CreateLookupTexture(
            kAreaTextureResource, kDdsDx10HeaderBytes, kAreaWidth, kAreaHeight,
            kAreaPitch, DXGI_FORMAT_R8G8_UNORM, &areaTexture_, &areaSrv_) ||
        !CreateLookupTexture(
            kSearchTextureResource, kDdsHeaderBytes, kSearchWidth, kSearchHeight,
            kSearchPitch, DXGI_FORMAT_R8_UNORM, &searchTexture_, &searchSrv_)) {
        return false;
    }
    return CreateAlignmentResources();
}

bool SmaaT2xPass::CreateAlignmentResources() noexcept {
    for (std::size_t eye = 0; eye < alignmentTexture_.size(); ++eye) {
        D3D11_TEXTURE2D_DESC target{};
        target.Width = kAlignmentWidth;
        target.Height = kAlignmentHeight;
        target.MipLevels = 1;
        target.ArraySize = 1;
        target.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        target.SampleDesc.Count = 1;
        target.Usage = D3D11_USAGE_DEFAULT;
        target.BindFlags = D3D11_BIND_RENDER_TARGET;
        HRESULT result = device_->CreateTexture2D(
            &target, nullptr, &alignmentTexture_[eye]);
        if (SUCCEEDED(result)) {
            result = device_->CreateRenderTargetView(
                alignmentTexture_[eye], nullptr, &alignmentRtv_[eye]);
        }
        D3D11_TEXTURE2D_DESC staging = target;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (std::size_t group = 0;
             SUCCEEDED(result) && group < ProbeGroupCount; ++group) {
            result = device_->CreateTexture2D(
                &staging, nullptr, &alignmentStaging_[eye][group]);
        }
        if (FAILED(result) || alignmentTexture_[eye] == nullptr ||
            alignmentRtv_[eye] == nullptr ||
            std::any_of(
                alignmentStaging_[eye].begin(), alignmentStaging_[eye].end(),
                [](ID3D11Texture2D* texture) { return texture == nullptr; })) {
            SetFailure(Status::DeviceResourceFailed, result);
            return false;
        }
        for (auto& targetValue : motionProbe_[eye]) {
            if (!CreateSmallTarget(
                    targetValue, kAlignmentWidth, kAlignmentHeight,
                    DXGI_FORMAT_R32G32_FLOAT)) {
                return false;
            }
        }
    }
    return true;
}

bool SmaaT2xPass::CreateSizeResources(
    const D3D11_TEXTURE2D_DESC& colorDescription,
    bool srgb) noexcept {
    const int family = FormatFamily(colorDescription.Format);
    const DXGI_FORMAT viewFormat = TypedFormat(family, srgb);
    if (edgeTexture_ != nullptr && colorDescription_.Width == colorDescription.Width &&
        colorDescription_.Height == colorDescription.Height &&
        FormatFamily(colorDescription_.Format) == family &&
        colorViewFormat_ == viewFormat) {
        return true;
    }
    ReleaseSizeResources();
    colorDescription_ = colorDescription;
    colorDescription_.Format = TypelessFormat(family);
    colorDescription_.Usage = D3D11_USAGE_DEFAULT;
    colorDescription_.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    colorDescription_.CPUAccessFlags = 0;
    colorDescription_.MiscFlags = 0;
    colorViewFormat_ = viewFormat;
    if (!CreateScratchTarget(
            DXGI_FORMAT_R8G8_UNORM, &edgeTexture_, &edgeSrv_, &edgeRtv_) ||
        !CreateScratchTarget(
            DXGI_FORMAT_R8G8B8A8_UNORM, &blendTexture_, &blendSrv_, &blendRtv_)) {
        ReleaseSizeResources();
        return false;
    }
    for (auto& eyeTargets : spatial_) {
        for (auto& target : eyeTargets) {
            if (!CreateColorTarget(target)) {
                ReleaseSizeResources();
                return false;
            }
        }
    }
    for (auto& target : resolved_) {
        if (!CreateColorTarget(target)) {
            ReleaseSizeResources();
            return false;
        }
    }
    UpdateAllocationBytes();
    ResetHistory();
    return true;
}

void SmaaT2xPass::UpdateAllocationBytes() noexcept {
    if (colorDescription_.Width == 0 || colorDescription_.Height == 0) {
        allocationBytes_ = 0;
        return;
    }
    allocationBytes_ =
        TextureBytes(colorDescription_.Width, colorDescription_.Height, 4, 6) +
        TextureBytes(colorDescription_.Width, colorDescription_.Height, 2, 1) +
        TextureBytes(colorDescription_.Width, colorDescription_.Height, 4, 1) +
        TextureBytes(
            colorDescription_.Width,
            colorDescription_.Height,
            motionBytesPerPixel_[0] + motionBytesPerPixel_[1],
            1) +
        static_cast<std::uint64_t>(kAreaPitch) * kAreaHeight +
        static_cast<std::uint64_t>(kSearchPitch) * kSearchHeight +
        TextureBytes(
            kAlignmentWidth, kAlignmentHeight, sizeof(float) * 4,
            static_cast<UINT>(2 * (1 + ProbeGroupCount))) +
        TextureBytes(kAlignmentWidth, kAlignmentHeight, sizeof(float) * 2, 4);
}

bool SmaaT2xPass::CreateLookupTexture(
    int resourceId,
    std::size_t dataOffset,
    UINT width,
    UINT height,
    UINT pitch,
    DXGI_FORMAT format,
    ID3D11Texture2D** texture,
    ID3D11ShaderResourceView** view) noexcept {
    const ResourceView resource = LoadEmbeddedResource(resourceId);
    const std::size_t payloadBytes = static_cast<std::size_t>(pitch) * height;
    if (resource.bytes == nullptr || resource.size < dataOffset + payloadBytes) {
        SetFailure(Status::EmbeddedResourceMissing, HRESULT_FROM_WIN32(ERROR_RESOURCE_DATA_NOT_FOUND));
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = resource.bytes + dataOffset;
    initial.SysMemPitch = pitch;
    HRESULT result = device_->CreateTexture2D(&description, &initial, texture);
    if (SUCCEEDED(result)) {
        result = device_->CreateShaderResourceView(*texture, nullptr, view);
    }
    if (FAILED(result) || *texture == nullptr || *view == nullptr) {
        ReleaseObject(*view);
        ReleaseObject(*texture);
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    return true;
}

bool SmaaT2xPass::CreateColorTarget(ColorTarget& target) noexcept {
    HRESULT result = device_->CreateTexture2D(&colorDescription_, nullptr, &target.texture);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = colorViewFormat_;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    if (SUCCEEDED(result)) {
        result = device_->CreateShaderResourceView(target.texture, &srv, &target.srv);
    }
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    rtv.Format = colorViewFormat_;
    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(result)) {
        result = device_->CreateRenderTargetView(target.texture, &rtv, &target.rtv);
    }
    if (FAILED(result) || target.texture == nullptr || target.srv == nullptr ||
        target.rtv == nullptr) {
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    return true;
}

bool SmaaT2xPass::CreateScratchTarget(
    DXGI_FORMAT format,
    ID3D11Texture2D** texture,
    ID3D11ShaderResourceView** srv,
    ID3D11RenderTargetView** rtv) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = colorDescription_.Width;
    description.Height = colorDescription_.Height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT result = device_->CreateTexture2D(&description, nullptr, texture);
    if (SUCCEEDED(result)) {
        result = device_->CreateShaderResourceView(*texture, nullptr, srv);
    }
    if (SUCCEEDED(result)) {
        result = device_->CreateRenderTargetView(*texture, nullptr, rtv);
    }
    if (FAILED(result) || *texture == nullptr || *srv == nullptr || *rtv == nullptr) {
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    return true;
}

bool SmaaT2xPass::EnsureColorSourceView(
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
    view.Texture2D.MipLevels = 1;
    const HRESULT result = device_->CreateShaderResourceView(
        source, &view, &colorSourceSrv_[eye]);
    if (FAILED(result) || colorSourceSrv_[eye] == nullptr) {
        SetFailure(Status::ViewCreationFailed, result);
        return false;
    }
    colorSourceIdentity_[eye] = source;
    return true;
}

bool SmaaT2xPass::EnsureMotionSnapshot(
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
    if (sourceDescription.MipLevels != 1) {
        lastMotionVectorDiagnostics_.mismatchMask |= MotionVectorMismatchMipLevels;
    }
    if (sourceDescription.ArraySize != 1) {
        lastMotionVectorDiagnostics_.mismatchMask |= MotionVectorMismatchArraySize;
    }
    if (sourceDescription.SampleDesc.Count != 1) {
        lastMotionVectorDiagnostics_.mismatchMask |= MotionVectorMismatchSampleCount;
    }
    if (colorDescription_.Width != 0 &&
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
        snapshot.pairToken = 0;
    }
    sourceDescription.Usage = D3D11_USAGE_DEFAULT;
    sourceDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sourceDescription.CPUAccessFlags = 0;
    sourceDescription.MiscFlags = 0;
    HRESULT result = device_->CreateTexture2D(
        &sourceDescription, nullptr, &snapshot.texture);
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = viewFormat;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MostDetailedMip = 0;
    view.Texture2D.MipLevels = 1;
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

bool SmaaT2xPass::RecordEye(
    std::size_t eye,
    ID3D11Texture2D*,
    int quality,
    std::uint32_t phase,
    bool collectReprojectionAlignment) noexcept {
    const float clear[4]{};
    deferredContext_->ClearRenderTargetView(edgeRtv_, clear);
    deferredContext_->ClearRenderTargetView(blendRtv_, clear);
    struct Constants {
        float metrics[4];
        float subsample[4];
        float probe[4];
    } constants{{
                    1.0F / static_cast<float>(colorDescription_.Width),
                    1.0F / static_cast<float>(colorDescription_.Height),
                    static_cast<float>(colorDescription_.Width),
                    static_cast<float>(colorDescription_.Height),
                },
                {
                    kSmaaT2xPhases[phase].subsampleIndices[0],
                    kSmaaT2xPhases[phase].subsampleIndices[1],
                     kSmaaT2xPhases[phase].subsampleIndices[2],
                     kSmaaT2xPhases[phase].subsampleIndices[3],
                },
                {
                    (kSmaaT2xPhases[phase ^ 1U].jitterX -
                     kSmaaT2xPhases[phase].jitterX) /
                        static_cast<float>(colorDescription_.Width),
                    (kSmaaT2xPhases[phase ^ 1U].jitterY -
                     kSmaaT2xPhases[phase].jitterY) /
                        static_cast<float>(colorDescription_.Height),
                    kVrResolveDeltaDeadzonePixels /
                        static_cast<float>(colorDescription_.Width),
                    0.0F,
                }};
    deferredContext_->UpdateSubresource(constants_, 0, nullptr, &constants, 0, 0);
    auto& shader = shaders_[static_cast<std::size_t>(quality)];

    deferredContext_->VSSetShader(shader.edgeVs, nullptr, 0);
    deferredContext_->PSSetShader(shader.edgePs, nullptr, 0);
    deferredContext_->OMSetRenderTargets(1, &edgeRtv_, nullptr);
    deferredContext_->PSSetShaderResources(0, 1, &colorSourceSrv_[eye]);
    deferredContext_->Draw(3, 0);
    UnbindShaderResources();

    deferredContext_->VSSetShader(shader.blendVs, nullptr, 0);
    deferredContext_->PSSetShader(shader.blendPs, nullptr, 0);
    deferredContext_->OMSetRenderTargets(1, &blendRtv_, nullptr);
    const std::array<ID3D11ShaderResourceView*, 3> blendInputs{
        edgeSrv_, areaSrv_, searchSrv_};
    deferredContext_->PSSetShaderResources(
        0, static_cast<UINT>(blendInputs.size()), blendInputs.data());
    deferredContext_->Draw(3, 0);
    UnbindShaderResources();

    const std::uint32_t currentIndex = currentSpatialIndex_[eye];
    auto& current = spatial_[eye][currentIndex];
    auto& previous = spatial_[eye][currentIndex ^ 1U];
    deferredContext_->VSSetShader(shader.neighborhoodVs, nullptr, 0);
    deferredContext_->PSSetShader(shader.neighborhoodPs, nullptr, 0);
    deferredContext_->OMSetRenderTargets(1, &current.rtv, nullptr);
    const std::array<ID3D11ShaderResourceView*, 3> neighborhoodInputs{
        colorSourceSrv_[eye], blendSrv_, motion_[eye].srv};
    deferredContext_->PSSetShaderResources(
        0, static_cast<UINT>(neighborhoodInputs.size()), neighborhoodInputs.data());
    deferredContext_->Draw(3, 0);
    UnbindShaderResources();

    const bool collectProbe = collectReprojectionAlignment && historyValid_[eye] &&
        motionProbeValid_[0] && motionProbeValid_[1];
    if (collectProbe) {
        const std::array<ID3D11ShaderResourceView*, 6> probeInputs{
            current.srv,
            previous.srv,
            motion_[eye].srv,
            motion_[eye ^ 1U].srv,
            motionProbe_[eye][motionProbeCurrentIndex_[eye]].srv,
            motionProbe_[eye ^ 1U][motionProbeCurrentIndex_[eye ^ 1U]].srv,
        };
        RecordProbeGroup(
            eye, ProbeGroup::Source, shader.sourceProbePs, probeInputs);
        RecordProbeGroup(eye, ProbeGroup::Age, shader.ageProbePs, probeInputs);
        RecordProbeGroup(
            eye, ProbeGroup::Jitter, shader.jitterProbePs, probeInputs);
        RecordProbeGroup(
            eye, ProbeGroup::Resolve, shader.resolveProbePs, probeInputs);
    }

    if (historyValid_[eye]) {
        deferredContext_->VSSetShader(shader.neighborhoodVs, nullptr, 0);
        deferredContext_->PSSetShader(shader.resolvePs, nullptr, 0);
        deferredContext_->OMSetRenderTargets(1, &resolved_[eye].rtv, nullptr);
        const std::array<ID3D11ShaderResourceView*, 3> resolveInputs{
            current.srv, previous.srv, motion_[eye].srv};
        deferredContext_->PSSetShaderResources(
            0, static_cast<UINT>(resolveInputs.size()), resolveInputs.data());
        deferredContext_->Draw(3, 0);
        UnbindShaderResources();
    } else {
        deferredContext_->OMSetRenderTargets(0, nullptr, nullptr);
        deferredContext_->CopyResource(resolved_[eye].texture, current.texture);
        historyValid_[eye] = true;
    }
    if (collectProbe) {
        const std::array<ID3D11ShaderResourceView*, 6> outputInputs{
            current.srv, resolved_[eye].srv, nullptr, nullptr, nullptr, nullptr};
        RecordProbeGroup(
            eye, ProbeGroup::Output, shader.outputProbePs, outputInputs);
        alignmentPending_[eye] = true;
    }
    currentSpatialIndex_[eye] ^= 1U;
    return true;
}

void SmaaT2xPass::RecordProbeGroup(
    std::size_t eye,
    ProbeGroup group,
    ID3D11PixelShader* shader,
    const std::array<ID3D11ShaderResourceView*, 6>& inputs) noexcept {
    const std::size_t groupIndex = static_cast<std::size_t>(group);
    SetViewport(kAlignmentWidth, kAlignmentHeight);
    deferredContext_->VSSetShader(
        shaders_[0].neighborhoodVs, nullptr, 0);
    deferredContext_->PSSetShader(shader, nullptr, 0);
    deferredContext_->OMSetRenderTargets(1, &alignmentRtv_[eye], nullptr);
    deferredContext_->PSSetShaderResources(
        0, static_cast<UINT>(inputs.size()), inputs.data());
    deferredContext_->Draw(3, 0);
    UnbindShaderResources();
    deferredContext_->OMSetRenderTargets(0, nullptr, nullptr);
    deferredContext_->CopyResource(
        alignmentStaging_[eye][groupIndex], alignmentTexture_[eye]);
    SetViewport(colorDescription_.Width, colorDescription_.Height);
}

void SmaaT2xPass::RecordMotionProbe(
    std::size_t eye,
    ID3D11PixelShader* shader) noexcept {
    const std::uint32_t writeIndex = motionProbeValid_[eye]
        ? motionProbeCurrentIndex_[eye] ^ 1U
        : motionProbeCurrentIndex_[eye];
    auto& target = motionProbe_[eye][writeIndex];
    SetViewport(kAlignmentWidth, kAlignmentHeight);
    deferredContext_->VSSetShader(shaders_[0].neighborhoodVs, nullptr, 0);
    deferredContext_->PSSetShader(shader, nullptr, 0);
    deferredContext_->OMSetRenderTargets(1, &target.rtv, nullptr);
    const std::array<ID3D11ShaderResourceView*, 6> inputs{
        nullptr, nullptr, motion_[eye].srv, nullptr, nullptr, nullptr};
    deferredContext_->PSSetShaderResources(
        0, static_cast<UINT>(inputs.size()), inputs.data());
    deferredContext_->Draw(3, 0);
    UnbindShaderResources();
    deferredContext_->OMSetRenderTargets(0, nullptr, nullptr);
    SetViewport(colorDescription_.Width, colorDescription_.Height);
}

void SmaaT2xPass::BindFullscreenState() noexcept {
    deferredContext_->IASetInputLayout(nullptr);
    deferredContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferredContext_->RSSetState(rasterizerState_);
    deferredContext_->OMSetDepthStencilState(depthStencilState_, 0);
    deferredContext_->VSSetConstantBuffers(0, 1, &constants_);
    deferredContext_->PSSetConstantBuffers(0, 1, &constants_);
    const std::array<ID3D11SamplerState*, 2> samplers{linearSampler_, pointSampler_};
    deferredContext_->PSSetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    SetViewport(colorDescription_.Width, colorDescription_.Height);
}

bool SmaaT2xPass::CreateSmallTarget(
    ColorTarget& target,
    UINT width,
    UINT height,
    DXGI_FORMAT format) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    HRESULT result = device_->CreateTexture2D(
        &description, nullptr, &target.texture);
    if (SUCCEEDED(result)) {
        result = device_->CreateShaderResourceView(
            target.texture, nullptr, &target.srv);
    }
    if (SUCCEEDED(result)) {
        result = device_->CreateRenderTargetView(
            target.texture, nullptr, &target.rtv);
    }
    if (FAILED(result) || target.texture == nullptr || target.srv == nullptr ||
        target.rtv == nullptr) {
        ReleaseObject(target.rtv);
        ReleaseObject(target.srv);
        ReleaseObject(target.texture);
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    return true;
}

void SmaaT2xPass::SetViewport(UINT width, UINT height) noexcept {
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;
    deferredContext_->RSSetViewports(1, &viewport);
}

void SmaaT2xPass::UnbindShaderResources() noexcept {
    const std::array<ID3D11ShaderResourceView*, 6> empty{};
    deferredContext_->PSSetShaderResources(
        0, static_cast<UINT>(empty.size()), empty.data());
}

void SmaaT2xPass::ReleaseSizeResources() noexcept {
    for (auto*& view : colorSourceSrv_) {
        ReleaseObject(view);
    }
    colorSourceIdentity_.fill(nullptr);
    for (auto& snapshot : motion_) {
        ReleaseObject(snapshot.srv);
        ReleaseObject(snapshot.texture);
        snapshot.pairToken = 0;
    }
    for (auto& eyeTargets : spatial_) {
        for (auto& target : eyeTargets) {
            ReleaseObject(target.rtv);
            ReleaseObject(target.srv);
            ReleaseObject(target.texture);
        }
    }
    for (auto& target : resolved_) {
        ReleaseObject(target.rtv);
        ReleaseObject(target.srv);
        ReleaseObject(target.texture);
    }
    ReleaseObject(blendRtv_);
    ReleaseObject(blendSrv_);
    ReleaseObject(blendTexture_);
    ReleaseObject(edgeRtv_);
    ReleaseObject(edgeSrv_);
    ReleaseObject(edgeTexture_);
    ResetHistory();
    allocationBytes_ = 0;
}

void SmaaT2xPass::ReleaseShaders() noexcept {
    for (auto& shader : shaders_) {
        ReleaseObject(shader.motionProbePs);
        ReleaseObject(shader.outputProbePs);
        ReleaseObject(shader.resolveProbePs);
        ReleaseObject(shader.jitterProbePs);
        ReleaseObject(shader.ageProbePs);
        ReleaseObject(shader.sourceProbePs);
        ReleaseObject(shader.resolvePs);
        ReleaseObject(shader.neighborhoodPs);
        ReleaseObject(shader.neighborhoodVs);
        ReleaseObject(shader.blendPs);
        ReleaseObject(shader.blendVs);
        ReleaseObject(shader.edgePs);
        ReleaseObject(shader.edgeVs);
    }
}

void SmaaT2xPass::SetFailure(Status status, HRESULT result) noexcept {
    lastStatus_ = status;
    lastHresult_ = result;
}

} // namespace gakumas::vr::d3d11
