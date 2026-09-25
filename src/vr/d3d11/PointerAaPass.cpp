#include "PointerAaPass.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>
#pragma comment(lib,"d3dcompiler.lib")

namespace gakumas::vr::d3d11 {
using Microsoft::WRL::ComPtr;
namespace {
constexpr char shader[] = R"(
cbuffer Parameters : register(b0) {
    float4 centerRadius;
    float4 axisX;
    float4 axisY;
    float4 tangents;
    float4 viewport;
};
struct Varying { float4 position : SV_Position; float2 local : TEXCOORD0; };
Varying VS(uint id : SV_VertexID) {
    const float2 corners[6] = {float2(-1,-1),float2(-1,1),float2(1,-1),
        float2(1,-1),float2(-1,1),float2(1,1)};
    // Pad geometry by several eye pixels; the AA fringe must not be clipped
    // at the circle's tight bounding box, even at the smallest file scale.
    float metresPerPixel = max((tangents.y-tangents.x)/viewport.x,
        (tangents.w-tangents.z)/viewport.y) * max(-centerRadius.z,0.001);
    float extent = 1 + 4*metresPerPixel/centerRadius.w;
    float2 local = corners[id] * extent;
    float3 p = centerRadius.xyz + centerRadius.w*(axisX.xyz*local.x + axisY.xyz*local.y);
    float depth = -p.z;
    Varying o;
    o.position = float4((2*p.x-depth*(tangents.y+tangents.x))/(tangents.y-tangents.x),
        (2*p.y-depth*(tangents.w+tangents.z))/(tangents.w-tangents.z),0,depth);
    o.local = local;
    return o;
}
float4 PS(Varying i) : SV_Target {
    float d = length(i.local);
    // Screen-space derivatives are measured in THIS eye image, not in a
    // fixed cursor texture. Keep colour solid; only fractional coverage fades.
    float footprint = max(length(float2(ddx(d),ddy(d))),0.00001);
    float alpha = saturate((1-d)/footprint + 0.5);
    float inner = saturate((26.0/30.0-d)/footprint + 0.5);
    float color = lerp(0.009134,1,inner); // sRGB 24 converted to linear
    return float4(color*alpha,color*alpha,color*alpha,alpha);
}
)";
struct Constants {
    PointerAaDisc disc;
    std::array<float,4> tangents;
    std::array<float,4> viewport;
};
}
void PointerAaPass::Reset() noexcept {
    blend_.Reset(); raster_.Reset(); constants_.Reset(); ps_.Reset(); vs_.Reset();
    deferred_.Reset(); device_.Reset();
}
bool PointerAaPass::Initialize(ID3D11Device* device) noexcept {
    if (device_.Get()==device && deferred_ && vs_ && ps_ && blend_) return true;
    Reset();
    if (!device) return false;
    ComPtr<ID3DBlob> vs, ps, error;
    if (FAILED(D3DCompile(shader,sizeof(shader)-1,"PointerAaPass",nullptr,nullptr,"VS","vs_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS,0,&vs,&error))) return false;
    error.Reset();
    if (FAILED(D3DCompile(shader,sizeof(shader)-1,"PointerAaPass",nullptr,nullptr,"PS","ps_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS,0,&ps,&error))) return false;
    device_=device;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth=sizeof(Constants); buffer.Usage=D3D11_USAGE_DEFAULT;
    buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE;
    raster.DepthClipEnable=TRUE;
    D3D11_BLEND_DESC blend{};
    auto& rt=blend.RenderTarget[0]; rt.BlendEnable=TRUE;
    rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;
    rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateDeferredContext(0,&deferred_)) ||
        FAILED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vs_)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&ps_)) ||
        FAILED(device->CreateBuffer(&buffer,nullptr,&constants_)) ||
        FAILED(device->CreateRasterizerState(&raster,&raster_)) ||
        FAILED(device->CreateBlendState(&blend,&blend_))) { Reset(); return false; }
    return true;
}
bool PointerAaPass::Render(ID3D11Device* device, ID3D11DeviceContext* immediate,
    ID3D11Texture2D* target, DXGI_FORMAT format, const std::array<PointerAaEye,2>& eyes) noexcept {
    if (!immediate || !target || !Initialize(device)) return false;
    D3D11_TEXTURE2D_DESC desc{}; target->GetDesc(&desc);
    if (desc.ArraySize!=2 || desc.SampleDesc.Count!=1 || !desc.Width || !desc.Height) return false;
    std::array<ComPtr<ID3D11RenderTargetView>,2> views;
    for (UINT eye=0; eye<2; ++eye) {
        D3D11_RENDER_TARGET_VIEW_DESC view{}; view.Format=format;
        view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.FirstArraySlice=eye; view.Texture2DArray.ArraySize=1;
        if (FAILED(device->CreateRenderTargetView(target,&view,&views[eye]))) return false;
    }
    deferred_->ClearState();
    const D3D11_VIEWPORT viewport{0,0,static_cast<float>(desc.Width),static_cast<float>(desc.Height),0,1};
    deferred_->RSSetViewports(1,&viewport); deferred_->RSSetState(raster_.Get());
    deferred_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferred_->VSSetShader(vs_.Get(),nullptr,0); deferred_->PSSetShader(ps_.Get(),nullptr,0);
    ID3D11Buffer* buffer=constants_.Get();
    deferred_->VSSetConstantBuffers(0,1,&buffer); deferred_->PSSetConstantBuffers(0,1,&buffer);
    deferred_->OMSetBlendState(blend_.Get(),nullptr,0xffffffffU);
    for (UINT eye=0; eye<2; ++eye) {
        auto* view=views[eye].Get();
        const float clear[4]{}; deferred_->ClearRenderTargetView(view,clear);
        deferred_->OMSetRenderTargets(1,&view,nullptr);
        for (const auto& disc:eyes[eye].discs) {
            if (!(disc.centerRadius[3]>0) || !(disc.centerRadius[2]<-0.001F)) continue;
            const Constants params{disc,eyes[eye].tangents,{viewport.Width,viewport.Height,0,0}};
            deferred_->UpdateSubresource(buffer,0,nullptr,&params,0,0);
            deferred_->Draw(6,0);
        }
    }
    ComPtr<ID3D11CommandList> commands;
    if (FAILED(deferred_->FinishCommandList(FALSE,&commands))) { deferred_->ClearState(); return false; }
    immediate->ExecuteCommandList(commands.Get(),TRUE); // preserve Unity's immediate-context state
    return true;
}
}
