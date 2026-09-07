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

// Native D3D11 TSCMAA pass: the pinned Intel/GameTechDev CMAA2 compute
// implementation supplies the spatial pass, followed by a VR motion-vector
// temporal resolve. All shader sources and notices are embedded in version.dll.
class TscmaaPass final {
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

    struct EyeResourceDiagnostics {
        std::uintptr_t motion = 0;
        std::uintptr_t spatial = 0;
        std::uintptr_t resolvedHistory = 0;
        std::uintptr_t resolvedOutput = 0;
        std::uint64_t motionPairToken = 0;
        std::uint32_t nextResolvedWriteIndex = 0;
        bool historyValid = false;
    };

    struct ResourceDiagnostics {
        std::array<EyeResourceDiagnostics, 2> eyes{};
        std::uintptr_t workingEdges = 0;
        std::uintptr_t workingShapeCandidates = 0;
        std::uintptr_t workingDeferredItems = 0;
    };

    // GPU-counted temporal work for one resolved pair. Sums are 1/1000 fixed
    // point: meanWeight = weightMilliSum / (1000 * blendedPixels), meanDelta
    // likewise. Filled only when ResolveStereo receives a stats pointer.
    struct TemporalEyeStats {
        std::uint32_t edgeCandidatePixels = 0;
        std::uint32_t blendedPixels = 0;
        std::uint32_t weightMilliSum = 0;
        std::uint32_t deltaMilliSum = 0;
    };

    struct TemporalStats {
        std::array<TemporalEyeStats, 2> eyes{};
        bool valid = false;
    };

    TscmaaPass() = default;
    ~TscmaaPass();

    TscmaaPass(const TscmaaPass&) = delete;
    TscmaaPass& operator=(const TscmaaPass&) = delete;

    [[nodiscard]] bool Prepare(
        ID3D11Device* device,
        const D3D11_TEXTURE2D_DESC& colorDescription,
        bool srgb,
        PrepareDiagnostics* diagnostics = nullptr) noexcept;

    [[nodiscard]] bool CaptureMotionVector(
        ID3D11DeviceContext* immediateContext,
        std::size_t eye,
        ID3D11Texture2D* source,
        std::uint64_t pairToken) noexcept;
    [[nodiscard]] bool HasFreshMotionVectors(std::uint64_t pairToken) const noexcept;
    [[nodiscard]] bool ReadMotionVectorValues(
        ID3D11DeviceContext* immediateContext,
        std::size_t eye,
        MotionVectorValueDiagnostics& diagnostics) const noexcept;

    // UI quality is 0=low, 1=medium, 2=high and maps to official CMAA2
    // presets 1/2/3 (edge thresholds .10/.07/.05). History weight is the
    // accepted stereo.209 uncapped contract: 0.7 through the inclusive 2px
    // deadzone, then official x30 UV attenuation only. Returned textures
    // remain owned by this pass. A non-null stats pointer additionally
    // dispatches the diagnostic counter shader and blocks on a 32-byte
    // readback, so it must stay on the sparse diagnostics cadence.
    [[nodiscard]] bool ResolveStereo(
        ID3D11DeviceContext* immediateContext,
        const std::array<ID3D11Texture2D*, 2>& colors,
        bool srgb,
        int quality,
        std::uint64_t pairToken,
        std::array<ID3D11Texture2D*, 2>& outputs,
        TemporalStats* stats = nullptr) noexcept;

    // CPU mirror of the shader's history-weight function, intentionally public
    // so offline/WARP contracts can pin pixel-domain x/y behavior and cutoffs.
    [[nodiscard]] static float TemporalHistoryWeight(
        float motionXUv,
        float motionYUv,
        UINT width,
        UINT height) noexcept;

    void ResetHistory() noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool IsPrepared() const noexcept;
    [[nodiscard]] bool HasHistory() const noexcept;
    [[nodiscard]] Status LastStatus() const noexcept;
    [[nodiscard]] HRESULT LastHresult() const noexcept;
    [[nodiscard]] MotionVectorDiagnostics LastMotionVectorDiagnostics() const noexcept;
    [[nodiscard]] const std::string& LastCompilerError() const noexcept;
    [[nodiscard]] std::uint64_t AllocationBytes() const noexcept;
    [[nodiscard]] ResourceDiagnostics GetResourceDiagnostics() const noexcept;
    [[nodiscard]] static const char* StatusName(Status status) noexcept;

private:
    struct ShaderVariant {
        ID3D11ComputeShader* edges = nullptr;
        ID3D11ComputeShader* computeDispatchArgs = nullptr;
        ID3D11ComputeShader* processCandidates = nullptr;
        ID3D11ComputeShader* deferredApply = nullptr;
    };

    struct ColorTarget {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11UnorderedAccessView* uav = nullptr;
    };

    struct MotionSnapshot {
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        std::uint64_t pairToken = 0;
    };

    struct BufferTarget {
        ID3D11Buffer* buffer = nullptr;
        ID3D11UnorderedAccessView* uav = nullptr;
        UINT byteWidth = 0;
    };

    [[nodiscard]] bool LoadAndCompileShaders() noexcept;
    [[nodiscard]] bool CompileVariant(
        int quality,
        bool srgb,
        const std::string& source) noexcept;
    [[nodiscard]] bool CompileComputeShader(
        const std::string& source,
        const char* entry,
        ID3D11ComputeShader** shader) noexcept;
    [[nodiscard]] bool CompileFullscreenShaders() noexcept;
    [[nodiscard]] bool CreateFixedResources() noexcept;
    [[nodiscard]] bool CreateSizeResources(
        const D3D11_TEXTURE2D_DESC& colorDescription,
        bool srgb) noexcept;
    [[nodiscard]] bool CreateColorTarget(ColorTarget& target, bool unorderedAccess) noexcept;
    [[nodiscard]] bool CreateWorkingTexture(
        UINT width,
        UINT height,
        DXGI_FORMAT format,
        ID3D11Texture2D** texture,
        ID3D11ShaderResourceView** srv,
        ID3D11UnorderedAccessView** uav) noexcept;
    [[nodiscard]] bool CreateStructuredBuffer(
        UINT elementCount,
        UINT stride,
        BufferTarget& target) noexcept;
    [[nodiscard]] bool CreateRawBuffer(
        UINT byteWidth,
        UINT miscFlags,
        const void* initialData,
        BufferTarget& target) noexcept;
    [[nodiscard]] bool EnsureColorSourceView(
        std::size_t eye,
        ID3D11Texture2D* source) noexcept;
    [[nodiscard]] bool EnsureMotionSnapshot(
        std::size_t eye,
        ID3D11Texture2D* source) noexcept;
    [[nodiscard]] bool RecordEye(
        std::size_t eye,
        int quality,
        bool collectStats) noexcept;
    void BindFullscreenState() noexcept;
    void SetViewport(UINT width, UINT height) noexcept;
    void UnbindGraphicsResources() noexcept;
    void UnbindComputeResources() noexcept;
    void UpdateAllocationBytes() noexcept;
    void ReleaseSizeResources() noexcept;
    void ReleaseShaders() noexcept;
    void SetFailure(Status status, HRESULT result) noexcept;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* deferredContext_ = nullptr;
    std::array<std::array<ShaderVariant, 2>, 3> shaders_{};
    ID3D11VertexShader* fullscreenVs_ = nullptr;
    ID3D11PixelShader* copyPs_ = nullptr;
    ID3D11PixelShader* temporalPs_ = nullptr;
    ID3D11ComputeShader* temporalStatsCs_ = nullptr;
    ID3D11RasterizerState* rasterizerState_ = nullptr;
    ID3D11DepthStencilState* depthStencilState_ = nullptr;
    ID3D11SamplerState* linearSampler_ = nullptr;
    ID3D11SamplerState* pointSampler_ = nullptr;
    ID3D11Buffer* constants_ = nullptr;

    ID3D11Texture2D* workingEdges_ = nullptr;
    ID3D11ShaderResourceView* workingEdgesSrv_ = nullptr;
    ID3D11UnorderedAccessView* workingEdgesUav_ = nullptr;
    ID3D11Texture2D* workingDeferredHeads_ = nullptr;
    ID3D11UnorderedAccessView* workingDeferredHeadsUav_ = nullptr;
    BufferTarget workingShapeCandidates_{};
    BufferTarget workingDeferredLocations_{};
    BufferTarget workingDeferredItems_{};
    BufferTarget workingControl_{};
    BufferTarget workingIndirect_{};
    BufferTarget temporalStats_{};
    ID3D11Buffer* temporalStatsStaging_ = nullptr;

    std::array<ColorTarget, 2> spatial_{};
    std::array<std::array<ColorTarget, 2>, 2> resolved_{};
    std::array<ID3D11Texture2D*, 2> colorSourceIdentity_{};
    std::array<ID3D11ShaderResourceView*, 2> colorSourceSrv_{};
    std::array<MotionSnapshot, 2> motion_{};
    std::array<UINT, 2> motionBytesPerPixel_{};

    D3D11_TEXTURE2D_DESC colorDescription_{};
    DXGI_FORMAT colorViewFormat_ = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT colorLinearFormat_ = DXGI_FORMAT_UNKNOWN;
    bool srgb_ = false;
    std::array<std::uint32_t, 2> nextResolvedWriteIndex_{};
    std::array<std::uint32_t, 2> lastResolvedOutputIndex_{};
    std::array<bool, 2> historyValid_{};
    std::uint64_t allocationBytes_ = 0;
    Status lastStatus_ = Status::NotInitialized;
    HRESULT lastHresult_ = S_OK;
    MotionVectorDiagnostics lastMotionVectorDiagnostics_{};
    std::string lastCompilerError_;
};

} // namespace gakumas::vr::d3d11
