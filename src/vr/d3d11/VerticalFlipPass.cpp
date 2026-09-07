#include "VerticalFlipPass.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace gakumas::vr::d3d11 {
namespace {

constexpr char kVertexShader[] = R"(
float4 main(uint vertexId : SV_VertexID) : SV_Position {
    float2 corner = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

constexpr char kPixelShader[] = R"(
Texture2D<float4> sourceTexture : register(t0);

float4 main(float4 position : SV_Position) : SV_Target {
    uint width = 0;
    uint height = 0;
    sourceTexture.GetDimensions(width, height);
    int2 destinationPixel = int2(position.xy);
    int2 sourcePixel = int2(destinationPixel.x, int(height) - 1 - destinationPixel.y);
    return sourceTexture.Load(int3(sourcePixel, 0));
}
)";

template <typename T>
void ReleaseObject(T*& value) noexcept {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
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

std::array<DXGI_FORMAT, 2> TypedFormats(int family) noexcept {
    if (family == 1) {
        return {
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        };
    }
    if (family == 2) {
        return {
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        };
    }
    return {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
}

} // namespace

VerticalFlipPass::~VerticalFlipPass() {
    Reset();
}

bool VerticalFlipPass::Initialize(ID3D11Device* device) noexcept {
    if (device == nullptr) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    if (device_ == device && deferredContext_ != nullptr &&
        vertexShader_ != nullptr && pixelShader_ != nullptr &&
        rasterizerState_ != nullptr) {
        lastStatus_ = Status::Ready;
        lastHresult_ = S_OK;
        return true;
    }
    Reset();

    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT result = D3DCompile(
        kVertexShader,
        std::strlen(kVertexShader),
        "VerticalFlipPass.vs",
        nullptr,
        nullptr,
        "main",
        "vs_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &vertexBlob,
        &errors);
    ReleaseObject(errors);
    if (FAILED(result) || vertexBlob == nullptr) {
        ReleaseObject(vertexBlob);
        SetFailure(Status::ShaderCompilationFailed, result);
        return false;
    }
    result = D3DCompile(
        kPixelShader,
        std::strlen(kPixelShader),
        "VerticalFlipPass.ps",
        nullptr,
        nullptr,
        "main",
        "ps_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS,
        0,
        &pixelBlob,
        &errors);
    ReleaseObject(errors);
    if (FAILED(result) || pixelBlob == nullptr) {
        ReleaseObject(vertexBlob);
        ReleaseObject(pixelBlob);
        SetFailure(Status::ShaderCompilationFailed, result);
        return false;
    }

    device->AddRef();
    device_ = device;
    result = device_->CreateDeferredContext(0, &deferredContext_);
    if (SUCCEEDED(result)) {
        result = device_->CreateVertexShader(
            vertexBlob->GetBufferPointer(),
            vertexBlob->GetBufferSize(),
            nullptr,
            &vertexShader_);
    }
    if (SUCCEEDED(result)) {
        result = device_->CreatePixelShader(
            pixelBlob->GetBufferPointer(),
            pixelBlob->GetBufferSize(),
            nullptr,
            &pixelShader_);
    }
    ReleaseObject(vertexBlob);
    ReleaseObject(pixelBlob);

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    if (SUCCEEDED(result)) {
        result = device_->CreateRasterizerState(&rasterizer, &rasterizerState_);
    }
    if (FAILED(result)) {
        Reset();
        SetFailure(Status::DeviceResourceFailed, result);
        return false;
    }
    lastStatus_ = Status::Ready;
    lastHresult_ = S_OK;
    return true;
}

bool VerticalFlipPass::FlipStereo(
    ID3D11DeviceContext* immediateContext,
    const std::array<ID3D11Texture2D*, 2>& sources,
    ID3D11Texture2D* destination) noexcept {
    if (device_ == nullptr || deferredContext_ == nullptr ||
        vertexShader_ == nullptr || pixelShader_ == nullptr ||
        rasterizerState_ == nullptr) {
        SetFailure(Status::NotInitialized, E_UNEXPECTED);
        return false;
    }
    if (immediateContext == nullptr || destination == nullptr ||
        sources[0] == nullptr || sources[1] == nullptr) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }

    D3D11_TEXTURE2D_DESC leftDescription{};
    D3D11_TEXTURE2D_DESC rightDescription{};
    D3D11_TEXTURE2D_DESC destinationDescription{};
    sources[0]->GetDesc(&leftDescription);
    sources[1]->GetDesc(&rightDescription);
    destination->GetDesc(&destinationDescription);
    const bool descriptionsMatch =
        leftDescription.Width == rightDescription.Width &&
        leftDescription.Height == rightDescription.Height &&
        leftDescription.Format == rightDescription.Format &&
        leftDescription.ArraySize == 1 && rightDescription.ArraySize == 1 &&
        leftDescription.MipLevels == 1 && rightDescription.MipLevels == 1 &&
        leftDescription.SampleDesc.Count == 1 &&
        rightDescription.SampleDesc.Count == 1 &&
        destinationDescription.Width == leftDescription.Width &&
        destinationDescription.Height == leftDescription.Height &&
        destinationDescription.ArraySize >= 2 &&
        destinationDescription.MipLevels == 1 &&
        destinationDescription.SampleDesc.Count == 1 &&
        FormatFamily(leftDescription.Format) != 0 &&
        FormatFamily(leftDescription.Format) ==
            FormatFamily(destinationDescription.Format);
    if (!descriptionsMatch) {
        SetFailure(Status::DescriptionMismatch, E_INVALIDARG);
        return false;
    }

    if (viewFormat_ == DXGI_FORMAT_UNKNOWN &&
        !SelectViewFormat(sources[0], destination)) {
        return false;
    }
    for (std::size_t eye = 0; eye < sources.size(); ++eye) {
        if (!EnsureSourceView(eye, sources[eye]) ||
            EnsureDestinationView(destination, static_cast<UINT>(eye)) == nullptr) {
            return false;
        }
    }

    deferredContext_->ClearState();
    for (std::size_t eye = 0; eye < sources.size(); ++eye) {
        if (sourceViews_[eye].scratch != nullptr) {
            deferredContext_->CopyResource(sourceViews_[eye].scratch, sources[eye]);
        }
    }
    deferredContext_->IASetInputLayout(nullptr);
    deferredContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferredContext_->VSSetShader(vertexShader_, nullptr, 0);
    deferredContext_->PSSetShader(pixelShader_, nullptr, 0);
    deferredContext_->RSSetState(rasterizerState_);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(leftDescription.Width);
    viewport.Height = static_cast<float>(leftDescription.Height);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;
    deferredContext_->RSSetViewports(1, &viewport);

    for (std::size_t eye = 0; eye < sources.size(); ++eye) {
        ID3D11RenderTargetView* target =
            EnsureDestinationView(destination, static_cast<UINT>(eye));
        deferredContext_->OMSetRenderTargets(1, &target, nullptr);
        ID3D11ShaderResourceView* sourceView = sourceViews_[eye].view;
        deferredContext_->PSSetShaderResources(0, 1, &sourceView);
        deferredContext_->Draw(3, 0);
        ID3D11ShaderResourceView* noSource = nullptr;
        deferredContext_->PSSetShaderResources(0, 1, &noSource);
    }
    deferredContext_->OMSetRenderTargets(0, nullptr, nullptr);

    ID3D11CommandList* commandList = nullptr;
    HRESULT result = deferredContext_->FinishCommandList(FALSE, &commandList);
    if (FAILED(result) || commandList == nullptr) {
        ReleaseObject(commandList);
        SetFailure(Status::CommandRecordingFailed, result);
        return false;
    }
    immediateContext->ExecuteCommandList(commandList, TRUE);
    commandList->Release();
    lastStatus_ = Status::Ready;
    lastHresult_ = S_OK;
    return true;
}

void VerticalFlipPass::Reset() noexcept {
    ReleaseCachedViews();
    ReleaseObject(rasterizerState_);
    ReleaseObject(pixelShader_);
    ReleaseObject(vertexShader_);
    ReleaseObject(deferredContext_);
    ReleaseObject(device_);
    viewFormat_ = DXGI_FORMAT_UNKNOWN;
    lastStatus_ = Status::NotInitialized;
    lastHresult_ = S_OK;
}

VerticalFlipPass::Status VerticalFlipPass::LastStatus() const noexcept {
    return lastStatus_;
}

HRESULT VerticalFlipPass::LastHresult() const noexcept {
    return lastHresult_;
}

DXGI_FORMAT VerticalFlipPass::ViewFormat() const noexcept {
    return viewFormat_;
}

const char* VerticalFlipPass::StatusName(Status status) noexcept {
    switch (status) {
    case Status::Ready: return "ready";
    case Status::NotInitialized: return "not-initialized";
    case Status::InvalidArgument: return "invalid-argument";
    case Status::DescriptionMismatch: return "description-mismatch";
    case Status::UnsupportedFormat: return "unsupported-format";
    case Status::ShaderCompilationFailed: return "shader-compilation-failed";
    case Status::DeviceResourceFailed: return "device-resource-failed";
    case Status::ViewCreationFailed: return "view-creation-failed";
    case Status::CommandRecordingFailed: return "command-recording-failed";
    }
    return "unknown";
}

bool VerticalFlipPass::SelectViewFormat(
    ID3D11Texture2D* source,
    ID3D11Texture2D* destination) noexcept {
    D3D11_TEXTURE2D_DESC sourceDescription{};
    D3D11_TEXTURE2D_DESC destinationDescription{};
    source->GetDesc(&sourceDescription);
    destination->GetDesc(&destinationDescription);
    const int family = FormatFamily(sourceDescription.Format);
    if (family == 0 || family != FormatFamily(destinationDescription.Format)) {
        SetFailure(Status::UnsupportedFormat, DXGI_ERROR_UNSUPPORTED);
        return false;
    }
    for (DXGI_FORMAT candidate : TypedFormats(family)) {
        ID3D11Texture2D* scratch = nullptr;
        ID3D11ShaderResourceView* sourceView = nullptr;
        ID3D11RenderTargetView* destinationView = nullptr;
        const bool sourceReady = CreateSourceView(
            source, candidate, &scratch, &sourceView);
        const bool destinationReady = sourceReady && CreateDestinationView(
            destination, 0, candidate, &destinationView);
        ReleaseObject(destinationView);
        ReleaseObject(sourceView);
        ReleaseObject(scratch);
        if (sourceReady && destinationReady) {
            viewFormat_ = candidate;
            lastStatus_ = Status::Ready;
            lastHresult_ = S_OK;
            return true;
        }
    }
    SetFailure(Status::ViewCreationFailed, DXGI_ERROR_UNSUPPORTED);
    return false;
}

bool VerticalFlipPass::EnsureSourceView(
    std::size_t eye,
    ID3D11Texture2D* source) noexcept {
    if (eye >= sourceViews_.size() || source == nullptr ||
        viewFormat_ == DXGI_FORMAT_UNKNOWN) {
        SetFailure(Status::InvalidArgument, E_INVALIDARG);
        return false;
    }
    auto& cached = sourceViews_[eye];
    if (cached.identity == source && cached.view != nullptr) {
        return true;
    }
    ReleaseObject(cached.view);
    ReleaseObject(cached.scratch);
    cached.identity = nullptr;
    if (!CreateSourceView(
            source, viewFormat_, &cached.scratch, &cached.view)) {
        SetFailure(Status::ViewCreationFailed, lastHresult_);
        return false;
    }
    cached.identity = source;
    return true;
}

ID3D11RenderTargetView* VerticalFlipPass::EnsureDestinationView(
    ID3D11Texture2D* destination,
    UINT arraySlice) noexcept {
    for (auto& cached : destinationViews_) {
        if (cached.identity == destination && cached.arraySlice == arraySlice) {
            return cached.view;
        }
    }
    DestinationView cached{};
    if (!CreateDestinationView(
            destination, arraySlice, viewFormat_, &cached.view)) {
        SetFailure(Status::ViewCreationFailed, lastHresult_);
        return nullptr;
    }
    cached.identity = destination;
    cached.arraySlice = arraySlice;
    destinationViews_.push_back(cached);
    return destinationViews_.back().view;
}

bool VerticalFlipPass::CreateSourceView(
    ID3D11Texture2D* source,
    DXGI_FORMAT viewFormat,
    ID3D11Texture2D** scratch,
    ID3D11ShaderResourceView** view) noexcept {
    if (source == nullptr || scratch == nullptr || view == nullptr) {
        lastHresult_ = E_INVALIDARG;
        return false;
    }
    *scratch = nullptr;
    *view = nullptr;
    D3D11_TEXTURE2D_DESC description{};
    source->GetDesc(&description);
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
    viewDescription.Format = viewFormat;
    viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDescription.Texture2D.MostDetailedMip = 0;
    viewDescription.Texture2D.MipLevels = 1;
    HRESULT result = device_->CreateShaderResourceView(
        source, &viewDescription, view);
    if (SUCCEEDED(result) && *view != nullptr) {
        lastHresult_ = S_OK;
        return true;
    }
    ReleaseObject(*view);

    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    description.CPUAccessFlags = 0;
    description.MiscFlags = 0;
    result = device_->CreateTexture2D(&description, nullptr, scratch);
    if (SUCCEEDED(result) && *scratch != nullptr) {
        result = device_->CreateShaderResourceView(
            *scratch, &viewDescription, view);
    }
    if (FAILED(result) || *scratch == nullptr || *view == nullptr) {
        ReleaseObject(*view);
        ReleaseObject(*scratch);
        lastHresult_ = result;
        return false;
    }
    lastHresult_ = S_OK;
    return true;
}

bool VerticalFlipPass::CreateDestinationView(
    ID3D11Texture2D* destination,
    UINT arraySlice,
    DXGI_FORMAT viewFormat,
    ID3D11RenderTargetView** view) noexcept {
    if (destination == nullptr || view == nullptr) {
        lastHresult_ = E_INVALIDARG;
        return false;
    }
    *view = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC description{};
    description.Format = viewFormat;
    description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    description.Texture2DArray.MipSlice = 0;
    description.Texture2DArray.FirstArraySlice = arraySlice;
    description.Texture2DArray.ArraySize = 1;
    const HRESULT result = device_->CreateRenderTargetView(
        destination, &description, view);
    lastHresult_ = result;
    return SUCCEEDED(result) && *view != nullptr;
}

void VerticalFlipPass::ReleaseCachedViews() noexcept {
    for (auto& cached : sourceViews_) {
        ReleaseObject(cached.view);
        ReleaseObject(cached.scratch);
        cached.identity = nullptr;
    }
    for (auto& cached : destinationViews_) {
        ReleaseObject(cached.view);
        cached.identity = nullptr;
    }
    destinationViews_.clear();
}

void VerticalFlipPass::SetFailure(Status status, HRESULT result) noexcept {
    lastStatus_ = status;
    lastHresult_ = result;
}

} // namespace gakumas::vr::d3d11
