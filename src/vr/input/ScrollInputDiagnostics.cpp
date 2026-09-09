#include "ScrollInputDiagnostics.hpp"
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
using GetFlickFn = std::int32_t (*)(Il2CppVec2 velocity, void* methodInfo);

InputScrollDeltaFn g_inputScrollDeltaOrig = nullptr;
OnScrollFn g_scrollRectOnScrollOrig = nullptr;
OnScrollFn g_campusScrollRectOnScrollOrig = nullptr;
OnScrollFn g_scrollGestureOnScrollOrig = nullptr;
OnScrollFn g_scrollFlickOnScrollOrig = nullptr;
OnScrollFn g_executeScrollHandlerOrig = nullptr;
GetFlickFn g_scrollFlickGetFlickOrig = nullptr;

std::atomic<std::uint64_t> g_sourceSerial{0};
std::atomic<std::uint64_t> g_sourceTotalUnits{0};
std::atomic<std::uint64_t> g_sourceLatestDelta{0};
std::atomic<std::uint64_t> g_sourceTickMs{0};
std::atomic<std::uint64_t> g_pendingAnalogDelta{0};
std::atomic<bool> g_inputHookReady{false};
std::atomic<bool> g_campusHookReady{false};
std::atomic<std::uint64_t> g_unityPollSerial{0};
std::atomic<float> g_unityLatestX{0.0F};
std::atomic<float> g_unityLatestY{0.0F};
std::atomic<std::uint64_t> g_inputLogs{0};
std::atomic<std::uint64_t> g_handlerLogs{0};
std::atomic<std::uint64_t> g_flickLogs{0};

std::int32_t g_pointerScrollDeltaOffset = -1;
std::int32_t g_scrollHorizontalOffset = -1;
std::int32_t g_scrollVerticalOffset = -1;
std::int32_t g_scrollSensitivityOffset = -1;
std::int32_t g_flickTotalDeltaOffset = -1;
bool g_inputInstallAttempted = false;
bool g_diagnosticInstallAttempted = false;

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

bool ShouldLog(std::atomic<std::uint64_t>& counter) noexcept {
    const std::uint64_t sample = counter.fetch_add(1U, std::memory_order_relaxed) + 1U;
    return sample <= 120U || (sample % 30U) == 0U;
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

std::string RuntimeClassName(void* instance) {
    if (instance == nullptr) {
        return "<null>";
    }
    const char* namespaze = nullptr;
    const char* name = nullptr;
    if (!ReadRuntimeClassIdentity(instance, &namespaze, &name)) {
        return "<unreadable>";
    }
    if (namespaze == nullptr || namespaze[0] == '\0') {
        return name;
    }
    return std::string(namespaze) + "." + name;
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

void LogHandler(
    const char* kind,
    void* self,
    void* eventData,
    bool includeSensitivity,
    const Il2CppVec2* totalAfter = nullptr) noexcept {
    Il2CppVec2 eventDelta{};
    const bool eventReady = ReadVec2(
        eventData, g_pointerScrollDeltaOffset, &eventDelta);
    float sensitivity = 0.0F;
    const bool sensitivityReady = includeSensitivity &&
        ReadFloat(self, g_scrollSensitivityOffset, &sensitivity);
    bool horizontal = false;
    bool vertical = false;
    const bool axesReady = includeSensitivity &&
        ReadBool(self, g_scrollHorizontalOffset, &horizontal) &&
        ReadBool(self, g_scrollVerticalOffset, &vertical);
    const std::uint64_t sourceSerial =
        g_sourceSerial.load(std::memory_order_acquire);
    const std::uint64_t unityPoll =
        g_unityPollSerial.load(std::memory_order_acquire);

    if (!ShouldLog(g_handlerLogs)) {
        return;
    }
    std::ostringstream stream;
    stream << "[VR][scroll-probe] HANDLER kind=" << kind
           << " class=" << RuntimeClassName(self)
           << " self=" << self
           << " sourceSerial=" << sourceSerial
           << " unityPoll=" << unityPoll
           << " unity=(" << std::fixed << std::setprecision(4)
           << g_unityLatestX.load(std::memory_order_relaxed) << ','
           << g_unityLatestY.load(std::memory_order_relaxed) << ')';
    if (eventReady) {
        stream << " event=(" << eventDelta.x << ',' << eventDelta.y << ')';
    } else {
        stream << " event=<unreadable>";
    }
    if (includeSensitivity) {
        if (sensitivityReady) {
            stream << " sensitivity=" << sensitivity;
        } else {
            stream << " sensitivity=<unreadable>";
        }
        if (axesReady) {
            stream << " horizontal=" << horizontal
                   << " vertical=" << vertical;
        } else {
            stream << " axes=<unreadable>";
        }
    }
    if (totalAfter != nullptr) {
        stream << " totalAfter=(" << totalAfter->x << ',' << totalAfter->y << ')';
    }
    Log(stream.str());
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

    static thread_local std::uint64_t previousSourceSerial = 0;
    static thread_local ScrollVector previousSourceUnits{};
    const std::uint64_t sourceSerial =
        g_sourceSerial.load(std::memory_order_acquire);
    const ScrollVector sourceUnits = UnpackScrollVector(
        g_sourceTotalUnits.load(std::memory_order_relaxed));
    const std::uint64_t sourceMessages = sourceSerial - previousSourceSerial;
    const ScrollVector unitsSincePoll{
        sourceUnits.x - previousSourceUnits.x,
        sourceUnits.y - previousSourceUnits.y,
    };
    previousSourceSerial = sourceSerial;
    previousSourceUnits = sourceUnits;

    g_unityLatestX.store(result.x, std::memory_order_relaxed);
    g_unityLatestY.store(result.y, std::memory_order_relaxed);
    const std::uint64_t unityPoll =
        g_unityPollSerial.fetch_add(1U, std::memory_order_release) + 1U;

    const bool interesting = sourceMessages != 0U || unitsSincePoll.x != 0.0F ||
        unitsSincePoll.y != 0.0F ||
        result.x != 0.0F || result.y != 0.0F;
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled && interesting &&
        ShouldLog(g_inputLogs)) {
        const std::uint64_t sourceTick =
            g_sourceTickMs.load(std::memory_order_relaxed);
        const std::uint64_t now = GetTickCount64();
        std::ostringstream stream;
        stream << "[VR][scroll-probe] UNITY_INPUT sourceSerial=" << sourceSerial
               << " sourceMessages=" << sourceMessages
               << " sourceUnits=(" << std::fixed << std::setprecision(4)
               << unitsSincePoll.x << ',' << unitsSincePoll.y << ')'
               << " latestDelta=(";
        const ScrollVector latestDelta = UnpackScrollVector(
            g_sourceLatestDelta.load(std::memory_order_relaxed));
        stream << latestDelta.x << ',' << latestDelta.y << ')'
               << " ageMs=" << (sourceTick <= now ? now - sourceTick : 0U)
               << " unityPoll=" << unityPoll
               << " physical=(" << physicalResult.x << ',' << physicalResult.y << ')'
               << " injected=(" << injectedDelta.x << ',' << injectedDelta.y << ')'
               << " delta=(" << result.x << ',' << result.y << ')';
        Log(stream.str());
    }
    return result;
}

void ScrollRectOnScrollDetour(void* self, void* eventData, void* methodInfo) {
    LogHandler("ScrollRect", self, eventData, true);
    if (g_scrollRectOnScrollOrig != nullptr) {
        g_scrollRectOnScrollOrig(self, eventData, methodInfo);
    }
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
    LogHandler("CampusScrollRect", self, eventData, true);
    if (g_campusScrollRectOnScrollOrig != nullptr) {
        g_campusScrollRectOnScrollOrig(self, eventData, methodInfo);
    }
}

void ScrollGestureOnScrollDetour(void* self, void* eventData, void* methodInfo) {
    LogHandler("ScrollGesture", self, eventData, false);
    if (g_scrollGestureOnScrollOrig != nullptr) {
        g_scrollGestureOnScrollOrig(self, eventData, methodInfo);
    }
}

void ScrollFlickOnScrollDetour(void* self, void* eventData, void* methodInfo) {
    if (g_scrollFlickOnScrollOrig != nullptr) {
        g_scrollFlickOnScrollOrig(self, eventData, methodInfo);
    }
    Il2CppVec2 total{};
    const bool totalReady = ReadVec2(self, g_flickTotalDeltaOffset, &total);
    LogHandler(
        "ScrollFlickGesture", self, eventData, false,
        totalReady ? &total : nullptr);
}

void ExecuteScrollHandlerDetour(void* handler, void* eventData, void* methodInfo) {
    // This exact non-generic IL2CPP instantiation is the common dispatch point
    // for every IScrollHandler. It names the real target even when the target
    // has a game-specific OnScroll override that is not in the known set below.
    LogHandler("ExecuteEvents<IScrollHandler>", handler, eventData, false);
    if (g_executeScrollHandlerOrig != nullptr) {
        g_executeScrollHandlerOrig(handler, eventData, methodInfo);
    }
}

std::int32_t ScrollFlickGetFlickDetour(Il2CppVec2 velocity, void* methodInfo) {
    const std::int32_t result = g_scrollFlickGetFlickOrig != nullptr
        ? g_scrollFlickGetFlickOrig(velocity, methodInfo)
        : 0;
    if (ShouldLog(g_flickLogs)) {
        std::ostringstream stream;
        stream << "[VR][scroll-probe] FLICK_CLASSIFY velocity=("
               << std::fixed << std::setprecision(4)
               << velocity.x << ',' << velocity.y << ") result=" << result
               << " sourceSerial="
               << g_sourceSerial.load(std::memory_order_acquire)
               << " unityPoll="
               << g_unityPollSerial.load(std::memory_order_acquire);
        Log(stream.str());
    }
    return result;
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

    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        AtomicAddClamped(
            g_sourceTotalUnits,
            {x, y},
            1'000'000.0F);
        g_sourceLatestDelta.store(
            PackScrollVector({x, y}), std::memory_order_relaxed);
        g_sourceTickMs.store(GetTickCount64(), std::memory_order_relaxed);
        g_sourceSerial.fetch_add(1U, std::memory_order_release);
    }
    return true;
}

void ResetUnityAnalogScroll() noexcept {
    g_pendingAnalogDelta.store(0, std::memory_order_release);
}

void InstallScrollInputDiagnosticHooks() noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled ||
        g_diagnosticInstallAttempted) {
        return;
    }
    g_diagnosticInstallAttempted = true;

    auto* scrollFlick = Il2cppUtils::GetClass(
        "Assembly-CSharp.dll", "Campus.Common", "ScrollFlickGesture");
    g_flickTotalDeltaOffset = FindInstanceFieldOffset(
        scrollFlick, "_totalScrollDelta");

    auto* scrollRectMethod = FindExactMethod(
        "UnityEngine.UI.dll", "UnityEngine.UI", "ScrollRect", "OnScroll",
        false, {"UnityEngine.EventSystems.PointerEventData"});
    auto* scrollGestureMethod = FindExactMethod(
        "Assembly-CSharp.dll", "Campus.Common", "ScrollGesture", "OnScroll",
        false, {"UnityEngine.EventSystems.PointerEventData"});
    auto* scrollFlickMethod = FindExactMethod(
        "Assembly-CSharp.dll", "Campus.Common", "ScrollFlickGesture", "OnScroll",
        false, {"UnityEngine.EventSystems.PointerEventData"});
    auto* getFlickMethod = FindExactMethod(
        "Assembly-CSharp.dll", "Campus.Common", "ScrollFlickGesture", "GetFlick",
        true, {"UnityEngine.Vector2"});
    auto* executeScrollHandlerMethod = FindExactMethod(
        "UnityEngine.UI.dll", "UnityEngine.EventSystems", "ExecuteEvents", "Execute",
        true,
        {"UnityEngine.EventSystems.IScrollHandler",
         "UnityEngine.EventSystems.BaseEventData"});

    const bool inputReady = g_inputHookReady.load(std::memory_order_acquire);
    const bool scrollRectReady = Install(
        scrollRectMethod, reinterpret_cast<void*>(&ScrollRectOnScrollDetour),
        reinterpret_cast<void**>(&g_scrollRectOnScrollOrig),
        "UnityEngine.UI.ScrollRect.OnScroll");
    const bool campusScrollRectReady =
        g_campusHookReady.load(std::memory_order_acquire);
    const bool scrollGestureReady = Install(
        scrollGestureMethod, reinterpret_cast<void*>(&ScrollGestureOnScrollDetour),
        reinterpret_cast<void**>(&g_scrollGestureOnScrollOrig),
        "Campus.Common.ScrollGesture.OnScroll");
    const bool scrollFlickReady = Install(
        scrollFlickMethod, reinterpret_cast<void*>(&ScrollFlickOnScrollDetour),
        reinterpret_cast<void**>(&g_scrollFlickOnScrollOrig),
        "Campus.Common.ScrollFlickGesture.OnScroll");
    const bool getFlickReady = Install(
        getFlickMethod, reinterpret_cast<void*>(&ScrollFlickGetFlickDetour),
        reinterpret_cast<void**>(&g_scrollFlickGetFlickOrig),
        "Campus.Common.ScrollFlickGesture.GetFlick");
    const bool executeScrollHandlerReady = Install(
        executeScrollHandlerMethod,
        reinterpret_cast<void*>(&ExecuteScrollHandlerDetour),
        reinterpret_cast<void**>(&g_executeScrollHandlerOrig),
        "UnityEngine.EventSystems.ExecuteEvents.Execute<IScrollHandler>");

    std::ostringstream stream;
    stream << "[VR][scroll-probe] SCROLL_PROBE_API input=" << inputReady
           << " scrollRect=" << scrollRectReady
           << " campusScrollRect=" << campusScrollRectReady
           << " scrollGesture=" << scrollGestureReady
           << " scrollFlick=" << scrollFlickReady
           << " getFlick=" << getFlickReady
           << " executeScrollHandler=" << executeScrollHandlerReady
           << " scrollDeltaOffset=" << g_pointerScrollDeltaOffset
           << " horizontalOffset=" << g_scrollHorizontalOffset
           << " verticalOffset=" << g_scrollVerticalOffset
           << " sensitivityOffset=" << g_scrollSensitivityOffset
           << " flickTotalOffset=" << g_flickTotalDeltaOffset;
    Log(stream.str());
}

} // namespace gakumas::vr::input
