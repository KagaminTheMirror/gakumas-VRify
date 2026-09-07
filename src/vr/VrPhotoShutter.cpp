#include "VrPhotoShutter.hpp"

#include "LivePause.hpp"
#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "../GakumasLocalify/Il2cppUtils.hpp"
#include "../deps/UnityResolve/UnityResolve.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

// Official quick-shutter for the right-A button. Metadata dig:
// evidence/a-button-photo/ — both photo-capable scenes expose the same
// self-contained button handler `OnPhotoButtonAsync(CancellationToken)`
// (capture -> PhotographyCaptureData -> OnCapturePhotoAsync -> local album),
// guarded by the shared Campus.Common.PhotographyPresenter.get_CanTakePhoto.
// Everything here re-resolves those symbols on the live method table; a miss
// disables the feature with a visible log instead of guessing.
namespace gakumas::vr {
namespace {

std::atomic<bool> g_photoSceneActive{false};
std::atomic<std::uint32_t> g_photoResultGeneration{0};
std::atomic<std::uint32_t> g_photoResultCode{0};

void* g_photographyPresenter = nullptr;
bool g_resolveLogged = false;

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
    if (!RuntimeInvokeRaw(method, instance, nullptr, &boxed) ||
        boxed == nullptr) {
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

// Exact shape for the button handler: instance, one CancellationToken arg.
UnityResolve::Method* CancellationTokenArg(
    UnityResolve::Class* klass, const char* name) noexcept {
    if (klass == nullptr || name == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method != nullptr && method->name == name &&
            method->args.size() == 1U && method->args[0] != nullptr &&
            method->args[0]->pType != nullptr &&
            method->args[0]->pType->name ==
                "System.Threading.CancellationToken" &&
            !method->static_function && method->address != nullptr) {
            return method;
        }
    }
    return nullptr;
}

void PublishPhotoResult(std::uint32_t code) noexcept {
    g_photoResultCode.store(code, std::memory_order_release);
    g_photoResultGeneration.fetch_add(1U, std::memory_order_acq_rel);
}

void* AlivePhotographyPresenter() noexcept {
    if (!IsUnityManagedObjectAlive(g_photographyPresenter)) {
        g_photographyPresenter = nullptr;
    }
    return g_photographyPresenter;
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

void TriggerPhotoShutter(void* photography, void* live) noexcept {
    // Right-A shortcut only. The in-Grip official photo button never enters
    // this function; leaving it unblocked is intentional.
    if (GakumasLocal::Config::vrSourceCameraTiny &&
        !LiveSourcePhotoProtectionActive()) {
        Log("[VR][photo] SHUTTER_BLOCKED reason=paused-2d");
        PublishPhotoResult(kPhotoShutterPaused2D);
        return;
    }

    static UnityResolve::Class* photographyClass = Il2cppUtils::GetClass(
        "Assembly-CSharp.dll", "Campus.Photography",
        "PhotographyScenePresenter");
    static UnityResolve::Class* liveClass = Il2cppUtils::GetClass(
        "Assembly-CSharp.dll", "Campus.Live", "LiveScenePresenter");
    static UnityResolve::Class* commonPhotoClass = Il2cppUtils::GetClass(
        "Assembly-CSharp.dll", "Campus.Common", "PhotographyPresenter");
    static UnityResolve::Method* photographyButton =
        CancellationTokenArg(photographyClass, "OnPhotoButtonAsync");
    static UnityResolve::Method* liveButton =
        CancellationTokenArg(liveClass, "OnPhotoButtonAsync");
    static UnityResolve::Method* canTakePhoto =
        ZeroArg(commonPhotoClass, "get_CanTakePhoto");

    if (!g_resolveLogged) {
        g_resolveLogged = true;
        Log(std::string("[VR][photo] API_RESOLVE photographyButton=") +
            (photographyButton != nullptr ? "1" : "0") + " liveButton=" +
            (liveButton != nullptr ? "1" : "0") + " canTakePhoto=" +
            (canTakePhoto != nullptr ? "1" : "0"));
    }

    UnityResolve::Class* targetClass = nullptr;
    UnityResolve::Method* buttonMethod = nullptr;
    void* target = nullptr;
    const char* tag = "none";
    if (photography != nullptr) {
        targetClass = photographyClass;
        buttonMethod = photographyButton;
        target = photography;
        tag = "Photography";
    } else if (live != nullptr) {
        targetClass = liveClass;
        buttonMethod = liveButton;
        target = live;
        tag = "Live";
        // stereo.222 hardware: the Start-hook cache can hold a Live presenter
        // whose `_photoPresenter` reads null while the game's own auto-photo
        // proves the real presenter is photo-capable. A press is an event, so
        // refresh from a single scan exactly like the B-pause path does.
        void* fresh = FindFirstInstance(liveClass);
        if (fresh != nullptr && fresh != target) {
            Log("[VR][photo] SHUTTER_TARGET_REFRESHED class=Live source=press-scan");
            NoteLiveScenePresenter(fresh);
            target = fresh;
        }
    }
    if (target == nullptr || buttonMethod == nullptr) {
        Log(std::string("[VR][photo] SHUTTER_IGNORED reason=") +
            (target == nullptr ? "no-photo-scene" : "method-unresolved"));
        PublishPhotoResult(kPhotoShutterUnavailable);
        return;
    }

    // Invoking the handler directly bypasses the UI's disabled button state,
    // so the shared stock/busy guard runs first — but it only blocks when it
    // is readable AND false. stereo.222 hardware: requiring a readable guard
    // ("photo-presenter-not-ready") dead-locked Live photos; an unreadable
    // guard now falls through to the invoke, whose no-op case surfaces as the
    // toast timeout instead of a fake success (event-driven toast).
    void* photoPresenter =
        ReadInstanceField(targetClass, target, "_photoPresenter");
    bool canTake = false;
    const bool haveCanTake = photoPresenter != nullptr &&
        canTakePhoto != nullptr &&
        RuntimeInvokeBool(canTakePhoto, photoPresenter, &canTake);
    if (haveCanTake && !canTake) {
        Log(std::string("[VR][photo] SHUTTER_BLOCKED class=") + tag +
            " reason=can-take-photo-false");
        PublishPhotoResult(kPhotoShutterUnavailable);
        return;
    }

    // CancellationToken has a single reference field; an all-zero value is
    // CancellationToken.None. The returned UniTask box is dropped: the async
    // state machine keeps running on the game's PlayerLoop.
    unsigned char cancellationToken[16] = {};
    void* arguments[] = {cancellationToken};
    void* ignored = nullptr;
    const bool invoked =
        RuntimeInvokeRaw(buttonMethod, target, arguments, &ignored);
    Log(std::string("[VR][photo] SHUTTER class=") + tag + " invoked=" +
        (invoked ? "1" : "0") + " canTake=" +
        (haveCanTake ? (canTake ? "1" : "0") : "unknown"));
    // Success is NOT published here: the handler can hit the 50-shot limit
    // internally and take nothing. The "photo taken" toast comes from the
    // game's own capture animation (NotePhotoCaptured); the worker times out
    // a press that produced no event.
    if (!invoked) {
        PublishPhotoResult(kPhotoShutterUnavailable);
    }
}

} // namespace

void NotePhotographyScenePresenter(void* instance) noexcept {
    if (IsUnityManagedObjectAlive(instance)) {
        g_photographyPresenter = instance;
        g_photoSceneActive.store(true, std::memory_order_release);
        Log("[VR][photo] PHOTOGRAPHY_SCENE presenter=noted");
    }
}

void ForgetPhotographyScenePresenter(void* instance) noexcept {
    if (instance == nullptr || instance == g_photographyPresenter) {
        if (g_photographyPresenter != nullptr) {
            Log("[VR][photo] PHOTOGRAPHY_SCENE presenter=finalized");
        }
        g_photographyPresenter = nullptr;
    }
}

void NotePhotographySceneAnchorEvent() noexcept {
    if (AlivePhotographyPresenter() != nullptr) {
        return;
    }
    // One scan per second at most, and only while a photography hook is
    // actually firing (never a free-running poll).
    static std::chrono::steady_clock::time_point lastScan{};
    const auto now = std::chrono::steady_clock::now();
    if (lastScan.time_since_epoch().count() != 0 &&
        now - lastScan < std::chrono::seconds(1)) {
        return;
    }
    lastScan = now;
    static UnityResolve::Class* photographyClass = Il2cppUtils::GetClass(
        "Assembly-CSharp.dll", "Campus.Photography",
        "PhotographyScenePresenter");
    void* presenter = FindFirstInstance(photographyClass);
    if (presenter != nullptr) {
        g_photographyPresenter = presenter;
        g_photoSceneActive.store(true, std::memory_order_release);
        Log("[VR][photo] PHOTOGRAPHY_SCENE presenter=noted source=anchor-scan");
    }
}

bool PhotoSceneActive() noexcept {
    return g_photoSceneActive.load(std::memory_order_acquire);
}

void NotePhotoCaptured() noexcept {
    Log("[VR][photo] PHOTO_CAPTURED source=capture-animation");
    PublishPhotoResult(kPhotoShutterTaken);
}

void UpdatePhotoSceneAndConsumeShutterRequests() noexcept {
    RefreshLiveSourcePhotoProtection();
    void* photography = AlivePhotographyPresenter();
    void* live = AliveLiveScenePresenter();
    const bool active = photography != nullptr || live != nullptr;
    const bool previous =
        g_photoSceneActive.exchange(active, std::memory_order_acq_rel);
    if (previous != active) {
        Log(std::string("[VR][photo] PHOTO_SCENE_ACTIVE value=") +
            (active ? "1" : "0") + " photography=" +
            (photography != nullptr ? "1" : "0") + " live=" +
            (live != nullptr ? "1" : "0"));
    }

    camera::VrCameraInputSample input;
    if (!ReadVrCameraInput(input) || !input.valid) {
        return;
    }
    static bool countersInitialized = false;
    static std::uint32_t consumedPresses = 0;
    if (!countersInitialized) {
        // Adopt the cumulative counter so worker history before the first
        // Unity-side consumption never fires phantom shutters.
        consumedPresses = input.photoPressCount;
        countersInitialized = true;
    }
    std::uint32_t presses = input.photoPressCount - consumedPresses;
    consumedPresses = input.photoPressCount;
    // Rapid taps queue like the pause button, but each shot is asynchronous
    // and stock-guarded; cap one invocation per Unity frame per press burst.
    while (presses-- > 0U) {
        TriggerPhotoShutter(photography, live);
    }
}

void ReadPhotoShutterResult(
    std::uint32_t& generation, std::uint32_t& code) noexcept {
    generation = g_photoResultGeneration.load(std::memory_order_acquire);
    code = g_photoResultCode.load(std::memory_order_acquire);
}

} // namespace gakumas::vr
