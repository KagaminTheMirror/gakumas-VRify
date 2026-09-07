#include "SceneReadyGate.hpp"

#include "VrRuntime.hpp"
#include "../GakumasLocalify/Il2cppUtils.hpp"
#include "../deps/UnityResolve/UnityResolve.hpp"
#include "../hooks/HookManager.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace gakumas::vr {
namespace {

constexpr auto kWatchdog = std::chrono::seconds(15);
constexpr auto kIdleLoadingSample = std::chrono::milliseconds(100);

using MoveNextFn = void (*)(void*, void*);

MoveNextFn g_moveNextOrig = nullptr;
std::int32_t g_activationStateOffset = -1;
std::atomic<std::uint64_t> g_activationSerial{0};
std::atomic<PortraitLatchPolicy> g_latchPolicy{PortraitLatchPolicy::FollowDesktop};

UnityResolve::Class* g_loadingClass = nullptr;
UnityResolve::Method* g_loadingIsActive = nullptr;

bool g_apiResolved = false;
bool g_apiLogged = false;
bool g_loadingApiReady = false;
bool g_activationHookReady = false;
bool g_activationHookAttempted = false;

bool g_transitionActive = false;
bool g_contentReady = false;
bool g_contentReadyEdgePending = false;
bool g_sourceBegan = false;
bool g_sourceEnded = false;
bool g_leftBegan = false;
bool g_leftEnded = false;
bool g_rightBegan = false;
bool g_rightEnded = false;
bool g_epochSawLoading = false;
bool g_epochSawLoadingFall = false;
bool g_epochSawActivationComplete = false;
bool g_epochSawIdentity = false;
bool g_sourcePresentAtEpochStart = false;
bool g_loadingKnown = false;
bool g_lastLoading = false;
bool g_stereoEligible = false;
bool g_sourcePresent = false;
bool g_unsupportedLogged = false;
bool g_watchdogLogged = false;
bool g_holdLogged = false;
bool g_followLogged = false;
bool g_publishHoldLogged = false;
std::uint64_t g_epoch = 0;
std::uint64_t g_revokeSerial = 0;
std::uint64_t g_releaseSerial = 0;
std::uint64_t g_activationSerialAtEpoch = 0;
std::chrono::steady_clock::time_point g_epochStartedAt{};
std::chrono::steady_clock::time_point g_lastLoadingSample{};

void Log(std::string_view message) noexcept {
    WriteVrLog(message);
}

bool ReadIntAt(void* instance, std::int32_t offset, int* out) noexcept {
    if (instance == nullptr || offset < 0 || out == nullptr) {
        return false;
    }
    __try {
        *out = *reinterpret_cast<int*>(
            reinterpret_cast<std::uintptr_t>(instance) +
            static_cast<std::uintptr_t>(offset));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RuntimeInvokeRaw(
    void* methodInfo,
    void* instance,
    void** arguments,
    void** result) noexcept {
    using Invoke = void* (*)(void*, void*, void**, void**);
    static const auto invoke = reinterpret_cast<Invoke>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_runtime_invoke"));
    if (invoke == nullptr || methodInfo == nullptr || result == nullptr) {
        return false;
    }
    void* exception = nullptr;
    __try {
        *result = invoke(methodInfo, instance, arguments, &exception);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return exception == nullptr;
}

bool InvokeBool(
    UnityResolve::Method* method,
    void* instance,
    bool* out) noexcept {
    using Unbox = void* (*)(void*);
    static const auto unbox = reinterpret_cast<Unbox>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_object_unbox"));
    if (method == nullptr || method->address == nullptr || out == nullptr ||
        unbox == nullptr) {
        return false;
    }
    if (!method->static_function && instance == nullptr) {
        return false;
    }
    void* boxed = nullptr;
    if (!RuntimeInvokeRaw(method->address, instance, nullptr, &boxed) ||
        boxed == nullptr) {
        return false;
    }
    __try {
        void* raw = unbox(boxed);
        if (raw == nullptr) {
            return false;
        }
        *out = *static_cast<bool*>(raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

UnityResolve::Method* FindZeroArg(
    UnityResolve::Class* klass,
    const char* name,
    bool requireStatic) noexcept {
    if (klass == nullptr || name == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method != nullptr && method->name == name &&
            method->args.empty() &&
            method->static_function == requireStatic &&
            method->address != nullptr) {
            return method;
        }
    }
    return nullptr;
}

UnityResolve::Class* FindActivationStateMachine() noexcept {
    auto* assembly = UnityResolve::Get("campus-submodule.Runtime.dll");
    if (assembly == nullptr || assembly->address == nullptr) {
        return nullptr;
    }
    void* image = UnityResolve::Invoke<void*>(
        "il2cpp_assembly_get_image", assembly->address);
    if (image == nullptr) {
        return nullptr;
    }
    const int count =
        UnityResolve::Invoke<int>("il2cpp_image_get_class_count", image);
    for (int index = 0; index < count; ++index) {
        void* klass =
            UnityResolve::Invoke<void*>("il2cpp_image_get_class", image, index);
        if (klass == nullptr) {
            continue;
        }
        const char* name =
            UnityResolve::Invoke<const char*>("il2cpp_class_get_name", klass);
        if (name == nullptr ||
            std::strstr(name, "OnCompleteActivationAsync") == nullptr) {
            continue;
        }
        const char* namespaze = UnityResolve::Invoke<const char*>(
            "il2cpp_class_get_namespace", klass);
        return assembly->Get(
            name, namespaze != nullptr && namespaze[0] != '\0' ? namespaze : "*");
    }
    return nullptr;
}

void MoveNextDetour(void* self, void* methodInfo) {
    if (g_moveNextOrig != nullptr) {
        g_moveNextOrig(self, methodInfo);
    }
    int state = 0;
    if (ReadIntAt(self, g_activationStateOffset, &state) && state < 0) {
        g_activationSerial.fetch_add(1U, std::memory_order_acq_rel);
    }
}

void EnsureApi() noexcept {
    if (g_apiResolved) {
        return;
    }
    if (UnityResolve::Get("Assembly-CSharp.dll") == nullptr) {
        return;
    }
    g_apiResolved = true;
    g_loadingClass =
        Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Common",
                              "LoadingManager");
    g_loadingIsActive =
        FindZeroArg(g_loadingClass, "get_IsActive", true);
    g_loadingApiReady = g_loadingIsActive != nullptr;

    if (!g_activationHookAttempted) {
        g_activationHookAttempted = true;
        auto* stateMachine = FindActivationStateMachine();
        UnityResolve::Method* moveNext = nullptr;
        UnityResolve::Field* stateField = nullptr;
        if (stateMachine != nullptr) {
            moveNext = FindZeroArg(stateMachine, "MoveNext", false);
            stateField =
                stateMachine->Get<UnityResolve::Field>("<>1__state");
        }
        if (moveNext != nullptr && moveNext->function != nullptr &&
            stateField != nullptr && !stateField->static_field &&
            stateField->offset >= 0) {
            g_activationStateOffset = stateField->offset;
            g_activationHookReady = GakumasVR::Hooks::CreateAndEnable(
                moveNext->function,
                reinterpret_cast<void*>(&MoveNextDetour),
                reinterpret_cast<void**>(&g_moveNextOrig),
                "CampusSceneManager.OnCompleteActivationAsync.MoveNext");
        }
    }

    if (g_apiLogged) {
        return;
    }
    g_apiLogged = true;
    std::ostringstream stream;
    stream << "[VR][scene-ready] SCENE_READY_API loading="
           << (g_loadingApiReady ? "1" : "0")
           << " activationHook=" << (g_activationHookReady ? "1" : "0")
           << " activationStateOffset=" << g_activationStateOffset;
    Log(stream.str());
}

[[nodiscard]] bool WantsIncoming3dLatch() noexcept {
    return g_epochSawIdentity ||
        (g_epochSawLoading && !g_sourcePresentAtEpochStart);
}

void PublishLatchPolicy() noexcept {
    PortraitLatchPolicy policy = PortraitLatchPolicy::FollowDesktop;
    if (g_transitionActive) {
        const bool loading = g_loadingKnown && g_lastLoading;
        const bool incoming3d = WantsIncoming3dLatch();
        // Capture every official loading frame. CaptureLoading still presents
        // the live desktop, so 2D page animations remain live, while a later
        // identity edge already has a valid loading frame to hold instead of
        // bootstrapping from a black or half-swapped camera view.
        if (loading) {
            policy = PortraitLatchPolicy::CaptureLoading;
        } else if (incoming3d && !g_contentReady) {
            policy = PortraitLatchPolicy::HoldFrozen;
        }
    }
    g_latchPolicy.store(policy, std::memory_order_release);
}

void ReleaseEpoch(const char* reason) noexcept {
    if (!g_transitionActive) {
        PublishLatchPolicy();
        return;
    }
    g_transitionActive = false;
    g_contentReady = true;
    const bool loadingFall = g_epochSawLoadingFall;
    const bool activation = g_epochSawActivationComplete;
    g_epochSawIdentity = false;
    g_epochSawLoading = false;
    g_epochSawLoadingFall = false;
    g_holdLogged = false;
    PublishLatchPolicy();
    ++g_releaseSerial;
    Log(std::string("[VR][scene-ready] SCENE_READY_RELEASE epoch=") +
        std::to_string(g_epoch) + " reason=" +
        (reason != nullptr ? reason : "unknown") +
        " stereo=" + (g_stereoEligible ? "1" : "0") +
        " loadingFall=" + (loadingFall ? "1" : "0") +
        " activation=" + (activation ? "1" : "0"));
}

void StartEpoch(const char* reason) noexcept {
    ++g_epoch;
    g_transitionActive = true;
    g_contentReady = false;
    g_contentReadyEdgePending = false;
    g_sourceBegan = false;
    g_sourceEnded = false;
    g_leftBegan = false;
    g_leftEnded = false;
    g_rightBegan = false;
    g_rightEnded = false;
    g_epochSawLoading = g_loadingKnown && g_lastLoading;
    g_epochSawLoadingFall = false;
    g_epochSawActivationComplete = false;
    g_epochSawIdentity = reason != nullptr &&
        std::strcmp(reason, "scene-identity") == 0;
    g_sourcePresentAtEpochStart = g_sourcePresent;
    g_unsupportedLogged = false;
    g_watchdogLogged = false;
    g_holdLogged = false;
    g_followLogged = false;
    g_publishHoldLogged = false;
    g_activationSerialAtEpoch =
        g_activationSerial.load(std::memory_order_acquire);
    g_epochStartedAt = std::chrono::steady_clock::now();
    ++g_revokeSerial;
    InvalidateUnityStereoFrame();
    PublishLatchPolicy();
    Log(std::string("[VR][scene-ready] SCENE_READY_REVOKE epoch=") +
        std::to_string(g_epoch) + " reason=" +
        (reason != nullptr ? reason : "unknown") +
        " loading=" + ((g_loadingKnown && g_lastLoading) ? "1" : "0") +
        " identity=" + (g_epochSawIdentity ? "1" : "0") +
        " sourceAtStart=" + (g_sourcePresentAtEpochStart ? "1" : "0"));
}

void EvaluateReady() noexcept {
    if (!g_transitionActive) {
        g_contentReady = true;
        PublishLatchPolicy();
        return;
    }

    const std::uint64_t activationSerial =
        g_activationSerial.load(std::memory_order_acquire);
    if (activationSerial > g_activationSerialAtEpoch &&
        !g_epochSawActivationComplete) {
        g_epochSawActivationComplete = true;
        Log("[VR][scene-ready] SCENE_READY_ACTIVATION_COMPLETE epoch=" +
            std::to_string(g_epoch));
    }

    const bool loadingActive = g_loadingKnown && g_lastLoading;
    bool genericReady = false;
    if (g_epochSawLoading) {
        genericReady = !loadingActive && g_epochSawLoadingFall;
    } else if (g_epochSawActivationComplete) {
        genericReady = !loadingActive;
    } else if (g_epochSawIdentity && !g_epochSawLoading) {
        // Identity-only leftover: do not wait forever for a loading
        // signal that this scene never raises.
        genericReady = !loadingActive;
    } else if (!g_loadingApiReady && !g_activationHookReady) {
        if (!g_unsupportedLogged) {
            g_unsupportedLogged = true;
            Log("[VR][scene-ready] SCENE_READY_UNSUPPORTED epoch=" +
                std::to_string(g_epoch) +
                " reason=no-loading-or-activation-signal");
        }
        genericReady = false;
    } else {
        genericReady = false;
    }

    const bool wasContentReady = g_contentReady;
    g_contentReady = genericReady;
    if (g_contentReady && !wasContentReady) {
        g_contentReadyEdgePending = true;
        Log("[VR][scene-ready] SCENE_READY_CONTENT_READY epoch=" +
            std::to_string(g_epoch) +
            " loadingFall=" + (g_epochSawLoadingFall ? "1" : "0") +
            " activation=" +
            (g_epochSawActivationComplete ? "1" : "0"));
    }
    const bool eyesComplete =
        g_sourceEnded && g_leftEnded && g_rightEnded;
    const bool hasImmersiveSource = g_sourcePresent || g_sourceBegan;
    const bool incoming3d = WantsIncoming3dLatch();
    if (g_contentReady && eyesComplete) {
        ReleaseEpoch("stereo-ready");
        return;
    }
    if (g_contentReady && !hasImmersiveSource && !loadingActive) {
        ReleaseEpoch("no-source");
        return;
    }
    if (!incoming3d && !g_followLogged) {
        g_followLogged = true;
        Log("[VR][scene-ready] SCENE_READY_PORTRAIT_FOLLOW epoch=" +
            std::to_string(g_epoch) + " reason=not-incoming-3d" +
            " stereo=" + (g_stereoEligible ? "1" : "0") +
            " sourcePresent=" + (g_sourcePresent ? "1" : "0") +
            " identity=" + (g_epochSawIdentity ? "1" : "0") +
            " loadingFall=" + (g_epochSawLoadingFall ? "1" : "0"));
    } else if (!loadingActive && !hasImmersiveSource && incoming3d &&
               !g_followLogged) {
        g_followLogged = true;
        Log("[VR][scene-ready] SCENE_READY_PORTRAIT_FOLLOW epoch=" +
            std::to_string(g_epoch) + " reason=no-source" +
            " stereo=" + (g_stereoEligible ? "1" : "0") +
            " loadingFall=" + (g_epochSawLoadingFall ? "1" : "0"));
    } else if (g_contentReady && incoming3d && hasImmersiveSource &&
               !g_holdLogged && !eyesComplete) {
        g_holdLogged = true;
        Log("[VR][scene-ready] SCENE_READY_HOLD epoch=" +
            std::to_string(g_epoch) + " reason=await-source-eyes" +
            " source=" + (g_sourceEnded ? "1" : "0") +
            " left=" + (g_leftEnded ? "1" : "0") +
            " right=" + (g_rightEnded ? "1" : "0"));
    }
    PublishLatchPolicy();
}

void MaybeStartLoadingEpoch() noexcept {
    if (g_transitionActive) {
        // A second official loading phase can begin after this epoch already
        // reached content-ready but before source/left/right proof completed
        // (Photography .167 did this twice). Revoke that proof immediately;
        // otherwise SceneReadyAllowsStereoRender stays true through the nested
        // unload and the shared-material writers race destroyed objects.
        g_contentReady = false;
        g_contentReadyEdgePending = false;
        g_sourceBegan = false;
        g_sourceEnded = false;
        g_leftBegan = false;
        g_leftEnded = false;
        g_rightBegan = false;
        g_rightEnded = false;
        g_epochSawLoading = true;
        g_epochSawLoadingFall = false;
        g_watchdogLogged = false;
        g_holdLogged = false;
        g_followLogged = false;
        g_publishHoldLogged = false;
        g_epochStartedAt = std::chrono::steady_clock::now();
        ++g_revokeSerial;
        InvalidateUnityStereoFrame();
        PublishLatchPolicy();
        Log("[VR][scene-ready] SCENE_READY_REVOKE epoch=" +
            std::to_string(g_epoch) + " reason=loading-active-repeat" +
            " identity=" + (g_epochSawIdentity ? "1" : "0") +
            " sourcePresent=" + (g_sourcePresent ? "1" : "0"));
        return;
    }
    StartEpoch("loading-active");
}

void SampleLoading() noexcept {
    if (!g_loadingApiReady) {
        return;
    }
    const bool needEdge =
        g_transitionActive || (g_loadingKnown && g_lastLoading);
    const auto now = std::chrono::steady_clock::now();
    if (!needEdge && g_loadingKnown &&
        now - g_lastLoadingSample < kIdleLoadingSample) {
        return;
    }
    g_lastLoadingSample = now;
    bool active = false;
    if (!InvokeBool(g_loadingIsActive, nullptr, &active)) {
        return;
    }
    if (!g_loadingKnown) {
        g_loadingKnown = true;
        g_lastLoading = active;
        if (active) {
            MaybeStartLoadingEpoch();
        }
        return;
    }
    if (active && !g_lastLoading) {
        g_lastLoading = true;
        MaybeStartLoadingEpoch();
        return;
    }
    if (!active && g_lastLoading) {
        g_lastLoading = false;
        if (g_transitionActive) {
            g_epochSawLoading = true;
            g_epochSawLoadingFall = true;
            Log("[VR][scene-ready] SCENE_READY_LOADING falling epoch=" +
                std::to_string(g_epoch));
        }
        return;
    }
    g_lastLoading = active;
    if (active && g_transitionActive) {
        g_epochSawLoading = true;
    }
}

void MaybeWatchdog() noexcept {
    if (!g_transitionActive || g_watchdogLogged) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - g_epochStartedAt < kWatchdog) {
        return;
    }
    g_watchdogLogged = true;
    Log("[VR][scene-ready] SCENE_READY_WATCHDOG epoch=" +
        std::to_string(g_epoch) +
        " holdMs=" +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - g_epochStartedAt)
                           .count()) +
        " loading=" + ((g_loadingKnown && g_lastLoading) ? "1" : "0") +
        " sawLoading=" + (g_epochSawLoading ? "1" : "0") +
        " sawFall=" + (g_epochSawLoadingFall ? "1" : "0") +
        " activation=" + (g_epochSawActivationComplete ? "1" : "0") +
        " content=" + (g_contentReady ? "1" : "0") +
        " source=" + (g_sourceEnded ? "1" : "0") +
        " sourcePresent=" + (g_sourcePresent ? "1" : "0") +
        " identity=" + (g_epochSawIdentity ? "1" : "0") +
        " incoming3d=" + (WantsIncoming3dLatch() ? "1" : "0") +
        " follow=" + (g_followLogged ? "1" : "0") +
        " - not releasing on elapsed time");
}

} // namespace

void SampleSceneReady(
    const char* where,
    bool stereoEligible,
    bool sourcePresent) noexcept {
    (void)where;
    EnsureApi();
    g_stereoEligible = stereoEligible;
    g_sourcePresent = sourcePresent;
    SampleLoading();
    EvaluateReady();
    MaybeWatchdog();
}

void NoteSceneReadyIdentityChanged(const char* where) noexcept {
    EnsureApi();
    (void)where;
    if (g_transitionActive) {
        g_epochSawIdentity = true;
        PublishLatchPolicy();
        Log("[VR][scene-ready] SCENE_READY_IDENTITY noted epoch=" +
            std::to_string(g_epoch) + " reason=already-in-epoch");
        EvaluateReady();
        MaybeWatchdog();
        return;
    }
    if (g_loadingKnown && g_lastLoading) {
        StartEpoch("scene-identity");
        EvaluateReady();
        MaybeWatchdog();
        return;
    }
    Log("[VR][scene-ready] SCENE_READY_IDENTITY ignored reason=no-loading" +
        std::string(" sourcePresent=") + (g_sourcePresent ? "1" : "0") +
        " stereo=" + (g_stereoEligible ? "1" : "0"));
}

void NoteSceneReadySourceCameraChanged() noexcept {
    if (!g_transitionActive) {
        return;
    }
    g_sourceBegan = false;
    g_sourceEnded = false;
    g_leftBegan = false;
    g_leftEnded = false;
    g_rightBegan = false;
    g_rightEnded = false;
    g_contentReady = false;
    g_contentReadyEdgePending = false;
    ++g_revokeSerial;
    Log("[VR][scene-ready] SCENE_READY_REVOKE epoch=" +
        std::to_string(g_epoch) + " reason=source-camera");
    EvaluateReady();
}

void NoteSceneReadyCameraBoundary(const char* role, bool begin) noexcept {
    if (!g_transitionActive || role == nullptr) {
        return;
    }
    if (!g_contentReady) {
        return;
    }
    const bool source = std::strcmp(role, "source") == 0;
    const bool left = std::strcmp(role, "left") == 0;
    const bool right = std::strcmp(role, "right") == 0;
    if (source) {
        if (begin) {
            g_sourceBegan = true;
            g_sourceEnded = false;
            g_leftBegan = false;
            g_leftEnded = false;
            g_rightBegan = false;
            g_rightEnded = false;
        } else if (g_sourceBegan) {
            g_sourceEnded = true;
        }
    } else if (g_sourceEnded && left) {
        if (begin) {
            g_leftBegan = true;
            g_leftEnded = false;
        } else if (g_leftBegan) {
            g_leftEnded = true;
        }
    } else if (g_sourceEnded && right) {
        if (begin) {
            g_rightBegan = true;
            g_rightEnded = false;
        } else if (g_rightBegan) {
            g_rightEnded = true;
        }
    }
    EvaluateReady();
}

bool SceneReadyAllowsStereoPublish() noexcept {
    return !g_transitionActive;
}

bool SceneReadyAllowsStereoRender() noexcept {
    return !g_transitionActive || g_contentReady;
}

bool SceneReadyConsumeContentReadyEdge() noexcept {
    if (!g_contentReadyEdgePending) {
        return false;
    }
    g_contentReadyEdgePending = false;
    return true;
}

bool SceneReadyConsumePublishHoldLog() noexcept {
    if (!g_transitionActive || g_publishHoldLogged) {
        return false;
    }
    g_publishHoldLogged = true;
    return true;
}

PortraitLatchPolicy CurrentPortraitLatchPolicy() noexcept {
    return g_latchPolicy.load(std::memory_order_acquire);
}

SceneReadyDiagnosticState CurrentSceneReadyDiagnosticState() noexcept {
    SceneReadyDiagnosticState state{};
    state.epoch = g_epoch;
    state.revokeSerial = g_revokeSerial;
    state.releaseSerial = g_releaseSerial;
    state.transitionActive = g_transitionActive;
    state.contentReady = g_contentReady;
    state.loadingKnown = g_loadingKnown;
    state.loadingActive = g_loadingKnown && g_lastLoading;
    state.stereoEligible = g_stereoEligible;
    state.sourcePresent = g_sourcePresent;
    state.epochSawIdentity = g_epochSawIdentity;
    state.renderAllowed = !g_transitionActive || g_contentReady;
    state.publishAllowed = !g_transitionActive;
    return state;
}

} // namespace gakumas::vr
