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
    style.FrameRounding = 8.0F;
    style.GrabRounding = 8.0F;
    style.GrabMinSize = 30.0F;
    style.FramePadding = ImVec2(14.0F, 9.0F);
    style.ItemSpacing = ImVec2(12.0F, 10.0F);
    style.ItemInnerSpacing = ImVec2(10.0F, 8.0F);
    style.WindowPadding = ImVec2(26.0F, 22.0F);
    style.ScrollbarSize = 22.0F;
    style.PopupRounding = 10.0F;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09F, 0.10F, 0.13F, 1.0F);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.13F, 0.15F, 0.19F, 1.0F);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.17F, 0.19F, 0.25F, 1.0F);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.23F, 0.27F, 0.35F, 1.0F);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.26F, 0.31F, 0.41F, 1.0F);
    style.Colors[ImGuiCol_Button] = ImVec4(0.20F, 0.23F, 0.30F, 1.0F);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.26F, 0.34F, 0.50F, 1.0F);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.22F, 0.45F, 0.85F, 1.0F);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.45F, 0.62F, 0.95F, 1.0F);
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
    const std::vector<std::string> menuStrings = GakumasVrI18n::ActiveStrings();
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
            "c:\\Windows\\Fonts\\msyh.ttc", 28.0F, &fontConfig);
    } else if (std::filesystem::exists("c:\\Windows\\Fonts\\segoeui.ttf")) {
        font = io.Fonts->AddFontFromFileTTF(
            "c:\\Windows\\Fonts\\segoeui.ttf", 28.0F, &fontConfig);
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
    float width = 0.0F;
};

MenuRow BeginRow(const char* label, bool enabled, bool selected = false) {
    MenuRow row;
    row.origin = ImGui::GetCursorScreenPos();
    row.width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 labelEnd(
        row.origin.x + kLabelWidth, row.origin.y + kRowHeight);
    const ImVec2 controlBegin(row.origin.x + kLabelWidth + kColumnGap, row.origin.y);
    const ImVec2 controlEnd(row.origin.x + row.width, row.origin.y + kRowHeight);
    draw->AddRectFilled(
        row.origin,
        labelEnd,
        selected ? IM_COL32(46, 72, 110, 255) : IM_COL32(41, 46, 58, 255),
        8.0F);
    draw->AddRectFilled(
        controlBegin,
        controlEnd,
        selected ? IM_COL32(36, 52, 78, 255) : IM_COL32(28, 32, 41, 255),
        8.0F);
    if (selected) {
        draw->AddRect(
            row.origin,
            controlEnd,
            IM_COL32(88, 158, 255, 255),
            8.0F,
            0,
            3.0F);
    }
    draw->AddText(
        ImVec2(
            row.origin.x + 20.0F,
            row.origin.y + (kRowHeight - ImGui::GetFontSize()) * 0.5F),
        enabled ? IM_COL32(230, 234, 243, 255) : IM_COL32(126, 132, 146, 255),
        label);
    ImGui::SetCursorScreenPos(ImVec2(
        controlBegin.x + 14.0F,
        row.origin.y + (kRowHeight - kControlHeight) * 0.5F));
    ImGui::PushItemWidth(row.width - kLabelWidth - kColumnGap - 28.0F);
    return row;
}

void EndRow(const MenuRow& row) {
    ImGui::PopItemWidth();
    ImGui::SetCursorScreenPos(
        ImVec2(row.origin.x, row.origin.y + kRowHeight + kRowSpacing));
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
    const MenuRow row = BeginRow(label, enabled);
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    *value = std::clamp(*value, 0, count - 1);
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
    const MenuRow row = BeginRow(label, enabled);
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    const bool active = *value;
    const float actionWidth =
        actionLabel != nullptr ? std::max(110.0F, ImGui::CalcTextSize(actionLabel).x + 36.0F)
                               : 0.0F;
    const float toggleWidth = ImGui::CalcItemWidth() -
        (actionLabel != nullptr ? actionWidth + 12.0F : 0.0F);
    if (ImGui::Button(
            active ? onText : offText,
            ImVec2(std::max(80.0F, toggleWidth), kControlHeight))) {
        *value = !active;
    }
    if (actionLabel != nullptr) {
        ImGui::SameLine(0.0F, 12.0F);
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
    const float actionWidth =
        actionLabel != nullptr ? std::max(110.0F, ImGui::CalcTextSize(actionLabel).x + 36.0F)
                               : 0.0F;
    const float sliderWidth = ImGui::CalcItemWidth() -
        (actionLabel != nullptr ? actionWidth + 12.0F : 0.0F);
    ImGui::PushItemWidth(std::max(80.0F, sliderWidth));
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.22F, 0.32F, 0.48F, 1.0F));
        ImGui::PushStyleColor(
            ImGuiCol_SliderGrab, ImVec4(0.70F, 0.85F, 1.0F, 1.0F));
        ImGui::PushStyleColor(
            ImGuiCol_SliderGrabActive, ImVec4(0.85F, 0.93F, 1.0F, 1.0F));
    }
    const float valueBefore = *value;
    ImGui::SliderFloat("##value", value, minimum, maximum, format);
    const bool sliderClicked = ImGui::IsItemClicked();
    const bool sliderActive = ImGui::IsItemActive();
    if (selected) {
        ImGui::PopStyleColor(3);
    }
    const ImVec2 rowMax(row.origin.x + row.width, row.origin.y + kRowHeight);
    const bool rowClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsMouseHoveringRect(row.origin, rowMax, false);
    if (enabled && (rowClicked || sliderClicked || sliderActive)) {
        g_selectedSlider = id;
        if (rowClicked || sliderClicked) {
            g_sliderRowClicked = true;
        }
    }
    if (selected && enabled) {
        openxr::NudgeAaMenuSlider(
            *value, minimum, maximum, ImGui::GetIO().DeltaTime, sticks);
    }
    ObserveAutoSlider(
        persist,
        sliderActive ||
            (selected && enabled && openxr::AaMenuSliderStickActive(sticks)),
        *value != valueBefore);
    ImGui::PopItemWidth();
    if (actionLabel != nullptr) {
        ImGui::SameLine(0.0F, 12.0F);
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

void HelpLine(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    ImGui::SetWindowFontScale(0.50F);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58F, 0.62F, 0.70F, 1.0F));
    ImGui::PushTextWrapPos(
        ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x);
    ImGui::TextWrapped("%s", text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0F);
    ImGui::Dummy(ImVec2(0.0F, 2.0F));
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
    ImGui::OpenPopup("##vr_localize_restart");
    log.Write(
        std::string("[VR][menu] localizeText=") +
        (GakumasLocal::Config::vrLocalizeText ? "1" : "0") +
        " saved restart-required");
}

bool TabButton(const char* text, bool active) {
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        active ? ImVec4(0.22F, 0.45F, 0.85F, 1.0F)
               : ImVec4(0.18F, 0.20F, 0.26F, 1.0F));
    ImGui::PushStyleColor(
        ImGuiCol_ButtonHovered,
        active ? ImVec4(0.28F, 0.53F, 0.94F, 1.0F)
               : ImVec4(0.25F, 0.29F, 0.38F, 1.0F));
    const bool clicked = ImGui::Button(
        text, ImVec2(std::max(150.0F, ImGui::CalcTextSize(text).x + 56.0F), 54.0F));
    ImGui::PopStyleColor(2);
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

    if (g_tab < 0 || g_tab > 3) {
        g_tab = 0;
    }
    if (TabButton(ts("vr_menu_tab_picture"), g_tab == 0)) {
        g_tab = 0;
    }
    ImGui::SameLine();
    if (TabButton(ts("vr_menu_tab_taa"), g_tab == 1)) {
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
        ImGui::SameLine();
        ImGui::TextUnformatted(ts("vr_aa_saved"));
        g_savedFlashSeconds = std::max(0.0F, g_savedFlashSeconds - io.DeltaTime);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

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
        // .119: spent diagnostics dropped from the menu (eye-as-main,
        // object-MV skip, JumpFlood temporal off) — all three proven
        // no-effect on the source-off TAA jitter in the .116/.117 runs.
        // .185: deferred-stencil skip also left config-only.
        // .268: projected-shadow / toon / shade-band live on the
        // experimental tab (default off).
        // Config flags remain for log-driven experiments.
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
    } else if (g_tab == 3) {
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
    } else {
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
                // High+ switches the URP pass to YCoCg on this title's
                // R11G11B10 history and can break LED cube / sign colors.
                if (Config::vrEyeTaaQuality >= 3 && taaQualityBefore < 3) {
                    g_taaQualityPendingValue = Config::vrEyeTaaQuality;
                    Config::vrEyeTaaQuality = taaQualityBefore;
                    ImGui::OpenPopup("##vr_taa_quality_confirm");
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
            const bool localizeBeforeReset = Config::vrLocalizeText;
            Config::ResetVrEyeAaToBaseline();
            Config::ResetVrPointerSettings();
            Config::vrLocalizeText = false;
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
            if (localizeBeforeReset != Config::vrLocalizeText) {
                PersistLocalizeAndPromptRestart(log);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(ts("cancel"), ImVec2(cancelWidth, 54.0F))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowSize(ImVec2(620.0F, 0.0F), ImGuiCond_Always);
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
        ImGui::SetWindowFontScale(2.1F);
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

void SetVrAaMenuTabForTest(int tab) noexcept {
    g_tab = std::clamp(tab, 0, 3);
}

std::uint32_t VrAaMenuMissingGlyphCount() noexcept {
    return g_missingGlyphs;
}

} // namespace gakumas::vr
