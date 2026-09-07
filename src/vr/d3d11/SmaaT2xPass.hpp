#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gakumas::vr::d3d11 {

// Reference SMAA T2x implementation. The official SMAA.hlsl and lookup
// textures are embedded as RCDATA in version.dll; no game-side shader files
// are loaded at runtime.
class SmaaT2xPass final {
public:
    enum class Status {
        Ready,
        NotInitialized,
        InvalidArgument,
        DescriptionMismatch,
        UnsupportedFormat,
        UnsupportedMotionVectors,
        EmbeddedResourceMissing,
        ShaderCompilationFailed,
        DeviceResourceFailed,
        ViewCreationFailed,
        StaleMotionVectors,
        CommandRecordingFailed,
    };

    struct PrepareDiagnostics {
        std::uint64_t allocationBytes = 0;
        double allocationMilliseconds = 0.0;
        UINT width = 0;
        UINT height = 0;
        DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    };

    enum MotionVectorMismatch : std::uint32_t {
        MotionVectorMismatchNone = 0,
        MotionVectorMismatchFormat = 1U << 0U,
        MotionVectorMismatchMipLevels = 1U << 1U,
        MotionVectorMismatchArraySize = 1U << 2U,
        MotionVectorMismatchSampleCount = 1U << 3U,
        MotionVectorMismatchDimensions = 1U << 4U,
    };

    struct MotionVectorDiagnostics {
        D3D11_TEXTURE2D_DESC source{};
        UINT expectedWidth = 0;
        UINT expectedHeight = 0;
        DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
        UINT bytesPerPixel = 0;
        std::uint32_t mismatchMask = MotionVectorMismatchNone;
    };

    struct MotionVectorValueDiagnostics {
        UINT sampleCount = 0;
        UINT finiteSampleCount = 0;
        UINT nonFiniteSampleCount = 0;
        float minimumX = 0.0F;
        float maximumX = 0.0F;
        float minimumY = 0.0F;
        float maximumY = 0.0F;
        float meanMagnitude = 0.0F;
        float maximumMagnitude = 0.0F;
        float minimumZ = 0.0F;
        float maximumZ = 0.0F;
        float minimumW = 0.0F;
        float maximumW = 0.0F;
    };

    static constexpr std::size_t ReprojectionCandidateCount = 4;
    enum class ProbeGroup : std::size_t {
        Source = 0,
        Age,
        Jitter,
        Resolve,
        Output,
        Count,
    };
    static constexpr std::size_t ProbeGroupCount =
        static_cast<std::size_t>(ProbeGroup::Count);

    struct ReprojectionAlignmentEyeDiagnostics {
        std::array<UINT, ReprojectionCandidateCount> validSampleCount{};
        std::array<float, ReprojectionCandidateCount> minimumValue{};
        std::array<float, ReprojectionCandidateCount> meanValue{};
        std::array<float, ReprojectionCandidateCount> maximumValue{};
    };

    struct ReprojectionAlignmentDiagnostics {
        std::array<
            std::array<ReprojectionAlignmentEyeDiagnostics, 2>,
            ProbeGroupCount> groups{};
    };

    struct EyeResourceDiagnostics {
        std::uintptr_t motion = 0;
        std::uintptr_t currentMotionProbe = 0;
        std::uintptr_t previousMotionProbe = 0;
        std::uintptr_t currentSpatial = 0;
        std::uintptr_t previousSpatial = 0;
        std::uintptr_t resolved = 0;
        std::uint64_t motionPairToken = 0;
        std::uint64_t currentMotionProbePairToken = 0;
        std::uint64_t previousMotionProbePairToken = 0;
        std::uint32_t nextSpatialWriteIndex = 0;
        bool historyValid = false;
    };

    struct ResourceDiagnostics {
        std::array<EyeResourceDiagnostics, 2> eyes{};
    };

    SmaaT2xPass() = default;
    ~SmaaT2xPass();

    SmaaT2xPass(const SmaaT2xPass&) = delete;
    SmaaT2xPass& operator=(const SmaaT2xPass&) = delete;

    [[nodiscard]] bool Prepare(
        ID3D11Device* device,
        const D3D11_TEXTURE2D_DESC& colorDescription,
        bool srgb,
        PrepareDiagnostics* diagnostics = nullptr) noexcept;

    // Copies a motion surface at an externally verified ordered boundary. Unity
    // records render-pass commands asynchronously, so a CPU pass return alone
    // is not sufficient evidence that the shared RTHandle contains that eye.
    [[nodiscard]] bool CaptureMotionVector(
        ID3D11DeviceContext* immediateContext,
        std::size_t eye,
        ID3D11Texture2D* source,
        std::uint64_t pairToken) noexcept;

    [[nodiscard]] bool HasFreshMotionVectors(std::uint64_t pairToken) const noexcept;

    // Diagnostics-only sparse readback. It samples distributed scan lines from
    // the immutable per-eye copy, never from the shared Unity RTHandle.
    [[nodiscard]] bool ReadMotionVectorValues(
        ID3D11DeviceContext* immediateContext,
        std::size_t eye,
        MotionVectorValueDiagnostics& diagnostics) const noexcept;

    // quality is 0=low, 1=medium, 2=high. phase is 0 or 1. Returned textures
    // are owned by this pass and remain valid until Reset()/resource rebuild.
    [[nodiscard]] bool ResolveStereo(
        ID3D11DeviceContext* immediateContext,
        const std::array<ID3D11Texture2D*, 2>& colors,
        bool srgb,
        int quality,
        std::uint32_t phase,
        std::uint64_t pairToken,
        bool collectReprojectionAlignment,
        std::array<ID3D11Texture2D*, 2>& outputs) noexcept;

    // Diagnostics-only GPU score covering MV source/age, jitter compensation,
    // official resolve telemetry and resolved/current output delta.
    [[nodiscard]] bool ReadReprojectionAlignment(
        ID3D11DeviceContext* immediateContext,
        ReprojectionAlignmentDiagnostics& diagnostics) noexcept;
    [[nodiscard]] ResourceDiagnostics GetResourceDiagnostics() const noexcept;
    [[nodiscard]] static const char* ProbeGroupName(ProbeGroup group) noexcept;

    void ResetHistory() noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool IsPrepared() const noexcept;
    [[nodiscard]] bool HasHistory() const noexcept;
    [[nodiscard]] Status LastStatus() const noexcept;
    [[nodiscard]] HRESULT LastHresult() const noexcept;
    [[nodiscard]] MotionVectorDiagnostics LastMotionVectorDiagnostics() const noexcept;
    [[nodiscard]] const std::string& LastCompilerError() const noexcept;
    [[nodiscard]] std::uint64_t AllocationBytes() const noexcept;
    [[nodiscard]] static const char* StatusName(Status status) noexcept;

private:
    struct ShaderVariant {
        ID3D11VertexShader* edgeVs = nullptr;
        ID3D11PixelShader* edgePs = nullptr;
        ID3D11VertexShader* blendVs = nullptr;
        ID3D11PixelShader* blendPs = nullptr;
        ID3D11VertexShader* neighborhoodVs = nullptr;
        ID3D11PixelShader* neighborhoodPs = nullptr;
        ID3D11PixelShader* resolvePs = nullptr;
        ID3D11PixelShader* sourceProbePs = nullptr;
        ID3D11PixelShader* ageProbePs = nullptr;
        ID3D11PixelShader* jitterProbePs = nullptr;
        ID3D11PixelShader* resolveProbePs = nullptr;
        ID3D11PixelShader* outputProbePs = nullptr;
        ID3D11PixelShader* motionProbePs = nullptr;
    };

    struct ColorTarget {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
    };

    struct MotionSnapshot {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        std::uint64_t pairToken = 0;
    };

    [[nodiscard]] bool LoadAndCompileShaders() noexcept;
    [[nodiscard]] bool CompileVariant(int quality, const std::string& officialSource) noexcept;
    [[nodiscard]] bool CompileShader(
        const std::string& source,
        const char* entry,
        const char* profile,
        ID3DBlob** blob) noexcept;
    [[nodiscard]] bool CreateFixedResources() noexcept;
    [[nodiscard]] bool CreateAlignmentResources() noexcept;
    [[nodiscard]] bool CreateSizeResources(
        const D3D11_TEXTURE2D_DESC& colorDescription,
        bool srgb) noexcept;
    [[nodiscard]] bool CreateLookupTexture(
        int resourceId,
        std::size_t dataOffset,
        UINT width,
        UINT height,
        UINT pitch,
        DXGI_FORMAT format,
        ID3D11Texture2D** texture,
        ID3D11ShaderResourceView** view) noexcept;
    [[nodiscard]] bool CreateColorTarget(ColorTarget& target) noexcept;
    [[nodiscard]] bool CreateSmallTarget(
        ColorTarget& target,
        UINT width,
        UINT height,
        DXGI_FORMAT format) noexcept;
    [[nodiscard]] bool CreateScratchTarget(
        DXGI_FORMAT format,
        ID3D11Texture2D** texture,
        ID3D11ShaderResourceView** srv,
        ID3D11RenderTargetView** rtv) noexcept;
    [[nodiscard]] bool EnsureColorSourceView(
        std::size_t eye,
        ID3D11Texture2D* source) noexcept;
    [[nodiscard]] bool EnsureMotionSnapshot(
        std::size_t eye,
        ID3D11Texture2D* source) noexcept;
    void UpdateAllocationBytes() noexcept;
    [[nodiscard]] bool RecordEye(
        std::size_t eye,
        ID3D11Texture2D* color,
        int quality,
        std::uint32_t phase,
        bool collectReprojectionAlignment) noexcept;
    void RecordProbeGroup(
        std::size_t eye,
        ProbeGroup group,
        ID3D11PixelShader* shader,
        const std::array<ID3D11ShaderResourceView*, 6>& inputs) noexcept;
    void RecordMotionProbe(std::size_t eye, ID3D11PixelShader* shader) noexcept;
    void BindFullscreenState() noexcept;
    void SetViewport(UINT width, UINT height) noexcept;
    void UnbindShaderResources() noexcept;
    void ReleaseSizeResources() noexcept;
    void ReleaseShaders() noexcept;
    void SetFailure(Status status, HRESULT result) noexcept;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* deferredContext_ = nullptr;
    std::array<ShaderVariant, 3> shaders_{};
    ID3D11RasterizerState* rasterizerState_ = nullptr;
    ID3D11DepthStencilState* depthStencilState_ = nullptr;
    ID3D11SamplerState* linearSampler_ = nullptr;
    ID3D11SamplerState* pointSampler_ = nullptr;
    ID3D11Buffer* constants_ = nullptr;
    ID3D11Texture2D* areaTexture_ = nullptr;
    ID3D11ShaderResourceView* areaSrv_ = nullptr;
    ID3D11Texture2D* searchTexture_ = nullptr;
    ID3D11ShaderResourceView* searchSrv_ = nullptr;
    std::array<ID3D11Texture2D*, 2> alignmentTexture_{};
    std::array<ID3D11RenderTargetView*, 2> alignmentRtv_{};
    std::array<std::array<ID3D11Texture2D*, ProbeGroupCount>, 2>
        alignmentStaging_{};
    std::array<bool, 2> alignmentPending_{};
    std::array<std::array<ColorTarget, 2>, 2> motionProbe_{};
    std::array<std::uint32_t, 2> motionProbeCurrentIndex_{};
    std::array<bool, 2> motionProbeValid_{};
    std::array<std::array<std::uint64_t, 2>, 2> motionProbePairTokens_{};

    ID3D11Texture2D* edgeTexture_ = nullptr;
    ID3D11ShaderResourceView* edgeSrv_ = nullptr;
    ID3D11RenderTargetView* edgeRtv_ = nullptr;
    ID3D11Texture2D* blendTexture_ = nullptr;
    ID3D11ShaderResourceView* blendSrv_ = nullptr;
    ID3D11RenderTargetView* blendRtv_ = nullptr;
    std::array<std::array<ColorTarget, 2>, 2> spatial_{};
    std::array<ColorTarget, 2> resolved_{};
    std::array<ID3D11Texture2D*, 2> colorSourceIdentity_{};
    std::array<ID3D11ShaderResourceView*, 2> colorSourceSrv_{};
    std::array<MotionSnapshot, 2> motion_{};
    std::array<UINT, 2> motionBytesPerPixel_{};

    D3D11_TEXTURE2D_DESC colorDescription_{};
    DXGI_FORMAT colorViewFormat_ = DXGI_FORMAT_UNKNOWN;
    std::array<std::uint32_t, 2> currentSpatialIndex_{};
    std::array<bool, 2> historyValid_{};
    std::uint64_t allocationBytes_ = 0;
    Status lastStatus_ = Status::NotInitialized;
    HRESULT lastHresult_ = S_OK;
    MotionVectorDiagnostics lastMotionVectorDiagnostics_{};
    std::string lastCompilerError_;
};

} // namespace gakumas::vr::d3d11
