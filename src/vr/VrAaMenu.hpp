#pragma once

#include "openxr/OpenXrContext.hpp"
#include "VrLog.hpp"

namespace gakumas::vr {

bool PaintVrAaMenu(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* destination,
    std::uint32_t width,
    std::uint32_t height,
    const openxr::OpenXrContext::AaMenuInput& input,
    openxr::OpenXrContext::AaMenuOutput& output,
    VrLog& log);

// Panel-adjust overlay (adjust bar strip + head-locked hint/toast strip in
// one texture). Shares the menu's ImGui context, fonts and recorder; only
// the offscreen render target is separate so the two paints never thrash a
// shared texture size.
bool PaintVrPanelOverlay(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* destination,
    std::uint32_t width,
    std::uint32_t height,
    const openxr::OpenXrContext::PanelOverlayInput& input,
    openxr::OpenXrContext::PanelOverlayOutput& output,
    VrLog& log);

void ShutdownVrAaMenu() noexcept;

// Write a released-but-still-debouncing slider before the menu is hidden.
void FlushVrAaMenuConfigSave(VrLog& log);

// Offline paint-test hook. Production never calls this; the menu otherwise
// only changes tabs through the on-screen buttons.
void SetVrAaMenuTabForTest(int tab) noexcept;

// Glyphs in the active translation that the menu font atlas cannot draw. Zero
// means no label can fall back to '?' in headset.
[[nodiscard]] std::uint32_t VrAaMenuMissingGlyphCount() noexcept;

} // namespace gakumas::vr
