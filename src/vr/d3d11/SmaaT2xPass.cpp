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
    float deadzone = currentSpeed <= smaaResolveParameters.z ? smaaResolveParameters.z : 0.0;
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
           << "    float4 smaaResolveParameters;\n"
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
    const bool partialCore = deferredContext_ != nullptr &&
        (rasterizerState_ == nullptr || depthStencilState_ == nullptr ||
         linearSampler_ == nullptr || pointSampler_ == nullptr ||
         constants_ == nullptr || areaSrv_ == nullptr || searchSrv_ == nullptr);
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



bool SmaaT2xPass::ResolveStereo(
    ID3D11DeviceContext* immediateContext,
    const std::array<ID3D11Texture2D*, 2>& colors,
    bool srgb,
    int quality,
    std::uint32_t phase,
    std::uint64_t pairToken,
    std::array<ID3D11Texture2D*, 2>& outputs) noexcept {
    outputs.fill(nullptr);
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
                eye, colors[eye], quality, phase)) {
            deferredContext_->ClearState();
            return false;
        }
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







void SmaaT2xPass::ResetHistory() noexcept {
    historyValid_.fill(false);
    currentSpatialIndex_.fill(0);
    for (auto& snapshot : motion_) {
        snapshot.pairToken = 0;
    }
}

void SmaaT2xPass::Reset() noexcept {
    ReleaseSizeResources();
    ReleaseObject(searchSrv_);
    ReleaseObject(searchTexture_);
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
    return device_ != nullptr && deferredContext_ != nullptr &&
           rasterizerState_ != nullptr && depthStencilState_ != nullptr &&
           linearSampler_ != nullptr && pointSampler_ != nullptr &&
           constants_ != nullptr && areaSrv_ != nullptr && searchSrv_ != nullptr &&
           edgeTexture_ != nullptr && blendTexture_ != nullptr &&
           resolved_[0].texture != nullptr && resolved_[1].texture != nullptr;
}

SmaaT2xPass::ResourceDiagnostics SmaaT2xPass::GetResourceDiagnostics() const noexcept {
    ResourceDiagnostics diagnostics{};
    for (std::size_t eye = 0; eye < diagnostics.eyes.size(); ++eye) {
        auto& output = diagnostics.eyes[eye];
        const std::uint32_t next = currentSpatialIndex_[eye];
        output.motion = reinterpret_cast<std::uintptr_t>(motion_[eye].texture);
        output.currentSpatial = reinterpret_cast<std::uintptr_t>(
            spatial_[eye][next ^ 1U].texture);
        output.previousSpatial = reinterpret_cast<std::uintptr_t>(
            spatial_[eye][next].texture);
        output.resolved = reinterpret_cast<std::uintptr_t>(resolved_[eye].texture);
        output.motionPairToken = motion_[eye].pairToken;
        output.nextSpatialWriteIndex = next;
        output.historyValid = historyValid_[eye];
    }
    return diagnostics;
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
    const std::array<ShaderRequest, 7> requests{{
        {"SmaaEdgeVS", "vs_4_0", true, reinterpret_cast<void**>(&variant.edgeVs)},
        {"SmaaEdgePS", "ps_4_0", false, reinterpret_cast<void**>(&variant.edgePs)},
        {"SmaaBlendVS", "vs_4_0", true, reinterpret_cast<void**>(&variant.blendVs)},
        {"SmaaBlendPS", "ps_4_0", false, reinterpret_cast<void**>(&variant.blendPs)},
        {"SmaaNeighborhoodVS", "vs_4_0", true, reinterpret_cast<void**>(&variant.neighborhoodVs)},
        {"SmaaNeighborhoodPS", "ps_4_0", false, reinterpret_cast<void**>(&variant.neighborhoodPs)},
        {"SmaaResolvePSMain", "ps_4_0", false, reinterpret_cast<void**>(&variant.resolvePs)},
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
        static_cast<std::uint64_t>(kSearchPitch) * kSearchHeight;
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
    std::uint32_t phase) noexcept {
    const float clear[4]{};
    deferredContext_->ClearRenderTargetView(edgeRtv_, clear);
    deferredContext_->ClearRenderTargetView(blendRtv_, clear);
    struct Constants {
        float metrics[4];
        float subsample[4];
        float resolveParameters[4];
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

    currentSpatialIndex_[eye] ^= 1U;
    return true;
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
