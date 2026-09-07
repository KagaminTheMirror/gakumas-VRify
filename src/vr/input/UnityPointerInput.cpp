#include "UnityPointerInput.hpp"
#include "NativePointerQueue.hpp"
#include "../../GakumasLocalify/Il2cppUtils.hpp"
#include "../../deps/UnityResolve/UnityResolve.hpp"
#include "../../hooks/HookManager.hpp"
#include "../config/VrifyConfig.hpp"
#include "../VrRuntime.hpp"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <sstream>
#include <string_view>

namespace gakumas::vr::input {
namespace {
struct Vec2 { float x, y; };
using UpdateFn = void (*)(void*, void*);
using FocusFn = void (*)(void*, bool, void*);
using MouseDataFn = void* (*)(void*, int, void*);
using PositionFn = Vec2 (*)(void*, void*);
using BoolFn = bool (*)(void*, void*);
using ButtonFn = bool (*)(void*, int, void*);
using InvokeFn = void* (*)(void*, void*, void**, void**);
using UnboxFn = void* (*)(void*);

UpdateFn g_update = nullptr;
FocusFn g_focus = nullptr;
MouseDataFn g_mouseData = nullptr;
PositionFn g_position = nullptr;
BoolFn g_present = nullptr;
ButtonFn g_down = nullptr, g_up = nullptr, g_held = nullptr;
InvokeFn g_invoke = nullptr;
UnboxFn g_unbox = nullptr;
UnityResolve::Method *g_current = nullptr, *g_width = nullptr, *g_height = nullptr;
int g_focusOffset = -1;
NativePointerQueue g_queue;
std::atomic<bool> g_ready{false};

struct UpdateScope {
    NativePointerSample sample;
    void* eventSystem = nullptr;
    Vec2 position{};
    bool savedFocus = false;
    bool consumed = false;
};
thread_local UpdateScope* g_scope = nullptr;

void Log(const std::string& text) noexcept {
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) WriteVrLog(text);
}
bool IsUnityManagedObjectAlive(void* instance) noexcept {
    if (!instance) return false;
    __try {
        return reinterpret_cast<UnityResolve::UnityType::UnityObject*>(instance)->m_CachedPtr != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FocusValue(void* instance, bool* value, bool write) noexcept {
    if (g_focusOffset < 0 || !IsUnityManagedObjectAlive(instance)) return false;
    __try {
        auto* field = reinterpret_cast<bool*>(static_cast<char*>(instance) + g_focusOffset);
        if (write) *field = *value; else *value = *field;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* Invoke(UnityResolve::Method* method) noexcept {
    if (!method || !g_invoke) return nullptr;
    void* exception = nullptr;
    __try {
        void* result = g_invoke(method->address, nullptr, nullptr, &exception);
        return exception ? nullptr : result;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
int ScreenSize(UnityResolve::Method* method) noexcept {
    void* box = Invoke(method);
    if (!box || !g_unbox) return 0;
    __try {
        void* value = g_unbox(box);
        return value ? *static_cast<int*>(value) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

Vec2 Position(void* self, void* method) {
    return g_scope ? g_scope->position : g_position(self, method);
}
bool Present(void* self, void* method) { return g_scope ? true : g_present(self, method); }
bool Down(void* self, int button, void* method) {
    return g_scope ? button == 0 && g_scope->sample.down : g_down(self, button, method);
}
bool Up(void* self, int button, void* method) {
    return g_scope ? button == 0 && g_scope->sample.up : g_up(self, button, method);
}
bool Held(void* self, int button, void* method) {
    return g_scope ? button == 0 && g_scope->sample.held : g_held(self, button, method);
}
void* MouseData(void* self, int id, void* method) {
    void* result = g_mouseData(self, id, method);
    if (g_scope && result && !g_scope->consumed) {
        g_scope->consumed = true;
        g_queue.Acknowledge(g_scope->sample);
        if (g_scope->sample.down || g_scope->sample.up) {
            std::ostringstream out;
            out << "[VR][input] NATIVE_POINTER_CONSUMED serial=" << g_scope->sample.serial
                << " down=" << g_scope->sample.down << " up=" << g_scope->sample.up
                << " cancelled=" << (g_scope->sample.u < 0)
                << " screen=" << g_scope->position.x << ',' << g_scope->position.y
                << " desktopFocus=" << g_scope->savedFocus;
            Log(out.str());
        }
    }
    return result;
}
void Focus(void* self, bool focused, void* method) {
    // Preserve a genuine focus event arriving reentrantly during a UI callback.
    if (g_scope && g_scope->eventSystem == self) g_scope->savedFocus = focused;
    g_focus(self, focused, method);
    if (g_scope && g_scope->eventSystem == self) {
        bool active = true;
        FocusValue(self, &active, true);
    }
}
void Update(void* self, void* method) {
    if (g_scope) {
        // A UI callback can update a different EventSystem recursively. Its
        // BaseInput reads must not borrow the outer system's pointer.
        struct Suspend {
            UpdateScope* saved = g_scope;
            Suspend() { g_scope = nullptr; }
            ~Suspend() { g_scope = saved; }
        } suspend;
        g_update(self, method);
        return;
    }
    if (!g_ready.load(std::memory_order_acquire) || Invoke(g_current) != self) {
        g_update(self, method);
        return;
    }
    UpdateScope scope;
    scope.sample = g_queue.Begin(GetTickCount64());
    if (!scope.sample.active) { g_update(self, method); return; }
    const int width = ScreenSize(g_width), height = ScreenSize(g_height);
    if (width <= 0 || height <= 0 || !FocusValue(self, &scope.savedFocus, false)) {
        g_queue.Cancel();
        Log("[VR][input] NATIVE_POINTER_SKIP reason=screen-or-eventsystem-unavailable");
        g_update(self, method);
        return;
    }
    scope.eventSystem = self;
    scope.position = {scope.sample.u * (width - 1), (1.0F - scope.sample.v) * (height - 1)};
    // The shipped IL2CPP code INLINES EventSystem.m_HasFocus in UpdateModule
    // and Process (archived disassembly). A getter hook cannot remove that gate.
    // Borrow it only during the current EventSystem's update, restoring the real
    // value before returning. No camera, object identity or OS focus is changed.
    bool active = true;
    if (!FocusValue(self, &active, true)) { g_update(self, method); return; }
    g_scope = &scope;
    if (scope.sample.down || scope.sample.up) {
        Log("[VR][input] NATIVE_POINTER_FOCUS_APPLY realFocus=" + std::to_string(scope.savedFocus));
    }
    struct Restore {
        UpdateScope& scope;
        ~Restore() {
            g_scope = nullptr;
            const bool restored = FocusValue(scope.eventSystem, &scope.savedFocus, true);
            if (!restored || scope.sample.down || scope.sample.up) {
                Log("[VR][input] NATIVE_POINTER_FOCUS_RESTORE ok=" + std::to_string(restored) +
                    " realFocus=" + std::to_string(scope.savedFocus));
            }
        }
    } restore{scope};
    g_update(self, method);
}

UnityResolve::Class* Class(const char* assembly, const char* ns, const char* name) {
    auto* klass = Il2cppUtils::GetClass(assembly, ns, name);
    if (klass && GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        std::ostringstream out;
        out << "[VR][input] NATIVE_POINTER_LIVE_TABLE " << ns << '.' << name;
        for (auto* method : klass->methods) {
            if (!method) continue;
            out << "\n  " << method->name << '(';
            for (auto* arg : method->args) out << (arg && arg->pType ? arg->pType->name : "?") << ',';
            out << ") static=" << method->static_function << " fn=" << method->function
                << " MethodInfo=" << method->address;
        }
        for (auto* field : klass->fields) {
            if (field) out << "\n  field " << field->name << " offset=" << field->offset;
        }
        Log(out.str());
    }
    return klass;
}
UnityResolve::Method* Exact(UnityResolve::Class* klass, const char* name, bool isStatic,
    std::initializer_list<std::string_view> args = {}) {
    UnityResolve::Method* result = nullptr;
    if (!klass) return nullptr;
    for (auto* method : klass->methods) {
        if (!method || method->name != name || method->static_function != isStatic ||
            !method->function || !method->address || method->args.size() != args.size()) continue;
        bool match = true;
        std::size_t i = 0;
        for (auto arg : args) {
            auto* actual = method->args[i++];
            if (!actual || !actual->pType || actual->pType->name != arg) match = false;
        }
        if (match) { if (result) return nullptr; result = method; }
    }
    return result;
}
} // namespace

void InstallUnityPointerInput() noexcept {
    static bool attempted = false;
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled || attempted) return;
    attempted = true;
    auto* base = Class("UnityEngine.UI.dll", "UnityEngine.EventSystems", "BaseInput");
    auto* events = Class("UnityEngine.UI.dll", "UnityEngine.EventSystems", "EventSystem");
    auto* campus = Class("Assembly-CSharp.dll", "Campus.Common", "CampusInputModule");
    auto* screen = Class("UnityEngine.CoreModule.dll", "UnityEngine", "Screen");
    auto* focusField = events ? events->Get<UnityResolve::Field>("m_HasFocus") : nullptr;
    if (focusField && !focusField->static_field && focusField->type &&
        focusField->type->name == "System.Boolean") g_focusOffset = focusField->offset;
    g_current = Exact(events, "get_current", true);
    g_width = Exact(screen, "get_width", true);
    g_height = Exact(screen, "get_height", true);
    const auto assembly = GetModuleHandleW(L"GameAssembly.dll");
    g_invoke = reinterpret_cast<InvokeFn>(GetProcAddress(assembly, "il2cpp_runtime_invoke"));
    g_unbox = reinterpret_cast<UnboxFn>(GetProcAddress(assembly, "il2cpp_object_unbox"));
    struct Hook { UnityResolve::Method* method; void* detour; void** original; };
    Hook hooks[] = {
        {Exact(base, "get_mousePosition", false), reinterpret_cast<void*>(&Position), reinterpret_cast<void**>(&g_position)},
        {Exact(base, "get_mousePresent", false), reinterpret_cast<void*>(&Present), reinterpret_cast<void**>(&g_present)},
        {Exact(base, "GetMouseButtonDown", false, {"System.Int32"}), reinterpret_cast<void*>(&Down), reinterpret_cast<void**>(&g_down)},
        {Exact(base, "GetMouseButtonUp", false, {"System.Int32"}), reinterpret_cast<void*>(&Up), reinterpret_cast<void**>(&g_up)},
        {Exact(base, "GetMouseButton", false, {"System.Int32"}), reinterpret_cast<void*>(&Held), reinterpret_cast<void**>(&g_held)},
        {Exact(campus, "GetMousePointerEventData", false, {"System.Int32"}), reinterpret_cast<void*>(&MouseData), reinterpret_cast<void**>(&g_mouseData)},
        {Exact(events, "OnApplicationFocus", false, {"System.Boolean"}), reinterpret_cast<void*>(&Focus), reinterpret_cast<void**>(&g_focus)},
        {Exact(events, "Update", false), reinterpret_cast<void*>(&Update), reinterpret_cast<void**>(&g_update)},
    };
    bool ready = g_focusOffset >= 0 && g_current && g_width && g_height && g_invoke && g_unbox;
    for (const auto& hook : hooks) ready = ready && hook.method;
    if (ready) for (const auto& hook : hooks) {
        if (!GakumasVR::Hooks::CreateAndEnable(hook.method->function, hook.detour,
            hook.original, hook.method->name.c_str())) { ready = false; break; }
    }
    // Partial installation is inert: every detour forwards without an UpdateScope.
    g_ready.store(ready, std::memory_order_release);
    Log("[VR][input] NATIVE_POINTER_READY ready=" + std::to_string(ready) +
        " focusOffset=" + std::to_string(g_focusOffset));
}
bool UnityPointerInputAvailable() noexcept { return g_ready.load(std::memory_order_acquire); }
bool QueueUnityPointer(float u, float v, bool held, bool down, bool up) noexcept {
    return UnityPointerInputAvailable() && std::isfinite(u) && std::isfinite(v) &&
        g_queue.Publish(u, v, held, down, up, GetTickCount64());
}
void RenewUnityPointer() noexcept { g_queue.Renew(GetTickCount64()); }
void CancelUnityPointer() noexcept { g_queue.Cancel(); }
} // namespace gakumas::vr::input
