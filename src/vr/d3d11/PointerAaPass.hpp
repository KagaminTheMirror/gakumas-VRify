#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>

namespace gakumas::vr::d3d11 {
// Coordinates are in the corresponding OpenXR eye's right-handed space.
struct PointerAaDisc {
    std::array<float,4> centerRadius{};
    std::array<float,4> axisX{};
    std::array<float,4> axisY{};
};
struct PointerAaEye {
    std::array<float,4> tangents{}; // left, right, down, up
    std::array<PointerAaDisc,2> discs{};
};
class PointerAaPass final {
public:
    bool Render(ID3D11Device* device, ID3D11DeviceContext* immediate,
        ID3D11Texture2D* target, DXGI_FORMAT format,
        const std::array<PointerAaEye,2>& eyes) noexcept;
    void Reset() noexcept;
private:
    bool Initialize(ID3D11Device* device) noexcept;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferred_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
};
}
