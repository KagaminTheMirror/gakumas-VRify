#pragma once

#include <d3d11.h>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gakumas::vr::d3d11 {

// Unity records four 16x16 tiles followed by an eight-byte completion cookie.
// The cookie is copied LAST in the SAME CommandBuffer as the tiles. A Present
// arriving before that buffer executes must not publish an old slot's pixels.
inline constexpr unsigned kGripTileWidth = 32;
inline constexpr unsigned kGripTileHeight = 33;
inline constexpr unsigned kGripCookieOffset = 32 * 32 * 4;

struct GripTracePixels {
    std::uint64_t token = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::array<std::uint8_t, kGripTileWidth * kGripTileHeight * 4> bytes{};
    std::string label;
    HRESULT error = S_OK;
    std::uint32_t retries = 0;
};

class GripTraceReadback final {
public:
    static constexpr std::size_t Capacity = 96;
    GripTraceReadback();
    ~GripTraceReadback();
    // Owns a COM reference after success. No managed pointers cross threads.
    bool Enqueue(ID3D11Texture2D* texture, std::uint64_t token, std::string label);
    // Only on the captured Unity Present thread. Never waits or flushes.
    void Pump(ID3D11DeviceContext* context) noexcept;
    std::vector<GripTracePixels> TakeCompleted();
    std::size_t Pending() const;

private:
    struct Entry;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
    std::vector<GripTracePixels> completed_;
};

} // namespace gakumas::vr::d3d11
