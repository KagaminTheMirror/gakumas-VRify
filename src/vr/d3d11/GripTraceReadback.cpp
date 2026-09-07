#include "GripTraceReadback.hpp"

#include <cstring>
#include <utility>

namespace gakumas::vr::d3d11 {
namespace {
template <class T> void Drop(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }
bool Supported(DXGI_FORMAT f) noexcept {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}
}

struct GripTraceReadback::Entry {
    ID3D11Texture2D* source = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11Query* query = nullptr;
    GripTracePixels result;
    bool submitted = false;
    unsigned ticks = 0;
    ~Entry() { Drop(source); Drop(staging); Drop(query); }
};

GripTraceReadback::GripTraceReadback() {
    entries_.reserve(Capacity); completed_.reserve(Capacity);
}
GripTraceReadback::~GripTraceReadback() = default;

bool GripTraceReadback::Enqueue(ID3D11Texture2D* texture, std::uint64_t token,
                               std::string label) {
    if (!texture || token == 0) return false;
    std::lock_guard lock(mutex_);
    if (entries_.size() + completed_.size() >= Capacity) return false;
    auto e = std::make_unique<Entry>();
    texture->AddRef();
    e->source = texture;
    e->result.token = token;
    e->result.label = std::move(label);
    entries_.push_back(std::move(e));
    return true;
}

void GripTraceReadback::Pump(ID3D11DeviceContext* context) noexcept {
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    std::lock_guard lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto& e = **it;
        bool done = false;
        HRESULT hr = S_OK;
        if (++e.ticks > 300) { hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT); done = true; }
        if (!done && !e.staging) {
            D3D11_TEXTURE2D_DESC d{};
            e.source->GetDesc(&d);
            e.result.format = d.Format;
            if (d.Width != kGripTileWidth || d.Height != kGripTileHeight ||
                d.SampleDesc.Count != 1 || d.ArraySize != 1 || d.MipLevels != 1 ||
                !Supported(d.Format)) {
                hr = E_INVALIDARG;
            } else {
                ID3D11Device* device = nullptr;
                ID3D11Device* sourceDevice = nullptr;
                context->GetDevice(&device);
                e.source->GetDevice(&sourceDevice);
                if (device != sourceDevice) hr = E_INVALIDARG;
                else {
                    d.Usage = D3D11_USAGE_STAGING;
                    d.BindFlags = 0; d.MiscFlags = 0;
                    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    hr = device->CreateTexture2D(&d, nullptr, &e.staging);
                    if (SUCCEEDED(hr)) {
                        D3D11_QUERY_DESC q{D3D11_QUERY_EVENT, 0};
                        hr = device->CreateQuery(&q, &e.query);
                    }
                }
                Drop(device); Drop(sourceDevice);
            }
            if (FAILED(hr)) done = true;
        }
        if (!done && !e.submitted) {
            context->CopyResource(e.staging, e.source);
            context->End(e.query);
            e.submitted = true;
        } else if (!done) {
            hr = context->GetData(e.query, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_OK) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr = context->Map(e.staging, 0, D3D11_MAP_READ,
                                  D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (SUCCEEDED(hr)) {
                    std::uint64_t cookie = 0;
                    const auto* row = static_cast<const std::uint8_t*>(mapped.pData) +
                                      mapped.RowPitch * 32;
                    std::memcpy(&cookie, row, sizeof(cookie));
                    if (cookie == e.result.token) {
                        for (unsigned y = 0; y < kGripTileHeight; ++y)
                            std::memcpy(e.result.bytes.data() + y * kGripTileWidth * 4,
                                static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                                kGripTileWidth * 4);
                        done = true;
                    } else {
                        ++e.result.retries;
                        e.submitted = false;
                    }
                    context->Unmap(e.staging, 0);
                } else if (hr != DXGI_ERROR_WAS_STILL_DRAWING) done = true;
            } else if (FAILED(hr)) done = true;
        }
        if (done) {
            e.result.error = hr;
            completed_.push_back(std::move(e.result));
            it = entries_.erase(it);
        } else ++it;
    }
}

std::vector<GripTracePixels> GripTraceReadback::TakeCompleted() {
    std::lock_guard lock(mutex_);
    std::vector<GripTracePixels> result;
    result.swap(completed_);
    completed_.reserve(Capacity);
    return result;
}
std::size_t GripTraceReadback::Pending() const {
    std::lock_guard lock(mutex_);
    return entries_.size() + completed_.size();
}
} // namespace gakumas::vr::d3d11
