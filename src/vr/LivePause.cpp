#include "LivePause.hpp"

#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "../hooks/HookManager.hpp"
#include "../GakumasLocalify/Il2cppUtils.hpp"
#include "../deps/UnityResolve/UnityResolve.hpp"

#include <Windows.h>

#include <cstdint>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <vector>
#include <string>
#include <string_view>

namespace gakumas::vr {
namespace {

void* g_cachedPresenter = nullptr;
void* g_cachedModel = nullptr;
bool g_resolvedLogged = false;
void* g_pausedStoryAdv = nullptr;
bool g_storyResolvedLogged = false;
// Separate from the pause cache: a B press may clear that cache during loading.
void* g_photoProtectionPresenter = nullptr;
int g_photoLiveFromType = -1;
std::atomic<bool> g_liveSourcePhotoProtection{false};
struct AutoPhotoOwner { void* presenter; unsigned depth; };
std::vector<AutoPhotoOwner> g_autoPhotoOwners;
bool g_autoPhotoHookReady = false;
using AutoPhotoMoveNextFn = void (*)(void*, void*);
AutoPhotoMoveNextFn g_autoPhotoMoveNextOrig = nullptr;
int g_autoPhotoStateOffset = -1;
int g_autoPhotoOwnerOffset = -1;

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

// Exact, live-table-checked getters; never use the permissive ZeroArg fallback.
UnityResolve::Method* PhotoGetter(
    UnityResolve::Class* klass, const char* name, const char* resultType) noexcept {
    if (klass == nullptr) return nullptr;
    for (auto* method : klass->methods) {
        if (method != nullptr && method->name == name && method->args.empty() &&
            !method->static_function && method->address != nullptr &&
            method->function != nullptr && method->return_type != nullptr &&
            method->return_type->name == resultType) return method;
    }
    return nullptr;
}

bool UnboxLiveFromType(void* boxed, int* value) noexcept {
    using Unbox = void* (*)(void*);
    static const auto unbox = reinterpret_cast<Unbox>(GetProcAddress(
        GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_object_unbox"));
    if (boxed == nullptr || unbox == nullptr) return false;
    __try {
        void* raw = unbox(boxed);
        if (raw == nullptr) return false;
        *value = *static_cast<int*>(raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool TryReadPhotoLiveFromType(void* scene, int* fromType) noexcept {
    // dump + current metadata: evidence/produce-live-photo/. Only the scene
    // presenter is a Unity object. LiveModel/LiveFixedData are managed data;
    // no m_CachedPtr test on either, and neither is cached across scene exits.
    static auto* sceneClass = LiveClass("LiveScenePresenter");
    static auto* modelClass = LiveClass("LiveModel");
    static auto* fixedClass = LiveDataClass("LiveFixedData");
    static auto* modelField = sceneClass != nullptr
        ? sceneClass->Get<UnityResolve::Field>("_liveModel") : nullptr;
    static auto* fixedGetter = PhotoGetter(
        modelClass, "get_FixedData", "Campus.Live.Data.LiveFixedData");
    static auto* fromGetter = PhotoGetter(
        fixedClass, "get_LiveFromType", "Campus.Live.LiveFromType");
    static const bool fieldReady = modelField != nullptr &&
        !modelField->static_field && modelField->offset >= 0 &&
        modelField->type != nullptr &&
        modelField->type->name == "Campus.Live.LiveModel";
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::ostringstream line;
        line << "[VR][photo] LIVE_SOURCE_PHOTO_API modelOffset="
             << (fieldReady ? modelField->offset : -1)
             << " fixedMethod=" << (fixedGetter ? fixedGetter->address : nullptr)
             << " fixedFunction=" << (fixedGetter ? fixedGetter->function : nullptr)
             << " fromMethod=" << (fromGetter ? fromGetter->address : nullptr)
             << " fromFunction=" << (fromGetter ? fromGetter->function : nullptr);
        Log(line.str());
    }
    if (!fieldReady || !IsUnityManagedObjectAlive(scene)) return false;
    void* model = ReadPointerAt(scene, modelField->offset);
    void* fixedData = nullptr;
    void* boxed = nullptr;
    return LooksAlive(model) &&
        RuntimeInvokeRaw(fixedGetter, model, nullptr, &fixedData) &&
        LooksAlive(fixedData) &&
        RuntimeInvokeRaw(fromGetter, fixedData, nullptr, &boxed) &&
        UnboxLiveFromType(boxed, fromType);
}

void BeginAutoPhoto(void* presenter, bool firstEntry) noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled ||
        !IsUnityManagedObjectAlive(presenter)) return;
    const auto found = std::find_if(g_autoPhotoOwners.begin(), g_autoPhotoOwners.end(),
        [presenter](const auto& entry) { return entry.presenter == presenter; });
    if (found != g_autoPhotoOwners.end()) {
        if (firstEntry) ++found->depth;
        return;
    }
    int fromType = -1;
    if (TryReadPhotoLiveFromType(presenter, &fromType) && fromType > 0 && fromType != 1) {
        if (firstEntry) Log("[VR][photo] AUTO_PHOTO_SKIP reason=non-produce");
        return;
    }
    // Unknown origin is conservative only inside the actual capture task.
    // Owner identity survives the state machine's stack -> runner migration.
    g_autoPhotoOwners.push_back({presenter, 1});
    g_liveSourcePhotoProtection.store(true, std::memory_order_release);
    Log(std::string("[VR][photo] AUTO_PHOTO_BEGIN fromType=") +
        std::to_string(fromType) + " source=full camera=authored hands=source-hidden hmd=unchanged");
}

void EndAutoPhoto(void* presenter) noexcept {
    const auto found = std::find_if(g_autoPhotoOwners.begin(), g_autoPhotoOwners.end(),
        [presenter](const auto& entry) { return entry.presenter == presenter; });
    if (found == g_autoPhotoOwners.end()) return;
    if (--found->depth != 0) return;
    g_autoPhotoOwners.erase(found);
    g_liveSourcePhotoProtection.store(!g_autoPhotoOwners.empty(), std::memory_order_release);
    Log("[VR][photo] AUTO_PHOTO_END reason=task-terminal restore=user-settings");
}

bool ReadAutoPhotoState(void* self, int* state, void** presenter) noexcept {
    if (self == nullptr || g_autoPhotoStateOffset < 0 || g_autoPhotoOwnerOffset < 0) return false;
    __try {
        *state = *reinterpret_cast<int*>(static_cast<char*>(self) + g_autoPhotoStateOffset);
        *presenter = *reinterpret_cast<void**>(static_cast<char*>(self) + g_autoPhotoOwnerOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void AutoPhotoMoveNextDetour(void* self, void* methodInfo) {
    int before = -2;
    void* presenter = nullptr;
    const bool readable = ReadAutoPhotoState(self, &before, &presenter);
    if (readable && before >= -1) BeginAutoPhoto(presenter, before == -1);
    if (g_autoPhotoMoveNextOrig != nullptr) g_autoPhotoMoveNextOrig(self, methodInfo);
    int after = 0;
    void* ignored = nullptr;
    // -1 also means running, NOT finished. Only -2 is terminal (success,
    // cancellation or the compiler-generated exception path); await states
    // stay protected while the PlayerLoop renders between MoveNext calls.
    const bool afterReadable = readable && ReadAutoPhotoState(self, &after, &ignored);
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        std::ostringstream log;
        log << "[VR][photo] AUTO_PHOTO_STEP self=" << self << " owner=" << presenter
            << " before=" << before << " after=" << after
            << " readable=" << readable << '/' << afterReadable;
        Log(log.str());
    }
    if (afterReadable && after == -2) {
        EndAutoPhoto(presenter);
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
        if (GakumasLocal::Config::vrRuntimeStartupEnabled &&
            instance != g_photoProtectionPresenter) {
            g_photoProtectionPresenter = instance;
            g_photoLiveFromType = -1;
            if (!g_autoPhotoHookReady) {
                g_liveSourcePhotoProtection.store(true, std::memory_order_release);
                Log("[VR][photo] LIVE_SOURCE_PHOTO_HOLD reason=auto-photo-hook-unavailable");
            }
        }
    }
}

void NoteLiveSceneModel(void* instance) noexcept {
    if (IsUnityManagedObjectAlive(instance)) {
        g_cachedModel = instance;
    }
}

void ForgetLiveScenePresenter(void* instance) noexcept {
    const auto previousSize = g_autoPhotoOwners.size();
    std::erase_if(g_autoPhotoOwners, [instance](const auto& entry) {
        return instance == nullptr || entry.presenter == instance;
    });
    if (previousSize != g_autoPhotoOwners.size()) {
        Log("[VR][photo] AUTO_PHOTO_END reason=scene-finalized restore=user-settings");
    }
    if (g_autoPhotoHookReady) {
        g_liveSourcePhotoProtection.store(!g_autoPhotoOwners.empty(), std::memory_order_release);
    }
    if (instance == nullptr || instance == g_cachedPresenter) {
        g_cachedPresenter = nullptr;
    }
    if (instance == nullptr || instance == g_photoProtectionPresenter) {
        if (g_photoProtectionPresenter != nullptr) {
            Log("[VR][photo] LIVE_SOURCE_PHOTO_RELEASE reason=scene-finalized");
        }
        g_photoProtectionPresenter = nullptr;
        g_photoLiveFromType = -1;
        if (!g_autoPhotoHookReady) {
            g_liveSourcePhotoProtection.store(false, std::memory_order_release);
        }
    }
}

void RefreshLiveSourcePhotoProtection() noexcept {
    if (g_autoPhotoHookReady) {
        const auto previousSize = g_autoPhotoOwners.size();
        std::erase_if(g_autoPhotoOwners, [](const auto& entry) {
            return !GakumasLocal::Config::vrRuntimeStartupEnabled ||
                !IsUnityManagedObjectAlive(entry.presenter);
        });
        if (previousSize != g_autoPhotoOwners.size()) {
            Log("[VR][photo] AUTO_PHOTO_END reason=scene-dead restore=user-settings");
        }
        g_liveSourcePhotoProtection.store(!g_autoPhotoOwners.empty(), std::memory_order_release);
        return;
    }
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled ||
        !IsUnityManagedObjectAlive(g_photoProtectionPresenter)) {
        if (g_photoProtectionPresenter != nullptr) {
            Log("[VR][photo] LIVE_SOURCE_PHOTO_RELEASE reason=scene-dead");
        }
        g_photoProtectionPresenter = nullptr;
        g_photoLiveFromType = -1;
        g_liveSourcePhotoProtection.store(false, std::memory_order_release);
        return;
    }
    if (g_photoLiveFromType >= 0) return;
    int fromType = -1;
    // Unknown (0) is not proof of an ordinary Live. Keep the source intact
    // on missing methods, an uninitialized model, or an invoke exception.
    if (!TryReadPhotoLiveFromType(g_photoProtectionPresenter, &fromType) ||
        fromType <= 0) return;
    g_photoLiveFromType = fromType;
    const bool protect = fromType == 1; // LiveFromType.Produce, persisted dump.
    g_liveSourcePhotoProtection.store(protect, std::memory_order_release);
    Log(std::string("[VR][photo] LIVE_SOURCE_PHOTO_CLASSIFIED fromType=") +
        std::to_string(fromType) + " protect=" + (protect ? "1" : "0"));
}

bool LiveSourcePhotoProtectionActive() noexcept {
    return g_liveSourcePhotoProtection.load(std::memory_order_acquire);
}

void InstallLiveAutoPhotoProtection() noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    static bool attempted = false;
    if (attempted) return;
    attempted = true;
    auto* assembly = UnityResolve::Get("Assembly-CSharp.dll");
    auto* scene = LiveClass("LiveScenePresenter");
    UnityResolve::Method* entry = nullptr;
    if (scene != nullptr) {
        for (auto* method : scene->methods) {
            if (method && method->name == "AutoPhotoAsync" && !method->static_function &&
                method->function && method->address && method->args.size() == 1 &&
                method->args[0] && method->args[0]->pType &&
                method->args[0]->pType->name == "System.Threading.CancellationToken" &&
                method->return_type && method->return_type->name == "Cysharp.Threading.Tasks.UniTask") {
                entry = method;
                break;
            }
        }
    }
    UnityResolve::Class* machine = nullptr;
    // Enumerate only this presenter's nested classes: similarly named state
    // machines on movie recording/other presenters are not this lifecycle.
    if (assembly != nullptr && scene != nullptr) {
        void* iterator = nullptr;
        while (void* nested = UnityResolve::Invoke<void*>(
            "il2cpp_class_get_nested_types", scene->address, &iterator)) {
            const char* name = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", nested);
            if (name == nullptr || std::string_view(name).find("<AutoPhotoAsync>d__") != 0) continue;
            const char* ns = UnityResolve::Invoke<const char*>("il2cpp_class_get_namespace", nested);
            auto* candidate = assembly->Get(name, ns != nullptr ? ns : "");
            if (candidate != nullptr && candidate->address == nested) machine = candidate;
            break;
        }
    }
    auto* moveNext = PhotoGetter(machine, "MoveNext", "System.Void");
    auto* state = machine ? machine->Get<UnityResolve::Field>("<>1__state") : nullptr;
    auto* owner = machine ? machine->Get<UnityResolve::Field>("<>4__this") : nullptr;
    const bool valueType = machine && UnityResolve::Invoke<bool>(
        "il2cpp_class_is_valuetype", machine->address);
    // IL2CPP field offsets include the boxed object header; MoveNext's
    // methodPointer receives an unboxed struct (same ABI as Cinemachine).
    const int headerSize = static_cast<int>(2 * sizeof(void*));
    const bool layout = valueType && state && owner && !state->static_field &&
        !owner->static_field && state->type && owner->type &&
        state->type->name == "System.Int32" &&
        owner->type->name == "Campus.Live.LiveScenePresenter" &&
        state->offset == headerSize && owner->offset >= headerSize + 8 &&
        owner->offset < headerSize + 256 && owner->offset % sizeof(void*) == 0;
    if (entry && layout && moveNext) {
        g_autoPhotoStateOffset = state->offset - headerSize;
        g_autoPhotoOwnerOffset = owner->offset - headerSize;
        g_autoPhotoHookReady = GakumasVR::Hooks::CreateAndEnable(
            moveNext->function, reinterpret_cast<void*>(&AutoPhotoMoveNextDetour),
            reinterpret_cast<void**>(&g_autoPhotoMoveNextOrig), "LiveScenePresenter.AutoPhotoAsync.MoveNext");
    }
    std::ostringstream log;
    log << "[VR][photo] AUTO_PHOTO_API ready=" << g_autoPhotoHookReady
        << " entry=" << (entry ? entry->function : nullptr)
        << " class=" << (machine ? machine->name : "missing")
        << " method=" << (moveNext ? moveNext->address : nullptr)
        << " function=" << (moveNext ? moveNext->function : nullptr)
        << " valueType=" << valueType << " stateOffset=" << g_autoPhotoStateOffset
        << " ownerOffset=" << g_autoPhotoOwnerOffset
        << " fallback=" << (g_autoPhotoHookReady ? "none" : "whole-live-protection");
    Log(log.str());
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
