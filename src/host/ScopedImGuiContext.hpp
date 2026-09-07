#pragma once

#include "imgui/imgui.h"

namespace gakumas::ui {
class ScopedImGuiContext {
public:
    ScopedImGuiContext() noexcept : previous_(ImGui::GetCurrentContext()) {}
    explicit ScopedImGuiContext(ImGuiContext* context) noexcept
        : ScopedImGuiContext() {
        ImGui::SetCurrentContext(context);
    }
    ~ScopedImGuiContext() { ImGui::SetCurrentContext(previous_); }
    void Forget(ImGuiContext* dying) noexcept {
        if (previous_ == dying) previous_ = nullptr;
    }
    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;
private:
    ImGuiContext* previous_;
};
}
