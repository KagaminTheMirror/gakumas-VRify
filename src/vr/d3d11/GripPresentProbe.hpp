#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gakumas::vr::d3d11 {
struct GripPresentSource {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    std::uint64_t node = 0;
    std::string label;
};
// Acquire while the caller has just checked the Unity object and read its
// public GetNativeTexturePtr. Only the owned COM reference crosses threads.
ID3D11Texture2D* AcquireGripPresentTexture(void* native) noexcept;

struct GripPresentPixels {
    std::uint64_t epoch = 0, present = 0, node = 0;
    std::uintptr_t native = 0;
    bool armed = false;
    std::string label;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    unsigned width = 0, height = 0, tileWidth = 0, tileHeight = 0, latency = 0;
    HRESULT error = S_OK;
    // Packed 2x2 atlas: top-left, top-right, centre, bottom-left tiles.
    // Each tile is min(16, source dimension). Raw bytes, no SRGB conversion.
    std::array<std::uint8_t, 32 * 32 * 4> bytes{};
};

class GripPresentProbe final {
public:
    static constexpr std::size_t SourceCapacity = 24;
    static constexpr std::size_t BatchCapacity = 2;
    GripPresentProbe();
    ~GripPresentProbe();
    bool Queue(std::vector<GripPresentSource> sources, std::uint64_t epoch, bool armed);
    // Same captured immediate context as the normal frame copy. Copies only
    // into private staging textures, before the existing post-Present clears.
    // Samples are PRESENT state, never advertised as pass-before/pass-after.
    void Pump(ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer);
    std::vector<GripPresentPixels> TakeCompleted();
private:
    struct Batch;
    struct Entry;
    std::mutex mutex_;
    std::vector<std::unique_ptr<Batch>> queued_;
    std::vector<std::unique_ptr<Entry>> pending_;
    std::vector<GripPresentPixels> completed_;
    std::uint64_t present_ = 0;
};
} // namespace gakumas::vr::d3d11
