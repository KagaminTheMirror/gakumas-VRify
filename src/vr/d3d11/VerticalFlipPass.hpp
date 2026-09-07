#pragma once

#include <d3d11.h>

#include <array>
#include <cstdint>
#include <vector>

namespace gakumas::vr::d3d11 {

// Copies two Unity eye RenderTextures into the two slices of an OpenXR
// projection swapchain image while reversing only the pixel-row order. The
// pass uses Texture2D.Load, so it performs no filtering or half-pixel sampling.
class VerticalFlipPass final {
public:
    enum class Status {
        Ready,
        NotInitialized,
        InvalidArgument,
        DescriptionMismatch,
        UnsupportedFormat,
        ShaderCompilationFailed,
        DeviceResourceFailed,
        ViewCreationFailed,
        CommandRecordingFailed,
    };

    VerticalFlipPass() = default;
    ~VerticalFlipPass();

    VerticalFlipPass(const VerticalFlipPass&) = delete;
    VerticalFlipPass& operator=(const VerticalFlipPass&) = delete;

    [[nodiscard]] bool Initialize(ID3D11Device* device) noexcept;
    [[nodiscard]] bool FlipStereo(
        ID3D11DeviceContext* immediateContext,
        const std::array<ID3D11Texture2D*, 2>& sources,
        ID3D11Texture2D* destination) noexcept;
    void Reset() noexcept;

    [[nodiscard]] Status LastStatus() const noexcept;
    [[nodiscard]] HRESULT LastHresult() const noexcept;
    [[nodiscard]] DXGI_FORMAT ViewFormat() const noexcept;
    [[nodiscard]] static const char* StatusName(Status status) noexcept;

private:
    struct SourceView {
        ID3D11Texture2D* identity = nullptr;
        ID3D11Texture2D* scratch = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
    };

    struct DestinationView {
        ID3D11Texture2D* identity = nullptr;
        UINT arraySlice = 0;
        ID3D11RenderTargetView* view = nullptr;
    };

    [[nodiscard]] bool SelectViewFormat(
        ID3D11Texture2D* source,
        ID3D11Texture2D* destination) noexcept;
    [[nodiscard]] bool EnsureSourceView(
        std::size_t eye,
        ID3D11Texture2D* source) noexcept;
    [[nodiscard]] ID3D11RenderTargetView* EnsureDestinationView(
        ID3D11Texture2D* destination,
        UINT arraySlice) noexcept;
    [[nodiscard]] bool CreateSourceView(
        ID3D11Texture2D* source,
        DXGI_FORMAT viewFormat,
        ID3D11Texture2D** scratch,
        ID3D11ShaderResourceView** view) noexcept;
    [[nodiscard]] bool CreateDestinationView(
        ID3D11Texture2D* destination,
        UINT arraySlice,
        DXGI_FORMAT viewFormat,
        ID3D11RenderTargetView** view) noexcept;
    void ReleaseCachedViews() noexcept;
    void SetFailure(Status status, HRESULT result) noexcept;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* deferredContext_ = nullptr;
    ID3D11VertexShader* vertexShader_ = nullptr;
    ID3D11PixelShader* pixelShader_ = nullptr;
    ID3D11RasterizerState* rasterizerState_ = nullptr;
    std::array<SourceView, 2> sourceViews_{};
    std::vector<DestinationView> destinationViews_;
    DXGI_FORMAT viewFormat_ = DXGI_FORMAT_UNKNOWN;
    Status lastStatus_ = Status::NotInitialized;
    HRESULT lastHresult_ = S_OK;
};

} // namespace gakumas::vr::d3d11
