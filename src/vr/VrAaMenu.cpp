#include "VrAaMenu.hpp"

#include "config/VrifyConfig.hpp"
#include "i18n/VrI18n.hpp"
#include "VrVersion.hpp"
#include "../imgui/imgui.h"
// ImTextCharFromUtf8 lives in the internal header; it is the only UTF-8 decoder
// that matches how ImGui itself walks the strings this menu draws.
#include "../imgui/imgui_internal.h"
#include "../imgui/imgui_impl_dx11.h"
#include "../host/ScopedImGuiContext.hpp"

#include "VrFreeCamera.hpp"
#include "LiveGaze.hpp"
#include "VrHandGlowSticks.hpp"
#include "VrLog.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <d3d11.h>
#include <filesystem>
#include <string>
#include <vector>
#include "VrMenuHoverHelp.hpp"

extern std::filesystem::path VrConfigJson;

namespace gakumas::vr {
namespace {

constexpr float kRowHeight = 62.0F;
constexpr float kRowSpacing = 8.0F;
constexpr float kLabelWidth = 360.0F;
constexpr float kColumnGap = 10.0F;
constexpr float kControlHeight = 46.0F;

ImGuiContext* g_context = nullptr;
ID3D11Device* g_device = nullptr;
// ImGui records into a deferred context. Issuing its dozens of state changes
// straight onto Unity's immediate context from the OpenXR worker corrupts the
// game's own pipeline state and hangs the render thread.
ID3D11DeviceContext* g_recorder = nullptr;
ID3D11Texture2D* g_rt = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ImVector<ImWchar> g_glyphRanges{};
std::uint32_t g_rtWidth = 0;
std::uint32_t g_rtHeight = 0;
DXGI_FORMAT g_rtFormat = DXGI_FORMAT_UNKNOWN;
bool g_backendReady = false;
bool g_initFailed = false;
bool g_paintOkLogged = false;
std::uint32_t g_missingGlyphs = 0;
int g_tab = 0;
ImGuiID g_selectedSlider = 0;
int g_fpFollowPendingValue = 0;
int g_taaQualityPendingValue = 0;
bool g_sliderRowClicked = false;
bool g_selectedSliderSeen = false;
float g_savedFlashSeconds = 0.0F;
float g_draftRenderScale = 1.0F;
bool g_draftRenderScaleValid = false;
bool g_autoSliderDirty = false;
bool g_autoSliderHeld = false;
float g_autoSliderQuietSeconds = 0.0F;
constexpr float kAutoSliderDebounceSeconds = 0.35F;
std::chrono::steady_clock::time_point g_lastPaintTime{};
bool g_lastPaintTimeValid = false;

// The panel-adjust overlay paints through the same ImGui context but into its
// own offscreen target: the menu is 1280x880 and the overlay is smaller, so a
// shared texture would be recreated on every alternating paint.
ID3D11Texture2D* g_overlayRt = nullptr;
ID3D11RenderTargetView* g_overlayRtv = nullptr;
std::uint32_t g_overlayRtWidth = 0;
std::uint32_t g_overlayRtHeight = 0;
DXGI_FORMAT g_overlayRtFormat = DXGI_FORMAT_UNKNOWN;
bool g_overlayPaintOkLogged = false;

void ReleaseOwnedTarget() noexcept {
    if (g_rtv != nullptr) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
    if (g_rt != nullptr) {
        g_rt->Release();
        g_rt = nullptr;
    }
    g_rtWidth = 0;
    g_rtHeight = 0;
    g_rtFormat = DXGI_FORMAT_UNKNOWN;
}

void ReleaseOverlayTarget() noexcept {
    if (g_overlayRtv != nullptr) {
        g_overlayRtv->Release();
        g_overlayRtv = nullptr;
    }
    if (g_overlayRt != nullptr) {
        g_overlayRt->Release();
        g_overlayRt = nullptr;
    }
    g_overlayRtWidth = 0;
    g_overlayRtHeight = 0;
    g_overlayRtFormat = DXGI_FORMAT_UNKNOWN;
    g_overlayPaintOkLogged = false;
}

void ShutdownImpl() noexcept {
    ReleaseOwnedTarget();
    ReleaseOverlayTarget();
    g_paintOkLogged = false;
    g_lastPaintTimeValid = false;
    g_selectedSlider = 0;
    g_sliderRowClicked = false;
    if (g_autoSliderDirty) {
        GakumasLocal::Config::ClampVrEyeAaSettings();
        GakumasLocal::Config::ClampVrPointerSettings();
        GakumasLocal::Config::SaveVrConfig(VrConfigJson);
        g_autoSliderDirty = false;
        g_autoSliderQuietSeconds = 0.0F;
    }
    g_autoSliderHeld = false;
    if (g_context != nullptr) {
        ImGuiContext* dying = g_context;
        ui::ScopedImGuiContext current(dying);
        current.Forget(dying);
        if (g_backendReady) {
            ImGui_ImplDX11_Shutdown();
        }
        ImGui::DestroyContext(dying);
    }
    g_backendReady = false;
    g_context = nullptr;
    if (g_recorder != nullptr) {
        g_recorder->Release();
        g_recorder = nullptr;
    }
    g_device = nullptr;
}

void ApplyMenuStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 14.0F;
    style.WindowBorderSize = 0.0F;
    style.FrameRounding = 6.0F;
    style.GrabRounding = 5.0F;
    style.GrabMinSize = 30.0F;
    style.FramePadding = ImVec2(14.0F, 7.0F);
    style.ButtonTextAlign = ImVec2(0.5F, 0.5F);
    style.ItemSpacing = ImVec2(12.0F, 10.0F);
    style.ItemInnerSpacing = ImVec2(10.0F, 8.0F);
    style.WindowPadding = ImVec2(26.0F, 22.0F);
    style.ScrollbarSize = 22.0F;
    style.PopupRounding = 10.0F;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.075F, 0.095F, 0.12F, 1.0F);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.13F, 0.15F, 0.19F, 1.0F);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.105F, 0.13F, 0.16F, 1.0F);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.105F, 0.13F, 0.16F, 1.0F);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.105F, 0.13F, 0.16F, 1.0F);
    style.Colors[ImGuiCol_Button] = ImVec4(0.15F, 0.19F, 0.23F, 1.0F);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.19F, 0.25F, 0.29F, 1.0F);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.22F, 0.45F, 0.85F, 1.0F);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.38F, 0.76F, 0.69F, 1.0F);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.58F, 0.74F, 1.0F, 1.0F);
    style.Colors[ImGuiCol_Header] = ImVec4(0.22F, 0.30F, 0.45F, 1.0F);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.27F, 0.38F, 0.58F, 1.0F);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.22F, 0.45F, 0.85F, 1.0F);
}

// A glyph outside the atlas renders as the font's fallback '?', so count them
// once at build time instead of discovering it from a headset photo.
std::uint32_t CountMissingGlyphs(
    const ImFont* font,
    const std::vector<std::string>& strings) {
    if (font == nullptr) {
        return 0;
    }
    std::uint32_t missing = 0;
    for (const std::string& text : strings) {
        const char* cursor = text.c_str();
        const char* end = cursor + text.size();
        while (cursor < end) {
            unsigned int codepoint = 0;
            const int consumed =
                ImTextCharFromUtf8(&codepoint, cursor, end);
            if (consumed <= 0) {
                break;
            }
            cursor += consumed;
            if (codepoint == 0 || codepoint == '\n' || codepoint == '\r' ||
                codepoint == '\t' ||
                codepoint > static_cast<unsigned int>(IM_UNICODE_CODEPOINT_MAX)) {
                continue;
            }
            if (font->FindGlyphNoFallback(
                    static_cast<ImWchar>(codepoint)) == nullptr) {
                ++missing;
            }
        }
    }
    return missing;
}

bool EnsureContext(ID3D11Device* device, VrLog& log) {
    ui::ScopedImGuiContext current;
    if (device == nullptr) {
        return false;
    }
    if (g_initFailed && g_device == device) {
        return false;
    }
    if (g_device != device) {
        current.Forget(g_context);
        ShutdownImpl();
        g_initFailed = false;
    }
    if (g_context != nullptr && g_backendReady && g_recorder != nullptr) {
        return true;
    }

    if (g_recorder == nullptr) {
        const HRESULT hr = device->CreateDeferredContext(0, &g_recorder);
        if (FAILED(hr) || g_recorder == nullptr) {
            log.Write(
                "[VR][menu] deferred context create failed hr=" +
                std::to_string(static_cast<long>(hr)));
            ShutdownImpl();
            g_initFailed = true;
            g_device = device;
            return false;
        }
    }

    IMGUI_CHECKVERSION();
    g_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags = ImGuiConfigFlags_None;
    io.MouseDrawCursor = false;
    ApplyMenuStyle();

    // The common-2500 Chinese range misses characters this UI actually uses
    // (锯, 齿, 摇, ...), and a missing glyph renders as '?' in headset. Feed
    // every translated string into the builder so the atlas covers the text
    // that can appear rather than a statistical subset.
    std::vector<std::string> menuStrings = GakumasVrI18n::ActiveStrings();
    menuStrings.emplace_back("\u3002\u300c\u300d\u3042\u3046\u304c\u304d\u3055\u3057\u3058\u3059\u305b\u3067\u3068\u306b\u306e\u306f\u3078\u307e\u3084\u308a\u308b\u308c\u308f\u3092\u3093\u30a4\u30ab\u30af\u30b2\u30b3\u30b7\u30bf\u30c3\u30c8\u30d5\u30dd\u30e0\u30e1\u30e9\u30ea\u30eb\u30ed\u30f3\u30fc\u4e00\u4e0a\u4e0b\u4e0d\u4e14\u4e2d\u4e3a\u4e5f\u4e8e\u4eae\u4ec5\u4ee5\u4ef6\u4f17\u4f1a\u4f4d\u4f53\u4f7f\u4f9d\u4fdd\u4fee\u5024\u503c\u504f\u505c\u5149\u514d\u5173\u518d\u51b2\u51bb\u51fa\u5206\u5207\u5212\u5230\u5237\u52d5\u533a\u5385\u53ca\u53cc\u53d8\u53ea\u53ef\u5404\u5408\u540c\u540e\u5411\u542f\u548c\u54cd\u56de\u5728\u573a\u57df\u5834\u5909\u591a\u5927\u5934\u5b98\u5b9a\u5bb9\u5bfc\u5c06\u5c11\u5c4f\u5d29\u5e26\u5e38\u5f00\u5f0f\u5f15\u5f62\u5f71\u5f8c\u5f93\u5fc5\u620f\u6210\u6216\u6240\u624b\u6269\u629e\u62e9\u6307\u6309\u6362\u63a7\u6536\u6539\u6548\u6570\u6574\u65b0\u65b9\u65e0\u65f6\u660e\u6613\u663e\u6642\u6655\u666f\u6682\u66dd\u66f4\u671d\u672c\u673a\u6765\u677f\u67e5\u6863\u68d2\u6a21\u6b21\u6b3e\u6b62\u6b63\u6b64\u6cd5\u6e38\u706f\u7136\u724c\u73b0\u751f\u7528\u7531\u753b\u754c\u7559\u767d\u7684\u76f8\u770b\u7729\u793a\u7a81\u7acb\u7af6\u7d27\u7ec6\u7ed3\u7f6e\u80fd\u819c\u81ea\u81f4\u8272\u8367\u8868\u8981\u89c2\u89d2\u8a2d\u8aac\u8ba9\u8bbe\u8be6\u8bf4\u8c03\u8d1f\u8d77\u8d8a\u8ddf\u8df3\u8eab\u8ee2\u8f6c\u8fb9\u8fc7\u8fd1\u8ffd\u9002\u9009\u900f\u9020\u904e\u9078\u907f\u90e8\u9154\u91cd\u9488\u9501\u95ed\u9608\u9634\u968f\u96fe\u9700\u9732\u9760\u9762\u9879\u989c\u9ad8\u9ed1\uff0c\uff1a\uff1b");
    ImFontConfig fontConfig;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(io.Fonts->GetGlyphRangesJapanese());
    builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    for (const std::string& text : menuStrings) {
        builder.AddText(text.c_str());
    }
    g_glyphRanges.clear();
    builder.BuildRanges(&g_glyphRanges);
    fontConfig.GlyphRanges = g_glyphRanges.Data;

    ImFont* font = nullptr;
    if (std::filesystem::exists("c:\\Windows\\Fonts\\msyh.ttc")) {
        font = io.Fonts->AddFontFromFileTTF(
            "c:\\Windows\\Fonts\\msyh.ttc", 32.0F, &fontConfig);
    } else if (std::filesystem::exists("c:\\Windows\\Fonts\\segoeui.ttf")) {
        font = io.Fonts->AddFontFromFileTTF(
            "c:\\Windows\\Fonts\\segoeui.ttf", 32.0F, &fontConfig);
    }
    if (font == nullptr) {
        font = io.Fonts->AddFontDefault();
    }
    io.Fonts->Build();
    g_missingGlyphs = CountMissingGlyphs(font, menuStrings);
    log.Write(
        "[VR][menu] font atlas ranges=" +
        std::to_string(g_glyphRanges.Size / 2) + " strings=" +
        std::to_string(menuStrings.size()) + " missingGlyphs=" +
        std::to_string(g_missingGlyphs));

    if (!ImGui_ImplDX11_Init(device, g_recorder)) {
        log.Write("[VR][menu] ImGui DX11 backend init failed");
        ShutdownImpl();
        g_initFailed = true;
        g_device = device;
        return false;
    }
    g_backendReady = true;
    g_device = device;
    log.Write("[VR][menu] VR settings menu renderer ready");
    return true;
}

// OpenXR quad images are typed sRGB and reject a UNORM render target view.
// Paint into an owned UNORM texture, then copy into the acquired image.
DXGI_FORMAT MenuRenderFormat(DXGI_FORMAT destination) noexcept {
    switch (destination) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    default:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

bool EnsureOwnedTarget(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height,
    DXGI_FORMAT destinationFormat,
    VrLog& log) {
    const DXGI_FORMAT format = MenuRenderFormat(destinationFormat);
    if (g_rt != nullptr && g_rtv != nullptr && g_rtWidth == width &&
        g_rtHeight == height && g_rtFormat == format) {
        return true;
    }
    ReleaseOwnedTarget();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_rt);
    if (FAILED(hr) || g_rt == nullptr) {
        log.Write(
            "[VR][menu] offscreen texture create failed hr=" +
            std::to_string(static_cast<long>(hr)) + " format=" +
            std::to_string(static_cast<int>(format)));
        return false;
    }
    hr = device->CreateRenderTargetView(g_rt, nullptr, &g_rtv);
    if (FAILED(hr) || g_rtv == nullptr) {
        log.Write(
            "[VR][menu] offscreen RTV create failed hr=" +
            std::to_string(static_cast<long>(hr)));
        ReleaseOwnedTarget();
        return false;
    }
    g_rtWidth = width;
    g_rtHeight = height;
    g_rtFormat = format;
    return true;
}

bool EnsureOverlayTarget(
    ID3D11Device* device,
    std::uint32_t width,
    std::uint32_t height,
    DXGI_FORMAT destinationFormat,
    VrLog& log) {
    const DXGI_FORMAT format = MenuRenderFormat(destinationFormat);
    if (g_overlayRt != nullptr && g_overlayRtv != nullptr &&
        g_overlayRtWidth == width && g_overlayRtHeight == height &&
        g_overlayRtFormat == format) {
        return true;
    }
    ReleaseOverlayTarget();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &g_overlayRt);
    if (FAILED(hr) || g_overlayRt == nullptr) {
        log.Write(
            "[VR][panel] overlay offscreen texture create failed hr=" +
            std::to_string(static_cast<long>(hr)) + " format=" +
            std::to_string(static_cast<int>(format)));
        return false;
    }
    hr = device->CreateRenderTargetView(g_overlayRt, nullptr, &g_overlayRtv);
    if (FAILED(hr) || g_overlayRtv == nullptr) {
        log.Write(
            "[VR][panel] overlay offscreen RTV create failed hr=" +
            std::to_string(static_cast<long>(hr)));
        ReleaseOverlayTarget();
        return false;
    }
    g_overlayRtWidth = width;
    g_overlayRtHeight = height;
    g_overlayRtFormat = format;
    return true;
}


struct MenuRow {
    ImVec2 origin{};
    float width = 0, height = 0;
    bool compact = false, stacked = false, toggle = false, combo = false;
    float controlTop = 0;
    std::string label;
};
struct HelpRecord { MenuRow row; std::string text; };
std::vector<HelpRecord> g_helpRows;
VrMenuHoverHelp g_helpState;
bool g_openLocalizeRestart = false;
bool g_openTaaConfirm = false;
float g_helpTestDelta = 0;
std::string g_helpCaptureForTest;
std::string g_popupCaptureForTest;
std::string g_comboCaptureForTest;
float g_scrollCaptureForTest = -1;
float g_bodyScrollForTest = 0;
float g_panelBarRightForTest = 0;
float g_nextActionWidth = 0;
float g_gridPendingHeight = 0;
float g_gridX = 0, g_gridY = 0, g_gridWidth = 0;
bool g_nextCombo = false;
bool g_nextToggle = false;
bool g_nextStacked = false;
bool g_gridRight = false, g_nextCompact = false, g_helpDragging = false;
int g_helpTab = -1, g_helpLanguage = -1;


const char* NoteForLabel(const char* label) {
    using GakumasVrI18n::ts;
    const char* pairs[][2] = {
        {"vr_menu_grip_transparent", "vr_menu_grip_transparent_note"},
        {"vr_menu_localize", "vr_menu_localize_note"},
        {"vr_menu_toon_follow_ref", "vr_menu_toon_follow_ref_note"},
        {"vr_taa_quality", "vr_taa_quality_note"},
        {"vr_menu_actor_shadow_anchor", "vr_menu_actor_shadow_anchor_note"},
        {"vr_menu_actor_toon_anchor", "vr_menu_actor_shadow_anchor_note"},
        {"vr_menu_bloom_follow_source", "vr_menu_bloom_follow_source_note"},
        {"vr_menu_source_tiny", "vr_menu_source_tiny_note"},
        {"vr_menu_fp_follow", "vr_menu_fp_follow_note"},
    };
    for (const auto& pair : pairs) if (std::string(label) == ts(pair[0])) return ts(pair[1]);
    return "";
}
MenuRow BeginRow(const char* label, bool enabled, bool selected = false) {
    MenuRow row;
    row.combo = g_nextCombo; g_nextCombo = false;
    row.toggle = g_nextToggle; g_nextToggle = false;
    row.compact = g_nextCompact;
    row.stacked = g_nextStacked; g_nextStacked = false;
    g_nextCompact = false;
    if (!row.compact && g_gridRight) { g_gridY += g_gridPendingHeight; g_gridRight = false; }
    row.width = row.compact ? (g_gridWidth - 36) / 2 : g_gridWidth;
    row.height = row.toggle ? 80.0F : (row.combo ? 72.0F : 90.0F);
    row.origin = ImVec2(g_gridX + (g_gridRight ? row.width + 36 : 0), g_gridY);
    row.label = label;
    auto* draw = ImGui::GetWindowDrawList();
    if (selected) draw->AddRectFilled(ImVec2(row.origin.x, row.origin.y + 2),
        ImVec2(row.origin.x + 4, row.origin.y + row.height - 2), IM_COL32(97,194,176,255), 2);

    if (row.toggle || row.combo) {
        const char* note = NoteForLabel(label);
        const float textWidth = row.combo ? (row.width - 36)*0.5F - 24
            : row.width - 144 - (g_nextActionWidth > 0 ? g_nextActionWidth + 20 : 0);
        const float labelHeight = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, textWidth, label).y;
        const float noteHeight = note[0] ? ImGui::GetFont()->CalcTextSizeA(20, FLT_MAX, textWidth, note).y : 0;
        const float blockHeight = labelHeight + (noteHeight > 0 ? 4 + noteHeight : 0);
        row.height = std::max(row.height, blockHeight + 16);
        const float groupHeight = std::max(52.0F, blockHeight);
        const float top = row.origin.y + (groupHeight-blockHeight)*0.5F;
        row.controlTop = row.origin.y + (groupHeight-46)*0.5F;
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(row.origin.x+12,top),
            enabled ? IM_COL32(230,234,243,255) : IM_COL32(155,165,177,255),label,nullptr,textWidth);
        if (noteHeight > 0) draw->AddText(ImGui::GetFont(),20,
            ImVec2(row.origin.x+12,top+labelHeight+4),
            IM_COL32(224,192,136,255),note,nullptr,textWidth);
    } else {
        draw->AddText(ImVec2(row.origin.x+14+ImGui::GetStyle().GrabMinSize*0.5F,row.origin.y+7),
            enabled ? IM_COL32(230,234,243,255) : IM_COL32(155,165,177,255),label);
    }
    ImGui::SetCursorScreenPos(ImVec2(row.origin.x + ((row.compact || row.stacked) ? 12 : 384),
        row.origin.y + ((row.compact || row.stacked) ? 36 : 0)));
    ImGui::PushItemWidth(row.width - ((row.compact || row.stacked) ? 24 : 398));
    g_helpRows.push_back({row, ""});
    return row;
}
void EndRow(const MenuRow& row) {
    ImGui::PopItemWidth();
    if (row.compact && !g_gridRight) { g_gridRight = true; g_gridPendingHeight = row.height; }
    else { g_gridY += g_gridRight ? std::max(row.height, g_gridPendingHeight) : row.height; g_gridRight = false; }
    g_nextActionWidth = 0;
    ImGui::SetCursorScreenPos(ImVec2(g_gridX, g_gridY));
}
void DrawHelpDock() {
    std::string hovered;
    for (const auto& entry : g_helpRows) {
        if (ImGui::GetIO().MousePos.y < 678 && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(entry.row.origin,
            ImVec2(entry.row.origin.x + entry.row.width, entry.row.origin.y + entry.row.height - 2), false)) {
            hovered = entry.row.label;
        }
    }
    g_helpState.Advance(g_helpTestDelta > 0 ? g_helpTestDelta : ImGui::GetIO().DeltaTime, hovered, g_helpDragging);
    std::string body = GakumasVrI18n::ts("vr_menu_idle_help");
    for (const auto& entry : g_helpRows) if (entry.row.label == g_helpState.shown) {
        body = entry.text;
    }
    if (!g_helpCaptureForTest.empty()) body = GakumasVrI18n::ts(g_helpCaptureForTest);
    auto* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(ImVec2(26,690), ImVec2(1254,828), IM_COL32(25,35,44,255),8);

    d->PushClipRect(ImVec2(44,707),ImVec2(1236,822),true);
    d->AddText(ImGui::GetFont(),32,ImVec2(44,709),IM_COL32(216,225,233,255),body.c_str(),nullptr,1180);
    d->PopClipRect();
}
struct StickInput {
    bool active = false;
    float x = 0.0F;
    bool consumed = false;
};

void ComboRow(
    const char* label,
    int* value,
    const char* const* items,
    int count,
    bool enabled,
    StickInput& stick) {
    if (value == nullptr || count <= 0) {
        return;
    }
    ImGui::PushID(label);
    g_nextCombo = true; g_nextCompact = false; g_nextStacked = false;
    const MenuRow row = BeginRow(label, enabled);
    ImGui::SetCursorScreenPos(ImVec2(row.origin.x + (row.width+36)*0.5F + 12, row.controlTop));
    ImGui::PopItemWidth();
    ImGui::PushItemWidth((row.width-36)*0.5F-24);
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    *value = std::clamp(*value, 0, count - 1);
    if (!g_comboCaptureForTest.empty() &&
        std::string(label) == GakumasVrI18n::ts(g_comboCaptureForTest)) {
        ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID("##value")), ImGuiPopupFlags_None);
        g_comboCaptureForTest.clear();
    }
    if (ImGui::BeginCombo("##value", items[*value], ImGuiComboFlags_HeightLarge)) {
        for (int index = 0; index < count; ++index) {
            if (ImGui::Selectable(items[index], index == *value, 0,
                                  ImVec2(0.0F, kControlHeight))) {
                *value = index;
            }
        }
        ImGui::EndCombo();
    }
    // Stick nudging keeps the row usable if the popup is hard to hit in VR.
    if (enabled && stick.active && !stick.consumed &&
        std::abs(stick.x) > 0.70F &&
        (ImGui::IsItemHovered() || ImGui::IsItemActive())) {
        *value = std::clamp(*value + (stick.x > 0.0F ? 1 : -1), 0, count - 1);
        stick.consumed = true;
    }
    if (!enabled) {
        ImGui::EndDisabled();
    }
    EndRow(row);
    ImGui::PopID();
}

// A full-width state button instead of an ImGui checkbox: the square box is a
// hard target for a VR laser pointer at this panel distance.
void ToggleRow(
    const char* label,
    bool* value,
    bool enabled,
    const char* onText,
    const char* offText,
    const char* actionLabel = nullptr,
    bool* actionClicked = nullptr) {
    if (value == nullptr || onText == nullptr || offText == nullptr) {
        return;
    }
    ImGui::PushID(label);
    // Toggles are not a sticky selection. Passing *value as selected made
    // On look identical to a focused slider (blue row + border).
    g_nextToggle = true;
    g_nextStacked = true;
    const float labelWidth = ImGui::CalcTextSize(label).x;
    const float minActionWidth = actionLabel ? std::max(120.0F, ImGui::CalcTextSize(actionLabel).x + 48) : 0;
    // Keep the switch in its own column even when a translation is long.
    // BeginRow wraps the title and grows both paired cells to the taller one.
    g_nextCompact = true;
    const float available = (g_nextCompact ? (g_gridWidth - 36) / 2 : g_gridWidth) - 24;
    g_nextActionWidth = actionLabel ? std::max(minActionWidth,
        std::min((available - 20)*0.5F - 32, available - 20 - labelWidth - 120)) : 0;
    const MenuRow row = BeginRow(label, enabled);
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    const float actionWidth = g_nextActionWidth;

    const float toggleWidth = ImGui::CalcItemWidth() - (actionLabel ? actionWidth + 20 : 0);
    const ImVec2 p(row.origin.x + 12, row.controlTop - 3);
    // Include the setting label and all space down to the control bottom.
    // The action half and its separating gap remain separate targets.
    ImGui::SetCursorScreenPos(ImVec2(p.x, row.origin.y));
    if (ImGui::InvisibleButton("##toggle", ImVec2(toggleWidth, row.height - 2))) *value = !*value;
    const bool hover = enabled && ImGui::IsItemHovered();
    const bool on = *value;
    const ImU32 ink = enabled ? IM_COL32(221,232,238,255) : IM_COL32(119,130,141,255);
    auto* d = ImGui::GetWindowDrawList();
    const float switchX = p.x + toggleWidth - 96;
    const float switchY = p.y + 5;
    d->AddRectFilled(ImVec2(switchX,switchY),ImVec2(switchX+96,switchY+42),
        enabled && on ? IM_COL32(57,145,132,255) : IM_COL32(71,84,96,255),21);
    d->AddCircleFilled(ImVec2(switchX+(on?75:21),switchY+21),16,ink);
    if (hover) d->AddRect(ImVec2(switchX-3,switchY-3),ImVec2(switchX+99,switchY+45),
        IM_COL32(126,214,198,255),24,0,2);
    if (actionLabel != nullptr) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + toggleWidth + 20.0F, p.y + 3));
        if (ImGui::Button(actionLabel, ImVec2(actionWidth, kControlHeight)) &&
            actionClicked != nullptr) {
            *actionClicked = true;
        }
    }
    if (!enabled) {
        ImGui::EndDisabled();
    }
    EndRow(row);
    ImGui::PopID();
}

enum class SliderPersist {
    Auto,
    Hold,
};

void ObserveAutoSlider(SliderPersist persist, bool held, bool changed) {
    if (persist != SliderPersist::Auto) {
        return;
    }
    if (changed) {
        g_autoSliderDirty = true;
    }
    if (held) {
        g_autoSliderHeld = true;
        g_autoSliderQuietSeconds = 0.0F;
    }
}

void SliderRow(
    const char* label,
    float* value,
    float minimum,
    float maximum,
    const char* format,
    bool enabled,
    const std::array<openxr::OpenXrContext::AaMenuStick, 2>& sticks,
    const char* actionLabel = nullptr,
    bool* actionClicked = nullptr,
    SliderPersist persist = SliderPersist::Auto) {
    if (value == nullptr) {
        return;
    }
    ImGui::PushID(label);
    const ImGuiID id = ImGui::GetID("##value");
    if (!enabled && g_selectedSlider == id) {
        g_selectedSlider = 0;
    }
    const bool selected = enabled && g_selectedSlider == id;
    const MenuRow row = BeginRow(label, enabled, selected);
    if (!enabled) {
        ImGui::BeginDisabled();
    }

    const float actionWidth = actionLabel ? std::max(120.0F, ImGui::CalcTextSize(actionLabel).x + 36) : 0;
    const float right = row.origin.x + row.width - 12;
    bool actionHover = false;
    if (actionLabel) {
        ImGui::SetCursorScreenPos(ImVec2(right - actionWidth, row.origin.y));
        if (ImGui::Button(actionLabel, ImVec2(actionWidth, kControlHeight)) && actionClicked) *actionClicked = true;
        actionHover = ImGui::IsItemHovered();
    }
    // Native SliderFloat owns input/rounding. Its frame, value and thumb are
    // invisible; the same value is presented as an explicit rail and handle.
    const ImVec2 track(row.origin.x + 12, row.origin.y + 46);
    const float width = row.width - 24;
    ImGui::SetCursorScreenPos(track);
    ImGui::PushItemWidth(width);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0,0));
    for (ImGuiCol c : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive,
            ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive, ImGuiCol_Text})
        ImGui::PushStyleColor(c, ImVec4(0,0,0,0));
    const float valueBefore = *value;
    ImGui::SliderFloat("##value", value, minimum, maximum, format);
    const bool sliderClicked = ImGui::IsItemClicked();
    const bool sliderActive = ImGui::IsItemActive();
    const bool sliderHovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar();
    ImGui::PopItemWidth();
    const bool rowClicked = !actionHover && ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsMouseHoveringRect(row.origin, ImVec2(row.origin.x + row.width, row.origin.y + row.height));
    if (enabled && (rowClicked || sliderClicked || sliderActive)) {
        g_selectedSlider = id;
        if (rowClicked || sliderClicked) g_sliderRowClicked = true;
    }
    const bool focused = enabled && g_selectedSlider == id;
    g_selectedSliderSeen |= focused;
    const bool stickActive = focused && openxr::AaMenuSliderStickActive(sticks);
    if (focused) openxr::NudgeAaMenuSlider(*value, minimum, maximum, ImGui::GetIO().DeltaTime, sticks);
    g_helpDragging |= sliderActive || stickActive;
    ObserveAutoSlider(persist, sliderActive || stickActive, *value != valueBefore);

    auto* d = ImGui::GetWindowDrawList();
    const float grab = std::min(ImGui::GetStyle().GrabMinSize, width - 4);
    const float left = track.x + 2 + grab * 0.5F;
    const float end = track.x + width - 2 - grab * 0.5F;
    const float fraction = std::clamp((*value - minimum) / (maximum - minimum), 0.0F, 1.0F);
    const ImVec2 centre(left + (end - left) * fraction, track.y + 16);
    d->AddLine(ImVec2(left,centre.y), ImVec2(end,centre.y), IM_COL32(74,89,101,255),8);
    d->AddLine(ImVec2(left,centre.y), centre, IM_COL32(71,147,138,255),8);
    d->AddCircleFilled(centre,12, focused ? IM_COL32(218,245,242,255) : IM_COL32(97,194,176,255));
    if (focused || sliderHovered) d->AddCircle(centre,16,
        focused ? IM_COL32(126,220,207,255) : IM_COL32(141,161,171,255),0,2);
    if (focused && !selected) d->AddRectFilled(ImVec2(row.origin.x,row.origin.y+2),
        ImVec2(row.origin.x+4,row.origin.y+row.height-2),IM_COL32(97,194,176,255),2);
    char valueText[64];
    ImFormatString(valueText, sizeof(valueText), format, *value);
    const float valueRight = right - (actionLabel ? actionWidth + 24 : 0);
    d->AddText(ImVec2(valueRight - ImGui::CalcTextSize(valueText).x,
        row.origin.y + (kControlHeight - ImGui::GetFontSize()) * 0.5F),
        IM_COL32(230,234,243,255),valueText);
    if (!enabled) {
        ImGui::EndDisabled();
    }
    EndRow(row);
    ImGui::PopID();
}

void HelpLine(const char* text) {
    if (g_helpRows.empty() || text == nullptr) return;
    auto& entry = g_helpRows.back();
    entry.text = text;


}

float ActionWidth(const char* text) {
    return std::max(150.0F, ImGui::CalcTextSize(text).x + 36.0F);
}

void EnsureDraftRenderScale() {
    if (g_draftRenderScaleValid) {
        return;
    }
    g_draftRenderScale = std::clamp(
        GakumasLocal::Config::vrStereoRenderScale, 0.25F, 1.5F);
    g_draftRenderScaleValid = true;
}

void PersistMenuConfig(VrLog& log, const char* reason) {
    namespace Config = GakumasLocal::Config;
    Config::ClampVrEyeAaSettings();
    Config::ClampVrPointerSettings();
    Config::vrStereoRenderScale =
        std::clamp(Config::vrStereoRenderScale, 0.25F, 1.5F);
    Config::vrEyeOutlineWidth =
        std::clamp(Config::vrEyeOutlineWidth, 0.0F, 1.0F);
    Config::vrEyeShadeBandBias =
        std::clamp(Config::vrEyeShadeBandBias, -0.5F, 0.5F);
    Config::vrToonFollowRef = std::clamp(Config::vrToonFollowRef, 0, 2);
    Config::vrMenuLanguage = std::clamp(
        Config::vrMenuLanguage,
        Config::kVrMenuLanguageSystem,
        Config::kVrMenuLanguageEn);
    Config::SaveVrConfig(VrConfigJson);
    g_autoSliderDirty = false;
    g_autoSliderQuietSeconds = 0.0F;
    g_savedFlashSeconds = 2.5F;
    log.Write(std::string("[VR][menu] autosave reason=") + reason);
}

void PersistConfigChange(bool changed, VrLog& log, const char* reason) {
    if (changed) {
        PersistMenuConfig(log, reason);
    }
}

void TickAutoSliderPersist(float deltaSeconds, VrLog& log) {
    if (!g_autoSliderDirty) {
        g_autoSliderQuietSeconds = 0.0F;
        return;
    }
    if (g_autoSliderHeld) {
        g_autoSliderQuietSeconds = 0.0F;
        return;
    }
    g_autoSliderQuietSeconds += deltaSeconds;
    if (g_autoSliderQuietSeconds >= kAutoSliderDebounceSeconds) {
        PersistMenuConfig(log, "slider");
    }
}

void PersistLocalizeAndPromptRestart(VrLog& log) {
    PersistMenuConfig(log, "localize");
    g_openLocalizeRestart = true;
    log.Write(
        std::string("[VR][menu] localizeText=") +
        (GakumasLocal::Config::vrLocalizeText ? "1" : "0") +
        " saved restart-required");
}

bool TabButton(const char* text, bool active) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.075F, 0.095F, 0.12F, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.11F, 0.15F, 0.18F, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, active ? ImVec4(0.53F, 0.87F, 0.80F, 1)
        : ImVec4(0.66F, 0.71F, 0.77F, 1));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float width = std::max(150.0F, ImGui::CalcTextSize(text).x + 56.0F);
    const bool clicked = ImGui::Button(text, ImVec2(width, 54.0F));
    if (active) ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(p.x + 28, p.y + 49), ImVec2(p.x + width - 28, p.y + 53),
        IM_COL32(97, 194, 176, 255), 2);
    ImGui::PopStyleColor(3);
    return clicked;
}

float ConsumeDeltaSeconds() {
    const auto now = std::chrono::steady_clock::now();
    float delta = 1.0F / 90.0F;
    if (g_lastPaintTimeValid) {
        delta = std::chrono::duration<float>(now - g_lastPaintTime).count();
    }
    g_lastPaintTime = now;
    g_lastPaintTimeValid = true;
    return std::clamp(delta, 1.0F / 240.0F, 1.0F / 15.0F);
}

} // namespace

void FlushVrAaMenuConfigSave(VrLog& log) {
    if (!g_autoSliderDirty) {
        return;
    }
    PersistMenuConfig(log, "menu-hide");
}

bool PaintVrAaMenu(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* destination,
    std::uint32_t width,
    std::uint32_t height,
    const openxr::OpenXrContext::AaMenuInput& input,
    openxr::OpenXrContext::AaMenuOutput& output,
    VrLog& log) {
    output = {};
    if (device == nullptr || context == nullptr || destination == nullptr ||
        width == 0 || height == 0) {
        return false;
    }
    if (!EnsureContext(device, log)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC destinationDesc{};
    destination->GetDesc(&destinationDesc);
    if (!EnsureOwnedTarget(device, width, height, destinationDesc.Format, log)) {
        return false;
    }

    ui::ScopedImGuiContext current(g_context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DisplayFramebufferScale = ImVec2(1.0F, 1.0F);
    io.FontGlobalScale = 1.0F;
    io.DeltaTime = ConsumeDeltaSeconds();
    if (input.hovering) {
        io.AddMousePosEvent(input.u * io.DisplaySize.x, input.v * io.DisplaySize.y);
    } else {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    io.AddMouseButtonEvent(0, input.hovering && input.triggerHeld);

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    using GakumasVrI18n::ts;
    namespace Config = GakumasLocal::Config;

    g_sliderRowClicked = false;
    g_selectedSliderSeen = false;
    g_autoSliderHeld = false;
    EnsureDraftRenderScale();

    StickInput stick;
    stick.active = input.thumbstickActive;
    stick.x = input.thumbstickX;

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin(
        "##vr_settings_menu",
        nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float headerY = ImGui::GetCursorPosY();
    ImGui::SetWindowFontScale(1.2F);
    ImGui::TextUnformatted(ts("vr_menu_title"));
    ImGui::SetWindowFontScale(1.0F);
    const float restoreWidth = ActionWidth(ts("vr_menu_restore_defaults"));
    const float closeWidth = ActionWidth(ts("vr_aa_close"));
    const float quitWidth = ActionWidth(ts("vr_menu_quit"));
    ImGui::SetCursorPos(ImVec2(
        ImGui::GetContentRegionMax().x - restoreWidth - closeWidth - quitWidth -
            24.0F,
        headerY));
    if (ImGui::Button(ts("vr_menu_restore_defaults"), ImVec2(restoreWidth, 54.0F))) {
        ImGui::OpenPopup("##vr_restore_confirm");
    }
    ImGui::SameLine();
    if (ImGui::Button(ts("vr_aa_close"), ImVec2(closeWidth, 54.0F))) {
        output.requestClose = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(ts("vr_menu_quit"), ImVec2(quitWidth, 54.0F))) {
        ImGui::OpenPopup("##vr_quit_confirm");
    }

    ImGui::SetCursorPosY(headerY + 54.0F + 8.0F);

    const float tabsY = ImGui::GetCursorScreenPos().y;
    if (g_tab < 0 || g_tab > 3) {
        g_tab = 0;
    }
    if (TabButton(ts("vr_menu_tab_general"), g_tab == 0)) {
        g_tab = 0;
    }
    ImGui::SameLine();
    if (TabButton(ts("vr_menu_tab_picture"), g_tab == 1)) {
        g_tab = 1;
    }
    ImGui::SameLine();
    if (TabButton(ts("vr_menu_tab_camera"), g_tab == 2)) {
        g_tab = 2;
    }
    ImGui::SameLine();
    if (TabButton(ts("vr_menu_tab_experimental"), g_tab == 3)) {
        g_tab = 3;
    }
    if (g_savedFlashSeconds > 0.0F) {
        const char* saved = ts("vr_aa_saved");
        ImGui::GetWindowDrawList()->AddText(ImVec2(io.DisplaySize.x - 26 - ImGui::CalcTextSize(saved).x,
            tabsY + (54 - ImGui::GetFontSize()) * 0.5F), IM_COL32(126,214,198,255), saved);
        g_savedFlashSeconds = std::max(0.0F, g_savedFlashSeconds - io.DeltaTime);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const bool pageChanged = g_helpTab != g_tab || g_helpLanguage != Config::vrMenuLanguage;
    if (pageChanged) {
        g_selectedSlider = 0;
        g_helpState.Reset(); g_helpTab = g_tab; g_helpLanguage = Config::vrMenuLanguage;
    }
    ImGui::BeginChild("##settings_body", ImVec2(0, 500), false);
    if (pageChanged) ImGui::SetScrollY(0);
    if (g_scrollCaptureForTest >= 0) {
        ImGui::SetScrollY(g_scrollCaptureForTest);
        g_scrollCaptureForTest = -1;
    }
    g_gridX = ImGui::GetCursorScreenPos().x;
    g_gridY = ImGui::GetCursorScreenPos().y;
    g_gridWidth = ImGui::GetContentRegionAvail().x;
    g_gridRight = false; g_helpDragging = false; g_helpRows.clear();
    const char* aaModeItems[] = {
        ts("vr_aa_mode_inherit"),
        ts("vr_aa_mode_taa"),
        ts("vr_aa_mode_smaa"),
        ts("vr_aa_mode_smaa_t2x"),
        ts("vr_aa_mode_tscmaa"),
        ts("vr_aa_mode_none"),
    };
    constexpr std::array<int, 6> kAaDisplayToStored{0, 1, 2, 4, 5, 3};
    int aaModeDisplay = 0;
    for (std::size_t index = 0; index < kAaDisplayToStored.size(); ++index) {
        if (kAaDisplayToStored[index] == Config::vrEyeAaMode) {
            aaModeDisplay = static_cast<int>(index);
            break;
        }
    }
    const char* taaQualityItems[] = {
        ts("vr_taa_quality_very_low"),
        ts("vr_taa_quality_low"),
        ts("vr_taa_quality_medium"),
        ts("vr_taa_quality_high"),
        ts("vr_taa_quality_very_high"),
    };
    const char* smaaQualityItems[] = {
        ts("low"),
        ts("middle"),
        ts("hign"),
    };

    if (g_tab == 0) {
        const char* languageItems[] = {
            ts("vr_menu_language_system"),
            ts("vr_menu_language_zh_cn"),
            ts("vr_menu_language_zh_tw"),
            ts("vr_menu_language_ja"),
            ts("vr_menu_language_en"),
        };
        const int languageBefore = Config::vrMenuLanguage;
        ComboRow(
            ts("vr_menu_language"),
            &Config::vrMenuLanguage,
            languageItems,
            5,
            true,
            stick);
        PersistConfigChange(
            languageBefore != Config::vrMenuLanguage, log, "language");

        const bool localizeBefore = Config::vrLocalizeText;
        ToggleRow(
            ts("vr_menu_localize"),
            &Config::vrLocalizeText,
            Config::enabled,
            ts("vr_menu_toggle_on"),
            ts("vr_menu_toggle_off"));
        HelpLine(ts("vr_menu_localize_help"));
        if (localizeBefore != Config::vrLocalizeText) {
            PersistLocalizeAndPromptRestart(log);
        }

        const bool gazeBefore = Config::vrLiveGaze;
        ToggleRow(ts("vr_menu_live_gaze"), &Config::vrLiveGaze, true,
            ts("vr_menu_toggle_on"), ts("vr_menu_toggle_off"));
        HelpLine(ts("vr_menu_live_gaze_help"));
        if (gazeBefore != Config::vrLiveGaze) {
            SetLiveGazeRequested(Config::vrLiveGaze);
            PersistMenuConfig(log, "live-gaze");
        }
        if (Config::vrLiveGaze) {
            const char* gazeScopes[] = {ts("vr_gaze_selected"), ts("vr_gaze_all")};
            const int gazeScopeBefore = Config::vrLiveGazeScope;
            ComboRow(ts("vr_menu_gaze_scope"), &Config::vrLiveGazeScope,
                gazeScopes, 2, true, stick);
            HelpLine(ts("vr_menu_gaze_scope_help"));
            if (gazeScopeBefore != Config::vrLiveGazeScope) {
                SetLiveGazeScope(Config::vrLiveGazeScope);
                PersistMenuConfig(log, "live-gaze-scope");
            }
            const char* gazePresets[] = {ts("vr_gaze_stable"), ts("vr_gaze_standard"), ts("vr_gaze_priority")};
            const int gazePresetBefore = Config::vrLiveGazePreset;
            ComboRow(ts("vr_menu_gaze_preset"), &Config::vrLiveGazePreset,
                gazePresets, 3, true, stick);
            HelpLine(ts("vr_menu_gaze_preset_help"));
            if (gazePresetBefore != Config::vrLiveGazePreset) {
                SetLiveGazePreset(Config::vrLiveGazePreset);
                PersistMenuConfig(log, "live-gaze-preset");
            }
        }

        const bool tinyBefore = Config::vrSourceCameraTiny;
        ToggleRow(
            ts("vr_menu_source_tiny"),
            &Config::vrSourceCameraTiny,
            true,
            ts("vr_menu_toggle_on"),
            ts("vr_menu_toggle_off"));
        HelpLine(ts("vr_menu_source_tiny_help"));
        if (tinyBefore != Config::vrSourceCameraTiny) {
            PersistMenuConfig(log, "source-tiny");
            log.Write(
                std::string("[VR][menu] source camera tiny=") +
                (Config::vrSourceCameraTiny ? "1" : "0"));
        }
        const bool gripTransparentBefore = Config::vrGripPanelTransparent;
        ToggleRow(
            ts("vr_menu_grip_transparent"),
            &Config::vrGripPanelTransparent,
            Config::vrSourceCameraTiny || Config::vrDisableSourceCamera,
            ts("vr_menu_toggle_on"),
            ts("vr_menu_toggle_off"));
        HelpLine(ts("vr_menu_grip_transparent_help"));
        if (gripTransparentBefore != Config::vrGripPanelTransparent) {
            PersistMenuConfig(log, "grip-transparent");
            log.Write(
                std::string("[VR][menu] grip panel transparent=") +
                (Config::vrGripPanelTransparent ? "1" : "0"));
        }
    } else if (g_tab == 1) {
        bool applyRenderScale = false;
        SliderRow(
            ts("vr_menu_render_scale"),
            &g_draftRenderScale,
            0.25F,
            1.5F,
            "%.2f",
            true,
            input.sticks,
            ts("vr_menu_apply"),
            &applyRenderScale,
            SliderPersist::Hold);
        if (applyRenderScale) {
            // Render scale resizes the eye buffers; only the session and
            // Unity's render thread may do that. AA knobs stay live without
            // this button. The draft is not written until Apply.
            Config::vrStereoRenderScale = g_draftRenderScale;
            output.requestRenderScale = true;
            output.renderScale = Config::vrStereoRenderScale;
            PersistMenuConfig(log, "render-scale-apply");
            log.Write(
                "[VR][menu] apply requested renderScale=" +
                std::to_string(Config::vrStereoRenderScale));
        }
        bool resetOutlineWidth = false;
        SliderRow(
            ts("vr_menu_outline_width"),
            &Config::vrEyeOutlineWidth,
            0.0F,
            1.0F,
            "%.2f",
            true,
            input.sticks,
            ts("vr_menu_reset"),
            &resetOutlineWidth);
        if (resetOutlineWidth) {
            Config::vrEyeOutlineWidth = Config::kDefaultVrEyeOutlineWidth;
            PersistMenuConfig(log, "outline-reset");
            log.Write(
                "[VR][menu] reset outline width to default=" +
                std::to_string(Config::vrEyeOutlineWidth));
        }
        if (camera::ReadVrFreeCameraMode() == camera::VrFreeCameraMode::Off) {
            const bool bloomFollowBefore = Config::vrBloomFollowSourceCamera;
            ToggleRow(
                ts("vr_menu_bloom_follow_source"),
                &Config::vrBloomFollowSourceCamera,
                true,
                ts("vr_menu_toggle_on"),
                ts("vr_menu_toggle_off"));
            HelpLine(ts("vr_menu_bloom_follow_source_help"));
            if (bloomFollowBefore != Config::vrBloomFollowSourceCamera) {
                PersistMenuConfig(log, "bloom-follow");
                log.Write(
                    std::string("[VR][menu] bloom follow source=") +
                    (Config::vrBloomFollowSourceCamera ? "1" : "0"));
            }
        }
        const bool volumeAnchorBefore = Config::vrVolumeSourceAnchor;
        ToggleRow(
            ts("vr_menu_volume_anchor"),
            &Config::vrVolumeSourceAnchor,
            true,
            ts("vr_menu_toggle_on"),
            ts("vr_menu_toggle_off"));
        HelpLine(ts("vr_menu_volume_anchor_help"));
        if (volumeAnchorBefore != Config::vrVolumeSourceAnchor) {
            PersistMenuConfig(log, "volume-anchor");
            log.Write(
                std::string("[VR][menu] volume source anchor=") +
                (Config::vrVolumeSourceAnchor ? "1" : "0"));
        }
        const bool overlayBefore = Config::vrHideUiTextureOverlay;
        ToggleRow(
            ts("vr_menu_hide_ui_texture_overlay"),
            &Config::vrHideUiTextureOverlay,
            true,
            ts("vr_menu_toggle_on"),
            ts("vr_menu_toggle_off"));
        HelpLine(ts("vr_menu_hide_ui_texture_overlay_help"));
        if (overlayBefore != Config::vrHideUiTextureOverlay) {
            PersistMenuConfig(log, "hide-ui-overlay");
            log.Write(
                std::string("[VR][menu] skip eye VL lens cards=") +
                (Config::vrHideUiTextureOverlay ? "1" : "0"));
        }
        const int aaModeBefore = Config::vrEyeAaMode;
        ComboRow(
            ts("vr_menu_aa_mode"), &aaModeDisplay, aaModeItems, 6, true, stick);
        Config::vrEyeAaMode = kAaDisplayToStored[static_cast<std::size_t>(
            std::clamp(aaModeDisplay, 0, 5))];
        PersistConfigChange(
            aaModeBefore != Config::vrEyeAaMode, log, "aa-mode");
        if (Config::vrEyeAaMode == 1) {
            const int taaQualityBefore = Config::vrEyeTaaQuality;
            ComboRow(
                ts("vr_taa_quality"),
                &Config::vrEyeTaaQuality,
                taaQualityItems,
                5,
                true,
                stick);
            HelpLine(ts("vr_taa_quality_help"));
            if (taaQualityBefore != Config::vrEyeTaaQuality) {
                // Hardware feedback confirms LED/sign color issues at Medium+.
                if (Config::vrEyeTaaQuality >= 2 && taaQualityBefore < 2) {
                    g_taaQualityPendingValue = Config::vrEyeTaaQuality;
                    Config::vrEyeTaaQuality = taaQualityBefore;
                    g_openTaaConfirm = true;
                } else {
                    PersistMenuConfig(log, "taa-quality");
                    log.Write(
                        "[VR][menu] taa quality=" +
                        std::to_string(Config::vrEyeTaaQuality));
                }
            }
            SliderRow(
                ts("vr_taa_jitter_scale"), &Config::vrEyeTaaJitterScale, 0.0F,
                2.0F, "%.2f", true, input.sticks);
            HelpLine(ts("vr_taa_jitter_scale_help"));
            SliderRow(
                ts("vr_taa_frame_influence"), &Config::vrEyeTaaFrameInfluence,
                0.0F, 1.0F, "%.3f", true, input.sticks);
            HelpLine(ts("vr_taa_frame_influence_help"));
        } else if (Config::vrEyeAaMode == 2 || Config::vrEyeAaMode == 4 ||
                   Config::vrEyeAaMode == 5) {
            const int smaaBefore = Config::vrEyeSmaaQuality;
            ComboRow(
                ts(Config::vrEyeAaMode == 5
                    ? "vr_tscmaa_quality"
                    : "vr_smaa_quality"),
                &Config::vrEyeSmaaQuality,
                smaaQualityItems,
                3,
                true,
                stick);
            PersistConfigChange(
                smaaBefore != Config::vrEyeSmaaQuality, log, "smaa-quality");
        }
    } else if (g_tab == 2) {
        // Free-camera tab. The mode combo mirrors the live rig state (the
        // Unity thread republishes it every frame); a user change posts an
        // atomic request the game thread consumes.
        const bool firstPersonEnabled =
            camera::IsVrFirstPersonEnabled(Config::vrFpDirectionFollow);
        const char* freecamModeItems[] = {
            ts("vr_freecam_mode_off"),
            ts("vr_freecam_mode_free"),
            ts("vr_freecam_mode_follow"),
            ts("vr_freecam_mode_fp"),
        };
        int modeDisplay =
            static_cast<int>(camera::ReadVrFreeCameraMode());
        const int modeCount = firstPersonEnabled ? 4 : 3;
        if (!firstPersonEnabled &&
            modeDisplay == static_cast<int>(camera::VrFreeCameraMode::FirstPerson)) {
            camera::RequestVrFreeCameraMode(camera::VrFreeCameraMode::Off);
            modeDisplay = 0;
        }
        modeDisplay = std::clamp(modeDisplay, 0, modeCount - 1);
        const int modeBefore = modeDisplay;
        ComboRow(
            ts("vr_menu_freecam_mode"),
            &modeDisplay,
            freecamModeItems,
            modeCount,
            true,
            stick);
        if (modeDisplay != modeBefore) {
            camera::RequestVrFreeCameraMode(
                static_cast<camera::VrFreeCameraMode>(modeDisplay));
            log.Write(
                "[VR][menu] freecam mode request=" +
                std::to_string(modeDisplay));
        }
        HelpLine(ts("vr_menu_freecam_mode_help"));

        const char* boneItems[camera::kVrFreeCameraBoneOptionCount];
        for (int i = 0; i < camera::kVrFreeCameraBoneOptionCount; ++i) {
            boneItems[i] = ts(camera::kVrFreeCameraBoneOptions[i].i18nKey);
        }
        const int boneValue = camera::ReadVrFreeCameraFollowBone();
        int boneDisplay = 0;
        for (int i = 0; i < camera::kVrFreeCameraBoneOptionCount; ++i) {
            if (camera::kVrFreeCameraBoneOptions[i].humanBodyBoneValue ==
                boneValue) {
                boneDisplay = i;
                break;
            }
        }
        const int boneBefore = boneDisplay;
        ComboRow(
            ts("vr_menu_freecam_bone"),
            &boneDisplay,
            boneItems,
            camera::kVrFreeCameraBoneOptionCount,
            true,
            stick);
        if (boneDisplay != boneBefore) {
            const int newBone = camera::kVrFreeCameraBoneOptions[
                std::clamp(
                    boneDisplay,
                    0,
                    camera::kVrFreeCameraBoneOptionCount - 1)]
                .humanBodyBoneValue;
            camera::SetVrFreeCameraFollowBone(newBone);
            log.Write(
                "[VR][menu] freecam follow bone=" + std::to_string(newBone));
        }
        HelpLine(ts("vr_menu_freecam_bone_help"));

        const char* yButtonItems[] = {
            ts("vr_freecam_y_chara"),
            ts("vr_freecam_y_bone"),
        };
        const int yBefore = Config::vrCameraYButtonBone;
        ComboRow(
            ts("vr_menu_freecam_y_button"),
            &Config::vrCameraYButtonBone,
            yButtonItems,
            2,
            true,
            stick);
        if (yBefore != Config::vrCameraYButtonBone) {
            PersistMenuConfig(log, "y-button");
            log.Write(
                "[VR][menu] freecam y button=" +
                std::to_string(Config::vrCameraYButtonBone));
        }

        const char* smoothingItems[] = {
            ts("vr_follow_smooth_auto"), ts("vr_follow_smooth_custom"),
        };
        const int smoothingBefore = Config::vrFollowSmoothingPreset;
        const float horizontalBefore = Config::vrFollowHorizontalMs;
        const float verticalBefore = Config::vrFollowVerticalMs;
        int smoothingDisplay = Config::vrFollowSmoothingPreset == 5 ? 1 : 0;
        ComboRow(ts("vr_menu_follow_smoothing"), &smoothingDisplay,
            smoothingItems, camera::kFollowSmoothingPresetCount, true, stick);
        Config::vrFollowSmoothingPreset = smoothingDisplay == 1 ? 5 : 0;
        HelpLine(ts("vr_menu_follow_smoothing_help"));
        if (Config::vrFollowSmoothingPreset == 5) {
            SliderRow(ts("vr_follow_horizontal"), &Config::vrFollowHorizontalMs,
                0.0F, 500.0F, "%.0f ms", true, input.sticks);
            HelpLine(ts("vr_follow_axis_help"));
            SliderRow(ts("vr_follow_vertical"), &Config::vrFollowVerticalMs,
                0.0F, 500.0F, "%.0f ms", true, input.sticks);
            HelpLine(ts("vr_follow_axis_help"));
        }
        if (smoothingBefore != Config::vrFollowSmoothingPreset ||
            horizontalBefore != Config::vrFollowHorizontalMs ||
            verticalBefore != Config::vrFollowVerticalMs) {
            camera::PublishFollowSmoothing(Config::vrFollowSmoothingPreset,
                Config::vrFollowHorizontalMs, Config::vrFollowVerticalMs);
            if (smoothingBefore != Config::vrFollowSmoothingPreset) {
                PersistMenuConfig(log, "follow-smoothing");
                const auto settings = camera::ReadFollowSmoothing();
                log.Write("[VR][menu] FOLLOW_SMOOTHING_APPLY preset=" +
                    std::to_string(settings.preset) + " horizontalMs=" +
                    std::to_string(settings.horizontalMs) + " verticalMs=" +
                    std::to_string(settings.verticalMs));
            }
            // Sliders use the existing debounced save; publication is immediate.
        }

        const char* fpFollowItems[] = {
            ts("vr_fp_follow_off"),
            ts("vr_fp_follow_none"),
            ts("vr_fp_follow_turn"),
            ts("vr_fp_follow_turn_tilt"),
        };
        int fpDisplay = camera::VrFpFollowToDisplay(Config::vrFpDirectionFollow);
        const int fpBefore = Config::vrFpDirectionFollow;
        ComboRow(
            ts("vr_menu_fp_follow"),
            &fpDisplay,
            fpFollowItems,
            camera::kVrFpFollowDisplayCount,
            true,
            stick);
        Config::vrFpDirectionFollow = camera::VrFpFollowFromDisplay(fpDisplay);
        HelpLine(ts("vr_menu_fp_follow_help"));
        if (fpBefore != Config::vrFpDirectionFollow) {
            if (camera::IsVrFpAutoFollow(Config::vrFpDirectionFollow)) {
                // Motion-sickness hazard: hold the new value until the user
                // confirms in the modal below; revert in the meantime.
                g_fpFollowPendingValue = Config::vrFpDirectionFollow;
                Config::vrFpDirectionFollow = fpBefore;
                ImGui::OpenPopup("##vr_fp_follow_confirm");
            } else {
                if (!camera::IsVrFirstPersonEnabled(Config::vrFpDirectionFollow) &&
                    camera::ReadVrFreeCameraMode() ==
                        camera::VrFreeCameraMode::FirstPerson) {
                    camera::RequestVrFreeCameraMode(camera::VrFreeCameraMode::Off);
                }
                PersistMenuConfig(log, "fp-follow");
                log.Write(
                    "[VR][menu] fp direction follow=" +
                    std::to_string(Config::vrFpDirectionFollow));
            }
        }
        if (g_popupCaptureForTest == "##vr_fp_follow_confirm") {
            ImGui::OpenPopup(g_popupCaptureForTest.c_str());
            g_popupCaptureForTest.clear();
        }
        ImGui::SetNextWindowSize(ImVec2(620.0F, 0.0F), ImGuiCond_Always);
        if (ImGui::BeginPopupModal(
                "##vr_fp_follow_confirm",
                nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoMove)) {
            ImGui::TextWrapped("%s", ts("vr_fp_follow_confirm"));
            ImGui::Spacing();
            const float confirmWidth = ActionWidth(ts("ok"));
            const float cancelWidth = ActionWidth(ts("cancel"));
            if (ImGui::Button(ts("ok"), ImVec2(confirmWidth, 54.0F))) {
                Config::vrFpDirectionFollow = g_fpFollowPendingValue;
                PersistMenuConfig(log, "fp-follow");
                log.Write(
                    "[VR][menu] fp direction follow=" +
                    std::to_string(Config::vrFpDirectionFollow) +
                    " (confirmed)");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(ts("cancel"), ImVec2(cancelWidth, 54.0F))) {
                log.Write("[VR][menu] fp direction follow change cancelled");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

    } else if (g_tab == 3) {
        const bool handGlowBefore = Config::vrHandGlowSticks;
        bool cycleHandGlow = false;
        ToggleRow(
            ts("vr_menu_hand_glow_sticks"),
            &Config::vrHandGlowSticks,
            true,
            ts("vr_menu_toggle_on"),
            ts("vr_menu_toggle_off"),
            ts("vr_menu_hand_glow_color"),
            &cycleHandGlow);
        HelpLine(ts("vr_menu_hand_glow_sticks_help"));
        if (handGlowBefore != Config::vrHandGlowSticks) {
            PersistMenuConfig(log, "hand-glow");
            log.Write(
                std::string("[VR][menu] hand glow sticks=") +
                (Config::vrHandGlowSticks ? "1" : "0"));
        }
        if (cycleHandGlow) {
            CycleHandGlowStickColor();
            log.Write("[VR][menu] hand glow color cycle");
        }
        const bool anchorBefore = Config::vrActorShadowSourceAnchor;
        ToggleRow(
            ts("vr_menu_actor_shadow_anchor"),
            &Config::vrActorShadowSourceAnchor,
            true,
            ts("vr_menu_toggle_on"),
            ts("vr_menu_toggle_off"));
        HelpLine(ts("vr_menu_actor_shadow_anchor_help"));
        if (anchorBefore != Config::vrActorShadowSourceAnchor) {
            PersistMenuConfig(log, "shadow-anchor");
            log.Write(
                std::string("[VR][menu] actor shadow source anchor=") +
                (Config::vrActorShadowSourceAnchor ? "1" : "0"));
        }
        const bool toonBefore = Config::vrActorToonSourceAnchor;
        ToggleRow(
            ts("vr_menu_actor_toon_anchor"),
            &Config::vrActorToonSourceAnchor,
            true,
            ts("vr_menu_toggle_on"),
            ts("vr_menu_toggle_off"));
        HelpLine(ts("vr_menu_actor_toon_anchor_help"));
        if (toonBefore != Config::vrActorToonSourceAnchor) {
            PersistMenuConfig(log, "toon-anchor");
            log.Write(
                std::string("[VR][menu] actor toon source anchor=") +
                (Config::vrActorToonSourceAnchor ? "1" : "0"));
        }
        const char* toonFollowItems[] = {
            ts("vr_toon_follow_source"),
            ts("vr_toon_follow_player"),
            ts("vr_toon_follow_headset"),
        };
        const int toonFollowBefore = Config::vrToonFollowRef;
        ComboRow(
            ts("vr_menu_toon_follow_ref"),
            &Config::vrToonFollowRef,
            toonFollowItems,
            3,
            Config::vrActorToonSourceAnchor,
            stick);
        HelpLine(ts("vr_menu_toon_follow_ref_help"));
        if (toonFollowBefore != Config::vrToonFollowRef) {
            PersistMenuConfig(log, "toon-follow");
            log.Write(
                "[VR][menu] toon follow ref=" +
                std::to_string(Config::vrToonFollowRef));
        }
        bool resetBandBias = false;
        SliderRow(
            ts("vr_menu_eye_shade_band_bias"),
            &Config::vrEyeShadeBandBias,
            -0.5F,
            0.5F,
            "%.2f",
            true,
            input.sticks,
            ts("vr_menu_reset"),
            &resetBandBias);
        HelpLine(ts("vr_menu_eye_shade_band_bias_help"));
        if (resetBandBias) {
            Config::vrEyeShadeBandBias = 0.0F;
            PersistMenuConfig(log, "shade-band-reset");
            log.Write("[VR][menu] reset character shade band bias to 0");
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(g_gridX, g_gridY + (g_gridRight ? g_gridPendingHeight : 0)));
    ImGui::Dummy(ImVec2(1,1));
    g_bodyScrollForTest = ImGui::GetScrollY();
    // Hidden controls and page changes must not retain slider ownership.
    if (!g_selectedSliderSeen ||
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g_sliderRowClicked)) {
        g_selectedSlider = 0;
    }
    // The pointing hand scrolls; selected sliders retain horizontal stick input.
    // Keep the body still while a dropdown, modal, or drag owns the interaction.
    if (!pageChanged && g_selectedSlider == 0 && input.hovering &&
        input.thumbstickActive && !input.triggerHeld && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        std::abs(input.thumbstickY) > openxr::kAaMenuSliderDeadzone) {
        constexpr float kScrollPixelsPerSecond = 600.0F;
        ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() -
            input.thumbstickY * kScrollPixelsPerSecond * io.DeltaTime,
            0.0F, ImGui::GetScrollMaxY()));
    }
    ImGui::EndChild();
    if (!g_popupCaptureForTest.empty()) {
        ImGui::OpenPopup(g_popupCaptureForTest.c_str());
        g_popupCaptureForTest.clear();
    }
    DrawHelpDock();
    // Open and render modal IDs in the same parent window, outside the scroll child.
    if (g_openTaaConfirm) {
        ImGui::OpenPopup("##vr_taa_quality_confirm");
        g_openTaaConfirm = false;
    }
    ImGui::SetNextWindowSize(ImVec2(620.0F, 0.0F), ImGuiCond_Always);
    if (ImGui::BeginPopupModal(
            "##vr_taa_quality_confirm",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("%s", ts("vr_taa_quality_confirm"));
        ImGui::Spacing();
        const float confirmWidth = ActionWidth(ts("ok"));
        const float cancelWidth = ActionWidth(ts("cancel"));
        if (ImGui::Button(ts("ok"), ImVec2(confirmWidth, 54.0F))) {
            Config::vrEyeTaaQuality = g_taaQualityPendingValue;
            PersistMenuConfig(log, "taa-quality");
            log.Write(
                "[VR][menu] taa quality=" +
                std::to_string(Config::vrEyeTaaQuality) +
                " (confirmed)");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(ts("cancel"), ImVec2(cancelWidth, 54.0F))) {
            log.Write("[VR][menu] taa quality change cancelled");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSize(ImVec2(620.0F, 0.0F), ImGuiCond_Always);
    if (ImGui::BeginPopupModal(
            "##vr_restore_confirm",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("%s", ts("vr_menu_restore_confirm"));
        ImGui::Spacing();
        const float confirmWidth = ActionWidth(ts("ok"));
        const float cancelWidth = ActionWidth(ts("cancel"));
        if (ImGui::Button(ts("ok"), ImVec2(confirmWidth, 54.0F))) {
            Config::ResetVrEyeAaToBaseline();
            Config::ResetVrPointerSettings();
            Config::vrFollowSmoothingPreset = 0;
            Config::vrLiveGaze = false;
            Config::vrLiveGazePreset = Config::kDefaultVrLiveGazePreset;
            Config::vrLiveGazeScope = Config::kDefaultVrLiveGazeScope;
            SetLiveGazePreset(Config::vrLiveGazePreset);
            SetLiveGazeScope(Config::vrLiveGazeScope);
            SetLiveGazeRequested(false);
            Config::vrFollowHorizontalMs = 0.0F;
            Config::vrFollowVerticalMs = 100.0F;
            camera::PublishFollowSmoothing(0, 0.0F, 100.0F);
            camera::SetVrFreeCameraFollowBone(9);
            Config::vrMenuLanguage = Config::kDefaultVrMenuLanguage;
            Config::vrEyeOutlineWidth = Config::kDefaultVrEyeOutlineWidth;
            Config::vrBloomFollowSourceCamera = false;
            Config::vrStereoRenderScale = 1.0F;
            g_draftRenderScale = 1.0F;
            g_draftRenderScaleValid = true;
            Config::vrActorShadowSourceAnchor = false;
            Config::vrActorToonSourceAnchor = false;
            Config::vrToonFollowRef = Config::kDefaultVrToonFollowRef;
            Config::vrVolumeSourceAnchor = true;
            Config::vrDisableSourceCamera = false;
            Config::vrGripPanelTransparent = false;
            Config::vrEyeDeferredStencilOff = false;
            Config::vrEyeShadeBandBias = 0.0F;
            Config::vrHideUiTextureOverlay = true;
            Config::vrHandGlowSticks = false;
            output.requestRenderScale = true;
            output.renderScale = Config::vrStereoRenderScale;
            PersistMenuConfig(log, "restore");
            log.Write("[VR][menu] restore defaults confirmed");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(ts("cancel"), ImVec2(cancelWidth, 54.0F))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSize(ImVec2(620.0F, 0.0F), ImGuiCond_Always);
    if (g_openLocalizeRestart) {
        ImGui::OpenPopup("##vr_localize_restart");
        g_openLocalizeRestart = false;
    }
    if (ImGui::BeginPopupModal(
            "##vr_localize_restart",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("%s", ts("vr_menu_localize_restart"));
        ImGui::Spacing();
        const float laterWidth = ActionWidth(ts("vr_menu_later"));
        const float quitWidth = ActionWidth(ts("vr_menu_quit_now"));
        if (ImGui::Button(ts("vr_menu_later"), ImVec2(laterWidth, 54.0F))) {
            log.Write("[VR][menu] localize restart later");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(ts("vr_menu_quit_now"), ImVec2(quitWidth, 54.0F))) {
            log.Write("[VR][menu] localize restart quit");
            output.requestQuit = true;
            output.requestQuitLocalize = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSize(ImVec2(620.0F, 0.0F), ImGuiCond_Always);
    if (ImGui::BeginPopupModal(
            "##vr_quit_confirm",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoMove)) {
        ImGui::TextWrapped("%s", ts("vr_menu_quit_confirm"));
        ImGui::Spacing();
        const float confirmWidth = ActionWidth(ts("ok"));
        const float cancelWidth = ActionWidth(ts("cancel"));
        if (ImGui::Button(ts("ok"), ImVec2(confirmWidth, 54.0F))) {
            log.Write("[VR][menu] game quit confirmed");
            output.requestQuit = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(ts("cancel"), ImVec2(cancelWidth, 54.0F))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !g_sliderRowClicked) {
        g_selectedSlider = 0;
    }

    Config::ClampVrEyeAaSettings();
    Config::ClampVrPointerSettings();
    g_draftRenderScale = std::clamp(g_draftRenderScale, 0.25F, 1.5F);
    Config::vrStereoRenderScale =
        std::clamp(Config::vrStereoRenderScale, 0.25F, 1.5F);
    Config::vrEyeOutlineWidth =
        std::clamp(Config::vrEyeOutlineWidth, 0.0F, 1.0F);
    Config::vrEyeShadeBandBias =
        std::clamp(Config::vrEyeShadeBandBias, -0.5F, 0.5F);
    Config::vrToonFollowRef = std::clamp(Config::vrToonFollowRef, 0, 2);
    Config::vrMenuLanguage = std::clamp(
        Config::vrMenuLanguage,
        Config::kVrMenuLanguageSystem,
        Config::kVrMenuLanguageEn);
    TickAutoSliderPersist(io.DeltaTime, log);
    if (output.requestClose) {
        FlushVrAaMenuConfigSave(log);
    }

    const ImVec2 versionSize = ImGui::CalcTextSize("gakumas-VRify " GAKUMAS_VR_VERSION);
    ImGui::SetCursorPos(ImVec2(
        io.DisplaySize.x - ImGui::GetStyle().WindowPadding.x - versionSize.x,
        io.DisplaySize.y - ImGui::GetStyle().WindowPadding.y - versionSize.y));
    ImGui::TextDisabled("%s", "gakumas-VRify " GAKUMAS_VR_VERSION);

    ImDrawList* overlay = ImGui::GetForegroundDrawList();
    for (const auto& cursor : input.cursors) {
        if (!cursor.hovering) {
            continue;
        }
        const ImVec2 point(
            cursor.u * io.DisplaySize.x, cursor.v * io.DisplaySize.y);
        overlay->AddCircleFilled(point, 11.0F, IM_COL32(255, 255, 255, 255));
        overlay->AddCircle(point, 14.0F, IM_COL32(20, 20, 24, 255), 0, 3.0F);
    }
    ImGui::End();
    ImGui::Render();

    // Record everything, including the copy into the OpenXR image, then hand a
    // single command list to the immediate context with state restore enabled.
    g_recorder->ClearState();
    const float clear[4] = {0.05F, 0.06F, 0.08F, 1.0F};
    g_recorder->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_recorder->ClearRenderTargetView(g_rtv, clear);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;
    g_recorder->RSSetViewports(1, &viewport);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ID3D11RenderTargetView* nullRtv = nullptr;
    g_recorder->OMSetRenderTargets(1, &nullRtv, nullptr);
    g_recorder->CopyResource(destination, g_rt);

    ID3D11CommandList* commandList = nullptr;
    const HRESULT recorded = g_recorder->FinishCommandList(FALSE, &commandList);
    if (FAILED(recorded) || commandList == nullptr) {
        if (commandList != nullptr) {
            commandList->Release();
        }
        log.Write(
            "[VR][menu] FinishCommandList failed hr=" +
            std::to_string(static_cast<long>(recorded)));
        return false;
    }
    context->ExecuteCommandList(commandList, TRUE);
    commandList->Release();
    if (!g_paintOkLogged) {
        g_paintOkLogged = true;
        log.Write(
            "[VR][menu] menu painted " + std::to_string(width) + "x" +
            std::to_string(height) + " path=deferred-command-list format=" +
            std::to_string(static_cast<int>(g_rtFormat)));
    }
    return true;
}

bool PaintVrPanelOverlay(
    ID3D11Device* device,
    ID3D11DeviceContext* context,
    ID3D11Texture2D* destination,
    std::uint32_t width,
    std::uint32_t height,
    const openxr::OpenXrContext::PanelOverlayInput& input,
    openxr::OpenXrContext::PanelOverlayOutput& output,
    VrLog& log) {
    output = {};
    if (device == nullptr || context == nullptr || destination == nullptr ||
        width == 0 || height == 0) {
        return false;
    }
    if (!EnsureContext(device, log)) {
        return false;
    }

    D3D11_TEXTURE2D_DESC destinationDesc{};
    destination->GetDesc(&destinationDesc);
    if (!EnsureOverlayTarget(device, width, height, destinationDesc.Format, log)) {
        return false;
    }

    // Strip layout must match the OpenXrContext quad subImage rects.
    const float barHeight = 168.0F;
    const float hintTop = 192.0F;
    const float hintHeight = 320.0F;

    ui::ScopedImGuiContext current(g_context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DisplayFramebufferScale = ImVec2(1.0F, 1.0F);
    io.FontGlobalScale = 1.0F;
    io.DeltaTime = ConsumeDeltaSeconds();
    // Only the bar strip is interactive; its cursor UV maps onto [0, barHeight).
    if (input.adjustMode && input.hovering) {
        io.AddMousePosEvent(
            input.u * io.DisplaySize.x, input.v * barHeight);
    } else {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    io.AddMouseButtonEvent(
        0, input.adjustMode && input.hovering && input.triggerHeld);

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    using GakumasVrI18n::ts;

    constexpr ImGuiWindowFlags kStripFlags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (input.adjustMode) {
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, barHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0F, 12.0F));
        ImGui::Begin("##vr_panel_bar", nullptr, kStripFlags);
        // Big glyphs: the buttons are the whole bar, so the label should
        // fill the cell instead of floating in it.
        // Keep all four translated buttons inside the fixed-width OpenXR strip.
        // Measure both pin states so toggling it does not change the font size.
        for (float scale = 2.1F; ; scale -= 0.05F) {
            ImGui::SetWindowFontScale(scale);
            float required = std::max(300.0F, std::max(
                ImGui::CalcTextSize(ts("vr_panel_pin_on")).x,
                ImGui::CalcTextSize(ts("vr_panel_pin_off")).x) + 40.0F);
            for (const char* key : {"vr_panel_height", "vr_panel_distance", "vr_panel_size"})
                required += std::max(190.0F, ImGui::CalcTextSize(ts(key)).x + 40.0F);
            required += ImGui::GetStyle().ItemSpacing.x * 3;
            if (required <= ImGui::GetContentRegionAvail().x || scale <= 1.0F) break;
        }
        const float buttonHeight = barHeight - 24.0F;
        const auto barButton = [&](const char* text, bool active, float minWidth) {
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                active ? ImVec4(0.22F, 0.45F, 0.85F, 1.0F)
                       : ImVec4(0.18F, 0.20F, 0.26F, 1.0F));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered,
                active ? ImVec4(0.28F, 0.53F, 0.94F, 1.0F)
                       : ImVec4(0.25F, 0.29F, 0.38F, 1.0F));
            const bool clicked = ImGui::Button(
                text,
                ImVec2(
                    std::max(minWidth, ImGui::CalcTextSize(text).x + 40.0F),
                    buttonHeight));
            ImGui::PopStyleColor(2);
            return clicked;
        };
        if (barButton(
                ts(input.pinned ? "vr_panel_pin_on" : "vr_panel_pin_off"),
                input.pinned,
                300.0F)) {
            output.pinClicked = true;
        }
        ImGui::SameLine();
        if (barButton(ts("vr_panel_height"), input.activeItem == 1, 190.0F)) {
            output.clickedItem = 1;
        }
        ImGui::SameLine();
        if (barButton(ts("vr_panel_distance"), input.activeItem == 2, 190.0F)) {
            output.clickedItem = 2;
        }
        ImGui::SameLine();
        if (barButton(ts("vr_panel_size"), input.activeItem == 3, 190.0F)) {
            output.clickedItem = 3;
        }
        g_panelBarRightForTest = ImGui::GetItemRectMax().x;
        ImGui::SetWindowFontScale(1.0F);
        if (input.hovering) {
            ImDrawList* overlayDraw = ImGui::GetForegroundDrawList();
            const ImVec2 point(
                input.u * io.DisplaySize.x, input.v * barHeight);
            overlayDraw->AddCircleFilled(
                point, 11.0F, IM_COL32(255, 255, 255, 255));
            overlayDraw->AddCircle(
                point, 14.0F, IM_COL32(20, 20, 24, 255), 0, 3.0F);
        }
        ImGui::PopStyleVar();
        ImGui::End();
    }

    const char* hintText = nullptr;
    bool hintIsToast = false;
    if (input.toast == 1) {
        hintText = ts("vr_photo_taken");
        hintIsToast = true;
    } else if (input.toast == 2) {
        hintText = ts("vr_photo_unavailable");
        hintIsToast = true;
    } else if (input.toast == 3) {
        hintText = ts("vr_photo_paused_2d");
        hintIsToast = true;
    } else if (input.adjustMode) {
        switch (input.activeItem) {
        case 1:
            hintText = ts("vr_panel_hint_height");
            break;
        case 2:
            hintText = ts("vr_panel_hint_distance");
            break;
        case 3:
            hintText = ts("vr_panel_hint_size");
            break;
        default:
            hintText = ts("vr_panel_adjust_hint");
            break;
        }
    }
    if (hintText != nullptr) {
        // Fit the box to the text: fixed max width forces wrapping, height
        // follows the wrapped text, and the box centres horizontally. The
        // OpenXrContext crops the hint quad to the reported pixel rect.
        const float fontScale = hintIsToast ? 1.5F : 1.05F;
        const ImVec2 padding(30.0F, 22.0F);
        const float maxBoxWidth =
            std::min(920.0F, io.DisplaySize.x - 16.0F);
        const float wrapWidth = (maxBoxWidth - padding.x * 2.0F) / fontScale;
        const ImVec2 textSize =
            ImGui::CalcTextSize(hintText, nullptr, false, wrapWidth);
        const float boxWidth = std::min(
            textSize.x * fontScale + padding.x * 2.0F + 2.0F, maxBoxWidth);
        const float boxHeight = std::min(
            textSize.y * fontScale + padding.y * 2.0F, hintHeight);
        const float boxX = std::floor((io.DisplaySize.x - boxWidth) * 0.5F);
        ImGui::SetNextWindowPos(ImVec2(boxX, hintTop));
        ImGui::SetNextWindowSize(ImVec2(boxWidth, boxHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
        ImGui::Begin("##vr_panel_hint", nullptr, kStripFlags);
        ImGui::SetWindowFontScale(fontScale);
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            hintIsToast
                ? (input.toast == 1 ? ImVec4(0.72F, 0.95F, 0.72F, 1.0F)
                                    : ImVec4(0.98F, 0.72F, 0.62F, 1.0F))
                : ImVec4(0.90F, 0.92F, 0.97F, 1.0F));
        ImGui::PushTextWrapPos(
            ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
        ImGui::TextWrapped("%s", hintText);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0F);
        ImGui::End();
        ImGui::PopStyleVar();
        output.hintWidthPx = static_cast<std::uint32_t>(std::ceil(boxWidth));
        output.hintHeightPx = static_cast<std::uint32_t>(std::ceil(boxHeight));
    }

    ImGui::Render();

    g_recorder->ClearState();
    const float clear[4] = {0.05F, 0.06F, 0.08F, 1.0F};
    g_recorder->OMSetRenderTargets(1, &g_overlayRtv, nullptr);
    g_recorder->ClearRenderTargetView(g_overlayRtv, clear);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0F;
    viewport.MaxDepth = 1.0F;
    g_recorder->RSSetViewports(1, &viewport);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ID3D11RenderTargetView* nullRtv = nullptr;
    g_recorder->OMSetRenderTargets(1, &nullRtv, nullptr);
    g_recorder->CopyResource(destination, g_overlayRt);

    ID3D11CommandList* commandList = nullptr;
    const HRESULT recorded = g_recorder->FinishCommandList(FALSE, &commandList);
    if (FAILED(recorded) || commandList == nullptr) {
        if (commandList != nullptr) {
            commandList->Release();
        }
        log.Write(
            "[VR][panel] overlay FinishCommandList failed hr=" +
            std::to_string(static_cast<long>(recorded)));
        return false;
    }
    context->ExecuteCommandList(commandList, TRUE);
    commandList->Release();
    if (!g_overlayPaintOkLogged) {
        g_overlayPaintOkLogged = true;
        log.Write(
            "[VR][panel] overlay painted " + std::to_string(width) + "x" +
            std::to_string(height) + " path=deferred-command-list format=" +
            std::to_string(static_cast<int>(g_overlayRtFormat)));
    }
    return true;
}

void ShutdownVrAaMenu() noexcept {
    ShutdownImpl();
}

void SetVrMenuHelpDeltaForTest(float dt) { g_helpTestDelta = dt; }
void SetVrMenuHelpCaptureForTest(const char* key) { g_helpCaptureForTest = key; }
void SetVrMenuScrollCaptureForTest(float scroll) { g_scrollCaptureForTest = scroll; }
float VrMenuScrollForTest() { return g_bodyScrollForTest; }
bool VrMenuRowRectForTest(const char* key, float* rect) {
    for (const auto& entry : g_helpRows) {
        if (entry.row.label != GakumasVrI18n::ts(key)) continue;
        const auto& row = entry.row;
        rect[0] = row.origin.x; rect[1] = row.origin.y;
        rect[2] = row.width; rect[3] = row.height;
        rect[4] = row.controlTop;
        return true;
    }
    return false;
}
void ClearVrMenuSavedFlashForTest() { g_savedFlashSeconds = 0.0F; }
void SetVrMenuComboCaptureForTest(const char* key) { g_comboCaptureForTest = key; }
float VrPanelBarRightForTest() { return g_panelBarRightForTest; }
bool VrMenuToggleColumnsFitForTest() {
    for (const auto& entry : g_helpRows)
        if (entry.row.toggle && (!entry.row.compact || entry.row.width > (g_gridWidth - 36) / 2))
            return false;
    return true;
}
bool VrMenuPopupRectForTest(float* rect) {
    ui::ScopedImGuiContext scope(g_context);
    if (g_context->OpenPopupStack.empty() || !g_context->OpenPopupStack.back().Window) return false;
    const auto* window = g_context->OpenPopupStack.back().Window;
    rect[0] = window->Pos.x; rect[1] = window->Pos.y;
    rect[2] = window->Size.x; rect[3] = window->Size.y;
    return true;
}
void SetVrMenuPopupCaptureForTest(const char* id) {
    ui::ScopedImGuiContext scope(g_context);
    ImGui::ClosePopupsOverWindow(nullptr, false);
    g_popupCaptureForTest = id;
}
float VrMenuTextHeightForTest(const char* text, float width, float size) {
    ui::ScopedImGuiContext scope(g_context);
    return g_context->IO.Fonts->Fonts[0]->CalcTextSizeA(size, FLT_MAX, width, text).y;
}
const char* VrMenuHelpTitleForTest() { return g_helpState.shown.c_str(); }
bool VrMenuModalOpenForTest() {
    ui::ScopedImGuiContext scope(g_context);
    return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}
void SetVrAaMenuTabForTest(int tab) noexcept {
    g_tab = std::clamp(tab, 0, 3);
}

std::uint32_t VrAaMenuMissingGlyphCount() noexcept {
    return g_missingGlyphs;
}

} // namespace gakumas::vr
