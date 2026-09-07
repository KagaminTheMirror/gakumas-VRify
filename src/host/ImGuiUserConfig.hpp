#pragma once

// Shared by every ImGui translation unit and both UI clients. The desktop
// window and OpenXR worker must never change each other's current context.
struct ImGuiContext;
inline thread_local ImGuiContext* g_localifyImGuiContext = nullptr;
#define GImGui g_localifyImGuiContext
