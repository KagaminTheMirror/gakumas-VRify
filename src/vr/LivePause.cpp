#include "LivePause.hpp"

#include "VrRuntime.hpp"
#include "../GakumasLocalify/Il2cppUtils.hpp"
#include "../deps/UnityResolve/UnityResolve.hpp"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace gakumas::vr {
namespace {

void* g_cachedPresenter = nullptr;
void* g_cachedModel = nullptr;
bool g_resolvedLogged = false;
void* g_pausedStoryAdv = nullptr;
bool g_storyResolvedLogged = false;

void Log(std::string_view message) noexcept {
    WriteVrLog(message);
}

bool LooksAlive(void* instance) noexcept {
    if (instance == nullptr) {
        return false;
    }
    void* klass = nullptr;
    __try {
        klass = *static_cast<void**>(instance);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return klass != nullptr;
}

bool IsUnityManagedObjectAlive(void* instance) noexcept {
    if (!LooksAlive(instance)) {
        return false;
    }
    __try {
        return reinterpret_cast<UnityResolve::UnityType::UnityObject*>(instance)
                   ->m_CachedPtr != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InvokeVoidRaw(void* function, void* instance, void* methodInfo) noexcept {
    using Fn = void (*)(void*, void*);
    if (function == nullptr || instance == nullptr) {
        return false;
    }
    __try {
        reinterpret_cast<Fn>(function)(instance, methodInfo);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* InvokePtrRaw(void* function, void* instance, void* methodInfo) noexcept {
    using Fn = void* (*)(void*, void*);
    if (function == nullptr || instance == nullptr) {
        return nullptr;
    }
    __try {
        return reinterpret_cast<Fn>(function)(instance, methodInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool InvokeBoolRaw(
    void* function, void* instance, void* methodInfo, bool* out) noexcept {
    using Fn = bool (*)(void*, void*);
    if (function == nullptr || instance == nullptr || out == nullptr) {
        return false;
    }
    __try {
        *out = reinterpret_cast<Fn>(function)(instance, methodInfo);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

UnityResolve::Class* LiveClass(const char* name) noexcept {
    return Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Live", name);
}

UnityResolve::Class* LiveDataClass(const char* name) noexcept {
    return Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Live.Data", name);
}

UnityResolve::Class* OutGameClass(const char* name) noexcept {
    return Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.OutGame", name);
}

UnityResolve::Class* CommonClass(const char* name) noexcept {
    return Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Common", name);
}

void* AliveUnityObject(void* instance) noexcept {
    return IsUnityManagedObjectAlive(instance) ? instance : nullptr;
}

void ForgetDeadLiveCache() noexcept {
    if (!IsUnityManagedObjectAlive(g_cachedPresenter)) {
        g_cachedPresenter = nullptr;
    }
    if (!IsUnityManagedObjectAlive(g_cachedModel)) {
        g_cachedModel = nullptr;
    }
}

void* FindFirstInstance(UnityResolve::Class* klass) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    auto objects = klass->FindObjectsByType<void*>();
    for (auto* object : objects) {
        if (IsUnityManagedObjectAlive(object)) {
            return object;
        }
    }
    return nullptr;
}

UnityResolve::Method* ZeroArg(
    UnityResolve::Class* klass, const char* name) noexcept {
    if (klass == nullptr || name == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method != nullptr && method->name == name &&
            method->args.empty() && !method->static_function &&
            method->function != nullptr) {
            return method;
        }
    }
    return klass->Get<UnityResolve::Method>(name);
}

UnityResolve::Method* OneBoolArg(
    UnityResolve::Class* klass, const char* name) noexcept {
    if (klass == nullptr || name == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method != nullptr && method->name == name &&
            method->args.size() == 1U && method->args[0] != nullptr &&
            method->args[0]->pType != nullptr &&
            method->args[0]->pType->name == "System.Boolean" &&
            !method->static_function && method->address != nullptr) {
            return method;
        }
    }
    return nullptr;
}

void* ReadPointerAt(void* instance, std::int32_t offset) noexcept {
    if (instance == nullptr || offset < 0) {
        return nullptr;
    }
    void* value = nullptr;
    __try {
        value = *reinterpret_cast<void**>(
            reinterpret_cast<std::uintptr_t>(instance) +
            static_cast<std::uintptr_t>(offset));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return value;
}

void* ReadInstanceField(
    UnityResolve::Class* klass, void* instance, const char* name) noexcept {
    if (klass == nullptr || instance == nullptr || name == nullptr ||
        !IsUnityManagedObjectAlive(instance)) {
        return nullptr;
    }
    const auto* field = klass->Get<UnityResolve::Field>(name);
    if (field == nullptr || field->static_field || field->offset < 0) {
        return nullptr;
    }
    void* value = ReadPointerAt(instance, field->offset);
    return LooksAlive(value) ? value : nullptr;
}

bool InvokeVoid(UnityResolve::Method* method, void* instance) noexcept {
    return method != nullptr &&
        InvokeVoidRaw(method->function, instance, method->address);
}

bool RuntimeInvokeRaw(
    UnityResolve::Method* method,
    void* instance,
    void** arguments,
    void** result) noexcept {
    using Invoke = void* (*)(void*, void*, void**, void**);
    static const auto invoke = reinterpret_cast<Invoke>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_runtime_invoke"));
    if (invoke == nullptr || method == nullptr || method->address == nullptr ||
        instance == nullptr || result == nullptr) {
        return false;
    }
    void* exception = nullptr;
    __try {
        *result = invoke(method->address, instance, arguments, &exception);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return exception == nullptr;
}

bool RuntimeInvokeVoid(
    UnityResolve::Method* method, void* instance, void** arguments) noexcept {
    void* ignored = nullptr;
    return RuntimeInvokeRaw(method, instance, arguments, &ignored);
}

bool RuntimeInvokeBool(
    UnityResolve::Method* method, void* instance, bool* value) noexcept {
    using Unbox = void* (*)(void*);
    static const auto unbox = reinterpret_cast<Unbox>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_object_unbox"));
    if (value == nullptr || unbox == nullptr) {
        return false;
    }
    void* boxed = nullptr;
    if (!RuntimeInvokeRaw(method, instance, nullptr, &boxed) || boxed == nullptr) {
        return false;
    }
    __try {
        void* raw = unbox(boxed);
        if (raw == nullptr) {
            return false;
        }
        *value = *static_cast<bool*>(raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* InvokePtr(UnityResolve::Method* method, void* instance) noexcept {
    if (method == nullptr) {
        return nullptr;
    }
    return InvokePtrRaw(method->function, instance, method->address);
}

bool TryReadIsPause(
    void* player, UnityResolve::Class* playerClass, bool* out) noexcept {
    if (player == nullptr || playerClass == nullptr || out == nullptr) {
        return false;
    }
    void* execution = InvokePtr(ZeroArg(playerClass, "get_ExecutionData"), player);
    if (execution == nullptr) {
        return false;
    }
    auto* executionClass = LiveDataClass("LiveExecutionData");
    auto* getter = ZeroArg(executionClass, "get_IsPause");
    return InvokeBoolRaw(
        getter != nullptr ? getter->function : nullptr,
        execution,
        getter != nullptr ? getter->address : nullptr,
        out);
}

bool ReadIsPause(void* player, UnityResolve::Class* playerClass) noexcept {
    bool isPause = false;
    return TryReadIsPause(player, playerClass, &isPause) && isPause;
}

void LogResolveOnce(
    UnityResolve::Class* sceneClass,
    UnityResolve::Class* liveClass,
    UnityResolve::Class* playerClass,
    UnityResolve::Class* threeDClass,
    void* scene,
    void* live,
    void* player,
    void* threeD) noexcept {
    if (g_resolvedLogged) {
        return;
    }
    g_resolvedLogged = true;
    Log(std::string("[VR][live] API_RESOLVE scene=") +
        (scene != nullptr ? "1" : "0") + " live=" + (live != nullptr ? "1" : "0") +
        " player=" + (player != nullptr ? "1" : "0") +
        " threeD=" + (threeD != nullptr ? "1" : "0") +
        " toggle=" +
        (ZeroArg(liveClass, "TogglePause") != nullptr ? "1" : "0") +
        " liveField=" +
        (sceneClass != nullptr && sceneClass->Get<UnityResolve::Field>("_live") !=
                nullptr
            ? "1"
            : "0") +
        " playerField=" +
        (sceneClass != nullptr &&
                sceneClass->Get<UnityResolve::Field>("_livePlayer") != nullptr
            ? "1"
            : "0") +
        " playerPause=" +
        (ZeroArg(playerClass, "Pause") != nullptr ? "1" : "0") +
        " threeDPause=" +
        (ZeroArg(threeDClass, "Pause") != nullptr ? "1" : "0"));
}

bool TryToggleLivePresenter(
    UnityResolve::Class* liveClass,
    void* live,
    void* player,
    UnityResolve::Class* playerClass) noexcept {
    if (liveClass == nullptr || live == nullptr ||
        !IsUnityManagedObjectAlive(live)) {
        return false;
    }
    auto* toggle = ZeroArg(liveClass, "TogglePause");
    if (toggle != nullptr && InvokeVoid(toggle, live)) {
        bool isPause = false;
        const bool haveState = TryReadIsPause(player, playerClass, &isPause);
        Log(std::string("[VR][live] PAUSE_TOGGLE class=LivePresenter method=TogglePause") +
            (haveState ? (std::string(" isPause=") + (isPause ? "1" : "0")) : ""));
        return true;
    }

    bool isPause = false;
    const bool haveState = TryReadIsPause(player, playerClass, &isPause);
    auto* named = ZeroArg(liveClass, haveState && isPause ? "UnPause" : "Pause");
    if (named != nullptr && InvokeVoid(named, live)) {
        Log(std::string("[VR][live] PAUSE_TOGGLE class=LivePresenter method=") +
            named->name);
        return true;
    }
    return false;
}

bool TryPauseResume(
    UnityResolve::Class* klass,
    void* instance,
    const char* tag,
    bool currentlyPaused) noexcept {
    if (klass == nullptr || instance == nullptr ||
        !IsUnityManagedObjectAlive(instance)) {
        return false;
    }
    auto* method = ZeroArg(klass, currentlyPaused ? "Resume" : "Pause");
    if (method == nullptr || !InvokeVoid(method, instance)) {
        return false;
    }
    Log(std::string("[VR][live] PAUSE_TOGGLE class=") + tag + " method=" +
        method->name);
    return true;
}

bool TryToggleStoryPause() noexcept {
    static UnityResolve::Class* storyClass =
        OutGameClass("StoryPlayerScreenPresenter");
    static UnityResolve::Class* advClass = CommonClass("AdvPresenterBase");
    static UnityResolve::Method* isPlaying =
        ZeroArg(advClass, "get_IsPlaying");
    static UnityResolve::Method* pause = OneBoolArg(advClass, "Pause");
    static UnityResolve::Method* play = ZeroArg(advClass, "Play");

    if (g_pausedStoryAdv != nullptr &&
        !IsUnityManagedObjectAlive(g_pausedStoryAdv)) {
        g_pausedStoryAdv = nullptr;
    }

    void* story = FindFirstInstance(storyClass);
    if (!IsUnityManagedObjectAlive(story)) {
        story = nullptr;
    }
    void* adv = ReadInstanceField(storyClass, story, "_advPresenter");
    if (story == nullptr || !IsUnityManagedObjectAlive(adv)) {
        return false;
    }

    if (!g_storyResolvedLogged) {
        g_storyResolvedLogged = true;
        Log(std::string("[VR][story] API_RESOLVE presenter=1 adv=1 field=") +
            (storyClass != nullptr &&
                    storyClass->Get<UnityResolve::Field>("_advPresenter") != nullptr
                ? "1"
                : "0") +
            " isPlaying=" + (isPlaying != nullptr ? "1" : "0") +
            " pauseBool=" + (pause != nullptr ? "1" : "0") +
            " play=" + (play != nullptr ? "1" : "0"));
    }

    if (adv == g_pausedStoryAdv) {
        if (play != nullptr && RuntimeInvokeVoid(play, adv, nullptr)) {
            g_pausedStoryAdv = nullptr;
            Log("[VR][story] PAUSE_TOGGLE class=AdvPresenterBase method=Play paused=0");
            return true;
        }
        Log("[VR][story] pause ignored reason=resume-call-failed");
        return true;
    }

    bool playing = false;
    if (isPlaying == nullptr || !RuntimeInvokeBool(isPlaying, adv, &playing)) {
        Log("[VR][story] pause ignored reason=is-playing-unavailable");
        return true;
    }
    if (!playing) {
        Log("[VR][story] pause ignored reason=not-playing");
        return true;
    }

    bool pauseBgm = true;
    void* arguments[] = {&pauseBgm};
    if (pause != nullptr && RuntimeInvokeVoid(pause, adv, arguments)) {
        g_pausedStoryAdv = adv;
        Log("[VR][story] PAUSE_TOGGLE class=AdvPresenterBase method=Pause pauseBgm=1 paused=1");
        return true;
    }
    Log("[VR][story] pause ignored reason=pause-call-failed");
    return true;
}

} // namespace

void NoteLiveScenePresenter(void* instance) noexcept {
    if (IsUnityManagedObjectAlive(instance)) {
        g_cachedPresenter = instance;
    }
}

void NoteLiveSceneModel(void* instance) noexcept {
    if (IsUnityManagedObjectAlive(instance)) {
        g_cachedModel = instance;
    }
}

void ForgetLiveScenePresenter(void* instance) noexcept {
    if (instance == nullptr || instance == g_cachedPresenter) {
        g_cachedPresenter = nullptr;
    }
}

void* AliveLiveScenePresenter() noexcept {
    ForgetDeadLiveCache();
    return g_cachedPresenter;
}

bool IsOfficialLivePaused() noexcept {
    void* scene = AliveLiveScenePresenter();
    if (scene == nullptr) {
        return false;
    }
    static UnityResolve::Class* sceneClass = LiveClass("LiveScenePresenter");
    static UnityResolve::Class* playerClass = LiveClass("LivePlayerPresenter");
    void* player = AliveUnityObject(
        ReadInstanceField(sceneClass, scene, "_livePlayer"));
    return ReadIsPause(player, playerClass);
}

void ToggleLivePause() noexcept {
    static UnityResolve::Class* sceneClass = LiveClass("LiveScenePresenter");
    static UnityResolve::Class* liveClass = LiveClass("LivePresenter");
    static UnityResolve::Class* playerClass = LiveClass("LivePlayerPresenter");
    static UnityResolve::Class* threeDClass = LiveClass("Live3DPresenter");

    ForgetDeadLiveCache();

    void* foundScene = FindFirstInstance(sceneClass);
    if (foundScene != nullptr) {
        g_cachedPresenter = foundScene;
    }
    void* scene = foundScene != nullptr ? foundScene : g_cachedPresenter;

    void* live = AliveUnityObject(ReadInstanceField(sceneClass, scene, "_live"));
    if (live == nullptr) {
        live = FindFirstInstance(liveClass);
    }
    void* player =
        AliveUnityObject(ReadInstanceField(sceneClass, scene, "_livePlayer"));
    if (player == nullptr) {
        player = FindFirstInstance(playerClass);
    }
    void* threeD = FindFirstInstance(threeDClass);
    if (threeD == nullptr && player != nullptr) {
        threeD = AliveUnityObject(
            InvokePtr(ZeroArg(playerClass, "get_ThreeDLive"), player));
    }

    // A leftover LiveScenePresenter after Live exit is not an active Live.
    // Only a living LivePresenter / player / 3D presenter may consume B.
    const bool haveLiveTarget =
        live != nullptr || player != nullptr || threeD != nullptr;
    if (haveLiveTarget) {
        LogResolveOnce(
            sceneClass, liveClass, playerClass, threeDClass, scene, live, player,
            threeD);

        if (TryToggleLivePresenter(liveClass, live, player, playerClass)) {
            return;
        }

        const bool paused = ReadIsPause(player, playerClass);
        if (TryPauseResume(playerClass, player, "LivePlayerPresenter", paused) ||
            TryPauseResume(threeDClass, threeD, "Live3DPresenter", paused)) {
            return;
        }

        Log("[VR][live] pause ignored reason=no-official-api");
        g_cachedPresenter = nullptr;
        g_cachedModel = nullptr;
        g_resolvedLogged = false;
    } else {
        if (scene != nullptr) {
            Log("[VR][live] leftover ignored reason=no-live-target");
        }
        g_cachedPresenter = nullptr;
        g_cachedModel = nullptr;
        g_resolvedLogged = false;
    }

    if (TryToggleStoryPause()) {
        return;
    }

    Log("[VR][pause] ignored reason=not-in-live-or-story");
}

} // namespace gakumas::vr
