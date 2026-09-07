#include "GripPresentProbe.hpp"
#include <algorithm>
#include <cstring>
#include <utility>

namespace gakumas::vr::d3d11 {
using Microsoft::WRL::ComPtr;
ID3D11Texture2D* AcquireGripPresentTexture(void* native) noexcept {
    if (!native) return nullptr;
    ID3D11Texture2D* texture = nullptr;
    __try {
        if (FAILED(static_cast<IUnknown*>(native)->QueryInterface(IID_PPV_ARGS(&texture))))
            return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    return texture;
}
namespace {
bool Supported(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}
}
struct GripPresentProbe::Batch {
    std::vector<GripPresentSource> sources;
    std::uint64_t epoch = 0;
    bool armed = false;
};
struct GripPresentProbe::Entry {
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> query;
    GripPresentPixels result;
};
GripPresentProbe::GripPresentProbe() = default;
GripPresentProbe::~GripPresentProbe() = default;

bool GripPresentProbe::Queue(std::vector<GripPresentSource> sources,
                              std::uint64_t epoch, bool armed) {
    std::lock_guard lock(mutex_);
    // Include completions in the cap: a stalled worker cannot grow memory.
    if (sources.size() > SourceCapacity || queued_.size() >= BatchCapacity ||
        pending_.size() + completed_.size() + (queued_.size() + 1) * (SourceCapacity + 1) >
            BatchCapacity * (SourceCapacity + 1)) return false;
    auto batch = std::make_unique<Batch>();
    batch->sources = std::move(sources); batch->epoch = epoch; batch->armed = armed;
    queued_.push_back(std::move(batch));
    return true;
}

void GripPresentProbe::Pump(ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer) {
    if (!context || !backbuffer || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    std::lock_guard lock(mutex_);
    ++present_;
    // Finish older copies only. No blocking Map, Flush, Unity calls or render
    // state changes. A timeout destroys this private slot; it is never reused.
    for (auto it = pending_.begin(); it != pending_.end();) {
        auto& e = **it;
        ComPtr<ID3D11Device> entryDevice, contextDevice;
        e.staging->GetDevice(&entryDevice); context->GetDevice(&contextDevice);
        HRESULT hr = entryDevice.Get() != contextDevice.Get() ? E_INVALIDARG :
            context->GetData(e.query.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
        bool done = FAILED(hr);
        if (hr == S_OK) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            hr = context->Map(e.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
            if (SUCCEEDED(hr)) {
                const auto stride = e.result.tileWidth * 2 * 4;
                for (unsigned y = 0; y < e.result.tileHeight * 2; ++y)
                    std::memcpy(e.result.bytes.data() + y * stride,
                        static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch, stride);
                context->Unmap(e.staging.Get(), 0); done = true;
            } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) done = true;
        }
        ++e.result.latency;
        if (!done && e.result.latency >= 300) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); done = true;
        }
        if (done) {
            e.result.error = hr;
            completed_.push_back(std::move(e.result)); it = pending_.erase(it);
        } else ++it;
    }
    ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    for (auto& batch : queued_) {
        GripPresentSource bb; bb.texture = backbuffer; bb.label = "backbuffer";
        batch->sources.push_back(std::move(bb));
        for (const auto& source : batch->sources) {
            auto e = std::make_unique<Entry>();
            auto& r = e->result;
            r.epoch = batch->epoch; r.armed = batch->armed; r.present = present_;
            r.node = source.node; r.label = source.label;
            r.native = reinterpret_cast<std::uintptr_t>(source.texture.Get());
            D3D11_TEXTURE2D_DESC d{};
            ComPtr<ID3D11Device> sourceDevice;
            if (source.texture) { source.texture->GetDesc(&d); source.texture->GetDevice(&sourceDevice); }
            r.width = d.Width; r.height = d.Height; r.format = d.Format;
            HRESULT hr = S_OK;
            if (!source.texture || device.Get() != sourceDevice.Get() || !d.Width || !d.Height ||
                d.SampleDesc.Count != 1 || d.ArraySize != 1 || !Supported(d.Format)) hr = E_INVALIDARG;
            if (SUCCEEDED(hr)) {
                r.tileWidth = (std::min)(16U, d.Width); r.tileHeight = (std::min)(16U, d.Height);
                d.Width = r.tileWidth * 2; d.Height = r.tileHeight * 2; d.MipLevels = 1;
                d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.MiscFlags = 0;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                hr = device->CreateTexture2D(&d, nullptr, &e->staging);
                if (SUCCEEDED(hr)) {
                    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT, 0};
                    hr = device->CreateQuery(&q, &e->query);
                }
            }
            if (FAILED(hr)) { r.error = hr; completed_.push_back(std::move(r)); continue; }
            const unsigned xs[]{0, r.width - r.tileWidth, (r.width - r.tileWidth) / 2, 0};
            const unsigned ys[]{0, 0, (r.height - r.tileHeight) / 2, r.height - r.tileHeight};
            for (unsigned i = 0; i < 4; ++i) {
                D3D11_BOX box{xs[i], ys[i], 0, xs[i] + r.tileWidth, ys[i] + r.tileHeight, 1};
                context->CopySubresourceRegion(e->staging.Get(), 0,
                    (i % 2) * r.tileWidth, (i / 2) * r.tileHeight, 0, source.texture.Get(), 0, &box);
            }
            context->End(e->query.Get());
            pending_.push_back(std::move(e));
        }
    }
    // Sources have no managed roots. Drop COM references after issuing copies;
    // D3D retains the resource for queued GPU work. Scene shells are never used.
    queued_.clear();
}
std::vector<GripPresentPixels> GripPresentProbe::TakeCompleted() {
    std::lock_guard lock(mutex_);
    std::vector<GripPresentPixels> result; result.swap(completed_); return result;
}
} // namespace gakumas::vr::d3d11
