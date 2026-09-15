#include "UnityAnalogScroll.hpp"
#include "ScrollAxisRouter.hpp"
#include "ThumbstickScroll.hpp"
#include "../frame/SingleFrameLoopContracts.hpp"

#include "../../GakumasLocalify/Il2cppUtils.hpp"
#include "../config/VrifyConfig.hpp"
#include "../../deps/UnityResolve/UnityResolve.hpp"
#include "../../hooks/HookManager.hpp"
#include "../VrRuntime.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>

namespace gakumas::vr::input {
namespace {

struct Il2CppVec2 {
    float x;
    float y;
};

using InputScrollDeltaFn = Il2CppVec2 (*)(void* methodInfo);
using OnScrollFn = void (*)(void* self, void* eventData, void* methodInfo);

InputScrollDeltaFn g_inputScrollDeltaOrig = nullptr;
OnScrollFn g_campusScrollRectOnScrollOrig = nullptr;

std::atomic<std::uint64_t> g_pendingAnalogDelta{0};
std::atomic<bool> g_inputHookReady{false};
std::atomic<bool> g_campusHookReady{false};

std::int32_t g_pointerScrollDeltaOffset = -1;
std::int32_t g_scrollHorizontalOffset = -1;
std::int32_t g_scrollVerticalOffset = -1;
std::int32_t g_scrollSensitivityOffset = -1;
bool g_inputInstallAttempted = false;

constexpr float kMaximumQueuedAnalogDelta = frame::kScrollMaxUnityUnitsPerTicket;

thread_local ScrollVector g_threadInjectedDelta{};

std::uint64_t PackScrollVector(ScrollVector value) noexcept {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    static_assert(sizeof(x) == sizeof(value.x));
    std::memcpy(&x, &value.x, sizeof(x));
    std::memcpy(&y, &value.y, sizeof(y));
    return static_cast<std::uint64_t>(x) |
        (static_cast<std::uint64_t>(y) << 32U);
}

ScrollVector UnpackScrollVector(std::uint64_t packed) noexcept {
    const std::uint32_t x = static_cast<std::uint32_t>(packed);
    const std::uint32_t y = static_cast<std::uint32_t>(packed >> 32U);
    ScrollVector value{};
    std::memcpy(&value.x, &x, sizeof(x));
    std::memcpy(&value.y, &y, sizeof(y));
    return value;
}



void Log(std::string_view message) noexcept {
    static_cast<void>(WriteVrLog(message));
}

ScrollVector AtomicAddClamped(
    std::atomic<std::uint64_t>& destination,
    ScrollVector delta,
    float limit) noexcept {
    std::uint64_t current = destination.load(std::memory_order_relaxed);
    for (;;) {
        const ScrollVector unpacked = UnpackScrollVector(current);
        const ScrollVector next{
            std::clamp(unpacked.x + delta.x, -limit, limit),
            std::clamp(unpacked.y + delta.y, -limit, limit),
        };
        const std::uint64_t packedNext = PackScrollVector(next);
        if (destination.compare_exchange_weak(
                current,
                packedNext,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            return next;
        }
    }
}

bool ReadVec2(void* instance, std::int32_t offset, Il2CppVec2* out) noexcept {
    if (instance == nullptr || offset < 0 || out == nullptr) {
        return false;
    }
    __try {
        std::memcpy(
            out,
            static_cast<const std::byte*>(instance) + offset,
            sizeof(*out));
        return std::isfinite(out->x) && std::isfinite(out->y);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteVec2(void* instance, std::int32_t offset, Il2CppVec2 value) noexcept {
    if (instance == nullptr || offset < 0 || !std::isfinite(value.x) ||
        !std::isfinite(value.y)) {
        return false;
    }
    __try {
        std::memcpy(
            static_cast<std::byte*>(instance) + offset,
            &value,
            sizeof(value));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadBool(void* instance, std::int32_t offset, bool* out) noexcept {
    if (instance == nullptr || offset < 0 || out == nullptr) {
        return false;
    }
    __try {
        std::uint8_t value = 0;
        std::memcpy(
            &value,
            static_cast<const std::byte*>(instance) + offset,
            sizeof(value));
        *out = value != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadFloat(void* instance, std::int32_t offset, float* out) noexcept {
    if (instance == nullptr || offset < 0 || out == nullptr) {
        return false;
    }
    __try {
        std::memcpy(out, static_cast<const std::byte*>(instance) + offset, sizeof(*out));
        return std::isfinite(*out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadRuntimeClassIdentity(
    void* instance,
    const char** namespaze,
    const char** name) noexcept {
    if (instance == nullptr || namespaze == nullptr || name == nullptr) {
        return false;
    }
    __try {
        const auto* klass = Il2cppUtils::get_class_from_instance(instance);
        if (klass == nullptr || klass->name == nullptr) {
            return false;
        }
        *namespaze = klass->namespaze;
        *name = klass->name;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}



UnityResolve::Method* FindExactMethod(
    const char* assemblyName,
    const char* namespaze,
    const char* className,
    const char* methodName,
    bool requireStatic,
    std::initializer_list<std::string_view> argumentTypes) noexcept {
    auto* klass = Il2cppUtils::GetClass(assemblyName, namespaze, className);
    if (klass == nullptr) {
        return nullptr;
    }
    UnityResolve::Method* match = nullptr;
    for (auto* method : klass->methods) {
        if (method == nullptr || method->name != methodName ||
            method->static_function != requireStatic || method->function == nullptr ||
            method->address == nullptr || method->args.size() != argumentTypes.size()) {
            continue;
        }
        bool argumentMatch = true;
        std::size_t index = 0;
        for (const std::string_view expected : argumentTypes) {
            const auto* argument = method->args[index++];
            if (argument == nullptr || argument->pType == nullptr ||
                argument->pType->name != expected) {
                argumentMatch = false;
                break;
            }
        }
        if (!argumentMatch) {
            continue;
        }
        if (match != nullptr) {
            return nullptr;
        }
        match = method;
    }
    return match;
}

std::int32_t FindInstanceFieldOffset(
    UnityResolve::Class* klass,
    const char* fieldName) noexcept {
    if (klass == nullptr || fieldName == nullptr) {
        return -1;
    }
    auto* field = klass->Get<UnityResolve::Field>(fieldName);
    if (field == nullptr || field->static_field || field->offset < 0) {
        return -1;
    }
    return field->offset;
}

bool Install(
    UnityResolve::Method* method,
    void* detour,
    void** original,
    const char* name) noexcept {
    return method != nullptr && method->function != nullptr &&
        GakumasVR::Hooks::CreateAndEnable(
            method->function, detour, original, name);
}



Il2CppVec2 InputScrollDeltaDetour(void* methodInfo) {
    const Il2CppVec2 physicalResult = g_inputScrollDeltaOrig != nullptr
        ? g_inputScrollDeltaOrig(methodInfo)
        : Il2CppVec2{};
    const ScrollVector injectedDelta = UnpackScrollVector(
        g_pendingAnalogDelta.exchange(0, std::memory_order_acq_rel));
    g_threadInjectedDelta = injectedDelta;
    Il2CppVec2 result = physicalResult;
    if (std::isfinite(injectedDelta.x) && std::isfinite(injectedDelta.y)) {
        result.x += injectedDelta.x;
        result.y += injectedDelta.y;
    }

    return result;
}



void CampusScrollRectOnScrollDetour(void* self, void* eventData, void* methodInfo) {
    const ScrollVector injected = g_threadInjectedDelta;
    if (injected.x != 0.0F || injected.y != 0.0F) {
        Il2CppVec2 eventDelta{};
        bool horizontal = false;
        bool vertical = false;
        float sensitivity = kReferenceScrollSensitivity;
        const bool routeReady = ReadVec2(
                eventData, g_pointerScrollDeltaOffset, &eventDelta) &&
            ReadBool(self, g_scrollHorizontalOffset, &horizontal) &&
            ReadBool(self, g_scrollVerticalOffset, &vertical) &&
            ReadFloat(self, g_scrollSensitivityOffset, &sensitivity);
        if (routeReady) {
            const ScrollVector routed = RouteVrScrollDelta(
                {eventDelta.x, eventDelta.y},
                injected,
                horizontal,
                vertical,
                sensitivity);
            static_cast<void>(WriteVec2(
                eventData, g_pointerScrollDeltaOffset, {routed.x, routed.y}));
        }
        // A Unity input poll owns at most one selected scroll handler. Do not
        // let its VR component leak into a later physical event on this thread.
        g_threadInjectedDelta = {};
    }
    if (g_campusScrollRectOnScrollOrig != nullptr) {
        g_campusScrollRectOnScrollOrig(self, eventData, methodInfo);
    }
}









} // namespace

void InstallUnityAnalogScrollHook() noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled || g_inputInstallAttempted) {
        return;
    }
    g_inputInstallAttempted = true;

    auto* pointerEventData = Il2cppUtils::GetClass(
        "UnityEngine.UI.dll", "UnityEngine.EventSystems", "PointerEventData");
    auto* scrollRect = Il2cppUtils::GetClass(
        "UnityEngine.UI.dll", "UnityEngine.UI", "ScrollRect");
    g_pointerScrollDeltaOffset = FindInstanceFieldOffset(
        pointerEventData, "<scrollDelta>k__BackingField");
    g_scrollHorizontalOffset = FindInstanceFieldOffset(
        scrollRect, "m_Horizontal");
    g_scrollVerticalOffset = FindInstanceFieldOffset(
        scrollRect, "m_Vertical");
    g_scrollSensitivityOffset = FindInstanceFieldOffset(
        scrollRect, "m_ScrollSensitivity");

    auto* inputMethod = FindExactMethod(
        "UnityEngine.InputLegacyModule.dll", "UnityEngine", "Input",
        "get_mouseScrollDelta", true, {});
    auto* campusScrollRectMethod = FindExactMethod(
        "Assembly-CSharp.dll", "Campus.Common", "CampusScrollRect", "OnScroll",
        false, {"UnityEngine.EventSystems.PointerEventData"});
    const bool inputReady = Install(
        inputMethod, reinterpret_cast<void*>(&InputScrollDeltaDetour),
        reinterpret_cast<void**>(&g_inputScrollDeltaOrig),
        "UnityEngine.Input.get_mouseScrollDelta");
    const bool campusReady = Install(
        campusScrollRectMethod,
        reinterpret_cast<void*>(&CampusScrollRectOnScrollDetour),
        reinterpret_cast<void**>(&g_campusScrollRectOnScrollOrig),
        "Campus.Common.CampusScrollRect.OnScroll");
    g_inputHookReady.store(inputReady, std::memory_order_release);
    g_campusHookReady.store(campusReady, std::memory_order_release);

    std::ostringstream stream;
    stream << "[VR][input] UNITY_ANALOG_SCROLL_API input=" << inputReady
           << " campusScrollRect=" << campusReady
           << " maxRate=" << std::fixed << std::setprecision(1)
           << ThumbstickScrollIntegrator::kMaximumUnityUnitsPerSecond
           << " maxQueuedDelta=" << std::setprecision(3)
           << kMaximumQueuedAnalogDelta
           << " scrollDeltaOffset=" << g_pointerScrollDeltaOffset
           << " horizontalOffset=" << g_scrollHorizontalOffset
           << " verticalOffset=" << g_scrollVerticalOffset
           << " sensitivityOffset=" << g_scrollSensitivityOffset;
    Log(stream.str());
}

bool UnityAnalogScrollAvailable() noexcept {
    return g_inputHookReady.load(std::memory_order_acquire) &&
        g_campusHookReady.load(std::memory_order_acquire) &&
        g_pointerScrollDeltaOffset >= 0 && g_scrollHorizontalOffset >= 0 &&
        g_scrollVerticalOffset >= 0 && g_scrollSensitivityOffset >= 0;
}

bool QueueUnityAnalogScrollDelta(float x, float y) noexcept {
    if (!UnityAnalogScrollAvailable() || !std::isfinite(x) || !std::isfinite(y) ||
        (x == 0.0F && y == 0.0F)) {
        return false;
    }
    AtomicAddClamped(
        g_pendingAnalogDelta,
        {x, y},
        kMaximumQueuedAnalogDelta);


    return true;
}

void ResetUnityAnalogScroll() noexcept {
    g_pendingAnalogDelta.store(0, std::memory_order_release);
}



} // namespace gakumas::vr::input
