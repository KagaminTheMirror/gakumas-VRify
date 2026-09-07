#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace gakumas::vr::d3d11 {

// Reads eight short, spatially distributed scan-line samples. This is not a
// visual-quality metric; it is a low-frequency diagnostic for determining
// whether a GPU texture actually changes between published frames. Mapping the
// staging texture also establishes that all preceding copies have completed.
inline bool ComputeTextureFingerprint(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* texture,
    UINT subresource,
    std::uint64_t& fingerprint) noexcept {
    fingerprint = 0;
    if (context == nullptr || texture == nullptr) {
        return false;
    }

    D3D11_TEXTURE2D_DESC source{};
    texture->GetDesc(&source);
    const UINT mipLevels = source.MipLevels == 0U ? 1U : source.MipLevels;
    if (source.Width == 0U || source.Height == 0U || source.ArraySize == 0U ||
        source.SampleDesc.Count != 1U || subresource >= mipLevels * source.ArraySize) {
        return false;
    }
    switch (source.Format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        break;
    default:
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

    ID3D11Device* device = nullptr;
    context->GetDevice(&device);
    if (device == nullptr) {
        return false;
    }
    ID3D11Texture2D* staging = nullptr;
    const HRESULT createResult = device->CreateTexture2D(
        &stagingDescription, nullptr, &staging);
    device->Release();
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
        const D3D11_BOX sourceBox{
            sourceX,
            sourceY,
            0U,
            sourceX + sampleWidth,
            sourceY + 1U,
            1U,
        };
        context->CopySubresourceRegion(
            staging, 0, 0, row, 0, texture, subresource, &sourceBox);
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapResult = context->Map(
        staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapResult) || mapped.pData == nullptr) {
        staging->Release();
        return false;
    }

    constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    std::uint64_t value = kFnvOffset;
    const std::size_t bytesPerRow = static_cast<std::size_t>(sampleWidth) * 4U;
    for (UINT row = 0; row < sampleRows; ++row) {
        const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData) +
            static_cast<std::size_t>(mapped.RowPitch) * row;
        for (std::size_t index = 0; index < bytesPerRow; ++index) {
            value ^= bytes[index];
            value *= kFnvPrime;
        }
    }
    context->Unmap(staging, 0);
    staging->Release();
    fingerprint = value;
    return true;
}

} // namespace gakumas::vr::d3d11
