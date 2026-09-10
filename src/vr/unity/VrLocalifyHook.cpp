#include "GakumasLocalify/Hook.h"
#include "GakumasLocalify/HookTexture.h"
#include "GakumasLocalify/Plugin.h"
#include "GakumasLocalify/Log.h"
#include "deps/UnityResolve/UnityResolve.hpp"
#include "GakumasLocalify/Il2cppUtils.hpp"
#include "GakumasLocalify/Local.h"
#include "GakumasLocalify/MasterLocal.h"
#include <array>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstring>
#include "GakumasLocalify/camera/camera.hpp"
#include "vr/config/VrifyConfig.hpp"
#include "vr/PerformanceTiming.hpp"
#include "vr/PerformanceProbe.hpp"
#include "vr/SrpPerformanceTrace.hpp"
// #include <jni.h>
#include <thread>
#include <map>
#include <set>
#include <list>
#include <limits>
#include <optional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>
#include <string_view>
#include <initializer_list>
#include <utility>
#include <cctype>
#include <intrin.h>
#include "host/localify/PlatformDefine.hpp"

#ifdef GKMS_WINDOWS
    #include "host/VrWindowsPlatform.hpp"
    #include "vr/VrRuntime.hpp"
    #include "vr/VrFreeCamera.hpp"
    #include "vr/LivePause.hpp"
    #include "vr/VrPhotoShutter.hpp"
    #include "vr/SkyRenderHooks.hpp"
    #include "vr/UnityStereoRenderer.hpp"
    #include "vr/frame/FrameLoopDriver.hpp"
    #include "vr/GripTransparencyTrace.hpp"
    #include "vr/GripBlurSource.hpp"
    #include "vr/VrHandGlowSticks.hpp"
    #include "vr/input/ScrollInputDiagnostics.hpp"
    #include "vr/input/UnityPointerInput.hpp"
    #include "vr/pose/RelativePoseBridge.hpp"
    #include "cpprest/details/http_helpers.h"
    #include "resourceUpdate/resourceUpdate.hpp"
#endif


std::unordered_set<void*> hookedStubs{};
extern std::filesystem::path gakumasLocalPath;

#define DEFINE_HOOK(returnType, name, params)                                                      \
	using name##_Type = returnType(*) params;                                                      \
	name##_Type name##_Addr = nullptr;                                                             \
	name##_Type name##_Orig = nullptr;                                                             \
	returnType name##_Hook params

/*
void UnHookAll() {
    for (const auto i: hookedStubs) {
        int result = shadowhook_unhook(i);
        if(result != 0)
        {
            int error_num = shadowhook_get_errno();
            const char *error_msg = shadowhook_to_errmsg(error_num);
            GakumasLocal::Log::ErrorFmt("unhook failed: %d - %s", error_num, error_msg);
        }
    }
}*/

namespace GakumasLocal::HookMain {
    using Il2cppString = UnityResolve::UnityType::String;
    using Il2CppGCHandle = void*;

    bool LocalizationActive() {
        return Config::vrLocalizeText;
    }

    void EnsureLocalizationData() {
        if (!LocalizationActive()) {
            return;
        }
        static std::once_flag once;
        std::call_once(once, []() {
            Local::LoadData();
            MasterLocal::LoadData();
        });
    }

    bool TryGetI18n(const std::string& key, std::string* out) {
        if (!LocalizationActive() || out == nullptr) {
            return false;
        }
        EnsureLocalizationData();
        return Local::GetI18n(key, out);
    }

    bool TryGetGenericText(const std::string& origText, std::string* out) {
        if (!LocalizationActive() || out == nullptr) {
            return false;
        }
        EnsureLocalizationData();
        return Local::GetGenericText(origText, out);
    }

    UnityResolve::UnityType::String* environment_get_stacktrace() {
        /*
        static auto mtd = Il2cppUtils::GetMethod("mscorlib.dll", "System",
                                                 "Environment", "get_StackTrace");
        return mtd->Invoke<UnityResolve::UnityType::String*>();*/
        const auto pClass = Il2cppUtils::GetClass("mscorlib.dll", "System.Diagnostics",
                                                  "StackTrace");

        const auto ctor_mtd = Il2cppUtils::GetMethod("mscorlib.dll", "System.Diagnostics",
                                                     "StackTrace", ".ctor");
        const auto toString_mtd = Il2cppUtils::GetMethod("mscorlib.dll", "System.Diagnostics",
                                                         "StackTrace", "ToString");

        const auto klassInstance = pClass->New<void*>();
        ctor_mtd->Invoke<void>(klassInstance);
        return toString_mtd->Invoke<Il2cppString*>(klassInstance);
    }

    // Unity 6: DebugLogHandler.Internal_Log* is resolved as a managed method
    // through GetMethod(), so the generated IL2CPP function includes the
    // trailing MethodInfo* argument.
    DEFINE_HOOK(void, Internal_LogException, (void* ex, void* obj, void* mtd)) {
        Internal_LogException_Orig(ex, obj, mtd);
        static auto Exception_ToString = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Exception", "ToString");
        Log::LogUnityLog(ANDROID_LOG_ERROR, "UnityLog - Internal_LogException:\n%s", Exception_ToString->Invoke<Il2cppString*>(ex)->ToString().c_str());
    }

    DEFINE_HOOK(void, Internal_Log, (int logType, int logOption, UnityResolve::UnityType::String* content, void* context, void* mtd)) {
        Internal_Log_Orig(logType, logOption, content, context, mtd);
        Log::LogUnityLog(ANDROID_LOG_VERBOSE, "Internal_Log:\n%s", content->ToString().c_str());
    }

    bool IsNativeObjectAlive(void* obj) {
        if (!obj) {
            return false;
        }

        static const auto IsNativeObjectAliveMtd = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll",
            "UnityEngine",
            "Object",
            "IsNativeObjectAlive",
            { "UnityEngine.Object" }
        );

        if (!IsNativeObjectAliveMtd || !IsNativeObjectAliveMtd->function) {
            return false;
        }

        using IsNativeObjectAliveFn = bool (*)(void* obj, void* method);
        const auto isNativeObjectAlive = reinterpret_cast<IsNativeObjectAliveFn>(
            IsNativeObjectAliveMtd->function
        );

        return isNativeObjectAlive(
            obj,
            IsNativeObjectAliveMtd->address
        );
    }

#ifdef GKMS_WINDOWS
    struct UnityCameraDiagnosticRecord {
        std::string name;
        std::uint64_t renderCount = 0;
        bool mainCamera = false;
        bool descriptorLogged = false;
        bool mainSelectionLogged = false;
    };

    std::unordered_map<void*, UnityCameraDiagnosticRecord> unityCameraDiagnosticRecords{};
    std::mutex vrCameraStateMutex{};
    void* vrSourceCamera = nullptr;
    std::uint64_t unityCameraRenderCallbacks = 0;
    std::atomic<std::uint64_t> cinemachinePoseSamples = 0;
    std::atomic<std::uint64_t> cinemachineInvalidPoseSamples = 0;
    std::atomic_bool vrUnityHookExceptionLogged = false;
    std::atomic_bool unityCameraDiagnosticMarkerLogged = false;
    std::atomic_bool unityCameraMainHookReady = false;
    std::atomic_bool unityCinemachineHookReady = false;
    std::atomic_bool unityCameraRenderHookReady = false;
    std::atomic_bool unityRenderPipelineGuardReady = false;
    std::atomic_bool unityRenderLoopHookHitLogged = false;
    std::atomic_bool vrSourceCameraEyeIgnoredLogged = false;
    std::atomic_bool unityCameraDiagnosticHooksConfigured = false;
    std::atomic_bool cinemachineStateReadsEnabled = true;
    std::atomic_bool cinemachineStateReadFailureLogged = false;
    std::atomic_bool produceTransitionLayoutLogged = false;
    std::mutex vrHeadPoseBridgeMutex{};
    gakumas::vr::pose::RelativePoseBridge vrHeadPoseBridge{};
    std::atomic_bool vrHeadPoseWritesEnabled = true;
    std::atomic_bool vrHeadPoseWriteFailureLogged = false;
    std::uint64_t vrHeadPoseAppliedSamples = 0;
    std::mutex vrStereoCameraFrameMutex{};
    gakumas::vr::UnityStereoCameraFrame vrStereoCameraFrame{};
    bool vrStereoCameraFrameValid = false;
    gakumas::vr::UnityStereoRenderer unityStereoRenderer{};
    void SetFpHeadColorSkip(bool hidden, const char* reason) noexcept;
    void LogFpHeadActorShadowTag(void* pass) noexcept;
    thread_local std::uint32_t unityRenderPipelineDepth = 0;
    thread_local std::uint32_t unityRenderLoopDepth = 0;
    UnityResolve::Method* eyePassEventGetter = nullptr;

    struct CinemachineFovDiagnosticLayout {
        bool attempted = false;
        bool logged = false;
        bool ready = false;
        bool stateValueType = false;
        bool lensValueType = false;
        bool hasLookAtShape = false;
        std::int32_t stateLens = -1;
        std::int32_t stateReferenceLookAt = -1;
        std::int32_t stateRawPosition = -1;
        std::int32_t lensFieldOfView = -1;
        std::int32_t lensFocalLength = -1;
        std::int32_t lensSensorSize = -1;
        std::int32_t lensPhysical = -1;
        std::int32_t lensShift = -1;
        UnityResolve::Method* stateHasLookAt = nullptr;
    };

    CinemachineFovDiagnosticLayout cinemachineFovDiagnosticLayout{};

    struct PendingCinemachineObservation {
        bool sampled = false;
        bool stateValid = false;
        bool fovValid = false;
        std::uint64_t sample = 0U;
        gakumas::vr::UnitySourceCameraStateDiagnostic fov{};
    };

    struct ProFlareProjectionLayout {
        bool attempted = false;
        bool logged = false;
        bool ready = false;
        bool displayValueType = false;
        bool elementValueType = false;
        bool scheduleShape = false;
        bool updateHelperShape = false;
        bool scaleTelemetry = false;
        bool anamorphicTelemetry = false;
        std::int32_t batchCamera = -1;
        std::int32_t displayElementPointer = -1;
        std::int32_t elementScale = -1;
        std::int32_t elementSize = -1;
        std::int32_t elementAnamorphic = -1;
        std::string elementSizeType{};
        std::string elementAnamorphicType{};
        UnityResolve::Method* scheduleFlares = nullptr;
        UnityResolve::Method* updateElementJobData = nullptr;
    };

    struct ProFlareEyeScheduleContext {
        bool active = false;
        bool logBatch = false;
        std::size_t eye = 0U;
        std::uint64_t sample = 0U;
        std::uint32_t elements = 0U;
        float projectionScaleX = 1.0F;
        float projectionScaleY = 1.0F;
        float scaleX = 1.0F;
        float scaleY = 1.0F;
        float firstSizeX = 0.0F;
        float firstSizeY = 0.0F;
        float firstWrittenX = 0.0F;
        float firstWrittenY = 0.0F;
        float firstElementScale = 0.0F;
        float firstAnamorphicX = 0.0F;
        float firstAnamorphicY = 0.0F;
        float firstAnamorphicZ = 0.0F;
    };

    ProFlareProjectionLayout proFlareProjectionLayout{};
    thread_local ProFlareEyeScheduleContext proFlareEyeScheduleContext{};
    std::array<std::uint64_t, 2> proFlareEyeScheduleSamples{};
    std::array<float, 2> proFlareLastLoggedScaleX{};
    std::array<float, 2> proFlareLastLoggedScaleY{};
    std::array<bool, 2> proFlareMissingProjectionLogged{};

    struct EyeRenderPassTraceEntry {
        void* pass = nullptr;
        Il2cppUtils::Il2CppClassHead* klass = nullptr;
        int event = (std::numeric_limits<int>::min)();
    };

    constexpr std::size_t kMaxEyeRenderPassTraceEntries = 160U;
    thread_local std::array<EyeRenderPassTraceEntry,
                            kMaxEyeRenderPassTraceEntries>
        eyeRenderPassTraceEntries{};
    thread_local std::size_t eyeRenderPassTraceCount = 0U;
    thread_local bool eyeRenderPassTraceActive = false;
    thread_local bool eyeRenderPassTraceOverflow = false;
    thread_local void* eyeRenderPassTraceCamera = nullptr;
    thread_local std::size_t eyeRenderPassTraceEye = 0U;
    std::array<std::uint64_t, 2> eyeRenderPassLastSignature{};
    std::array<std::uint64_t, 2> eyeRenderPassFrameSerial{};

    struct EyeRenderPassExecutionContext {
        void* pass = nullptr;
        Il2cppUtils::Il2CppClassHead* klass = nullptr;
        int event = (std::numeric_limits<int>::min)();
    };

    enum class EyeFullscreenDrawApi : std::uint8_t {
        BlitterTriangle,
        BlitterQuad,
        CommandDrawProcedural,
        CommandBlit,
    };

    struct EyeFullscreenDrawKey {
        void* renderPass = nullptr;
        void* material = nullptr;
        int shaderPass = 0;
        int topology = -1;
        int vertexCount = -1;
        int instanceCount = -1;
        std::uint64_t stackSignature = 0U;
        std::size_t eye = 0U;
        EyeFullscreenDrawApi api = EyeFullscreenDrawApi::BlitterTriangle;
    };

    constexpr std::size_t kMaxEyeFullscreenDrawKeys = 1024U;
    thread_local EyeRenderPassExecutionContext eyeRenderPassExecution{};
    thread_local std::array<EyeFullscreenDrawKey, kMaxEyeFullscreenDrawKeys>
        eyeFullscreenDrawKeys{};
    thread_local std::size_t eyeFullscreenDrawKeyCount = 0U;
    thread_local std::array<void*, 2> eyeFullscreenEpochFirstPass{};
    std::atomic_bool eyeFullscreenDrawOverflowLogged = false;
    std::atomic_bool eyeCommandDrawProceduralHookReady = false;
    UnityResolve::Method* eyeMaterialGetShader = nullptr;

    struct ActorShadowStereoReuseState {
        UnityResolve::Field* featurePass = nullptr;
        UnityResolve::Field* shadowData = nullptr;
        UnityResolve::Field* shadowBias = nullptr;
        UnityResolve::Field* supportsShadow = nullptr;
        UnityResolve::Field* clearFlag = nullptr;
        UnityResolve::Field* actorShadowsKeyword = nullptr;
        UnityResolve::Field* startDistance2 = nullptr;
        UnityResolve::Field* endDistance2 = nullptr;
        UnityResolve::Field* fade = nullptr;
        UnityResolve::Field* strength = nullptr;
        std::int32_t dataFieldHeader = 0;
        UnityResolve::Method* setupReceiver = nullptr;
        UnityResolve::Method* setKeyword = nullptr;
        UnityResolve::Method* configureClear = nullptr;
        std::array<std::byte, 512> leftData{};
        std::size_t dataSize = 0;
        void* leftPass = nullptr;
        bool leftReady = false;
        bool leftSupportsShadow = false;
        std::uint64_t leftCaptures = 0;
        std::uint64_t rightReuses = 0;
        std::uint64_t rightClearPreserves = 0;
        float leftStartDistance2 = 0.0F;
        float leftEndDistance2 = 0.0F;
        float leftFade = 0.0F;
        float leftStrength = 0.0F;
        bool leftDistanceDataReady = false;
    };
    ActorShadowStereoReuseState actorShadowStereoReuse{};
    void ResetActorShadowLeftReuse() noexcept;

    struct EyeRenderObjectsPassFields {
        UnityResolve::Class* passClass = nullptr;
        UnityResolve::Field* renderQueueType = nullptr;
        UnityResolve::Field* filteringSettings = nullptr;
        UnityResolve::Field* cameraSettings = nullptr;
        UnityResolve::Field* profilerTag = nullptr;
        UnityResolve::Field* overrideMaterial = nullptr;
        UnityResolve::Field* overrideMaterialPassIndex = nullptr;
        UnityResolve::Field* overrideShader = nullptr;
        UnityResolve::Field* overrideShaderPassIndex = nullptr;
        UnityResolve::Class* filteringClass = nullptr;
        UnityResolve::Field* renderQueueRange = nullptr;
        UnityResolve::Field* layerMask = nullptr;
        UnityResolve::Field* renderingLayerMask = nullptr;
        UnityResolve::Field* excludeMotionVectorObjects = nullptr;
        UnityResolve::Class* renderQueueRangeClass = nullptr;
        UnityResolve::Field* lowerBound = nullptr;
        UnityResolve::Field* upperBound = nullptr;

        bool Ready() const noexcept {
            return passClass != nullptr && renderQueueType != nullptr &&
                filteringSettings != nullptr && cameraSettings != nullptr &&
                profilerTag != nullptr && overrideMaterial != nullptr &&
                overrideMaterialPassIndex != nullptr && overrideShader != nullptr &&
                overrideShaderPassIndex != nullptr && filteringClass != nullptr &&
                renderQueueRange != nullptr && layerMask != nullptr &&
                renderingLayerMask != nullptr &&
                excludeMotionVectorObjects != nullptr &&
                renderQueueRangeClass != nullptr && lowerBound != nullptr &&
                upperBound != nullptr;
        }
    };

    EyeRenderObjectsPassFields eyeRenderObjectsPassFields{};
    std::atomic<void*> eyeRenderObjectsLastInstance{nullptr};
    int eyeRenderObjectsLogs[3]{};
    int eyeRenderObjectsLastAction[3]{-1, -1, -1};
    int proFlareRenderLogs[3]{};

    bool IsVrUnityRuntimeEnabled() noexcept {
        return Config::vrRuntimeStartupEnabled && !Config::vrNativeOnly;
    }

    bool AreVrUnityCameraDiagnosticsEnabled() noexcept {
        return Config::vrDiagnosticsStartupEnabled && !Config::vrNativeOnly;
    }

    bool DoesVrOwnCamera() noexcept {
        return IsVrUnityRuntimeEnabled() && Config::vrHeadPoseEnabled;
    }

    bool IsVrHeadPoseBridgeEnabled() noexcept {
        return DoesVrOwnCamera() &&
            cinemachineStateReadsEnabled.load(std::memory_order_acquire) &&
            vrHeadPoseWritesEnabled.load(std::memory_order_acquire);
    }

    bool IsLocalifyFreeCameraEnabled() noexcept {
        // In combined mode, VR owns the camera transform. Localify translation
        // and its non-camera features remain enabled, but its legacy free-camera
        // writers stay inert so two controllers never fight over one Transform.
        return Config::enableFreeCamera && !Config::vrRuntimeStartupEnabled;
    }

    void InvalidateVrStereoCameraFrame() noexcept {
        std::lock_guard lock(vrStereoCameraFrameMutex);
        vrStereoCameraFrame = {};
        vrStereoCameraFrameValid = false;
    }

    bool IsSelectedVrMainCamera(void* camera) noexcept {
        if (camera == nullptr) {
            return false;
        }
        std::lock_guard lock(vrCameraStateMutex);
        return camera == vrSourceCamera;
    }

    gakumas::vr::UnityStereoCameraFrame ReadVrStereoCameraFrame() noexcept {
        std::lock_guard lock(vrStereoCameraFrameMutex);
        if (vrStereoCameraFrameValid) {
            return vrStereoCameraFrame;
        }
        // Grip-only runs still write the head pose into the source camera, so
        // the actor-shadow anchor needs the authored shot even when the eye
        // ladder is off. Everything else stays cleared.
        gakumas::vr::UnityStereoCameraFrame frame{};
        frame.sourcePose = vrStereoCameraFrame.sourcePose;
        frame.sourcePoseValid = vrStereoCameraFrame.sourcePoseValid;
        frame.cinematicPose = vrStereoCameraFrame.cinematicPose;
        frame.cinematicPoseValid = vrStereoCameraFrame.cinematicPoseValid;
        return frame;
    }

    bool IsUnityRenderPipelineIdle() noexcept {
        return unityRenderPipelineGuardReady.load(std::memory_order_acquire) &&
            unityRenderLoopDepth == 0U;
    }

    void* InvokeCinemachineOutputCameraRaw(
        void* function,
        void* brain,
        void* methodInfo) noexcept {
        using GetOutputCamera = void* (*)(void*, void*);
        __try {
            return reinterpret_cast<GetOutputCamera>(function)(brain, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    void* ReadCinemachineOutputCamera(void* brain) noexcept {
        if (brain == nullptr) {
            return nullptr;
        }
        static UnityResolve::Method* outputCameraMethod = []() {
            auto* klass = Il2cppUtils::GetClass(
                "Cinemachine.dll", "Cinemachine", "CinemachineBrain");
            if (klass == nullptr) {
                return static_cast<UnityResolve::Method*>(nullptr);
            }
            UnityResolve::Method* exact = nullptr;
            for (auto* candidate : klass->methods) {
                if (candidate == nullptr || candidate->name != "get_OutputCamera" ||
                    candidate->static_function || !candidate->args.empty() ||
                    candidate->return_type == nullptr ||
                    candidate->return_type->name != "UnityEngine.Camera" ||
                    candidate->function == nullptr || candidate->address == nullptr) {
                    continue;
                }
                if (exact != nullptr) {
                    return static_cast<UnityResolve::Method*>(nullptr);
                }
                exact = candidate;
            }
            return exact;
        }();
        if (outputCameraMethod == nullptr) {
            return nullptr;
        }
        return InvokeCinemachineOutputCameraRaw(
            outputCameraMethod->function,
            brain,
            outputCameraMethod->address);
    }

    bool WriteUnityCameraDiagnosticEvent(std::string_view message) noexcept {
        if (AreVrUnityCameraDiagnosticsEnabled()) {
            return gakumas::vr::WriteVrLog(message);
        }
        return false;
    }

    void EnsureUnityCameraDiagnosticMarker() {
        if (!unityCameraDiagnosticHooksConfigured.load(std::memory_order_acquire) ||
            unityCameraDiagnosticMarkerLogged.load(std::memory_order_acquire)) {
            return;
        }
        std::ostringstream stream;
        stream << "[VR][camera] UNITY_CAMERA_DIAGNOSTICS_ENABLED readOnly="
               << !Config::vrHeadPoseEnabled
               << " headPoseBridge=" << Config::vrHeadPoseEnabled
               << " cameraMainHook="
               << unityCameraMainHookReady.load(std::memory_order_relaxed)
               << " cinemachineHook="
               << unityCinemachineHookReady.load(std::memory_order_relaxed)
               << " renderHook="
               << unityCameraRenderHookReady.load(std::memory_order_relaxed)
               << " pipelineGuard="
               << unityRenderPipelineGuardReady.load(std::memory_order_relaxed)
               << " transformHooks=0 fovHooks=0";
        if (WriteUnityCameraDiagnosticEvent(stream.str())) {
            unityCameraDiagnosticMarkerLogged.store(true, std::memory_order_release);
        }
    }

    void ReportVrUnityHookException() noexcept {
        if (vrUnityHookExceptionLogged.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        static_cast<void>(gakumas::vr::WriteVrLog(
            "[VR][runtime] UNITY_HOOK_EXCEPTION; current callback failed"));
    }

    template <typename Return>
    bool InvokeUnityGetterRaw(
        UnityResolve::Method* method,
        void* self,
        Return* result) noexcept {
        if (method == nullptr || method->function == nullptr || self == nullptr ||
            result == nullptr) {
            return false;
        }
        using Getter = Return (*)(void*, void*);
        __try {
            *result = reinterpret_cast<Getter>(method->function)(self, method->address);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    template <typename Return>
    std::optional<Return> InvokeUnityGetter(
        UnityResolve::Method* method,
        void* self) noexcept {
        Return result{};
        if (!InvokeUnityGetterRaw(method, self, &result)) {
            return std::nullopt;
        }
        return result;
    }

    bool TryCopyUnityString(
        const UnityResolve::UnityType::String* value,
        wchar_t* output,
        std::size_t capacity,
        int* copiedLength) noexcept {
        if (value == nullptr || output == nullptr || capacity < 2U ||
            copiedLength == nullptr) {
            return false;
        }
        __try {
            const int length = value->length;
            if (length < 0 || length > 4096) {
                return false;
            }
            const int copyLength = length < static_cast<int>(capacity - 1U)
                ? length
                : static_cast<int>(capacity - 1U);
            for (int index = 0; index < copyLength; ++index) {
                output[index] = value->start_char[index];
            }
            output[copyLength] = L'\0';
            *copiedLength = copyLength;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::string ReadUnityStringValue(
        const UnityResolve::UnityType::String* value) {
        if (value == nullptr) {
            return {};
        }
        std::array<wchar_t, 97> wideName{};
        int wideLength = 0;
        if (!TryCopyUnityString(value, wideName.data(), wideName.size(), &wideLength) ||
            wideLength == 0) {
            return {};
        }
        const int utf8Length = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            wideName.data(),
            wideLength,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (utf8Length <= 0) {
            return {};
        }
        std::string name(static_cast<std::size_t>(utf8Length), '\0');
        if (WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                wideName.data(),
                wideLength,
                name.data(),
                utf8Length,
                nullptr,
                nullptr) != utf8Length) {
            return {};
        }
        for (char& character : name) {
            if (static_cast<unsigned char>(character) < 0x20U) {
                character = ' ';
            }
        }
        if (name.size() > 96U) {
            name.resize(96U);
        }
        return name;
    }

    std::string ReadUnityObjectName(void* object) {
        static auto* method = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll",
            "UnityEngine",
            "Object",
            "get_name");
        const auto value = InvokeUnityGetter<UnityResolve::UnityType::String*>(method, object);
        return value.has_value() ? ReadUnityStringValue(*value) : std::string{};
    }

    template <typename Value>
    std::optional<Value> ReadUnityInstanceField(
        void* instance,
        const UnityResolve::Field* field) noexcept {
        if (instance == nullptr || field == nullptr || field->static_field ||
            field->offset < 0) {
            return std::nullopt;
        }
        Value value{};
        __try {
            std::memcpy(
                &value,
                static_cast<const std::byte*>(instance) + field->offset,
                sizeof(value));
            return value;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return std::nullopt;
        }
    }

    template <typename Value>
    std::optional<Value> ReadUnityValueField(
        void* value,
        const UnityResolve::Field* field) noexcept {
        // UnityResolve reports value-type fields with the 0x10 boxed-object
        // header included. Embedded structs have no header, so nested reads
        // must use the unboxed offset.
        constexpr std::int32_t boxedHeader =
            static_cast<std::int32_t>(sizeof(void*) * 2U);
        if (value == nullptr || field == nullptr || field->static_field ||
            field->offset < boxedHeader) {
            return std::nullopt;
        }
        Value result{};
        __try {
            std::memcpy(
                &result,
                static_cast<const std::byte*>(value) +
                    (field->offset - boxedHeader),
                sizeof(result));
            return result;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return std::nullopt;
        }
    }

    bool TryLogUnityCameraDescriptor(
        void* camera,
        UnityCameraDiagnosticRecord& record) {
        static auto* getDepth = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_depth");
        static auto* getFov = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_fieldOfView");
        static auto* getNear = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_nearClipPlane");
        static auto* getFar = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_farClipPlane");
        static auto* getOrthographic = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_orthographic");
        static auto* getCullingMask = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_cullingMask");
        static auto* getTargetTexture = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_targetTexture");
        static auto* getPixelWidth = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_pixelWidth");
        static auto* getPixelHeight = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_pixelHeight");

        const auto depth = InvokeUnityGetter<float>(getDepth, camera);
        const auto fov = InvokeUnityGetter<float>(getFov, camera);
        const auto nearClip = InvokeUnityGetter<float>(getNear, camera);
        const auto farClip = InvokeUnityGetter<float>(getFar, camera);
        const auto orthographic = InvokeUnityGetter<bool>(getOrthographic, camera);
        const auto cullingMask = InvokeUnityGetter<int>(getCullingMask, camera);
        const auto targetTexture = InvokeUnityGetter<void*>(getTargetTexture, camera);
        const auto pixelWidth = InvokeUnityGetter<int>(getPixelWidth, camera);
        const auto pixelHeight = InvokeUnityGetter<int>(getPixelHeight, camera);

        std::ostringstream stream;
        stream << "[VR][camera] CAMERA_DISCOVERED id=0x" << std::hex
               << reinterpret_cast<std::uintptr_t>(camera) << std::dec
               << " name=\"" << record.name << "\""
               << " main=" << record.mainCamera;
        stream << std::fixed << std::setprecision(3);
        if (depth.has_value()) {
            stream << " depth=" << *depth;
        }
        if (fov.has_value()) {
            stream << " fov=" << *fov;
        }
        if (nearClip.has_value()) {
            stream << " near=" << *nearClip;
        }
        if (farClip.has_value()) {
            stream << " far=" << *farClip;
        }
        if (orthographic.has_value()) {
            stream << " orthographic=" << *orthographic;
        }
        if (cullingMask.has_value()) {
            stream << " cullingMask=0x" << std::hex
                   << static_cast<std::uint32_t>(*cullingMask) << std::dec;
        }
        if (targetTexture.has_value()) {
            stream << " targetTexture=0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(*targetTexture) << std::dec;
        }
        if (pixelWidth.has_value() && pixelHeight.has_value()) {
            stream << " pixels=" << *pixelWidth << 'x' << *pixelHeight;
        }
        return WriteUnityCameraDiagnosticEvent(stream.str());
    }

    UnityCameraDiagnosticRecord& EnsureUnityCameraDiagnosticRecord(void* camera) {
        auto [entry, inserted] = unityCameraDiagnosticRecords.try_emplace(camera);
        auto& record = entry->second;
        if (inserted) {
            record.name = ReadUnityObjectName(camera);
            record.mainCamera = camera == vrSourceCamera;
        }
        if (!record.descriptorLogged &&
            unityCameraDiagnosticMarkerLogged.load(std::memory_order_acquire) &&
            TryLogUnityCameraDescriptor(camera, record)) {
            record.descriptorLogged = true;
        }
        return record;
    }

    void EnsureUnityMainSelectionLog(
        void* camera,
        UnityCameraDiagnosticRecord& record) {
        if (!record.mainCamera || record.mainSelectionLogged ||
            !unityCameraDiagnosticMarkerLogged.load(std::memory_order_acquire)) {
            return;
        }
        std::ostringstream stream;
        stream << "[VR][camera] MAIN_CAMERA_SELECTED id=0x" << std::hex
               << reinterpret_cast<std::uintptr_t>(camera) << std::dec
               << " name=\"" << record.name << "\"";
        if (WriteUnityCameraDiagnosticEvent(stream.str())) {
            record.mainSelectionLogged = true;
        }
    }

    bool TryReadCinemachineState(
        const void* state,
        UnityResolve::UnityType::Vector3* position,
        UnityResolve::UnityType::Quaternion* rotation,
        float* fov) noexcept {
        if (state == nullptr || position == nullptr || rotation == nullptr ||
            fov == nullptr) {
            return false;
        }
        const auto* bytes = static_cast<const unsigned char*>(state);
        __try {
            std::memcpy(position, bytes + 0x48, sizeof(*position));
            std::memcpy(rotation, bytes + 0x54, sizeof(*rotation));
            std::memcpy(fov, bytes + 0x00, sizeof(*fov));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool FieldTypeContains(
        const UnityResolve::Field* field,
        std::string_view expected) noexcept {
        return field != nullptr && field->type != nullptr &&
            field->type->name.find(expected) != std::string::npos;
    }

    void EnsureProFlareProjectionLayout() noexcept {
        auto& layout = proFlareProjectionLayout;
        if (!layout.attempted) {
            layout.attempted = true;
            constexpr std::int32_t kBoxedHeader = 0x10;
            auto* assembly = UnityResolve::Get("ProFlare.Runtime.dll");
            auto* batchClass = assembly != nullptr
                ? assembly->Get("ProFlareBatchForSRPData", "") : nullptr;
            if (batchClass == nullptr && assembly != nullptr) {
                batchClass = assembly->Get("ProFlareBatchForSRPData");
            }
            auto* elementClass = assembly != nullptr
                ? assembly->Get("ProFlareUpdateElementData", "") : nullptr;
            if (elementClass == nullptr && assembly != nullptr) {
                elementClass = assembly->Get("ProFlareUpdateElementData");
            }
            auto* displayClass = assembly != nullptr
                ? assembly->Get("<>c__DisplayClass48_0", "") : nullptr;
            if (displayClass == nullptr && assembly != nullptr) {
                displayClass = assembly->Get("<>c__DisplayClass48_0");
            }

            const auto* batchCamera = batchClass != nullptr
                ? batchClass->Get<UnityResolve::Field>("camera") : nullptr;
            const auto* displayElement = displayClass != nullptr
                ? displayClass->Get<UnityResolve::Field>(
                      "pFlareUpdateElement") : nullptr;
            const auto* elementScale = elementClass != nullptr
                ? elementClass->Get<UnityResolve::Field>("Scale") : nullptr;
            const auto* elementSize = elementClass != nullptr
                ? elementClass->Get<UnityResolve::Field>("Size") : nullptr;
            const auto* elementAnamorphic = elementClass != nullptr
                ? elementClass->Get<UnityResolve::Field>("Anamorphic") : nullptr;

            layout.displayValueType = displayClass != nullptr &&
                displayClass->address != nullptr &&
                UnityResolve::Invoke<bool>(
                    "il2cpp_class_is_valuetype", displayClass->address);
            layout.elementValueType = elementClass != nullptr &&
                elementClass->address != nullptr &&
                UnityResolve::Invoke<bool>(
                    "il2cpp_class_is_valuetype", elementClass->address);
            layout.batchCamera = batchCamera != nullptr
                ? batchCamera->offset : -1;
            const auto unboxedOffset = [kBoxedHeader](
                                           const UnityResolve::Field* field) {
                return field != nullptr && field->offset >= kBoxedHeader
                    ? field->offset - kBoxedHeader : -1;
            };
            layout.displayElementPointer = unboxedOffset(displayElement);
            layout.elementScale = unboxedOffset(elementScale);
            layout.elementSize = unboxedOffset(elementSize);
            layout.elementAnamorphic = unboxedOffset(elementAnamorphic);
            layout.elementSizeType =
                elementSize != nullptr && elementSize->type != nullptr
                    ? elementSize->type->name : "?";
            layout.elementAnamorphicType =
                elementAnamorphic != nullptr &&
                        elementAnamorphic->type != nullptr
                    ? elementAnamorphic->type->name : "?";

            if (batchClass != nullptr) {
                for (auto* method : batchClass->methods) {
                    if (method == nullptr) {
                        continue;
                    }
                    if (method->name == "ScheduleFlares") {
                        if (layout.scheduleFlares != nullptr) {
                            layout.scheduleFlares = nullptr;
                            break;
                        }
                        layout.scheduleFlares = method;
                    }
                }
                for (auto* method : batchClass->methods) {
                    if (method != nullptr && method->name ==
                            "<ScheduleFlares>g__UpdateElementJobData|48_0") {
                        if (layout.updateElementJobData != nullptr) {
                            layout.updateElementJobData = nullptr;
                            break;
                        }
                        layout.updateElementJobData = method;
                    }
                }
            }
            const auto argContains = [](const UnityResolve::Method* method,
                                        std::size_t index,
                                        std::string_view expected) {
                return method != nullptr && index < method->args.size() &&
                    method->args[index] != nullptr &&
                    method->args[index]->pType != nullptr &&
                    method->args[index]->pType->name.find(expected) !=
                        std::string::npos;
            };
            layout.scheduleShape = layout.scheduleFlares != nullptr &&
                !layout.scheduleFlares->static_function &&
                layout.scheduleFlares->return_type != nullptr &&
                layout.scheduleFlares->return_type->name == "System.Void" &&
                layout.scheduleFlares->args.size() == 1U &&
                argContains(layout.scheduleFlares, 0U, "System.Boolean") &&
                layout.scheduleFlares->function != nullptr &&
                layout.scheduleFlares->address != nullptr;
            layout.updateHelperShape =
                layout.updateElementJobData != nullptr &&
                layout.updateElementJobData->static_function &&
                layout.updateElementJobData->return_type != nullptr &&
                layout.updateElementJobData->return_type->name == "System.Void" &&
                layout.updateElementJobData->args.size() == 8U &&
                argContains(layout.updateElementJobData, 0U, "UnityEngine.Color") &&
                argContains(layout.updateElementJobData, 1U, "System.Single") &&
                argContains(layout.updateElementJobData, 2U, "System.Single") &&
                argContains(layout.updateElementJobData, 3U, "System.Single") &&
                argContains(layout.updateElementJobData, 4U,
                            "<>c__DisplayClass48_0") &&
                argContains(layout.updateElementJobData, 5U,
                            "<>c__DisplayClass48_1") &&
                argContains(layout.updateElementJobData, 6U,
                            "<>c__DisplayClass48_2") &&
                argContains(layout.updateElementJobData, 7U,
                            "<>c__DisplayClass48_3") &&
                layout.updateElementJobData->function != nullptr &&
                layout.updateElementJobData->address != nullptr;

            layout.scaleTelemetry =
                FieldTypeContains(elementScale, "System.Single") &&
                layout.elementScale >= 0 && layout.elementScale <= 0x200;
            layout.anamorphicTelemetry =
                FieldTypeContains(elementAnamorphic, "UnityEngine.Vector3") &&
                layout.elementAnamorphic >= 0 &&
                layout.elementAnamorphic <= 0x200;
            // Functional readiness contains only fields used by the write.
            // Scale/Anamorphic are optional log telemetry; `.173` incorrectly
            // made Anamorphic:Vector2 mandatory and disabled the whole hook.
            layout.ready = layout.displayValueType &&
                layout.elementValueType && layout.scheduleShape &&
                layout.updateHelperShape &&
                FieldTypeContains(batchCamera, "UnityEngine.Camera") &&
                FieldTypeContains(displayElement,
                                  "ProFlareUpdateElementData") &&
                FieldTypeContains(elementSize, "UnityEngine.Vector2") &&
                layout.batchCamera > 0 &&
                layout.displayElementPointer >= 0 &&
                layout.displayElementPointer <= 0x100 &&
                layout.elementSize >= 0 && layout.elementSize <= 0x200;
        }
        if (!layout.logged) {
            std::ostringstream stream;
            stream << "[VR][fov] PRO_FLARE_BATCH_LAYOUT ready="
                   << (layout.ready ? 1 : 0)
                   << " displayValueType=" << (layout.displayValueType ? 1 : 0)
                   << " elementValueType=" << (layout.elementValueType ? 1 : 0)
                   << " scheduleShape=" << (layout.scheduleShape ? 1 : 0)
                   << " helperShape=" << (layout.updateHelperShape ? 1 : 0)
                   << " scaleTelemetry=" << (layout.scaleTelemetry ? 1 : 0)
                   << " anamorphicTelemetry="
                   << (layout.anamorphicTelemetry ? 1 : 0)
                   << " camera=" << layout.batchCamera
                   << " displayElement=" << layout.displayElementPointer
                   << " scale=" << layout.elementScale
                   << " size=" << layout.elementSize
                   << " anamorphic=" << layout.elementAnamorphic
                   << " sizeType=" << layout.elementSizeType
                   << " anamorphicType=" << layout.elementAnamorphicType
                   << " boxedHeader=16";
            layout.logged = gakumas::vr::WriteVrLog(stream.str());
        }
    }

    bool TryReadProFlareBatchCamera(void* batch, void** camera) noexcept {
        if (batch == nullptr || camera == nullptr ||
            !proFlareProjectionLayout.ready ||
            proFlareProjectionLayout.batchCamera <= 0) {
            return false;
        }
        __try {
            std::memcpy(
                camera,
                static_cast<const unsigned char*>(batch) +
                    proFlareProjectionLayout.batchCamera,
                sizeof(*camera));
            return *camera != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool TryReadCurrentProFlareElement(
        void* display0,
        void** element) noexcept {
        const auto& layout = proFlareProjectionLayout;
        if (display0 == nullptr || element == nullptr || !layout.ready) {
            return false;
        }
        __try {
            std::memcpy(
                element,
                static_cast<const unsigned char*>(display0) +
                    layout.displayElementPointer,
                sizeof(*element));
            return *element != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool ScaleCurrentProFlareElementForEye(void* element) noexcept {
        auto& context = proFlareEyeScheduleContext;
        const auto& layout = proFlareProjectionLayout;
        if (element == nullptr || !context.active || !layout.ready ||
            !std::isfinite(context.scaleX) ||
            !std::isfinite(context.scaleY) || context.scaleX <= 0.0F ||
            context.scaleY <= 0.0F || context.scaleX > 64.0F ||
            context.scaleY > 64.0F) {
            return false;
        }
        UnityResolve::UnityType::Vector2 authored{};
        float elementScale = 0.0F;
        UnityResolve::UnityType::Vector3 anamorphic{};
        __try {
            auto* elementBytes = static_cast<unsigned char*>(element);
            std::memcpy(
                &authored, elementBytes + layout.elementSize,
                sizeof(authored));
            if (layout.scaleTelemetry) {
                std::memcpy(
                    &elementScale, elementBytes + layout.elementScale,
                    sizeof(elementScale));
            }
            if (layout.anamorphicTelemetry) {
                std::memcpy(
                    &anamorphic, elementBytes + layout.elementAnamorphic,
                    sizeof(anamorphic));
            }
            if (!std::isfinite(authored.x) || !std::isfinite(authored.y) ||
                std::abs(authored.x) > 1000000.0F ||
                std::abs(authored.y) > 1000000.0F) {
                return false;
            }
            UnityResolve::UnityType::Vector2 written{};
            if (!gakumas::vr::camera::TryScaleViewportSize(
                    authored.x, authored.y, context.scaleX, context.scaleY,
                    written.x, written.y)) {
                return false;
            }
            std::memcpy(
                elementBytes + layout.elementSize, &written,
                sizeof(written));
            if (context.elements == 0U) {
                context.firstSizeX = authored.x;
                context.firstSizeY = authored.y;
                context.firstWrittenX = written.x;
                context.firstWrittenY = written.y;
                context.firstElementScale = elementScale;
                context.firstAnamorphicX = anamorphic.x;
                context.firstAnamorphicY = anamorphic.y;
                context.firstAnamorphicZ = anamorphic.z;
            }
            ++context.elements;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void EnsureCinemachineFovDiagnosticLayout() noexcept {
        auto& layout = cinemachineFovDiagnosticLayout;
        if (!layout.attempted) {
            layout.attempted = true;
            constexpr std::int32_t kBoxedHeader = 0x10;
            auto* stateClass = Il2cppUtils::GetClass(
                "Cinemachine.dll", "Cinemachine", "CameraState");
            auto* lensClass = Il2cppUtils::GetClass(
                "Cinemachine.dll", "Cinemachine", "LensSettings");
            layout.stateValueType = stateClass != nullptr &&
                UnityResolve::Invoke<bool>(
                    "il2cpp_class_is_valuetype", stateClass->address);
            layout.lensValueType = lensClass != nullptr &&
                UnityResolve::Invoke<bool>(
                    "il2cpp_class_is_valuetype", lensClass->address);
            const auto* stateLens = stateClass != nullptr
                ? stateClass->Get<UnityResolve::Field>("<Lens>k__BackingField")
                : nullptr;
            const auto* stateReferenceLookAt = stateClass != nullptr
                ? stateClass->Get<UnityResolve::Field>(
                      "<ReferenceLookAt>k__BackingField")
                : nullptr;
            const auto* stateRawPosition = stateClass != nullptr
                ? stateClass->Get<UnityResolve::Field>(
                      "<RawPosition>k__BackingField")
                : nullptr;
            const auto* lensFieldOfView = lensClass != nullptr
                ? lensClass->Get<UnityResolve::Field>("FieldOfView")
                : nullptr;
            const auto* lensFocalLength = lensClass != nullptr
                ? lensClass->Get<UnityResolve::Field>("FocalLength")
                : nullptr;
            const auto* lensSensorSize = lensClass != nullptr
                ? lensClass->Get<UnityResolve::Field>(
                      "<SensorSize>k__BackingField")
                : nullptr;
            const auto* lensPhysical = lensClass != nullptr
                ? lensClass->Get<UnityResolve::Field>(
                      "<IsPhysicalCamera>k__BackingField")
                : nullptr;
            const auto* lensShift = lensClass != nullptr
                ? lensClass->Get<UnityResolve::Field>("LensShift")
                : nullptr;
            layout.stateHasLookAt = stateClass != nullptr
                ? stateClass->Get<UnityResolve::Method>("get_HasLookAt")
                : nullptr;

            const auto unboxedOffset = [kBoxedHeader](
                                           const UnityResolve::Field* field) {
                return field != nullptr && field->offset >= kBoxedHeader
                    ? field->offset - kBoxedHeader : -1;
            };
            layout.stateLens = unboxedOffset(stateLens);
            layout.stateReferenceLookAt = stateReferenceLookAt != nullptr
                ? unboxedOffset(stateReferenceLookAt) : -1;
            layout.stateRawPosition = stateRawPosition != nullptr
                ? unboxedOffset(stateRawPosition) : -1;
            layout.lensFieldOfView = lensFieldOfView != nullptr
                ? unboxedOffset(lensFieldOfView) : -1;
            layout.lensFocalLength = lensFocalLength != nullptr
                ? unboxedOffset(lensFocalLength) : -1;
            layout.lensSensorSize = lensSensorSize != nullptr
                ? unboxedOffset(lensSensorSize) : -1;
            layout.lensPhysical = lensPhysical != nullptr
                ? unboxedOffset(lensPhysical) : -1;
            layout.lensShift = unboxedOffset(lensShift);

            layout.hasLookAtShape = layout.stateHasLookAt != nullptr &&
                layout.stateHasLookAt->function != nullptr &&
                layout.stateHasLookAt->address != nullptr &&
                !layout.stateHasLookAt->static_function &&
                layout.stateHasLookAt->args.empty() &&
                layout.stateHasLookAt->return_type != nullptr &&
                layout.stateHasLookAt->return_type->name == "System.Boolean";
            // Old dump offsets are accepted only after the installed build's
            // live class table reports the same fields, types, and offsets.
            // UnityResolve reports value-type fields against a boxed object;
            // the hook receives the unboxed struct, so strip the proven 0x10
            // object header before comparing or reading.
            layout.ready =
                layout.stateValueType && layout.lensValueType &&
                FieldTypeContains(stateLens, "Cinemachine.LensSettings") &&
                FieldTypeContains(stateReferenceLookAt, "UnityEngine.Vector3") &&
                FieldTypeContains(stateRawPosition, "UnityEngine.Vector3") &&
                FieldTypeContains(lensFieldOfView, "System.Single") &&
                FieldTypeContains(lensFocalLength, "System.Single") &&
                FieldTypeContains(lensSensorSize, "UnityEngine.Vector2") &&
                FieldTypeContains(lensPhysical, "System.Boolean") &&
                FieldTypeContains(lensShift, "UnityEngine.Vector2") &&
                layout.stateLens == 0x00 &&
                layout.stateReferenceLookAt == 0x3C &&
                layout.stateRawPosition == 0x48 &&
                layout.lensFieldOfView == 0x00 &&
                layout.lensFocalLength == 0x14 &&
                layout.lensSensorSize == 0x1C &&
                layout.lensPhysical == 0x24 &&
                layout.lensShift == 0x28 && layout.hasLookAtShape;
        }
        if (!layout.logged) {
            layout.logged = WriteUnityCameraDiagnosticEvent(
                std::string("[VR][fov] CAMERA_STATE_LAYOUT ready=") +
                (layout.ready ? "1" : "0") +
                " stateValueType=" +
                    (layout.stateValueType ? "1" : "0") +
                " lensValueType=" +
                    (layout.lensValueType ? "1" : "0") +
                " stateLens=" + std::to_string(layout.stateLens) +
                " referenceLookAt=" +
                    std::to_string(layout.stateReferenceLookAt) +
                " rawPosition=" + std::to_string(layout.stateRawPosition) +
                " lensFov=" + std::to_string(layout.lensFieldOfView) +
                " focal=" + std::to_string(layout.lensFocalLength) +
                " sensor=" + std::to_string(layout.lensSensorSize) +
                " physical=" + std::to_string(layout.lensPhysical) +
                " lensShift=" + std::to_string(layout.lensShift) +
                " boxedHeader=16" +
                " hasLookAtMethod=" +
                    (layout.stateHasLookAt != nullptr ? "1" : "0") +
                " hasLookAtShape=" +
                    (layout.hasLookAtShape ? "1" : "0"));
        }
    }

    bool TryReadCinemachineFovDiagnostic(
        const void* state,
        std::uint64_t sample,
        gakumas::vr::UnitySourceCameraStateDiagnostic* result) noexcept {
        if (state == nullptr || result == nullptr) {
            return false;
        }
        EnsureCinemachineFovDiagnosticLayout();
        const auto& layout = cinemachineFovDiagnosticLayout;
        if (!layout.ready) {
            return false;
        }
        using Vector2 = UnityResolve::UnityType::Vector2;
        using Vector3 = UnityResolve::UnityType::Vector3;
        const auto* bytes = static_cast<const unsigned char*>(state);
        const auto* lens = bytes + layout.stateLens;
        Vector3 rawPosition{};
        Vector3 referenceLookAt{};
        Vector2 sensorSize{};
        Vector2 lensShift{};
        float fov = 0.0F;
        float focal = 0.0F;
        bool physical = false;
        bool hasLookAt = false;
        __try {
            std::memcpy(&rawPosition, bytes + layout.stateRawPosition,
                        sizeof(rawPosition));
            std::memcpy(&referenceLookAt,
                        bytes + layout.stateReferenceLookAt,
                        sizeof(referenceLookAt));
            std::memcpy(&fov, lens + layout.lensFieldOfView, sizeof(fov));
            std::memcpy(&focal, lens + layout.lensFocalLength, sizeof(focal));
            std::memcpy(&sensorSize, lens + layout.lensSensorSize,
                        sizeof(sensorSize));
            std::memcpy(&physical, lens + layout.lensPhysical,
                        sizeof(physical));
            std::memcpy(&lensShift, lens + layout.lensShift,
                        sizeof(lensShift));
            using GetHasLookAt = bool (*)(const void*, void*);
            hasLookAt = reinterpret_cast<GetHasLookAt>(
                layout.stateHasLookAt->function)(
                    state, layout.stateHasLookAt->address);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        const bool rawFinite = std::isfinite(rawPosition.x) &&
            std::isfinite(rawPosition.y) && std::isfinite(rawPosition.z) &&
            std::abs(rawPosition.x) < 1000000.0F &&
            std::abs(rawPosition.y) < 1000000.0F &&
            std::abs(rawPosition.z) < 1000000.0F;
        const bool lensFinite = std::isfinite(fov) && fov > 0.1F &&
            fov < 179.9F && std::isfinite(focal) &&
            std::isfinite(sensorSize.x) && std::isfinite(sensorSize.y) &&
            std::isfinite(lensShift.x) && std::isfinite(lensShift.y);
        const bool lookAtFinite = !hasLookAt ||
            (std::isfinite(referenceLookAt.x) &&
             std::isfinite(referenceLookAt.y) &&
             std::isfinite(referenceLookAt.z) &&
             std::abs(referenceLookAt.x) < 1000000.0F &&
             std::abs(referenceLookAt.y) < 1000000.0F &&
             std::abs(referenceLookAt.z) < 1000000.0F);
        if (!rawFinite || !lensFinite || !lookAtFinite) {
            return false;
        }
        float lookAtDistance = 0.0F;
        if (hasLookAt) {
            const float dx = referenceLookAt.x - rawPosition.x;
            const float dy = referenceLookAt.y - rawPosition.y;
            const float dz = referenceLookAt.z - rawPosition.z;
            lookAtDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (!std::isfinite(lookAtDistance) || lookAtDistance <= 0.0F) {
                return false;
            }
        }
        *result = {};
        result->sample = sample;
        result->valid = true;
        result->hasLookAt = hasLookAt;
        result->physical = physical;
        result->fieldOfViewDegrees = fov;
        result->focalLengthMillimeters = focal;
        result->sensorWidthMillimeters = sensorSize.x;
        result->sensorHeightMillimeters = sensorSize.y;
        result->lensShiftX = lensShift.x;
        result->lensShiftY = lensShift.y;
        result->rawPositionX = rawPosition.x;
        result->rawPositionY = rawPosition.y;
        result->rawPositionZ = rawPosition.z;
        result->referenceLookAtX = referenceLookAt.x;
        result->referenceLookAtY = referenceLookAt.y;
        result->referenceLookAtZ = referenceLookAt.z;
        result->lookAtDistance = lookAtDistance;
        return true;
    }

    bool IsValidCinemachinePose(
        const UnityResolve::UnityType::Vector3& position,
        const UnityResolve::UnityType::Quaternion& rotation,
        float fov) noexcept {
        const float rotationNormSquared =
            rotation.x * rotation.x + rotation.y * rotation.y +
            rotation.z * rotation.z + rotation.w * rotation.w;
        return std::isfinite(position.x) && std::isfinite(position.y) &&
            std::isfinite(position.z) && std::abs(position.x) < 1000000.0F &&
            std::abs(position.y) < 1000000.0F && std::abs(position.z) < 1000000.0F &&
            std::isfinite(rotation.x) && std::isfinite(rotation.y) &&
            std::isfinite(rotation.z) && std::isfinite(rotation.w) &&
            std::isfinite(rotationNormSquared) && rotationNormSquared > 0.25F &&
            rotationNormSquared < 2.25F && std::isfinite(fov) &&
            fov > 0.1F && fov < 179.9F;
    }

    bool TryWriteCinemachineRawPose(
        void* state,
        const UnityResolve::UnityType::Vector3& position,
        const UnityResolve::UnityType::Quaternion& rotation) noexcept {
        if (state == nullptr) {
            return false;
        }
        auto* bytes = static_cast<unsigned char*>(state);
        __try {
            std::memcpy(bytes + 0x48, &position, sizeof(position));
            std::memcpy(bytes + 0x54, &rotation, sizeof(rotation));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void ReportVrHeadPoseWriteFailure() noexcept {
        if (vrHeadPoseWriteFailureLogged.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        WriteUnityCameraDiagnosticEvent(
            "[VR][camera] HEAD_POSE_WRITE_FAILED; camera writes disabled");
    }

    // ---- VR free camera (upstream FREE/FOLLOW/FIRST_PERSON migration) ----
    // The rig replaces Cinemachine's requested pose as the base the headset
    // delta is composed onto. Everything here runs on the Unity main thread
    // (CinemachineBrain.PushStateToUnityCamera and
    // CampusActorController.LateUpdate), so the anchor globals need no locks;
    // only the controller input crosses threads via VrCameraInputMailbox.
    gakumas::vr::camera::VrFreeCameraRig vrFreeCameraRig;
    UnityResolve::UnityType::Vector3 vrCameraAnchorPosition{};
    UnityResolve::UnityType::Vector3 vrCameraAnchorForward{};
    UnityResolve::UnityType::Quaternion vrCameraAnchorRotation{};
    std::int64_t vrCameraAnchorTimeNanoseconds = 0;
    bool vrFreeCameraLastComposedValid = false;
    gakumas::vr::pose::Pose vrFreeCameraLastComposed{};

    bool IsVrCameraAnchorFresh(std::int64_t nowNanoseconds) noexcept {
        constexpr std::int64_t kAnchorStaleNanoseconds = 500'000'000LL;
        return vrCameraAnchorTimeNanoseconds != 0 &&
            nowNanoseconds - vrCameraAnchorTimeNanoseconds <
                kAnchorStaleNanoseconds;
    }

    // Actor indices are allocated globally by CampusActorManager (LRU cache +
    // reusable-index stack), so a scene's actors are NOT guaranteed to start
    // at 0 — a Live's members can occupy any indices while followCharaIndex
    // still points at a departed lobby actor. Track which indices actually
    // ran CampusActorController.LateUpdate recently so the rig can adopt and
    // cycle real actors instead of hoping for index 0.
    struct VrCameraSeenActorSlot {
        int index = -1;
        std::int64_t lastSeenNanoseconds = 0;
    };
    constexpr std::size_t kVrCameraSeenActorCapacity = 16;
    VrCameraSeenActorSlot vrCameraSeenActors[kVrCameraSeenActorCapacity]{};
    constexpr std::int64_t kVrCameraSeenStaleNanoseconds = 1'000'000'000LL;

    void RecordVrCameraActorIndex(int index, std::int64_t now) noexcept {
        VrCameraSeenActorSlot* stalest = &vrCameraSeenActors[0];
        for (auto& slot : vrCameraSeenActors) {
            if (slot.index == index) {
                slot.lastSeenNanoseconds = now;
                return;
            }
            if (slot.lastSeenNanoseconds < stalest->lastSeenNanoseconds) {
                stalest = &slot;
            }
        }
        stalest->index = index;
        stalest->lastSeenNanoseconds = now;
    }

    // Fills `out` (capacity kVrCameraSeenActorCapacity) with the indices seen
    // within the last second, ascending. Returns the count.
    std::size_t CollectFreshVrCameraActorIndices(
        std::int64_t now,
        int* out) noexcept {
        std::size_t count = 0;
        for (const auto& slot : vrCameraSeenActors) {
            if (slot.index >= 0 &&
                now - slot.lastSeenNanoseconds < kVrCameraSeenStaleNanoseconds) {
                out[count++] = slot.index;
            }
        }
        std::sort(out, out + count);
        return count;
    }

    // Consumes the controller-input mailbox, advances the rig and, when a
    // mode is active, replaces the pose the head-pose bridge composes onto.
    void UpdateVrFreeCameraRig(
        const gakumas::vr::pose::Pose& gameRequested,
        gakumas::vr::pose::Pose& bridgeBase) {
        namespace vrcam = gakumas::vr::camera;

        static std::int64_t lastUpdateNanoseconds = 0;
        static bool countersInitialized = false;
        static std::uint32_t consumedModePresses = 0;
        static std::uint32_t consumedCharaPresses = 0;
        static std::uint32_t consumedResetPresses = 0;

        const std::int64_t now = gakumas::vr::pose::MonotonicNowNanoseconds();
        float dtSeconds = 0.0F;
        if (lastUpdateNanoseconds != 0 && now > lastUpdateNanoseconds) {
            dtSeconds = static_cast<float>(
                static_cast<double>(now - lastUpdateNanoseconds) * 1.0e-9);
        }
        lastUpdateNanoseconds = now;
        constexpr float kMaxFreeCameraDeltaSeconds = 0.10F;
        if (dtSeconds > kMaxFreeCameraDeltaSeconds) {
            dtSeconds = kMaxFreeCameraDeltaSeconds;
        }

        vrcam::VrFreeCameraCommands commands;
        commands.dtSeconds = dtSeconds;
        commands.fpDirectionFollow = Config::vrFpDirectionFollow;
        const int menuModeRequest = vrcam::ConsumeVrFreeCameraModeRequest();
        if (menuModeRequest >= 0) {
            commands.hasModeRequest = true;
            commands.modeRequest =
                static_cast<vrcam::VrFreeCameraMode>(menuModeRequest);
        }
        std::uint32_t charaPresses = 0;

        gakumas::vr::camera::VrCameraInputSample input;
        if (gakumas::vr::ReadVrCameraInput(input) && input.valid) {
            if (!countersInitialized) {
                // First valid sample after startup: adopt the cumulative
                // counters without treating history as fresh presses.
                consumedModePresses = input.modePressCount;
                consumedCharaPresses = input.charaPressCount;
                consumedResetPresses = input.resetPressCount;
                countersInitialized = true;
            }
            commands.modePresses = input.modePressCount - consumedModePresses;
            commands.resetPresses = input.resetPressCount - consumedResetPresses;
            charaPresses = input.charaPressCount - consumedCharaPresses;
            consumedModePresses = input.modePressCount;
            consumedCharaPresses = input.charaPressCount;
            consumedResetPresses = input.resetPressCount;

            gakumas::vr::pose::PoseAdmission admission{};
            const bool ticketOk =
                gakumas::vr::VrRuntime::Instance().CurrentPoseAdmission(admission) &&
                !input.cancelled && input.frameId == admission.frameId &&
                input.sessionGeneration == admission.sessionGeneration;
            if (ticketOk && !input.menuVisible) {
                commands.leftStickX = input.leftStickX;
                commands.leftStickY = input.leftStickY;
                commands.rightStickX = input.rightStickX;
                commands.rightStickY = input.rightStickY;
                commands.sprintHeld = input.sprintHeld;
            }
        }

        vrcam::VrFreeCameraAnchor anchor;
        anchor.valid = IsVrCameraAnchorFresh(now);
        if (anchor.valid) {
            anchor.position = {
                vrCameraAnchorPosition.x,
                vrCameraAnchorPosition.y,
                vrCameraAnchorPosition.z,
            };
            anchor.forward = {
                vrCameraAnchorForward.x,
                vrCameraAnchorForward.y,
                vrCameraAnchorForward.z,
            };
            anchor.rotationValid = true;
            anchor.rotation = {
                vrCameraAnchorRotation.x,
                vrCameraAnchorRotation.y,
                vrCameraAnchorRotation.z,
                vrCameraAnchorRotation.w,
            };
        }

        const auto result = vrFreeCameraRig.Update(
            commands,
            anchor,
            gameRequested,
            vrFreeCameraLastComposedValid,
            vrFreeCameraLastComposed);

        if (result.modeChanged) {
            std::ostringstream stream;
            stream << "[VR][camera] FREECAM_MODE mode="
                   << vrcam::VrFreeCameraModeName(result.mode)
                   << " source=left-x anchorFresh=" << (anchor.valid ? 1 : 0);
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }
        if (result.resetApplied) {
            std::ostringstream stream;
            stream << "[VR][camera] FREECAM_RESET mode="
                   << vrcam::VrFreeCameraModeName(result.mode);
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }
        {
            static bool lastSprintHeld = false;
            const bool sprinting =
                (result.mode == vrcam::VrFreeCameraMode::Free ||
                 result.mode == vrcam::VrFreeCameraMode::Follow) &&
                commands.sprintHeld;
            if (sprinting != lastSprintHeld) {
                std::ostringstream stream;
                stream << "[VR][camera] FREECAM_SPRINT held="
                       << (sprinting ? 1 : 0) << " mode="
                       << vrcam::VrFreeCameraModeName(result.mode);
                static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
                lastSprintHeld = sprinting;
            }
        }

        const bool boneAnchorMode =
            result.mode == vrcam::VrFreeCameraMode::Follow ||
            result.mode == vrcam::VrFreeCameraMode::FirstPerson;

        if (charaPresses > 0 && boneAnchorMode &&
            Config::vrCameraYButtonBone == 1) {
            // Menu setting: the Y button switches the FOLLOW anchor bone
            // instead of the character.
            const int bone =
                vrcam::CycleVrFreeCameraFollowBone(charaPresses);
            std::ostringstream stream;
            stream << "[VR][camera] FREECAM_BONE value=" << bone << " key="
                   << vrcam::VrFreeCameraBoneKey(bone);
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        } else if (charaPresses > 0 && boneAnchorMode) {
            // Cycle through the actor indices that actually exist right now
            // (Live members can occupy any global indices; see the seen-actor
            // table). Without live data, fall back to wrapping at 0.
            int seen[kVrCameraSeenActorCapacity];
            const std::size_t seenCount = CollectFreshVrCameraActorIndices(now, seen);
            if (seenCount > 0) {
                int next = seen[0];
                for (std::uint32_t press = 0; press < charaPresses; ++press) {
                    const int current = GKCamera::followCharaIndex;
                    next = seen[0];
                    for (std::size_t i = 0; i < seenCount; ++i) {
                        if (seen[i] > current) {
                            next = seen[i];
                            break;
                        }
                    }
                    GKCamera::followCharaIndex = next;
                }
            } else if (!anchor.valid) {
                GKCamera::followCharaIndex = 0;
            } else {
                GKCamera::followCharaIndex += static_cast<int>(charaPresses);
            }
            std::ostringstream stream;
            stream << "[VR][camera] FREECAM_CHARA index="
                   << GKCamera::followCharaIndex
                   << " anchorFresh=" << (anchor.valid ? 1 : 0)
                   << " seen=";
            for (std::size_t i = 0; i < seenCount; ++i) {
                if (i > 0) stream << ',';
                stream << seen[i];
            }
            if (seenCount == 0) stream << '-';
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }

        // Self-heal a dead follow index: if the anchor has gone stale while
        // actors are demonstrably running (Live entered with a leftover lobby
        // index), adopt the lowest live index instead of freezing forever.
        static std::int64_t lastAdoptNanoseconds = 0;
        if (boneAnchorMode && !anchor.valid &&
            now - lastAdoptNanoseconds > 1'000'000'000LL) {
            int seen[kVrCameraSeenActorCapacity];
            const std::size_t seenCount = CollectFreshVrCameraActorIndices(now, seen);
            bool currentSeen = false;
            for (std::size_t i = 0; i < seenCount; ++i) {
                if (seen[i] == GKCamera::followCharaIndex) {
                    currentSeen = true;
                    break;
                }
            }
            if (seenCount > 0 && !currentSeen) {
                lastAdoptNanoseconds = now;
                const int previous = GKCamera::followCharaIndex;
                GKCamera::followCharaIndex = seen[0];
                std::ostringstream stream;
                stream << "[VR][camera] FREECAM_INDEX_ADOPT previous="
                       << previous << " adopted=" << seen[0] << " seen=";
                for (std::size_t i = 0; i < seenCount; ++i) {
                    if (i > 0) stream << ',';
                    stream << seen[i];
                }
                static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
            }
        }

        // Mirror the VR mode into the upstream GKCamera mode so the existing
        // first-person helpers (HideHead face/hair skip targets and the
        // LateUpdate isFirstPerson bone choice) work unchanged. Upstream
        // hooks stay inert while VR owns the camera transform.
        if (!IsLocalifyFreeCameraEnabled()) {
            GKCamera::CameraMode mirrored = GKCamera::CameraMode::FREE;
            if (result.mode == vrcam::VrFreeCameraMode::Follow) {
                mirrored = GKCamera::CameraMode::FOLLOW;
            } else if (result.mode == vrcam::VrFreeCameraMode::FirstPerson) {
                mirrored = GKCamera::CameraMode::FIRST_PERSON;
            }
            if (GKCamera::GetCameraMode() != mirrored) {
                GKCamera::SetCameraMode(mirrored);
            }
        }

        if (result.poseValid) {
            bridgeBase = result.rigPose;
        }
    }
    // ---- end VR free camera ----

    void ApplyVrHeadPose(void* brain, void* state) {
        if (!IsVrHeadPoseBridgeEnabled() || state == nullptr) {
            InvalidateVrStereoCameraFrame();
            return;
        }

        using UnityVector3 = UnityResolve::UnityType::Vector3;
        using UnityQuaternion = UnityResolve::UnityType::Quaternion;
        UnityVector3 gamePosition{};
        UnityQuaternion gameRotation{};
        float gameFov = 0.0F;
        if (!TryReadCinemachineState(
                state, &gamePosition, &gameRotation, &gameFov) ||
            !IsValidCinemachinePose(gamePosition, gameRotation, gameFov)) {
            return;
        }

        gakumas::vr::pose::StereoPoseSample trackedPose;
        gakumas::vr::pose::PoseAdmission admission{};
        if (!gakumas::vr::VrRuntime::Instance().CurrentPoseAdmission(admission) ||
            !gakumas::vr::VrRuntime::Instance().ReadStereoPoseForAdmission(
                admission, trackedPose)) {
            InvalidateVrStereoCameraFrame();
            return;
        }
        const gakumas::vr::pose::Pose gameRequested{
            {gamePosition.x, gamePosition.y, gamePosition.z},
            {gameRotation.x, gameRotation.y, gameRotation.z, gameRotation.w},
        };
        // The VR free camera substitutes its rig pose for the game pose when
        // a mode is active; in Off mode bridgeBase stays == gameRequested.
        gakumas::vr::pose::Pose bridgeBase = gameRequested;
        try {
            UpdateVrFreeCameraRig(gameRequested, bridgeBase);
        } catch (...) {
            ReportVrUnityHookException();
            bridgeBase = gameRequested;
        }
        gakumas::vr::pose::StereoComposedPose composed{};
        const float worldScale = Config::vrWorldScale;
        gakumas::vr::pose::BridgeUpdateResult result;
        {
            std::lock_guard lock(vrHeadPoseBridgeMutex);
            result = vrHeadPoseBridge.UpdateStereo(
                bridgeBase,
                trackedPose,
                gakumas::vr::pose::MonotonicNowNanoseconds(),
                -1,
                worldScale,
                composed,
                &admission);
            if (result == gakumas::vr::pose::BridgeUpdateResult::BaselineLatched) {
                std::ostringstream stream;
                stream << "[VR][camera] HEAD_POSE_BASELINE_LATCHED session="
                       << trackedPose.sessionGeneration
                       << " epoch=" << trackedPose.poseEpoch
                       << " referenceSpace=" << trackedPose.referenceSpaceType
                       << " brain=0x" << std::hex
                       << reinterpret_cast<std::uintptr_t>(brain) << std::dec
                       << " worldScale=" << std::fixed << std::setprecision(3)
                       << worldScale;
                WriteUnityCameraDiagnosticEvent(stream.str());
            }
        }
        if (result != gakumas::vr::pose::BridgeUpdateResult::Applied) {
            InvalidateVrStereoCameraFrame();
            return;
        }

        const UnityVector3 outputPosition(
            composed.center.position.x,
            composed.center.position.y,
            composed.center.position.z);
        const UnityQuaternion outputRotation(
            composed.center.orientation.x,
            composed.center.orientation.y,
            composed.center.orientation.z,
            composed.center.orientation.w);
        // Automatic Memory photos must see the game's authored source shot.
        // Still compose/publish the SAME free-rig + HMD pose below for the VR
        // eyes; never switch modes or reset the rig just to take these photos.
        const bool authoredPhotoSource = gakumas::vr::LiveSourcePhotoProtectionActive();
        static bool photoSourceBypassLogged = false;
        if (authoredPhotoSource != photoSourceBypassLogged) {
            photoSourceBypassLogged = authoredPhotoSource;
            static_cast<void>(gakumas::vr::WriteVrLog(authoredPhotoSource
                ? "[VR][photo] AUTO_PHOTO_SOURCE_POSE bypass=1 hmd=unchanged"
                : "[VR][photo] AUTO_PHOTO_SOURCE_POSE bypass=0 hmd=unchanged"));
        }
        if (!authoredPhotoSource && !TryWriteCinemachineRawPose(
                state, outputPosition, outputRotation)) {
            vrHeadPoseWritesEnabled.store(false, std::memory_order_release);
            InvalidateVrStereoCameraFrame();
            ReportVrHeadPoseWriteFailure();
            return;
        }

        // Remember the composed head pose: next frame's free-camera update
        // uses it as the movement basis and the snap-turn pivot.
        vrFreeCameraLastComposed = composed.center;
        vrFreeCameraLastComposedValid = true;

        {
            std::lock_guard lock(vrStereoCameraFrameMutex);
            vrStereoCameraFrame.trackingSample = trackedPose;
            vrStereoCameraFrame.composedPose = composed;
            // `bridgeBase` is the pose the headset delta was composed onto
            // (Cinemachine's own shot, or the free-camera rig pose when a
            // mode is active). Actor-shadow uses this as `sourcePose`.
            // Volume / default toon use `cinematicPose` (gameRequested).
            vrStereoCameraFrame.sourcePose = bridgeBase;
            vrStereoCameraFrame.sourcePoseValid = true;
            vrStereoCameraFrame.cinematicPose = gameRequested;
            vrStereoCameraFrame.cinematicPoseValid = true;
            vrStereoCameraFrameValid = Config::vrStereoEnabled;
        }
        gakumas::vr::pose::Pose openXrHeadCenter{};
        const bool openXrHeadValid = gakumas::vr::pose::TryCenterStereoPose(
            {trackedPose.eyes[0].pose, trackedPose.eyes[1].pose},
            openXrHeadCenter);
        gakumas::vr::UpdateHandGlowComposeBridge(
            composed.center, openXrHeadCenter, openXrHeadValid, worldScale);
        unityStereoRenderer.NoteToonFollowIndex(GKCamera::followCharaIndex);

        std::lock_guard lock(vrHeadPoseBridgeMutex);
        ++vrHeadPoseAppliedSamples;
        if (vrHeadPoseAppliedSamples == 1U ||
            vrHeadPoseAppliedSamples % 300U == 0U) {
            std::ostringstream stream;
            stream << "[VR][camera] HEAD_POSE_APPLIED sample="
                   << vrHeadPoseAppliedSamples
                   << " session=" << trackedPose.sessionGeneration
                   << " epoch=" << trackedPose.poseEpoch
                   << " sourceRevision=" << trackedPose.revision
                   << " brain=0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(brain) << std::dec
                   << std::fixed << std::setprecision(5)
                   << " gamePosition=(" << gamePosition.x << ',' << gamePosition.y << ','
                   << gamePosition.z << ')'
                   << " outputPosition=(" << outputPosition.x << ',' << outputPosition.y << ','
                   << outputPosition.z << ')'
                   << " gameRotation=(" << gameRotation.x << ',' << gameRotation.y << ','
                   << gameRotation.z << ',' << gameRotation.w << ')'
                   << " outputRotation=(" << outputRotation.x << ',' << outputRotation.y << ','
                   << outputRotation.z << ',' << outputRotation.w << ')';
            WriteUnityCameraDiagnosticEvent(stream.str());
        }
    }

    void ReportUnreadableCinemachineState(
        std::uint64_t sample,
        void* brain) {
        if (cinemachineStateReadFailureLogged.load(std::memory_order_acquire)) {
            return;
        }
        std::ostringstream stream;
        stream << "[VR][camera] CINEMACHINE_STATE_UNREADABLE sample=" << sample
               << " brain=0x" << std::hex << reinterpret_cast<std::uintptr_t>(brain)
               << std::dec << " readsDisabled=1";
        if (WriteUnityCameraDiagnosticEvent(stream.str())) {
            cinemachineStateReadFailureLogged.store(true, std::memory_order_release);
        }
    }

    void BeginEyeRenderPassTrace(void* camera) noexcept {
        if (!AreVrUnityCameraDiagnosticsEnabled() || camera == nullptr ||
            !unityStereoRenderer.IsEyeCamera(camera)) {
            return;
        }
        const std::string_view role = unityStereoRenderer.ClassifyCamera(camera);
        eyeRenderPassTraceEye = role == "right" ? 1U : 0U;
        eyeRenderPassTraceCamera = camera;
        eyeRenderPassTraceCount = 0U;
        eyeRenderPassTraceOverflow = false;
        eyeRenderPassTraceActive = true;
    }

    int ReadEyeRenderPassEvent(void* pass) noexcept {
        if (pass == nullptr || eyePassEventGetter == nullptr) {
            return (std::numeric_limits<int>::min)();
        }
        const auto value = InvokeUnityGetter<int>(eyePassEventGetter, pass);
        return value.has_value()
            ? *value
            : (std::numeric_limits<int>::min)();
    }

    void RecordEyeRenderPass(void* pass) noexcept {
        if (!eyeRenderPassTraceActive || pass == nullptr) {
            return;
        }
        if (eyeRenderPassTraceCount == 0U &&
            eyeFullscreenEpochFirstPass[eyeRenderPassTraceEye] != pass) {
            eyeFullscreenEpochFirstPass[eyeRenderPassTraceEye] = pass;
            std::size_t writeIndex = 0U;
            for (std::size_t index = 0U;
                 index < eyeFullscreenDrawKeyCount; ++index) {
                if (eyeFullscreenDrawKeys[index].eye == eyeRenderPassTraceEye) {
                    continue;
                }
                eyeFullscreenDrawKeys[writeIndex++] = eyeFullscreenDrawKeys[index];
            }
            eyeFullscreenDrawKeyCount = writeIndex;
            eyeFullscreenDrawOverflowLogged.store(false, std::memory_order_release);
            std::ostringstream epoch;
            epoch << "[VR][eye-pass] EYE_FULLSCREEN_EPOCH role="
                  << (eyeRenderPassTraceEye == 0U ? "left" : "right")
                  << " firstPass=0x" << std::hex
                  << reinterpret_cast<std::uintptr_t>(pass) << std::dec
                  << " retainedOtherEye=" << writeIndex;
            WriteUnityCameraDiagnosticEvent(epoch.str());
        }
        if (eyeRenderPassTraceCount >= eyeRenderPassTraceEntries.size()) {
            eyeRenderPassTraceOverflow = true;
            return;
        }
        const int event = ReadEyeRenderPassEvent(pass);
        eyeRenderPassTraceEntries[eyeRenderPassTraceCount++] = {
            pass,
            Il2cppUtils::get_class_from_instance(pass),
            event,
        };
    }

    bool RememberEyeFullscreenDraw(
        void* renderPass,
        void* material,
        int shaderPass,
        int topology,
        int vertexCount,
        int instanceCount,
        std::uint64_t stackSignature,
        std::size_t eye,
        EyeFullscreenDrawApi api) noexcept {
        for (std::size_t index = 0U; index < eyeFullscreenDrawKeyCount; ++index) {
            const auto& key = eyeFullscreenDrawKeys[index];
            if (key.renderPass == renderPass && key.material == material &&
                key.shaderPass == shaderPass && key.topology == topology &&
                key.vertexCount == vertexCount &&
                key.instanceCount == instanceCount &&
                key.stackSignature == stackSignature && key.eye == eye &&
                key.api == api) {
                return false;
            }
        }
        if (eyeFullscreenDrawKeyCount >= eyeFullscreenDrawKeys.size()) {
            if (!eyeFullscreenDrawOverflowLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                WriteUnityCameraDiagnosticEvent(
                    "[VR][eye-pass] EYE_FULLSCREEN_DRAW_OVERFLOW capacity=1024");
            }
            return false;
        }
        eyeFullscreenDrawKeys[eyeFullscreenDrawKeyCount++] = {
            renderPass,
            material,
            shaderPass,
            topology,
            vertexCount,
            instanceCount,
            stackSignature,
            eye,
            api,
        };
        return true;
    }

    void ObserveEyeFullscreenDraw(
        EyeFullscreenDrawApi api,
        void* material,
        int shaderPass,
        int topology,
        int vertexCount,
        int instanceCount) noexcept {
        void* camera = unityStereoRenderer.CurrentCamera();
        if (!eyeRenderPassTraceActive || eyeRenderPassExecution.pass == nullptr ||
            camera == nullptr || !unityStereoRenderer.IsEyeCamera(camera)) {
            return;
        }
        const std::string_view role = unityStereoRenderer.ClassifyCamera(camera);
        const std::size_t eye = role == "right" ? 1U : 0U;

        std::array<void*, 16> stack{};
        const USHORT stackCount = CaptureStackBackTrace(
            0, static_cast<DWORD>(stack.size()), stack.data(), nullptr);
        static const auto gameBase = reinterpret_cast<std::uintptr_t>(
            GetModuleHandleW(L"GameAssembly.dll"));
        std::uint64_t stackSignature = 1469598103934665603ULL;
        bool hasGameFrame = false;
        for (USHORT index = 0U; index < stackCount; ++index) {
            const auto address = reinterpret_cast<std::uintptr_t>(stack[index]);
            if (gameBase == 0U || address < gameBase ||
                address - gameBase >= 0x40000000ULL) {
                continue;
            }
            stackSignature ^= static_cast<std::uint64_t>(address - gameBase);
            stackSignature *= 1099511628211ULL;
            hasGameFrame = true;
        }
        if (!hasGameFrame) {
            stackSignature = 0U;
        }
        if (!RememberEyeFullscreenDraw(
                eyeRenderPassExecution.pass, material, shaderPass, topology,
                vertexCount, instanceCount, stackSignature, eye, api)) {
            return;
        }

        void* shader = nullptr;
        if (material != nullptr && eyeMaterialGetShader != nullptr) {
            const auto value = InvokeUnityGetter<void*>(eyeMaterialGetShader, material);
            if (value.has_value()) {
                shader = *value;
            }
        }

        const char* nameSpace = eyeRenderPassExecution.klass != nullptr &&
                eyeRenderPassExecution.klass->namespaze != nullptr
            ? eyeRenderPassExecution.klass->namespaze
            : "?";
        const char* name = eyeRenderPassExecution.klass != nullptr &&
                eyeRenderPassExecution.klass->name != nullptr
            ? eyeRenderPassExecution.klass->name
            : "?";
        const char* apiName = "?";
        const char* primitive = "?";
        switch (api) {
            case EyeFullscreenDrawApi::BlitterTriangle:
                apiName = "Blitter.DrawTriangle";
                primitive = "triangle";
                break;
            case EyeFullscreenDrawApi::BlitterQuad:
                apiName = "Blitter.DrawQuad";
                primitive = "quad";
                break;
            case EyeFullscreenDrawApi::CommandDrawProcedural:
                apiName = "CommandBuffer.DrawProcedural";
                primitive = "procedural";
                break;
            case EyeFullscreenDrawApi::CommandBlit:
                apiName = "CommandBuffer.Blit";
                primitive = "blit";
                break;
        }
        std::ostringstream line;
        line << "[VR][eye-pass] EYE_FULLSCREEN_DRAW role=" << role
             << " api=" << apiName << " primitive=" << primitive
             << " parent=" << nameSpace << '.' << name
             << " event=";
        if (eyeRenderPassExecution.event == (std::numeric_limits<int>::min)()) {
            line << '?';
        } else {
            line << eyeRenderPassExecution.event;
        }
        line << " renderPass=0x" << std::hex
             << reinterpret_cast<std::uintptr_t>(eyeRenderPassExecution.pass)
             << " material=0x" << reinterpret_cast<std::uintptr_t>(material)
             << std::dec << " materialName=\"" << ReadUnityObjectName(material)
             << "\" shader=0x" << std::hex
             << reinterpret_cast<std::uintptr_t>(shader) << std::dec
             << " shaderName=\"" << ReadUnityObjectName(shader)
             << "\" shaderPass=" << shaderPass << " topology=";
        if (topology < 0) {
            line << '-';
        } else {
            line << topology;
        }
        line << " vertices=";
        if (vertexCount < 0) {
            line << '-';
        } else {
            line << vertexCount;
        }
        line << " instances=";
        if (instanceCount < 0) {
            line << '-';
        } else {
            line << instanceCount;
        }
        line << " stackSignature=0x" << std::hex << stackSignature << std::dec;
        line << " stackGameRva=";
        bool wroteStack = false;
        for (USHORT index = 0U; index < stackCount; ++index) {
            const auto address = reinterpret_cast<std::uintptr_t>(stack[index]);
            if (gameBase == 0U || address < gameBase ||
                address - gameBase >= 0x40000000ULL) {
                continue;
            }
            if (wroteStack) {
                line << ',';
            }
            line << "+0x" << std::hex << (address - gameBase) << std::dec;
            wroteStack = true;
        }
        if (!wroteStack) {
            line << '-';
        }
        line << " originalCalled=1";
        WriteUnityCameraDiagnosticEvent(line.str());
    }

    void DescribeEyeRenderObjectsPass(void* pass) noexcept {
        if (pass == nullptr ||
            eyeRenderObjectsLastInstance.exchange(
                pass, std::memory_order_acq_rel) == pass) {
            return;
        }
        const auto& fields = eyeRenderObjectsPassFields;
        const auto renderQueueType =
            ReadUnityInstanceField<int>(pass, fields.renderQueueType);
        const auto profilerTag =
            ReadUnityInstanceField<UnityResolve::UnityType::String*>(
                pass, fields.profilerTag);
        const auto cameraSettings =
            ReadUnityInstanceField<void*>(pass, fields.cameraSettings);
        const auto overrideMaterial =
            ReadUnityInstanceField<void*>(pass, fields.overrideMaterial);
        const auto overrideMaterialPass =
            ReadUnityInstanceField<int>(pass, fields.overrideMaterialPassIndex);
        const auto overrideShader =
            ReadUnityInstanceField<void*>(pass, fields.overrideShader);
        const auto overrideShaderPass =
            ReadUnityInstanceField<int>(pass, fields.overrideShaderPassIndex);

        std::optional<int> lowerBound;
        std::optional<int> upperBound;
        std::optional<int> layerMask;
        std::optional<std::uint32_t> renderingLayerMask;
        std::optional<int> excludeMotionVectors;
        if (fields.filteringSettings != nullptr) {
            void* filtering = static_cast<std::byte*>(pass) +
                fields.filteringSettings->offset;
            layerMask = ReadUnityValueField<int>(filtering, fields.layerMask);
            renderingLayerMask = ReadUnityValueField<std::uint32_t>(
                filtering, fields.renderingLayerMask);
            excludeMotionVectors = ReadUnityValueField<int>(
                filtering, fields.excludeMotionVectorObjects);
            constexpr std::int32_t boxedHeader =
                static_cast<std::int32_t>(sizeof(void*) * 2U);
            if (fields.renderQueueRange != nullptr &&
                fields.renderQueueRange->offset >= boxedHeader) {
                void* range = static_cast<std::byte*>(filtering) +
                    (fields.renderQueueRange->offset - boxedHeader);
                lowerBound = ReadUnityValueField<int>(range, fields.lowerBound);
                upperBound = ReadUnityValueField<int>(range, fields.upperBound);
            }
        }

        const auto optionalInt = [](const std::optional<int>& value) {
            return value.has_value() ? std::to_string(*value) : std::string("?");
        };
        std::ostringstream line;
        line << "[VR][eye-pass] EYE_RENDER_OBJECTS_INSTANCE pass=0x"
             << std::hex << reinterpret_cast<std::uintptr_t>(pass) << std::dec
             << " profilerTag=\""
             << (profilerTag.has_value()
                     ? ReadUnityStringValue(*profilerTag)
                     : std::string{})
             << "\" renderQueueType=" << optionalInt(renderQueueType)
             << " queue=" << optionalInt(lowerBound) << ','
             << optionalInt(upperBound)
             << " layerMask=" << optionalInt(layerMask)
             << " renderingLayerMask=";
        if (renderingLayerMask.has_value()) {
            line << "0x" << std::hex << *renderingLayerMask << std::dec;
        } else {
            line << '?';
        }
        line << " excludeMotionVectors=" << optionalInt(excludeMotionVectors)
             << " cameraSettings="
             << (cameraSettings.has_value() ? *cameraSettings : nullptr)
             << " overrideMaterial="
             << (overrideMaterial.has_value() ? *overrideMaterial : nullptr)
             << " overrideMaterialName=\""
             << (overrideMaterial.has_value()
                     ? ReadUnityObjectName(*overrideMaterial)
                     : std::string{})
             << "\" overrideMaterialPass=" << optionalInt(overrideMaterialPass)
             << " overrideShader="
             << (overrideShader.has_value() ? *overrideShader : nullptr)
             << " overrideShaderName=\""
             << (overrideShader.has_value()
                     ? ReadUnityObjectName(*overrideShader)
                     : std::string{})
             << "\" overrideShaderPass=" << optionalInt(overrideShaderPass);
        WriteUnityCameraDiagnosticEvent(line.str());
    }

    void ObserveEyeRenderObjectsPass(void* pass) noexcept {
        if (!eyeRenderObjectsPassFields.Ready() || !eyeRenderPassTraceActive ||
            pass == nullptr ||
            ReadEyeRenderPassEvent(pass) != 600) {
            return;
        }
        DescribeEyeRenderObjectsPass(pass);
    }

    int EyeRenderObjectsRoleIndex(const char* role) noexcept {
        if (role != nullptr && std::strcmp(role, "left") == 0) {
            return 0;
        }
        if (role != nullptr && std::strcmp(role, "right") == 0) {
            return 1;
        }
        return 2;
    }

    void LogEyeRenderObjectsPolicy(
        const char* role,
        void* camera,
        void* pass,
        int event,
        bool isEye,
        bool skipped,
        bool originalCalled) noexcept {
        const int roleIndex = EyeRenderObjectsRoleIndex(role);
        auto& count = eyeRenderObjectsLogs[roleIndex];
        auto& lastAction = eyeRenderObjectsLastAction[roleIndex];
        const int action = skipped ? 1 : 0;
        const bool changed = lastAction != action;
        lastAction = action;
        ++count;
        if (!changed && count > 8 && count % 600 != 0) {
            return;
        }
        std::ostringstream line;
        line << "[VR][eye-pass] EYE_RENDER_OBJECTS role="
             << (role != nullptr ? role : "?") << " camera=0x" << std::hex
             << reinterpret_cast<std::uintptr_t>(camera) << std::dec
             << " eye=" << (isEye ? 1 : 0)
             << " toggle="
             << (Config::vrHideUiTextureOverlay ? 1 : 0)
             << " event=";
        if (event == (std::numeric_limits<int>::min)()) {
            line << '?';
        } else {
            line << event;
        }
        line << " action=" << (skipped ? "skip" : "execute")
             << " originalCalled=" << (originalCalled ? 1 : 0)
             << " pass=0x" << std::hex
             << reinterpret_cast<std::uintptr_t>(pass) << std::dec;
        static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
    }

    // .149/.150/.181: the only pass after VLPostProcessPass(550) and before
    // FinalBlitPass(1001) is RenderObjectsPass(600) profilerTag OverlayCanvas.
    // Returning without orig leaves that layer's DrawRenderers empty. Event
    // getter miss fails open (event != 600). Source / Grip / other events
    // always execute.
    bool ShouldSkipEyeRenderObjectsPass(void* pass) noexcept {
        if (pass == nullptr || !Config::vrHideUiTextureOverlay) {
            return false;
        }
        void* camera = unityStereoRenderer.CurrentCamera();
        if (!unityStereoRenderer.IsEyeCamera(camera)) {
            return false;
        }
        return ReadEyeRenderPassEvent(pass) == 600;
    }

    // Observe-only: dump.cs ProFlareRenderingSystem.ComputeAndRenderFlares
    // is the actual raster. Do not skip — that would kill accepted .175
    // ordinary flares. Log camera role during the White night 8 s intro.
    void LogProFlareRender(void* camera) noexcept {
        const bool isEye = unityStereoRenderer.IsEyeCamera(camera);
        const char* role = unityStereoRenderer.ClassifyCamera(camera);
        const int roleIndex = EyeRenderObjectsRoleIndex(role);
        auto& count = proFlareRenderLogs[roleIndex];
        ++count;
        if (count > 24 && count % 90 != 0) {
            return;
        }
        std::ostringstream line;
        line << "[VR][fov] PRO_FLARE_RENDER role="
             << (role != nullptr ? role : "?") << " camera=0x" << std::hex
             << reinterpret_cast<std::uintptr_t>(camera) << std::dec
             << " eye=" << (isEye ? 1 : 0)
             << " method=ComputeAndRenderFlares count=" << count;
        static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
    }

    void EndEyeRenderPassTrace(void* camera) noexcept {
        if (!eyeRenderPassTraceActive || camera == nullptr ||
            camera != eyeRenderPassTraceCamera) {
            return;
        }
        eyeRenderPassTraceActive = false;
        const std::size_t eye = eyeRenderPassTraceEye;
        std::uint64_t signature = 1469598103934665603ULL;
        const auto mix = [&signature](std::uint64_t value) {
            signature ^= value;
            signature *= 1099511628211ULL;
        };
        mix(static_cast<std::uint64_t>(eyeRenderPassTraceCount));
        for (std::size_t index = 0U; index < eyeRenderPassTraceCount; ++index) {
            const auto& entry = eyeRenderPassTraceEntries[index];
            mix(static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(entry.klass)));
            mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(entry.event)));
        }
        const std::uint64_t serial = ++eyeRenderPassFrameSerial[eye];
        const bool changed = eyeRenderPassLastSignature[eye] != signature;
        const bool periodic = eye == 0U && serial % 600U == 0U;
        if (!changed && !periodic) {
            return;
        }
        eyeRenderPassLastSignature[eye] = signature;
        const char* role = eye == 0U ? "left" : "right";
        std::ostringstream begin;
        begin << "[VR][eye-pass] EYE_PASS_QUEUE_BEGIN role=" << role
              << " frame=" << serial << " count=" << eyeRenderPassTraceCount
              << " signature=0x" << std::hex << signature << std::dec
              << " changed=" << (changed ? 1 : 0)
              << " overflow=" << (eyeRenderPassTraceOverflow ? 1 : 0);
        WriteUnityCameraDiagnosticEvent(begin.str());
        for (std::size_t index = 0U; index < eyeRenderPassTraceCount; ++index) {
            const auto& entry = eyeRenderPassTraceEntries[index];
            const char* nameSpace = entry.klass != nullptr &&
                    entry.klass->namespaze != nullptr
                ? entry.klass->namespaze
                : "?";
            const char* name = entry.klass != nullptr && entry.klass->name != nullptr
                ? entry.klass->name
                : "?";
            std::ostringstream line;
            line << "[VR][eye-pass] EYE_PASS role=" << role
                 << " index=" << index << " event=";
            if (entry.event == (std::numeric_limits<int>::min)()) {
                line << '?';
            } else {
                line << entry.event;
            }
            line << " type=" << nameSpace << '.' << name
                 << " pass=0x" << std::hex
                 << reinterpret_cast<std::uintptr_t>(entry.pass) << std::dec;
            WriteUnityCameraDiagnosticEvent(line.str());
        }
        WriteUnityCameraDiagnosticEvent(
            std::string("[VR][eye-pass] EYE_PASS_QUEUE_END role=") + role +
            " frame=" + std::to_string(serial));
    }

    void TrackVrSourceCamera(void* camera) {
        if (!IsVrUnityRuntimeEnabled()) {
            return;
        }
        const bool diagnostics = AreVrUnityCameraDiagnosticsEnabled();
        if (diagnostics) {
            EnsureUnityCameraDiagnosticMarker();
        }
        if (camera == nullptr) {
            return;
        }
        // .114 black screen: the left eye got the MainCamera tag, native
        // get_main returned it, this tracker re-pointed vrSourceCamera
        // at the eye, and Tick — gated on IsSelectedVrMainCamera(brain
        // output camera) — never fired again. Eyes stopped re-arming (black
        // HMD) and RestoreSourceCamera was unreachable. The VR queue
        // cameras are never eligible as the tracked source camera.
        if (unityStereoRenderer.IsEyeCamera(camera)) {
            if (!vrSourceCameraEyeIgnoredLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                static_cast<void>(gakumas::vr::WriteVrLog(
                    "[VR][stereo] SOURCE_CAMERA_EYE_IGNORED tracker keeps source"));
            }
            return;
        }
        std::lock_guard lock(vrCameraStateMutex);
        if (camera == vrSourceCamera) {
            if (diagnostics) {
                auto& record = EnsureUnityCameraDiagnosticRecord(camera);
                EnsureUnityMainSelectionLog(camera, record);
            }
            return;
        }
        if (diagnostics && vrSourceCamera != nullptr) {
            const auto previous = unityCameraDiagnosticRecords.find(vrSourceCamera);
            if (previous != unityCameraDiagnosticRecords.end()) {
                previous->second.mainCamera = false;
            }
        }
        vrSourceCamera = camera;
        std::ostringstream tracked;
        tracked << "[VR][stereo] SOURCE_CAMERA_TRACKED id=0x" << std::hex
                << reinterpret_cast<std::uintptr_t>(camera) << std::dec;
        static_cast<void>(gakumas::vr::WriteVrLog(tracked.str()));
        if (diagnostics) {
            auto& record = EnsureUnityCameraDiagnosticRecord(camera);
            record.mainCamera = true;
            record.mainSelectionLogged = false;
            EnsureUnityMainSelectionLog(camera, record);
        }
    }

    void ObserveUnityCameraRender(void* camera) {
        if (!AreVrUnityCameraDiagnosticsEnabled()) {
            return;
        }
        EnsureUnityCameraDiagnosticMarker();
        if (camera == nullptr) {
            return;
        }
        std::lock_guard lock(vrCameraStateMutex);
        auto& record = EnsureUnityCameraDiagnosticRecord(camera);
        EnsureUnityMainSelectionLog(camera, record);
        ++record.renderCount;
        ++unityCameraRenderCallbacks;
        if (unityCameraRenderCallbacks != 1U &&
            unityCameraRenderCallbacks % 600U != 0U) {
            return;
        }

        std::ostringstream stream;
        stream << "[VR][camera] CAMERA_ACTIVITY callbacks="
               << unityCameraRenderCallbacks
               << " unique=" << unityCameraDiagnosticRecords.size()
               << " cinemachineSamples="
               << cinemachinePoseSamples.load(std::memory_order_relaxed)
               << " invalidPoseSamples="
               << cinemachineInvalidPoseSamples.load(std::memory_order_relaxed);
        std::size_t emitted = 0;
        for (const auto& [cameraId, cameraRecord] : unityCameraDiagnosticRecords) {
            if (emitted >= 8U) {
                stream << " more=" << (unityCameraDiagnosticRecords.size() - emitted);
                break;
            }
            stream << " camera" << emitted << "=(0x" << std::hex
                   << reinterpret_cast<std::uintptr_t>(cameraId) << std::dec
                   << ",\"" << cameraRecord.name << "\",renders="
                   << cameraRecord.renderCount << ",main="
                   << cameraRecord.mainCamera << ')';
            ++emitted;
        }
        WriteUnityCameraDiagnosticEvent(stream.str());
    }

    PendingCinemachineObservation CaptureCinemachinePose(
        void* brain,
        void* state) {
        PendingCinemachineObservation observation{};
        if (!AreVrUnityCameraDiagnosticsEnabled()) {
            return observation;
        }
        EnsureUnityCameraDiagnosticMarker();
        using Vector3 = UnityResolve::UnityType::Vector3;
        using Quaternion = UnityResolve::UnityType::Quaternion;
        const auto sample = cinemachinePoseSamples.fetch_add(
            1U,
            std::memory_order_relaxed) + 1U;
        observation.sampled = true;
        observation.sample = sample;
        if (!cinemachineStateReadsEnabled.load(std::memory_order_acquire)) {
            ReportUnreadableCinemachineState(sample, brain);
            return observation;
        }
        Vector3 position{};
        Quaternion rotation{};
        float fov = 0.0F;
        if (!TryReadCinemachineState(state, &position, &rotation, &fov)) {
            cinemachineInvalidPoseSamples.fetch_add(1U, std::memory_order_relaxed);
            cinemachineStateReadsEnabled.store(false, std::memory_order_release);
            ReportUnreadableCinemachineState(sample, brain);
            return observation;
        }
        const float rotationNormSquared =
            rotation.x * rotation.x + rotation.y * rotation.y +
            rotation.z * rotation.z + rotation.w * rotation.w;
        const bool layoutValid = IsValidCinemachinePose(position, rotation, fov);
        if (!layoutValid) {
            const auto invalidSample = cinemachineInvalidPoseSamples.fetch_add(
                1U,
                std::memory_order_relaxed) + 1U;
            if (invalidSample == 1U || invalidSample % 300U == 0U) {
                std::ostringstream invalidStream;
                invalidStream << "[VR][camera] CINEMACHINE_LAYOUT_INVALID sample=" << sample
                              << " invalid=" << invalidSample
                              << " brain=0x" << std::hex
                              << reinterpret_cast<std::uintptr_t>(brain) << std::dec
                              << std::fixed << std::setprecision(5)
                              << " position=(" << position.x << ',' << position.y << ','
                              << position.z << ')'
                              << " rotation=(" << rotation.x << ',' << rotation.y << ','
                              << rotation.z << ',' << rotation.w << ')'
                              << " norm2=" << rotationNormSquared
                              << " fov=" << std::setprecision(3) << fov;
                WriteUnityCameraDiagnosticEvent(invalidStream.str());
            }
            return observation;
        }
        observation.stateValid = true;
        observation.fovValid = TryReadCinemachineFovDiagnostic(
            state, sample, &observation.fov);
        if (sample != 1U && sample % 300U != 0U) {
            return observation;
        }

        std::ostringstream stream;
        stream << "[VR][camera] CINEMACHINE_POSE sample=" << sample
               << " brain=0x" << std::hex << reinterpret_cast<std::uintptr_t>(brain)
               << std::dec << std::fixed << std::setprecision(5)
               << " position=(" << position.x << ',' << position.y << ',' << position.z << ')'
               << " rotation=(" << rotation.x << ',' << rotation.y << ','
               << rotation.z << ',' << rotation.w << ')'
               << " fov=" << std::setprecision(3) << fov;
        WriteUnityCameraDiagnosticEvent(stream.str());
        return observation;
    }

    void PublishCinemachineObservation(
        void* brain,
        void* outputCamera,
        bool selectedSource,
        const PendingCinemachineObservation& observation) noexcept {
        if (!AreVrUnityCameraDiagnosticsEnabled() || !observation.sampled) {
            return;
        }
        const bool periodic = observation.sample == 1U ||
            observation.sample % 300U == 0U;
        if (!observation.stateValid || !observation.fovValid) {
            if (periodic) {
                WriteUnityCameraDiagnosticEvent(
                    "[VR][fov] CAMERA_STATE_REJECTED sample=" +
                    std::to_string(observation.sample) +
                    " reason=invalid-state-or-layout");
            }
            return;
        }
        if (outputCamera == nullptr) {
            if (periodic) {
                WriteUnityCameraDiagnosticEvent(
                    "[VR][fov] CAMERA_STATE_REJECTED sample=" +
                    std::to_string(observation.sample) +
                    " reason=no-output-camera");
            }
            return;
        }
        if (!selectedSource) {
            if (periodic) {
                std::ostringstream stream;
                stream << "[VR][fov] CAMERA_STATE_REJECTED sample="
                       << observation.sample
                       << " reason=output-not-selected-source brain=0x"
                       << std::hex << reinterpret_cast<std::uintptr_t>(brain)
                       << " output=0x"
                       << reinterpret_cast<std::uintptr_t>(outputCamera)
                       << std::dec;
                WriteUnityCameraDiagnosticEvent(stream.str());
            }
            return;
        }
        unityStereoRenderer.ObserveSourceCameraStateForDiagnostics(
            observation.fov);
        if (periodic) {
            WriteUnityCameraDiagnosticEvent(
                "[VR][fov] CAMERA_STATE_PAIRED sample=" +
                std::to_string(observation.sample) +
                " outputSelected=1 hasLookAt=" +
                (observation.fov.hasLookAt ? "1" : "0") +
                " motionSemantics=" +
                (observation.fov.hasLookAt ? "projection+distance"
                                           : "projection-only"));
        }
    }

    bool TryCopyNativeBytes(
        const void* source,
        void* destination,
        std::size_t size) noexcept {
        if (source == nullptr || destination == nullptr || size == 0U) {
            return false;
        }
        __try {
            std::memcpy(destination, source, size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void LogProduceTransitionLayoutForDiagnostics() noexcept {
        if (!AreVrUnityCameraDiagnosticsEnabled() ||
            produceTransitionLayoutLogged.exchange(
                true, std::memory_order_acq_rel)) {
            return;
        }
        auto* menuClass = Il2cppUtils::GetClass(
            "Assembly-CSharp.dll", "Campus.Common", "VerticalAdvMenuView");
        auto* buttonViewClass = Il2cppUtils::GetClass(
            "Assembly-CSharp.dll", "Campus.Common",
            "VerticalAdvMenuButtonView");
        auto* buttonBaseClass = Il2cppUtils::GetClass(
            "quaunity-ui.Runtime.dll", "Qua.UI", "ButtonBase");
        UnityResolve::Method* initialize = nullptr;
        if (menuClass != nullptr) {
            for (auto* method : menuClass->methods) {
                if (method == nullptr || method->name != "Initialize" ||
                    method->static_function || !method->args.empty() ||
                    method->return_type == nullptr ||
                    method->return_type->name != "System.Void" ||
                    method->function == nullptr || method->address == nullptr) {
                    continue;
                }
                if (initialize != nullptr) {
                    initialize = nullptr;
                    break;
                }
                initialize = method;
            }
        }
        std::ostringstream summary;
        summary << "[VR][produce] VERTICAL_ADV_LAYOUT menu="
                << (menuClass != nullptr ? 1 : 0)
                << " buttonView=" << (buttonViewClass != nullptr ? 1 : 0)
                << " buttonBase=" << (buttonBaseClass != nullptr ? 1 : 0)
                << " initializeShape=" << (initialize != nullptr ? 1 : 0)
                << " initializeFunction=0x" << std::hex
                << reinterpret_cast<std::uintptr_t>(
                       initialize != nullptr ? initialize->function : nullptr)
                << " methodInfo=0x"
                << reinterpret_cast<std::uintptr_t>(
                       initialize != nullptr ? initialize->address : nullptr)
                << std::dec;
        static_cast<void>(gakumas::vr::WriteVrLog(summary.str()));

        const auto logFields = [](const char* owner,
                                  UnityResolve::Class* klass) {
            if (klass == nullptr) {
                return;
            }
            for (auto* field : klass->fields) {
                if (field == nullptr) {
                    continue;
                }
                std::ostringstream stream;
                stream << "[VR][produce] VERTICAL_ADV_FIELD owner=" << owner
                       << " name=" << field->name
                       << " type="
                       << (field->type != nullptr ? field->type->name : "?")
                       << " offset=" << field->offset
                       << " static=" << (field->static_field ? 1 : 0);
                static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
            }
        };
        logFields("VerticalAdvMenuView", menuClass);
        logFields("VerticalAdvMenuButtonView", buttonViewClass);
        logFields("ButtonBase", buttonBaseClass);

        if (initialize != nullptr) {
            std::array<unsigned char, 96> body{};
            const bool bodyReadable = TryCopyNativeBytes(
                initialize->function, body.data(), body.size());
            std::ostringstream stream;
            stream << "[VR][produce] VERTICAL_ADV_INITIALIZE_BYTES readable="
                   << (bodyReadable ? 1 : 0) << " count="
                   << (bodyReadable ? body.size() : 0U) << " hex=";
            if (bodyReadable) {
                stream << std::hex << std::setfill('0');
                for (unsigned char byte : body) {
                    stream << std::setw(2) << static_cast<unsigned int>(byte);
                }
            }
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }
    }
#endif

    UnityResolve::UnityType::Camera* mainCameraCache = nullptr;
    UnityResolve::UnityType::Transform* cameraTransformCache = nullptr;

    void CheckAndUpdateMainCamera(UnityResolve::UnityType::Camera* fallbackCamera = nullptr) {
        if (!IsLocalifyFreeCameraEnabled()) return;
        if (IsNativeObjectAlive(mainCameraCache) && IsNativeObjectAlive(cameraTransformCache)) return;

        // 优先使用游戏传入的真实相机（渲染回调 / FOV hook 捕获）。
        // 注意：不要用 UnityResolve 的 managed Invoke 直接调 Camera.get_main / get_transform，
        // Unity 6 下该调用缺少 MethodInfo* 参数，可能返回垃圾指针（崩溃根因）。
        if (!mainCameraCache || !IsNativeObjectAlive(mainCameraCache)) {
            mainCameraCache = fallbackCamera;
        }
        // 不再回退到 Camera.GetMain()/GetCurrent()：UnityResolve 的 managed Invoke 可能返回垃圾指针。
        // 相机只从游戏自己的调用捕获（EndCameraRendering 参数 / FOV hook / get_main hook）。
        // cameraTransformCache 由 Component.get_transform hook 在游戏调用时捕获。

        if (!mainCameraCache) {
            cameraTransformCache = nullptr;
        }
    }

    bool TryLookRotationQuat(const UnityResolve::UnityType::Vector3& forwardIn,
                             const UnityResolve::UnityType::Vector3& upIn,
                             UnityResolve::UnityType::Quaternion* outQuat);

    Il2cppUtils::Resolution_t GetResolution() {
        static auto GetResolution = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll", "UnityEngine",
                                                           "Screen", "get_currentResolution");
        return GetResolution->Invoke<Il2cppUtils::Resolution_t>();
    }

    Il2cppString* ToJsonStr(void* object) {
        static Il2cppString* (*toJsonStr)(void*) = nullptr;
		if (!toJsonStr) {
			toJsonStr = reinterpret_cast<Il2cppString * (*)(void*)>(Il2cppUtils::GetMethodPointer("Newtonsoft.Json.dll", "Newtonsoft.Json",
                "JsonConvert", "SerializeObject", { "*" }));
        }
        if (!toJsonStr) {
			return nullptr;
        }
		return toJsonStr(object);
    }

    DEFINE_HOOK(void, Unity_set_fieldOfView, (UnityResolve::UnityType::Camera* self, float value)) {
        if (IsLocalifyFreeCameraEnabled()) {
            // self 是游戏传的真实相机，直接捕获。
            // Cinemachine 每帧会把镜头参数应用到输出相机，这里拿到的就是实际游戏相机。
            if (self != mainCameraCache) {
                mainCameraCache = self;
                cameraTransformCache = nullptr;  // 相机变了，Transform 需要重新解析
            }
            if (self == mainCameraCache) {
                value = GKCamera::baseCamera.fov;
            }
        }
        Unity_set_fieldOfView_Orig(self, value);
    }

    DEFINE_HOOK(float, Unity_get_fieldOfView, (UnityResolve::UnityType::Camera* self)) {
        if (IsLocalifyFreeCameraEnabled()) {
            if (self != mainCameraCache) {
                mainCameraCache = self;
                cameraTransformCache = nullptr;
            }
            if (self == mainCameraCache) {
                static auto get_orthographic = reinterpret_cast<bool (*)(void*)>(Il2cppUtils::il2cpp_resolve_icall(
                        "UnityEngine.Camera::get_orthographic()"
                ));
                static auto set_orthographic = reinterpret_cast<bool (*)(void*, bool)>(Il2cppUtils::il2cpp_resolve_icall(
                        "UnityEngine.Camera::set_orthographic(System.Boolean)"
                ));

                for (const auto& i : UnityResolve::UnityType::Camera::GetAllCamera()) {
                    if (get_orthographic) {
                        // Log::DebugFmt("get_orthographic: %d", get_orthographic(i));
                    }
                    // set_orthographic(i, false);
                    Unity_set_fieldOfView_Orig(i, GKCamera::baseCamera.fov);
                }
                Unity_set_fieldOfView_Orig(self, GKCamera::baseCamera.fov);

                // Log::DebugFmt("main - get_orthographic: %d", get_orthographic(self));
                return GKCamera::baseCamera.fov;
            }
        }
        return Unity_get_fieldOfView_Orig(self);
    }

    // 从游戏自己的调用里捕获真实对象指针：
    // Camera.get_main / Component.get_transform 是带 MethodInfo* 尾参的 managed 方法，
    // 用 hook（而不是 UnityResolve 的 managed Invoke）才能拿到可靠结果。
    DEFINE_HOOK(UnityResolve::UnityType::Camera*, Camera_get_main, (void* mtd)) {
        auto ret = Camera_get_main_Orig(mtd);
#ifdef GKMS_WINDOWS
        try {
            TrackVrSourceCamera(ret);
        } catch (...) {
            ReportVrUnityHookException();
        }
        if (void* overrideCam = unityStereoRenderer.MainCameraOverride()) {
            return reinterpret_cast<UnityResolve::UnityType::Camera*>(
                overrideCam);
        }
#endif
        if (IsLocalifyFreeCameraEnabled() && ret && ret != mainCameraCache) {
            mainCameraCache = ret;
            cameraTransformCache = nullptr;
        }
        return ret;
    }

    DEFINE_HOOK(UnityResolve::UnityType::Transform*, Component_get_transform, (void* self, void* mtd)) {
        auto ret = Component_get_transform_Orig(self, mtd);
        if (IsLocalifyFreeCameraEnabled() && mainCameraCache && self == mainCameraCache) {
            cameraTransformCache = ret;
        }
        return ret;
    }

    UnityResolve::UnityType::Transform* cacheTrans = nullptr;
    UnityResolve::UnityType::Quaternion cacheRotation{};
    UnityResolve::UnityType::Vector3 cachePosition{};
    UnityResolve::UnityType::Vector3 cacheForward{};
    UnityResolve::UnityType::Vector3 cacheLookAt{};

    // 计算当前模式下的相机位置/朝向。返回 false 表示暂不可用（NaN/目标丢失）。
    bool ComputeFreeCameraPose(UnityResolve::UnityType::Vector3* pos, UnityResolve::UnityType::Quaternion* rot) {
        using Vector3 = UnityResolve::UnityType::Vector3;
        using Quaternion = UnityResolve::UnityType::Quaternion;

        Vector3 lookAt{};
        switch (GKCamera::GetCameraMode()) {
            case GKCamera::CameraMode::FREE: {
                *pos = GKCamera::baseCamera.GetPos();
                lookAt = GKCamera::baseCamera.GetLookAt();
            } break;
            case GKCamera::CameraMode::FIRST_PERSON: {
                if (!cacheTrans || !IsNativeObjectAlive(cacheTrans)) return false;
                *pos = GKCamera::CalcFirstPersonPosition(cachePosition, cacheForward, GKCamera::firstPersonPosOffset);
                lookAt = cacheLookAt;
            } break;
            case GKCamera::CameraMode::FOLLOW: {
                lookAt = GKCamera::CalcFollowModeLookAt(cachePosition, GKCamera::followPosOffset);
                *pos = GKCamera::CalcPositionFromLookAt(lookAt, GKCamera::followPosOffset);
            } break;
            default:
                return false;
        }

        if (!std::isfinite(pos->x) || !std::isfinite(pos->y) || !std::isfinite(pos->z) ||
            !std::isfinite(lookAt.x) || !std::isfinite(lookAt.y) || !std::isfinite(lookAt.z)) {
            return false;
        }

        const auto forward = Vector3(lookAt.x - pos->x, lookAt.y - pos->y, lookAt.z - pos->z);
        return TryLookRotationQuat(forward, Vector3(0, 1, 0), rot);
    }

    bool IsCameraControlTransform(void* self) {
        if (!self || !cameraTransformCache) return false;
        return self == cameraTransformCache;
    }

    // Cinemachine 把最终相机状态写入 Unity 相机的唯一入口（CinemachineBrain.PushStateToUnityCamera）。
    // CameraState 是大结构体，按值传递时经隐藏指针传入（hook 的 state 参数即指向该副本），
    // 直接改副本，游戏会用自己内部的原生路径应用——不需要调用任何 Transform API，绝对安全。
    // 字段偏移来自 hook-reference dump.cs：
    //   0x00 Lens.FieldOfView | 0x48 RawPosition | 0x54 RawOrientation
    //   0x64 PositionDampingBypass | 0x74 PositionCorrection | 0x80 OrientationCorrection
    DEFINE_HOOK(void, CinemachineBrain_PushStateToUnityCamera, (void* self, void* state, void* mtd)) {
        VR_PERF_SCOPE(whole, "camera.push-state-hook", [](std::string_view line) noexcept { WriteUnityCameraDiagnosticEvent(line); });
        VR_PERF_SCOPE(pose, "camera.pose-mod", [](std::string_view line) noexcept { WriteUnityCameraDiagnosticEvent(line); });

#ifdef GKMS_WINDOWS
        PendingCinemachineObservation cameraObservation{};
        try {
            cameraObservation = CaptureCinemachinePose(self, state);
            ApplyVrHeadPose(self, state);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        if (IsLocalifyFreeCameraEnabled() && state) {
            CheckAndUpdateMainCamera();

            using Vector3 = UnityResolve::UnityType::Vector3;
            using Quaternion = UnityResolve::UnityType::Quaternion;
            auto* rawPos = reinterpret_cast<Vector3*>(static_cast<unsigned char*>(state) + 0x48);
            auto* rawRot = reinterpret_cast<Quaternion*>(static_cast<unsigned char*>(state) + 0x54);
            auto* damping = reinterpret_cast<Vector3*>(static_cast<unsigned char*>(state) + 0x64);
            auto* posCorr = reinterpret_cast<Vector3*>(static_cast<unsigned char*>(state) + 0x74);
            auto* rotCorr = reinterpret_cast<Quaternion*>(static_cast<unsigned char*>(state) + 0x80);
            auto* fov = reinterpret_cast<float*>(static_cast<unsigned char*>(state) + 0x00);

            if (ComputeFreeCameraPose(rawPos, rawRot)) {
                *damping = Vector3(0, 0, 0);
                *posCorr = Vector3(0, 0, 0);
                *rotCorr = Quaternion(0, 0, 0, 1);
                *fov = GKCamera::baseCamera.fov;
            }
        }

        pose.Stop();
        VR_PERF_SCOPE(original, "camera.push-state-original", [](std::string_view line) noexcept { WriteUnityCameraDiagnosticEvent(line); });
        CinemachineBrain_PushStateToUnityCamera_Orig(self, state, mtd);
        original.Stop();
        VR_PERF_SCOPE(post, "camera.post-state", [](std::string_view line) noexcept { WriteUnityCameraDiagnosticEvent(line); });


#ifdef GKMS_WINDOWS
        try {
            while (gakumas::vr::ConsumeLivePauseToggle()) {
                gakumas::vr::ToggleLivePause();
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
        // Photo-scene flag upkeep + queued right-A shutter presses. Same
        // Unity-thread boundary as the pause toggle; alive-checked caches
        // only, no scene scans.
        try {
            gakumas::vr::UpdatePhotoSceneAndConsumeShutterRequests();
        } catch (...) {
            ReportVrUnityHookException();
        }
        // PushStateToUnityCamera is the point where Cinemachine has finished
        // composing the source pose. The A/B/C diagnostic ladder prepares
        // inactive Cameras here and admits them to Unity's ordinary camera list
        // only while no SRP context is active. No render API is called manually.
        try {
            void* outputCamera = ReadCinemachineOutputCamera(self);
            const bool selectedSource =
                outputCamera != nullptr && IsSelectedVrMainCamera(outputCamera);
            PublishCinemachineObservation(
                self, outputCamera, selectedSource, cameraObservation);
            const bool pipelineIdle = IsUnityRenderPipelineIdle();
            const bool queueDiagnosticHooksReady =
                unityCameraRenderHookReady.load(std::memory_order_acquire) &&
                unityRenderPipelineGuardReady.load(std::memory_order_acquire);
            if (queueDiagnosticHooksReady && selectedSource) {
                unityStereoRenderer.Tick(
                    outputCamera,
                    ReadVrStereoCameraFrame(),
                    pipelineIdle);
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

#ifdef GKMS_WINDOWS
    // ProFlare first copies scene data into a per-camera NativeArray and only
    // then schedules its update job.  Bracket that synchronous copy so the
    // local helper can resize the eye-owned element entry after the authored
    // fields have been populated.  No scene-shared ProFlare field is changed.
    DEFINE_HOOK(void, ProFlareBatchForSRPData_ScheduleFlares,
                (void* self, bool isDirty, void* mtd)) {
        if (!unityStereoRenderer.ProjectionEquivalentProFlareReady()) {
            ProFlareBatchForSRPData_ScheduleFlares_Orig(self, isDirty, mtd);
            return;
        }
        const ProFlareEyeScheduleContext previous =
            proFlareEyeScheduleContext;
        proFlareEyeScheduleContext = {};
        EnsureProFlareProjectionLayout();
        void* camera = nullptr;
        std::size_t eye = 2U;
        float projectionScaleX = 1.0F;
        float projectionScaleY = 1.0F;
        float appliedScaleX = 1.0F;
        float appliedScaleY = 1.0F;
        if (TryReadProFlareBatchCamera(self, &camera) &&
            unityStereoRenderer.IsEyeCamera(camera)) {
            const bool projectionValid =
                unityStereoRenderer.TryGetEyeProjectionScale(
                    camera, &projectionScaleX, &projectionScaleY, &eye) &&
                gakumas::vr::camera::TryBuildVerticalUniformProjectionScale(
                    projectionScaleY, appliedScaleX, appliedScaleY);
            if (eye < proFlareEyeScheduleSamples.size()) {
                const std::uint64_t sample =
                    ++proFlareEyeScheduleSamples[eye];
                if (projectionValid) {
                    const float previousX = proFlareLastLoggedScaleX[eye];
                    const float previousY = proFlareLastLoggedScaleY[eye];
                    const bool moved = previousX > 0.0F && previousY > 0.0F &&
                        (std::abs(std::log(appliedScaleX / previousX)) >=
                             0.02F ||
                         std::abs(std::log(appliedScaleY / previousY)) >=
                             0.02F);
                    const bool logBatch = sample == 1U ||
                        sample % 300U == 0U || moved;
                    proFlareEyeScheduleContext.active = true;
                    proFlareEyeScheduleContext.logBatch = logBatch;
                    proFlareEyeScheduleContext.eye = eye;
                    proFlareEyeScheduleContext.sample = sample;
                    proFlareEyeScheduleContext.projectionScaleX =
                        projectionScaleX;
                    proFlareEyeScheduleContext.projectionScaleY =
                        projectionScaleY;
                    proFlareEyeScheduleContext.scaleX = appliedScaleX;
                    proFlareEyeScheduleContext.scaleY = appliedScaleY;
                    if (logBatch) {
                        proFlareLastLoggedScaleX[eye] = appliedScaleX;
                        proFlareLastLoggedScaleY[eye] = appliedScaleY;
                    }
                } else if (!proFlareMissingProjectionLogged[eye]) {
                    proFlareMissingProjectionLogged[eye] = true;
                    std::ostringstream stream;
                    stream << "[VR][fov] PRO_FLARE_BATCH_AUTHORED eye="
                           << (eye == 0U ? "left" : "right")
                           << " sample=" << sample
                           << " reason=no-current-projection fixedFallback=0";
                    static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
                }
            }
        }

        ProFlareBatchForSRPData_ScheduleFlares_Orig(self, isDirty, mtd);

        const ProFlareEyeScheduleContext completed =
            proFlareEyeScheduleContext;
        proFlareEyeScheduleContext = previous;
        if (completed.active && completed.logBatch) {
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(6)
                   << "[VR][fov] PRO_FLARE_BATCH_APPLIED eye="
                   << (completed.eye == 0U ? "left" : "right")
                   << " sample=" << completed.sample
                   << " projectionScaleX=" << completed.projectionScaleX
                   << " projectionScaleY=" << completed.projectionScaleY
                   << " scaleX=" << completed.scaleX
                   << " scaleY=" << completed.scaleY
                   << " axisPolicy=vertical-uniform"
                   << " elements=" << completed.elements
                   << " firstSize=" << completed.firstSizeX << ','
                   << completed.firstSizeY
                   << " firstWritten=" << completed.firstWrittenX << ','
                   << completed.firstWrittenY
                   << " firstElementScale=" << completed.firstElementScale
                   << " firstAnamorphic="
                   << completed.firstAnamorphicX << ','
                   << completed.firstAnamorphicY << ','
                   << completed.firstAnamorphicZ
                   << " sharedGlobalWrites=0";
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }
    }

    DEFINE_HOOK(void, ProFlare_UpdateElementJobData,
                (UnityResolve::UnityType::Color tintColor,
                 float subScale,
                 float angle,
                 float position,
                 void* display0,
                 void* display1,
                 void* display2,
                 void* display3,
                 void* mtd)) {
        void* currentElement = nullptr;
        static_cast<void>(TryReadCurrentProFlareElement(
            display0, &currentElement));
        ProFlare_UpdateElementJobData_Orig(
            tintColor, subScale, angle, position,
            display0, display1, display2, display3, mtd);
        static_cast<void>(ScaleCurrentProFlareElementForEye(currentElement));
    }
#endif

    // Cinemachine 3（Unity 6）用 SetPositionAndRotation 一次性写相机位置+旋转，
    // set_position/set_rotation 拦截不到相机，必须在这里接管。
    DEFINE_HOOK(void, Unity_SetPositionAndRotation_Injected, (UnityResolve::UnityType::Transform* self,
                                                              UnityResolve::UnityType::Vector3* position,
                                                              UnityResolve::UnityType::Quaternion* rotation)) {
        if (IsLocalifyFreeCameraEnabled()) {
            CheckAndUpdateMainCamera();

            const auto isControl = IsCameraControlTransform(self);
            if (isControl) {
                UnityResolve::UnityType::Vector3 pos{};
                UnityResolve::UnityType::Quaternion rot{};
                if (ComputeFreeCameraPose(&pos, &rot)) {
                    *position = pos;
                    *rotation = rot;
                }
            }
        }

        return Unity_SetPositionAndRotation_Injected_Orig(self, position, rotation);
    }

    // 兜底：游戏若用局部坐标写相机（rig 挂载场景），位置同样接管。
    DEFINE_HOOK(void, Unity_set_localPosition_Injected, (UnityResolve::UnityType::Transform* self,
                                                         UnityResolve::UnityType::Vector3* data)) {
        if (IsLocalifyFreeCameraEnabled()) {
            CheckAndUpdateMainCamera();

            const auto isControl = IsCameraControlTransform(self);
            if (isControl) {
                const auto cameraMode = GKCamera::GetCameraMode();
                if (cameraMode == GKCamera::CameraMode::FIRST_PERSON) {
                    if (cacheTrans && IsNativeObjectAlive(cacheTrans)) {
                        *data = GKCamera::CalcFirstPersonPosition(cachePosition, cacheForward, GKCamera::firstPersonPosOffset);
                    }
                }
                else if (cameraMode == GKCamera::CameraMode::FOLLOW) {
                    auto newLookAtPos = GKCamera::CalcFollowModeLookAt(cachePosition, GKCamera::followPosOffset);
                    auto pos = GKCamera::CalcPositionFromLookAt(newLookAtPos, GKCamera::followPosOffset);
                    data->x = pos.x;
                    data->y = pos.y;
                    data->z = pos.z;
                }
                else {
                    auto& origCameraPos = GKCamera::baseCamera.pos;
                    data->x = origCameraPos.x;
                    data->y = origCameraPos.y;
                    data->z = origCameraPos.z;
                }
            }
        }

        return Unity_set_localPosition_Injected_Orig(self, data);
    }

    DEFINE_HOOK(void, Unity_set_rotation_Injected, (UnityResolve::UnityType::Transform* self, UnityResolve::UnityType::Quaternion* value)) {
        if (IsLocalifyFreeCameraEnabled()) {
            const auto isControl = IsCameraControlTransform(self);
            if (isControl) {
                const auto cameraMode = GKCamera::GetCameraMode();
                if (cameraMode == GKCamera::CameraMode::FIRST_PERSON) {
                    if (cacheTrans && IsNativeObjectAlive(cacheTrans)) {
                        if (GKCamera::GetFirstPersonRoll() == GKCamera::FirstPersonRoll::ENABLE_ROLL) {
                            *value = cacheRotation;
                        }
                        else {
                            static GakumasLocal::Misc::FixedSizeQueue<float> recordsY(60);
                            const auto newY = GKCamera::CheckNewY(cacheLookAt, true, recordsY);
                            UnityResolve::UnityType::Vector3 newCacheLookAt{cacheLookAt.x, newY, cacheLookAt.z};
                            const auto pos = GKCamera::CalcFirstPersonPosition(
                                cachePosition, cacheForward, GKCamera::firstPersonPosOffset);
                            UnityResolve::UnityType::Quaternion q{};
                            const auto forward = UnityResolve::UnityType::Vector3(
                                newCacheLookAt.x - pos.x, newCacheLookAt.y - pos.y, newCacheLookAt.z - pos.z);
                            if (TryLookRotationQuat(forward, UnityResolve::UnityType::Vector3(0, 1, 0), &q)) {
                                *value = q;
                            }
                        }
                    }
                }
                else if (cameraMode == GKCamera::CameraMode::FOLLOW) {
                    auto newLookAtPos = GKCamera::CalcFollowModeLookAt(cachePosition,
                                                                       GKCamera::followPosOffset, true);
                    const auto pos = GKCamera::CalcPositionFromLookAt(newLookAtPos, GKCamera::followPosOffset);
                    UnityResolve::UnityType::Quaternion q{};
                    const auto forward = UnityResolve::UnityType::Vector3(
                        newLookAtPos.x - pos.x, newLookAtPos.y - pos.y, newLookAtPos.z - pos.z);
                    if (TryLookRotationQuat(forward, UnityResolve::UnityType::Vector3(0, 1, 0), &q)) {
                        *value = q;
                    }
                }
                else {
                    auto& origCameraLookat = GKCamera::baseCamera.lookAt;
                    auto& origCameraPos = GKCamera::baseCamera.pos;
                    UnityResolve::UnityType::Quaternion q{};
                    const auto forward = UnityResolve::UnityType::Vector3(
                        origCameraLookat.x - origCameraPos.x,
                        origCameraLookat.y - origCameraPos.y,
                        origCameraLookat.z - origCameraPos.z);
                    if (TryLookRotationQuat(forward, UnityResolve::UnityType::Vector3(0, 1, 0), &q)) {
                        *value = q;
                    }
                }
            }
        }
        return Unity_set_rotation_Injected_Orig(self, value);
    }

    DEFINE_HOOK(void, Unity_set_position_Injected, (UnityResolve::UnityType::Transform* self, UnityResolve::UnityType::Vector3* data)) {
        if (IsLocalifyFreeCameraEnabled()) {
            CheckAndUpdateMainCamera();

            const auto isControl = IsCameraControlTransform(self);
            if (isControl) {
                const auto cameraMode = GKCamera::GetCameraMode();
                if (cameraMode == GKCamera::CameraMode::FIRST_PERSON) {
                    if (cacheTrans && IsNativeObjectAlive(cacheTrans)) {
                        *data = GKCamera::CalcFirstPersonPosition(cachePosition, cacheForward, GKCamera::firstPersonPosOffset);
                    }

                }
                else if (cameraMode == GKCamera::CameraMode::FOLLOW) {
                    auto newLookAtPos = GKCamera::CalcFollowModeLookAt(cachePosition, GKCamera::followPosOffset);
                    auto pos = GKCamera::CalcPositionFromLookAt(newLookAtPos, GKCamera::followPosOffset);
                    data->x = pos.x;
                    data->y = pos.y;
                    data->z = pos.z;
                }
                else {
                    //Log::DebugFmt("MainCamera set pos: %f, %f, %f", data->x, data->y, data->z);
                    auto& origCameraPos = GKCamera::baseCamera.pos;
                    data->x = origCameraPos.x;
                    data->y = origCameraPos.y;
                    data->z = origCameraPos.z;
                }
            }
        }

        return Unity_set_position_Injected_Orig(self, data);
    }

    // 用 forward/up 构造 Unity 兼容的四元数（等价 Quaternion.LookRotation）。
    // 返回 false 表示 forward 与 up 平行（垂直看天/看地），此时不应写入旋转，保持当前朝向。
    bool TryLookRotationQuat(const UnityResolve::UnityType::Vector3& forwardIn,
                             const UnityResolve::UnityType::Vector3& upIn,
                             UnityResolve::UnityType::Quaternion* outQuat) {
        using Vector3 = UnityResolve::UnityType::Vector3;
        using Quaternion = UnityResolve::UnityType::Quaternion;

        Vector3 forward = forwardIn;
        const auto fwdLenSq = forward.x * forward.x + forward.y * forward.y + forward.z * forward.z;
        if (fwdLenSq < 1e-8f) {
            return false;
        }
        const auto invFwdLen = 1.0f / std::sqrt(fwdLenSq);
        forward = Vector3(forward.x * invFwdLen, forward.y * invFwdLen, forward.z * invFwdLen);

        Vector3 right = Vector3(
            upIn.y * forward.z - upIn.z * forward.y,
            upIn.z * forward.x - upIn.x * forward.z,
            upIn.x * forward.y - upIn.y * forward.x);
        const auto rightLenSq = right.x * right.x + right.y * right.y + right.z * right.z;
        if (rightLenSq < 1e-8f) {
            return false;
        }
        const auto invRightLen = 1.0f / std::sqrt(rightLenSq);
        right = Vector3(right.x * invRightLen, right.y * invRightLen, right.z * invRightLen);

        const Vector3 up = Vector3(
            forward.y * right.z - forward.z * right.y,
            forward.z * right.x - forward.x * right.z,
            forward.x * right.y - forward.y * right.x);

        // 旋转矩阵列向量（right/up/forward）-> 标准矩阵转四元数
        const float m00 = right.x, m01 = up.x, m02 = forward.x;
        const float m10 = right.y, m11 = up.y, m12 = forward.y;
        const float m20 = right.z, m21 = up.z, m22 = forward.z;

        const float tr = m00 + m11 + m22;
        if (tr > 0.0f) {
            const float s = std::sqrt(tr + 1.0f) * 2.0f;
            *outQuat = Quaternion((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s);
            return true;
        }
        if (m00 > m11 && m00 > m22) {
            const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            *outQuat = Quaternion(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
            return true;
        }
        if (m11 > m22) {
            const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            *outQuat = Quaternion((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
            return true;
        }
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        *outQuat = Quaternion((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
        return true;
    }

#ifdef GKMS_WINDOWS
    DEFINE_HOOK(void*, InternalSetOrientationAsync, (void* retstr, void* self, int type, void* c, void* tc, void* mtd)) {
        switch (Config::gameOrientation) {
        case 1: type = 0x2; break;  // FixedPortrait
        case 2: type = 0x3; break;  // FixedLandscape
        default: break;
        }
        return InternalSetOrientationAsync_Orig(retstr, self, type, c, tc, mtd);
    }
#else
    DEFINE_HOOK(void*, InternalSetOrientationAsync, (void* self, int type, void* c, void* tc, void* mtd)) {
        switch (Config::gameOrientation) {
        case 1: type = 0x2; break;  // FixedPortrait
        case 2: type = 0x3; break;  // FixedLandscape
        default: break;
        }
        return InternalSetOrientationAsync_Orig(self, type, c, tc, mtd);
    }
#endif

    using gakumas::vr::perf::srpPerformance;

    struct SrpNativeCount {
        std::atomic<std::uint64_t> calls{0}, sampled{0};
        std::atomic<unsigned long> firstTid{0}, lastTid{0};
        std::atomic<std::uintptr_t> firstCaller{0};
    };
    std::array<SrpNativeCount, 10> srpNativeCounts;
    constexpr const char* srpNativeNames[]{"wrapper.cull", "wrapper.submit", "wrapper.command",
        "native.cull", "native.submit", "native.execute-command-buffer",
        "pipeline.initialize-rendering-data", "pipeline.add-render-passes",
        "pipeline.pre-cull-passes", "ui.draw-base"};

    void CountSrpNative(std::size_t index, void* caller) noexcept {
        auto& c = srpNativeCounts[index];
        const auto tid = GetCurrentThreadId();
        c.calls.fetch_add(1, std::memory_order_relaxed);
        if (srpPerformance.active) c.sampled.fetch_add(1, std::memory_order_relaxed);
        unsigned long zeroTid = 0;
        c.firstTid.compare_exchange_strong(zeroTid, tid, std::memory_order_relaxed);
        c.lastTid.store(tid, std::memory_order_relaxed);
        std::uintptr_t zero = 0;
        c.firstCaller.compare_exchange_strong(zero,
            reinterpret_cast<std::uintptr_t>(caller), std::memory_order_relaxed);
    }

    void DumpSrpBytes(const char* name, const void* address, std::size_t count) {
        if (!Config::vrDiagnosticsStartupEnabled || !address) return;
        // Bounded code evidence, SEH-guarded read; no field writes or calls.
        std::array<unsigned char, 256> bytes{};
        for (std::size_t offset = 0; offset < (std::min)(count, std::size_t{8192}); offset += bytes.size()) {
            const auto size = (std::min)(bytes.size(), count - offset);
            const auto* p = static_cast<const unsigned char*>(address) + offset;
            const bool ok = TryCopyNativeBytes(p, bytes.data(), size);
            std::ostringstream line;
            line.imbue(std::locale::classic());
            line << "[VR][perf] SRP_PERF_BYTES name=" << name << " address=0x"
                 << std::hex << reinterpret_cast<std::uintptr_t>(p)
                 << " readable=" << ok << " raw=" << std::setfill('0');
            if (ok) for (std::size_t i = 0; i < size; ++i) line << std::setw(2) << unsigned(bytes[i]);
            WriteUnityCameraDiagnosticEvent(line.str());
            if (!ok) break;
        }
    }

    void EmitSrpNativeCounts() {
        static std::array<bool, 10> callerDumped{}; // owner-thread flush only
        for (std::size_t i = 0; i < srpNativeCounts.size(); ++i) {
            auto& c = srpNativeCounts[i];
            const auto caller = c.firstCaller.load(std::memory_order_relaxed);
            char line[384]{};
            std::snprintf(line, sizeof(line),
                "[VR][perf] SRP_PERF_HITS name=%s calls=%llu sampled=%llu firstTid=%lu lastTid=%lu firstCaller=0x%llx",
                srpNativeNames[i], static_cast<unsigned long long>(c.calls.load()),
                static_cast<unsigned long long>(c.sampled.load()), c.firstTid.load(), c.lastTid.load(),
                static_cast<unsigned long long>(caller));
            WriteUnityCameraDiagnosticEvent(line);
            if (caller && !callerDumped[i]) {
                callerDumped[i] = true;
                DumpSrpBytes(srpNativeNames[i], reinterpret_cast<void*>(caller - 64), 256);
            }
        }
    }

    // Native icall ABI: no MethodInfo. Argument registers are forwarded intact.
    // These signatures are evidenced by the exact UnityPlayer/PDB disassembly
    // and current live managed signature; binding and bytes are checked below.
    DEFINE_HOOK(void, SrpNativeCull, (void* parameters, void* context, void* results)) {
        auto* caller = _ReturnAddress();
        CountSrpNative(3, caller);
        gakumas::vr::perf::SrpSpan span(srpPerformance, "native.cull", reinterpret_cast<std::uintptr_t>(caller));
        SrpNativeCull_Orig(parameters, context, results);
    }
    DEFINE_HOOK(void, SrpNativeSubmit, (void* context)) {
        auto* caller = _ReturnAddress();
        CountSrpNative(4, caller);
        gakumas::vr::perf::SrpSpan span(srpPerformance, "native.submit", reinterpret_cast<std::uintptr_t>(caller));
        SrpNativeSubmit_Orig(context);
    }
    DEFINE_HOOK(void, SrpNativeCommand, (void* context, void* commandBuffer)) {
        auto* caller = _ReturnAddress();
        CountSrpNative(5, caller);
        gakumas::vr::perf::SrpSpan span(srpPerformance, "native.execute-command-buffer", reinterpret_cast<std::uintptr_t>(caller));
        SrpNativeCommand_Orig(context, commandBuffer);
    }

    // Exact signatures and static/instance shape are checked against the live method
    // table before installation. These only forward; no fields/state are changed.
    DEFINE_HOOK(void, SrpPerfCull,
                (void* parameters, void* context, void* results, void* method)) {
        CountSrpNative(0, _ReturnAddress());
        gakumas::vr::perf::SrpSpan span(srpPerformance, "wrapper.cull");
        SrpPerfCull_Orig(parameters, context, results, method);
    }
    DEFINE_HOOK(void, SrpPerfSubmit, (void* context, void* method)) {
        CountSrpNative(1, _ReturnAddress());
        gakumas::vr::perf::SrpSpan span(srpPerformance, "wrapper.submit");
        SrpPerfSubmit_Orig(context, method);
    }
    DEFINE_HOOK(void, SrpPerfExecuteCommandBuffer,
                (void* context, void* commandBuffer, void* method)) {
        CountSrpNative(2, _ReturnAddress());
        gakumas::vr::perf::SrpSpan span(srpPerformance, "wrapper.command");
        SrpPerfExecuteCommandBuffer_Orig(context, commandBuffer, method);
    }

    // dev.398 live signatures + actual pipeline call sites are
    // archived. Only observe/forward these existing pipeline calls.
    DEFINE_HOOK(void, SrpInitializeRenderingData,
                (void* settings, void* cameraData, void* cullResults, void* command,
                 void* renderingData, void* method)) {
        CountSrpNative(6, _ReturnAddress());
        gakumas::vr::perf::SrpSpan span(srpPerformance, "pipeline.initialize-rendering-data");
        SrpInitializeRenderingData_Orig(settings, cameraData, cullResults, command, renderingData, method);
    }
    DEFINE_HOOK(void, SrpAddRenderPasses, (void* self, void* renderingData, void* method)) {
        CountSrpNative(7, _ReturnAddress());
        gakumas::vr::perf::SrpSpan span(srpPerformance, "pipeline.add-render-passes");
        SrpAddRenderPasses_Orig(self, renderingData, method);
    }
    DEFINE_HOOK(void, SrpPreCullPasses, (void* self, void* cameraData, void* method)) {
        CountSrpNative(8, _ReturnAddress());
        gakumas::vr::perf::SrpSpan span(srpPerformance, "pipeline.pre-cull-passes");
        SrpPreCullPasses_Orig(self, cameraData, method);
    }
    DEFINE_HOOK(void, SrpUiDrawBase,
                (void* self, void* context, void* renderingData, void* method)) {
        CountSrpNative(9, _ReturnAddress());
        gakumas::vr::perf::SrpSpan span(srpPerformance, "ui.draw-base");
        SrpUiDrawBase_Orig(self, context, renderingData, method);
    }

    DEFINE_HOOK(void, BeginContextRendering, (void* ctx, void* cameras, void* method)) {
        if (unityRenderPipelineGuardReady.load(std::memory_order_acquire) &&
            unityRenderPipelineDepth !=
                (std::numeric_limits<std::uint32_t>::max)()) {
            ++unityRenderPipelineDepth;
        }
        BeginContextRendering_Orig(ctx, cameras, method);
    }

    DEFINE_HOOK(void, EndContextRendering, (void* ctx, void* cameras, void* method)) {
        EndContextRendering_Orig(ctx, cameras, method);
        if (unityRenderPipelineGuardReady.load(std::memory_order_acquire) &&
            unityRenderPipelineDepth != 0) {
            --unityRenderPipelineDepth;
        }
    }

    DEFINE_HOOK(void, BeginCameraRendering, (void* ctx, void* camera, void* method)) {
#ifdef GKMS_WINDOWS
        if (srpPerformance.active) srpPerformance.BeginCamera(
            reinterpret_cast<std::uintptr_t>(camera), unityStereoRenderer.ClassifyCamera(camera));
#endif
        gakumas::vr::perf::SrpSpan callback(srpPerformance, "mod.camera-begin");
#ifdef GKMS_WINDOWS
        try {
            unityStereoRenderer.OnBeginCamera(camera);
            if (gakumas::vr::LiveSourcePhotoProtectionActive()) {
                SetFpHeadColorSkip(
                    unityStereoRenderer.IsEyeCamera(camera) &&
                        gakumas::vr::camera::IsVrFreeCameraFirstPerson(),
                    "auto-photo-camera-begin");
            }
            if (std::string_view(unityStereoRenderer.ClassifyCamera(camera)) ==
                "left") {
                ResetActorShadowLeftReuse();
            }
            BeginEyeRenderPassTrace(camera);
            gakumas::vr::GripTraceBeginCamera(camera, unityStereoRenderer);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        callback.Stop();
        gakumas::vr::perf::SrpSpan original(srpPerformance, "event.camera-begin");
        BeginCameraRendering_Orig(ctx, camera, method);
    }

    DEFINE_HOOK(void, BeginCameraRenderingManager, (void* ctx, void* camera, void* method)) {
#ifdef GKMS_WINDOWS
        if (srpPerformance.active) srpPerformance.BeginCamera(
            reinterpret_cast<std::uintptr_t>(camera), unityStereoRenderer.ClassifyCamera(camera));
#endif
        gakumas::vr::perf::SrpSpan callback(srpPerformance, "mod.camera-begin-manager");
#ifdef GKMS_WINDOWS
        try {
            unityStereoRenderer.OnBeginCamera(camera);
            if (gakumas::vr::LiveSourcePhotoProtectionActive()) {
                SetFpHeadColorSkip(
                    unityStereoRenderer.IsEyeCamera(camera) &&
                        gakumas::vr::camera::IsVrFreeCameraFirstPerson(),
                    "auto-photo-camera-begin");
            }
            if (std::string_view(unityStereoRenderer.ClassifyCamera(camera)) ==
                "left") {
                ResetActorShadowLeftReuse();
            }
            BeginEyeRenderPassTrace(camera);
            gakumas::vr::GripTraceBeginCamera(camera, unityStereoRenderer);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        callback.Stop();
        gakumas::vr::perf::SrpSpan original(srpPerformance, "event.camera-begin-manager");
        BeginCameraRenderingManager_Orig(ctx, camera, method);
    }

    DEFINE_HOOK(void, EndCameraRendering, (void* ctx, void* camera, void* method)) {
        gakumas::vr::perf::SrpSpan original(srpPerformance, "event.camera-end");
        EndCameraRendering_Orig(ctx, camera, method);
        original.Stop();
        gakumas::vr::perf::SrpSpan callback(srpPerformance, "mod.camera-end");

        bool ownedByVrQueue = false;
#ifdef GKMS_WINDOWS
        try {
            EndEyeRenderPassTrace(camera);
            gakumas::vr::GripTraceEndCamera(camera);
            ownedByVrQueue = unityStereoRenderer.OnEndCamera(camera);
            if (gakumas::vr::LiveSourcePhotoProtectionActive()) {
                SetFpHeadColorSkip(gakumas::vr::camera::IsVrFreeCameraFirstPerson(),
                    "auto-photo-camera-end");
            }
            if (!ownedByVrQueue) {
                ObserveUnityCameraRender(camera);
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif

        if (!ownedByVrQueue && IsLocalifyFreeCameraEnabled()) {
            // Camera.main 可能拿不到 3.0.0 实际渲染相机，用当前正在渲染的相机兜底
            CheckAndUpdateMainCamera(static_cast<UnityResolve::UnityType::Camera*>(camera));
            if (mainCameraCache && IsNativeObjectAlive(mainCameraCache)) {
                // 注意：不能在渲染回调里写相机 Transform（UnityPlayer 会崩），
                // 位置/朝向由独立驱动线程负责，这里只保留 FOV/近裁剪等属性设置。
                Unity_set_fieldOfView_Orig(mainCameraCache, GKCamera::baseCamera.fov);
                if (GKCamera::GetCameraMode() == GKCamera::CameraMode::FIRST_PERSON) {
                    mainCameraCache->SetNearClipPlane(0.001f);
                }
            }
        }
        callback.Stop();
        if (srpPerformance.active) srpPerformance.EndCamera(
            reinterpret_cast<std::uintptr_t>(camera));
    }

    DEFINE_HOOK(void, ScriptableRenderer_ExecuteRenderPass,
                (void* self, void* context, void* renderPass,
                  void* renderingData, void* method)) {
        gakumas::vr::perf::SrpSpan pass(srpPerformance, "pass.unknown",
            reinterpret_cast<std::uintptr_t>(renderPass));
        gakumas::vr::perf::SrpSpan before(srpPerformance, "mod.pass-before");
        if (srpPerformance.active && renderPass != nullptr) {
            const auto* klass = Il2cppUtils::get_class_from_instance(renderPass);
            pass.Describe(klass != nullptr ? klass->name : "pass.unknown",
                ReadEyeRenderPassEvent(renderPass));
        }
#ifdef GKMS_WINDOWS
        if (Config::vrDiagnosticsStartupEnabled) {
            gakumas::vr::GripTracePass(self, renderPass, context,
                ReadEyeRenderPassEvent(renderPass), false);
        }
        const EyeRenderPassExecutionContext previousExecution =
            eyeRenderPassExecution;
        try {
            RecordEyeRenderPass(renderPass);
            if (eyeRenderPassTraceActive && renderPass != nullptr) {
                eyeRenderPassExecution = {
                    renderPass,
                    Il2cppUtils::get_class_from_instance(renderPass),
                    ReadEyeRenderPassEvent(renderPass),
                };
            } else {
                eyeRenderPassExecution = {};
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        before.Stop();
        gakumas::vr::perf::SrpSpan original(srpPerformance, "pass.original");
        ScriptableRenderer_ExecuteRenderPass_Orig(
            self, context, renderPass, renderingData, method);
        original.Stop();
        gakumas::vr::perf::SrpSpan after(srpPerformance, "mod.pass-after");
#ifdef GKMS_WINDOWS
        if (Config::vrDiagnosticsStartupEnabled) {
            gakumas::vr::GripTracePass(self, renderPass, context,
                ReadEyeRenderPassEvent(renderPass), true);
        }
        try {
            gakumas::vr::ObserveSmaaT2xRenderPass(
                renderPass,
                context,
                renderingData,
                ReadEyeRenderPassEvent(renderPass),
                unityStereoRenderer);
        } catch (...) {
            ReportVrUnityHookException();
        }
        eyeRenderPassExecution = previousExecution;
#endif
    }

    DEFINE_HOOK(void, Blitter_DrawTriangle,
                (void* cmd, void* material, int shaderPass, void* method)) {
        Blitter_DrawTriangle_Orig(cmd, material, shaderPass, method);
#ifdef GKMS_WINDOWS
        try {
            if (!eyeCommandDrawProceduralHookReady.load(
                    std::memory_order_acquire)) {
                ObserveEyeFullscreenDraw(
                    EyeFullscreenDrawApi::BlitterTriangle,
                    material, shaderPass, 0, 3, 1);
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, Blitter_DrawQuad,
                (void* cmd, void* material, int shaderPass, void* method)) {
        Blitter_DrawQuad_Orig(cmd, material, shaderPass, method);
#ifdef GKMS_WINDOWS
        try {
            if (!eyeCommandDrawProceduralHookReady.load(
                    std::memory_order_acquire)) {
                ObserveEyeFullscreenDraw(
                    EyeFullscreenDrawApi::BlitterQuad,
                    material, shaderPass, 2, 4, 1);
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, CommandBuffer_DrawProcedural,
                (void* self, void* matrix, void* material, int shaderPass,
                 int topology, int vertexCount, int instanceCount,
                 void* properties, void* method)) {
        CommandBuffer_DrawProcedural_Orig(
            self, matrix, material, shaderPass, topology, vertexCount,
            instanceCount, properties, method);
        gakumas::vr::perf::SrpSpan observe(srpPerformance, "mod.draw-procedural-observe");
#ifdef GKMS_WINDOWS
        try {
            const bool fullScreenShape = instanceCount == 1 &&
                ((topology == 0 && vertexCount == 3) ||
                 (topology == 2 && vertexCount == 4));
            if (fullScreenShape) {
                gakumas::vr::GripTraceFullscreen(material, shaderPass);
                ObserveEyeFullscreenDraw(
                    EyeFullscreenDrawApi::CommandDrawProcedural,
                    material, shaderPass, topology, vertexCount,
                    instanceCount);
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, CommandBuffer_Blit,
                (void* self, void* source, void* destination, void* material,
                 int shaderPass, void* method)) {
        CommandBuffer_Blit_Orig(
            self, source, destination, material, shaderPass, method);
        gakumas::vr::perf::SrpSpan observe(srpPerformance, "mod.blit-observe");
#ifdef GKMS_WINDOWS
        gakumas::vr::GripTraceFullscreen(material, shaderPass);
        try {
            ObserveEyeFullscreenDraw(
                EyeFullscreenDrawApi::CommandBlit,
                material, shaderPass, -1, -1, -1);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, RenderObjectsPass_Execute,
                (void* self, void* context, void* renderingData, void* method)) {
#ifdef GKMS_WINDOWS
        try {
            ObserveEyeRenderObjectsPass(self);
            void* camera = unityStereoRenderer.CurrentCamera();
            const bool isEye = unityStereoRenderer.IsEyeCamera(camera);
            const char* role = unityStereoRenderer.ClassifyCamera(camera);
            const int event = ReadEyeRenderPassEvent(self);
            const bool skip = ShouldSkipEyeRenderObjectsPass(self);
            LogEyeRenderObjectsPolicy(
                role, camera, self, event, isEye, skip, !skip);
            if (skip) {
                return;
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        RenderObjectsPass_Execute_Orig(self, context, renderingData, method);
    }

    DEFINE_HOOK(void, ProFlareRenderingSystem_ComputeAndRenderFlares,
                (void* self, void* cmd, void* camera, void* method)) {
#ifdef GKMS_WINDOWS
        try {
            LogProFlareRender(camera);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        ProFlareRenderingSystem_ComputeAndRenderFlares_Orig(
            self, cmd, camera, method);
    }

    DEFINE_HOOK(void, DoRenderLoopInternal,
                (void* pipelineAsset, std::intptr_t loopPtr,
                 void* renderRequest, void* method)) {
        const auto perfSink = [](std::string_view line) noexcept { WriteUnityCameraDiagnosticEvent(line); };
        VR_PERF_SCOPE(whole, "unity.render-hook", perfSink);
        VR_PERF_SCOPE(before, "unity.before-srp", perfSink);
        const bool outerNormalLoop = renderRequest == nullptr &&
            unityRenderLoopDepth == 0U;
        ++unityRenderLoopDepth;
#ifdef GKMS_WINDOWS
        // The current Unity 6000 player invokes the camera callbacks from this
        // native render-loop bridge but does not pass through the managed
        // Begin/EndContextRendering event wrappers we can hook. Treat the
        // outer normal DoRenderLoop invocation itself as the authoritative
        // frame boundary so owned EndCamera callbacks can be attributed.
        if (outerNormalLoop &&
            unityRenderPipelineGuardReady.load(std::memory_order_acquire)) {
            gakumas::vr::VrRuntime::Instance().EnsureGraphicsBegun();
            if (!unityRenderLoopHookHitLogged.exchange(
                    true, std::memory_order_acq_rel)) {
                std::ostringstream boundary;
                boundary << "[VR][stereo] RENDER_LOOP_HOOK_HIT tid="
                         << GetCurrentThreadId()
                         << " loopPtr=0x" << std::hex
                         << static_cast<std::uintptr_t>(loopPtr);
                WriteUnityCameraDiagnosticEvent(boundary.str());
            }
            try {
                unityStereoRenderer.OnBeginContext();
            } catch (...) {
                ReportVrUnityHookException();
            }
        }
#endif
        before.Stop();
        static thread_local gakumas::vr::perf::Accumulator renderTiming;
        gakumas::vr::perf::Scope renderScope(renderTiming,
            Config::vrDiagnosticsStartupEnabled && outerNormalLoop,
            "unity.render-loop-original",
            [](std::string_view line) noexcept { WriteUnityCameraDiagnosticEvent(line); });
        bool traceSelected = false;
#ifdef GKMS_WINDOWS
        if (outerNormalLoop && Config::vrDiagnosticsStartupEnabled) {
            traceSelected = srpPerformance.Select(true);
            if (traceSelected) {
                gakumas::vr::pose::PoseAdmission admission{};
                (void)gakumas::vr::VrRuntime::Instance().CurrentPoseAdmission(admission);
                srpPerformance.Begin(admission.frameId, admission.sessionGeneration,
                    admission.inputEpoch);
            }
        }
#endif
        DoRenderLoopInternal_Orig(pipelineAsset, loopPtr, renderRequest, method);
        if (traceSelected) srpPerformance.End();
        renderScope.Stop();
#ifdef GKMS_WINDOWS
        if (traceSelected) {
            const auto begin = gakumas::vr::perf::SrpTrace::Clock::now();
            srpPerformance.Emit(GetCurrentThreadId(),
                [](std::string_view line) noexcept { WriteUnityCameraDiagnosticEvent(line); });
            EmitSrpNativeCounts();
            char line[192]{};
            std::snprintf(line, sizeof(line),
                "[VR][perf] SRP_PERF_FLUSH tid=%lu loop=%llu wallMs=%.6f",
                GetCurrentThreadId(), static_cast<unsigned long long>(srpPerformance.loop),
                gakumas::vr::perf::SrpTrace::Ms(begin, gakumas::vr::perf::SrpTrace::Clock::now()));
            WriteUnityCameraDiagnosticEvent(line);
        }
#endif
        if (unityRenderLoopDepth != 0U) {
            --unityRenderLoopDepth;
        }
#ifdef GKMS_WINDOWS
        // This is the first hook point after Unity has returned from the whole
        // top-level SRP render loop. Unlike EndContextRendering, managed camera
        // mutation and native texture access are no longer inside an SRP callback.
        if (outerNormalLoop && unityRenderLoopDepth == 0U &&
            unityRenderPipelineGuardReady.load(std::memory_order_acquire)) {
            try {
                VR_PERF_SCOPE(contextEnd, "unity.after-srp-context", perfSink);
                unityStereoRenderer.OnEndContext(true);
                contextEnd.Stop();
                unityStereoRenderer.OnRenderLoopCompleted();
                VR_PERF_SCOPE(driver, "unity.after-srp-driver", perfSink);
                gakumas::vr::FrameLoopDriverAfterSrp();
            } catch (...) {
                ReportVrUnityHookException();
            }
        }
#endif
    }

    DEFINE_HOOK(void, Unity_set_targetFrameRate, (int value)) {
        const auto configFps = Config::targetFrameRate;
        return Unity_set_targetFrameRate_Orig(configFps == 0 ? value : configFps);
    }

    std::unordered_map<void*, std::string> loadHistory{};


    DEFINE_HOOK(void*, AssetBundle_LoadAsset, (void* self, Il2cppString* name, void* type)) {
        auto result = AssetBundle_LoadAsset_Orig(self, name, type);
        if (name) {
            result = ReplaceTextureOrSpriteAsset(result, name->ToString());
        }
        return result;
    }

    DEFINE_HOOK(void*, AssetBundle_LoadAssetAsync, (void* self, Il2cppString* name, void* type)) {
        // Log::InfoFmt("AssetBundle_LoadAssetAsync: %s, type: %s", name->ToString().c_str());
        auto ret = AssetBundle_LoadAssetAsync_Orig(self, name, type);
        if (ret && name) {
            loadHistory.emplace(ret, name->ToString());
        }
        return ret;
    }

    DEFINE_HOOK(void*, AssetBundleRequest_GetResult, (void* self)) {
        auto result = AssetBundleRequest_GetResult_Orig(self);
        if (const auto iter = loadHistory.find(self); iter != loadHistory.end()) {
            const auto name = iter->second;
            loadHistory.erase(iter);

            // const auto assetClass = Il2cppUtils::get_class_from_instance(result);
            // Log::InfoFmt("AssetBundleRequest_GetResult: %s, type: %s", name.c_str(), static_cast<Il2CppClassHead*>(assetClass)->name);
            result = ReplaceTextureOrSpriteAsset(result, name);
        }
        return result;
    }

    DEFINE_HOOK(void*, AssetBundleRequest_get_asset, (void* self)) {
        std::string name;
        if (const auto iter = loadHistory.find(self); iter != loadHistory.end()) {
            name = iter->second;
            loadHistory.erase(iter);
        }

        auto result = AssetBundleRequest_get_asset_Orig(self);
        if (!name.empty()) {
            result = ReplaceTextureOrSpriteAsset(result, name);
        }
        return result;
    }

    DEFINE_HOOK(void*, AssetBundleRequest_get_allAssets, (void* self)) {
        auto result = AssetBundleRequest_get_allAssets_Orig(self);
        ReplaceAllAssetTextures(result);
        return result;
    }

    DEFINE_HOOK(void*, Resources_Load, (Il2cppString* path, void* systemTypeInstance)) {
        auto ret = Resources_Load_Orig(path, systemTypeInstance);

        // if (ret) Log::DebugFmt("Resources_Load: %s, type: %s", path->ToString().c_str(), Il2cppUtils::get_class_from_instance(ret)->name);
        if (path) {
            ret = ReplaceTextureOrSpriteAsset(ret, path->ToString());
        }

        return ret;
    }

    DEFINE_HOOK(void*, Sprite_get_texture, (void* self)) {
        return ReplaceSpriteTexture(Sprite_get_texture_Orig(self));
    }

    DEFINE_HOOK(void, Image_set_sprite, (void* self, void* sprite)) {
        Image_set_sprite_Orig(self, ReplaceSpriteAssetByTextureName(sprite));
    }

    DEFINE_HOOK(void, Image_set_overrideSprite, (void* self, void* sprite)) {
        Image_set_overrideSprite_Orig(self, ReplaceSpriteAssetByTextureName(sprite));
    }

    DEFINE_HOOK(void, CanvasRenderer_SetTexture, (void* self, void* texture, void* method)) {
        void* selected = LocalizationActive() ? ReplaceTextureOrSpriteByObjectName(texture) : texture;
        CanvasRenderer_SetTexture_Orig(self, selected, method);
#ifdef GKMS_WINDOWS
        gakumas::vr::GripTraceCanvasTexture(self, selected);
#endif
    }

    DEFINE_HOOK(void, SpriteRenderer_set_sprite, (void* self, void* sprite)) {
        SpriteRenderer_set_sprite_Orig(self, ReplaceSpriteAssetByTextureName(sprite));
    }

    DEFINE_HOOK(void, I18nHelper_SetUpI18n, (void* self, Il2cppString* lang, Il2cppString* localizationText, int keyComparison)) {
        // Log::InfoFmt("SetUpI18n lang: %s, key: %d text: %s", lang->ToString().c_str(), keyComparison, localizationText->ToString().c_str());
        // TODO 此处为 dump 原文 csv
        I18nHelper_SetUpI18n_Orig(self, lang, localizationText, keyComparison);
    }

    DEFINE_HOOK(void, I18nHelper_SetValue, (void* self, Il2cppString* key, Il2cppString* value)) {
        // Log::InfoFmt("I18nHelper_SetValue: %s - %s", key->ToString().c_str(), value->ToString().c_str());
        std::string local;
        if (TryGetI18n(key->ToString(), &local)) {
            I18nHelper_SetValue_Orig(self, key, UnityResolve::UnityType::String::New(local));
            return;
        }
        if (!LocalizationActive()) {
            I18nHelper_SetValue_Orig(self, key, value);
            return;
        }
        Local::DumpI18nItem(key->ToString(), value->ToString());
        if (Config::textTest) {
            I18nHelper_SetValue_Orig(self, key, Il2cppString::New("[I18]" + value->ToString()));
        }
        else {
            I18nHelper_SetValue_Orig(self, key, value);
        }
    }

#ifdef GKMS_WINDOWS
    struct TransparentStringHash : std::hash<std::wstring>, std::hash<std::wstring_view>
    {
        using is_transparent = void;
    };

    typedef std::unordered_set<std::wstring, TransparentStringHash, std::equal_to<void>> AssetPathsType;
    std::map<std::string, AssetPathsType> CustomAssetBundleAssetPaths;
    std::unordered_map<std::string, Il2CppGCHandle> CustomAssetBundleHandleMap{};
    std::unordered_set<std::string> CustomAssetBundleFailedPaths{};
    std::list<std::string> g_extra_assetbundle_paths{};
    std::atomic_bool extraAssetBundleLoadAllowed{false};
    std::mutex extraAssetBundleLoadMutex;

    void LoadExtraAssetBundle() {
        using Il2CppString = UnityResolve::UnityType::String;
        std::lock_guard<std::mutex> lock(extraAssetBundleLoadMutex);

        if (g_extra_assetbundle_paths.empty()) {
            return;
        }
        // CustomAssetBundleHandleMap.clear();
        // CustomAssetBundleAssetPaths.clear();
        // assert(!ExtraAssetBundleHandle && ExtraAssetBundleAssetPaths.empty());

        static auto AssetBundle_GetAllAssetNames = Il2cppUtils::GetMethod(
            "UnityEngine.AssetBundleModule.dll",
            "UnityEngine",
            "AssetBundle",
            "GetAllAssetNames"
        );

        for (const auto& i : g_extra_assetbundle_paths) {
            if (CustomAssetBundleHandleMap.contains(i) ||
                CustomAssetBundleFailedPaths.contains(i)) {
                continue;
            }

            const auto extraAssetBundle = WinHooks::LoadAssetBundle(i);
            if (extraAssetBundle)
            {
                const auto allAssetPaths = AssetBundle_GetAllAssetNames->Invoke<void*>(extraAssetBundle);
                AssetPathsType assetPath{};
                Il2cppUtils::iterate_IEnumerable<Il2CppString*>(allAssetPaths, [&assetPath](Il2CppString* path)
                    {
                        // ExtraAssetBundleAssetPaths.emplace(path->start_char);
                        // printf("Asset loaded: %ls\n", path->start_char);
                        assetPath.emplace(path->start_char);
                    });
                CustomAssetBundleAssetPaths.emplace(i, assetPath);
                const auto bundleHandle = UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", extraAssetBundle, false);
                CustomAssetBundleHandleMap.emplace(i, bundleHandle);
            }
            else
            {
                CustomAssetBundleFailedPaths.insert(i);
                Log::ErrorFmt("Cannot load asset bundle: %s\n", i.c_str());
            }
        }
    }

    void EnsureExtraAssetBundle() {
        if (!LocalizationActive()) {
            return;
        }
#ifdef GKMS_WINDOWS
        if (g_extra_assetbundle_paths.empty()) {
            g_extra_assetbundle_paths.push_back(
                (gakumasLocalPath / "local-files/gakumasassets").string());
        }
        extraAssetBundleLoadAllowed.store(true, std::memory_order_release);
#endif
        if (!extraAssetBundleLoadAllowed.load(std::memory_order_acquire)) {
            return;
        }
        LoadExtraAssetBundle();
    }

    Il2CppGCHandle GetBundleHandleByAssetName(std::wstring assetName) {
        EnsureExtraAssetBundle();
        for (const auto& i : CustomAssetBundleAssetPaths) {
            for (const auto& m : i.second) {
                if (std::equal(m.begin(), m.end(), assetName.begin(), assetName.end(),
                    [](wchar_t c1, wchar_t c2) {
                        return std::tolower(c1, std::locale()) == std::tolower(c2, std::locale());
                    })) {
                    return CustomAssetBundleHandleMap.at(i.first);
                }
            }
        }
        return nullptr;
    }

    Il2CppGCHandle GetBundleHandleByAssetName(std::string assetName) {
        return GetBundleHandleByAssetName(utility::conversions::to_string_t(assetName));
    }

    Il2CppGCHandle ReplaceFontHandle = nullptr;

    void* GetReplaceFont() {
        static auto FontClass = Il2cppUtils::GetClass("UnityEngine.TextRenderingModule.dll", "UnityEngine", "Font");
        static auto Font_Type = UnityResolve::Invoke<Il2cppUtils::Il2CppReflectionType*>("il2cpp_type_get_object",
            UnityResolve::Invoke<void*>("il2cpp_class_get_type", FontClass->address));

        using Il2CppString = UnityResolve::UnityType::String;
        const auto fontPath = "assets/fonts/gkamszhfontmix.otf";

        void* replaceFont{};
        const auto bundleHandle = GetBundleHandleByAssetName(fontPath);
        if (bundleHandle)
        {
            if (ReplaceFontHandle)
            {
                replaceFont = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", ReplaceFontHandle);
                // 加载场景时会被 Resources.UnloadUnusedAssets 干掉，且不受 DontDestroyOnLoad 影响，暂且判断是否存活，并在必要的时候重新加载
                // TODO: 考虑挂载到 GameObject 上
                // AssetBundle 不会被干掉
                if (IsNativeObjectAlive(replaceFont))
                {
                    return replaceFont;
                }
                else
                {
                    UnityResolve::Invoke<void>("il2cpp_gchandle_free", std::exchange(ReplaceFontHandle, nullptr));
                }
            }

            const auto extraAssetBundle = UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", bundleHandle);
            static auto AssetBundle_LoadAsset = Il2cppUtils::GetMethod(
                "UnityEngine.AssetBundleModule.dll",
                "UnityEngine",
                "AssetBundle",
                "LoadAsset_Internal",
                { "System.String", "System.Type" }
            );

            replaceFont = AssetBundle_LoadAsset->Invoke<void*>(extraAssetBundle, Il2cppString::New(fontPath), Font_Type);
            if (replaceFont)
            {
                ReplaceFontHandle = UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", replaceFont, false);
            }
            else
            {
                Log::Error("Cannot load asset font\n");
            }
        }
        else
        {
            Log::Error("Cannot find asset font\n");
        }
        return replaceFont;
    }
#else
    void* fontCache = nullptr;
    bool CreateFontFromPath(void* font, const std::filesystem::path& fontName) {
        if (!font) {
            return false;
        }

        const auto fontPath = Il2cppString::New(fontName.string());
        if (!fontPath) {
            Log::Error("CreateFontFromPath failed: cannot create path string");
            return false;
        }

        static auto CreateFontFromPathIcall = reinterpret_cast<void (*)(void* self, Il2cppString* path)>(
                Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Font::Internal_CreateFontFromPath(UnityEngine.Font,System.String)")
        );
        if (CreateFontFromPathIcall) {
            CreateFontFromPathIcall(font, fontPath);
            return true;
        }

        static auto CreateFontFromPathMethod = [] {
            auto method = Il2cppUtils::GetMethod(
                    "UnityEngine.TextRenderingModule.dll",
                    "UnityEngine",
                    "Font",
                    "Internal_CreateFontFromPath",
                    { "UnityEngine.Font", "System.String" }
            );
            if (method) {
                return method;
            }

            return Il2cppUtils::GetMethod(
                    "UnityEngine.TextRenderingModule.dll",
                    "UnityEngine",
                    "Font",
                    "Internal_CreateFontFromPath"
            );
        }();
        if (!CreateFontFromPathMethod || !CreateFontFromPathMethod->function || !CreateFontFromPathMethod->address) {
            Log::Error("CreateFontFromPath failed: method not found");
            return false;
        }

        using CreateFontFromPathManagedFn = void (*)(void* font, Il2cppString* path, void* method);
        const auto createFontFromPath = reinterpret_cast<CreateFontFromPathManagedFn>(
                CreateFontFromPathMethod->function
        );
        createFontFromPath(font, fontPath, CreateFontFromPathMethod->address);
        return true;
    }

    void* GetReplaceFont() {
        static auto fontName = Local::GetBasePath() / "local-files" / "gkamsZHFontMIX.otf";
        if (!std::filesystem::exists(fontName)) {
            return nullptr;
        }

        static auto Font_klass = Il2cppUtils::GetClass("UnityEngine.TextRenderingModule.dll",
                                                       "UnityEngine", "Font");
        static auto Font_ctor = Il2cppUtils::GetMethod("UnityEngine.TextRenderingModule.dll",
                                                       "UnityEngine", "Font", ".ctor");
        if (!Font_klass || !Font_ctor) {
            Log::Error("GetReplaceFont failed: Font class or constructor not found");
            return nullptr;
        }

        if (fontCache) {
            if (IsNativeObjectAlive(fontCache)) {
                return fontCache;
            }
        }

        const auto newFont = Font_klass->New<void*>();
        if (!newFont) {
            Log::Error("GetReplaceFont failed: cannot create Font instance");
            return nullptr;
        }
        Font_ctor->Invoke<void>(newFont);

        if (!CreateFontFromPath(newFont, fontName)) {
            return nullptr;
        }

        fontCache = newFont;
        return newFont;
    }
#endif

    std::unordered_set<void*> updatedFontPtrs{};
    void UpdateFont(void* TMP_Textself) {
        if (!Config::replaceFont || !LocalizationActive()) return;
        static auto get_font = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                      "TMPro", "TMP_Text", "get_font");
        static auto set_font = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
                                                      "TMPro", "TMP_Text", "set_font");
        static auto get_name = Il2cppUtils::GetMethod("UnityEngine.CoreModule.dll",
                                                      "UnityEngine", "Object", "get_name");
//        static auto set_fontMaterial = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
//                                                      "TMPro", "TMP_Text", "set_fontMaterial");
//        static auto ForceMeshUpdate = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
//                                                      "TMPro", "TMP_Text", "ForceMeshUpdate");
//
//        static auto get_material = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll",
//                                                      "TMPro", "TMP_Asset", "get_material");

        static auto set_sourceFontFile = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                "TMP_FontAsset", "set_sourceFontFile");
        static auto UpdateFontAssetData = Il2cppUtils::GetMethod("Unity.TextMeshPro.dll", "TMPro",
                                                                 "TMP_FontAsset", "UpdateFontAssetData");

        auto fontAsset = get_font->Invoke<void*>(TMP_Textself);
        if (!fontAsset) {
            return;
        }

        // 检查字体名称，跳过 CampusAlphanumeric 系列字体
        auto fontAssetName = get_name->Invoke<Il2cppString*>(fontAsset);
        if (fontAssetName) {
            std::string fontName = fontAssetName->ToString();
            std::transform(fontName.begin(), fontName.end(), fontName.begin(),
                [](unsigned char c) { return std::tolower(c); });
            if (fontName.find("campusalphanumeric") != std::string::npos) {
                return;  // 保持原版数字字体
            }
        }

        auto newFont = GetReplaceFont();
        if (!newFont) return;

        set_sourceFontFile->Invoke<void>(fontAsset, newFont);
        if (!updatedFontPtrs.contains(fontAsset)) {
            updatedFontPtrs.emplace(fontAsset);
            UpdateFontAssetData->Invoke<void>(fontAsset);
        }
        if (updatedFontPtrs.size() > 200) updatedFontPtrs.clear();

        set_font->Invoke<void>(TMP_Textself, fontAsset);

//        auto fontMaterial = get_material->Invoke<void*>(fontAsset);
//        set_fontMaterial->Invoke<void>(TMP_Textself, fontMaterial);
//        ForceMeshUpdate->Invoke<void>(TMP_Textself, false, false);
    }

    DEFINE_HOOK(void, TMP_Text_PopulateTextBackingArray, (void* self, UnityResolve::UnityType::String* text, int start, int length)) {
        if (!text) {
            return TMP_Text_PopulateTextBackingArray_Orig(self, text, start, length);
        }

        static auto Substring = Il2cppUtils::GetMethod("mscorlib.dll", "System", "String", "Substring",
                                                       {"System.Int32", "System.Int32"});

        const std::string origText = Substring->Invoke<Il2cppString*>(text, start, length)->ToString();
        std::string transText;
        if (TryGetGenericText(origText, &transText)) {
            const auto newText = UnityResolve::UnityType::String::New(transText);
            UpdateFont(self);
            return TMP_Text_PopulateTextBackingArray_Orig(self, newText, 0, newText->length);
        }

        if (Config::textTest) {
            TMP_Text_PopulateTextBackingArray_Orig(self, UnityResolve::UnityType::String::New("[TP]" + text->ToString()), start, length + 4);
        }
        else {
            TMP_Text_PopulateTextBackingArray_Orig(self, text, start, length);
        }
        UpdateFont(self);
    }

    DEFINE_HOOK(void, TMP_Text_set_text, (void* self, Il2cppString* value, void* mtd)) {
        if (!value) {
            return TMP_Text_set_text_Orig(self, value, mtd);
        }
        const std::string origText = value->ToString();
        std::string transText;
        if (TryGetGenericText(origText, &transText)) {
            const auto newText = UnityResolve::UnityType::String::New(transText);
            UpdateFont(self);
            return TMP_Text_set_text_Orig(self, newText, mtd);
        }
        if (Config::textTest) {
            TMP_Text_set_text_Orig(self, UnityResolve::UnityType::String::New("[TT]" + origText), mtd);
        }
        else {
            TMP_Text_set_text_Orig(self, value, mtd);
        }
        UpdateFont(self);
    }

    DEFINE_HOOK(void, TMP_Text_SetText_1, (void* self, Il2cppString* sourceText, void* mtd)) {
        if (!sourceText) {
            return TMP_Text_SetText_1_Orig(self, sourceText, mtd);
        }
        const std::string origText = sourceText->ToString();
        std::string transText;
        if (TryGetGenericText(origText, &transText)) {
            const auto newText = UnityResolve::UnityType::String::New(transText);
            UpdateFont(self);
            return TMP_Text_SetText_1_Orig(self, newText, mtd);
        }
        if (Config::textTest) {
            TMP_Text_SetText_1_Orig(self, UnityResolve::UnityType::String::New("[T1]" + origText), mtd);
        }
        else {
            TMP_Text_SetText_1_Orig(self, sourceText, mtd);
        }
        UpdateFont(self);
    }

    DEFINE_HOOK(void, TMP_Text_SetText_2, (void* self, Il2cppString* sourceText, bool syncTextInputBox, void* mtd)) {
		if (!sourceText) {
			return TMP_Text_SetText_2_Orig(self, sourceText, syncTextInputBox, mtd);
		}
		const std::string origText = sourceText->ToString();
		std::string transText;
		if (TryGetGenericText(origText, &transText)) {
			const auto newText = UnityResolve::UnityType::String::New(transText);
			UpdateFont(self);
			return TMP_Text_SetText_2_Orig(self, newText, syncTextInputBox, mtd);
		}
		if (Config::textTest) {
			TMP_Text_SetText_2_Orig(self, UnityResolve::UnityType::String::New("[TS]" + sourceText->ToString()), syncTextInputBox, mtd);
		}
		else {
			TMP_Text_SetText_2_Orig(self, sourceText, syncTextInputBox, mtd);
		}
		UpdateFont(self);
    }

    DEFINE_HOOK(void, TextMeshProUGUI_Awake, (void* self, void* method)) {
        // Log::InfoFmt("TextMeshProUGUI_Awake at %p, self at %p", TextMeshProUGUI_Awake_Orig, self);

        const auto TMP_Text_klass = Il2cppUtils::GetClass("Unity.TextMeshPro.dll",
                                                                     "TMPro", "TMP_Text");
        const auto get_Text_method = TMP_Text_klass->Get<UnityResolve::Method>("get_text");
        const auto set_Text_method = TMP_Text_klass->Get<UnityResolve::Method>("set_text");
        const auto currText = get_Text_method->Invoke<UnityResolve::UnityType::String*>(self);
        if (currText) {
            //Log::InfoFmt("TextMeshProUGUI_Awake: %s", currText->ToString().c_str());
            std::string transText;
            if (TryGetGenericText(currText->ToString(), &transText)) {
                if (Config::textTest) {
                    set_Text_method->Invoke<void>(self, UnityResolve::UnityType::String::New("[TA]" + transText));
                }
                else {
                    set_Text_method->Invoke<void>(self, UnityResolve::UnityType::String::New(transText));
                }
            }
        }

        // set_font->Invoke<void>(self, font);
        UpdateFont(self);
        TextMeshProUGUI_Awake_Orig(self, method);
    }

    // Legacy UnityEngine.UI.Text hook（礼物/邮件等非TMP界面）
    DEFINE_HOOK(void, UIText_set_text, (void* self, Il2cppString* value)) {
        if (!value) {
            return UIText_set_text_Orig(self, value);
        }
        const std::string origText = value->ToString();
        std::string transText;
        if (TryGetGenericText(origText, &transText)) {
            const auto newText = UnityResolve::UnityType::String::New(transText);
            return UIText_set_text_Orig(self, newText);
        }
        if (Config::textTest) {
            UIText_set_text_Orig(self, UnityResolve::UnityType::String::New("[UI]" + origText));
        }
        else {
            UIText_set_text_Orig(self, value);
        }
    }

    // TMP_Text.SetCharArray(char[], int, int) — 礼物/邮件描述文字通过此路径设置
    DEFINE_HOOK(void, TMP_Text_SetCharArray, (void* self, void* charArray, int start, int count, void* mtd)) {
        if (charArray && start >= 0 && count > 0) {
            // IL2CPP char[] elements are uint16_t (UTF-16)
            auto arr = reinterpret_cast<UnityResolve::UnityType::Array<uint16_t>*>(charArray);
            // 边界检查：确保 start+count 不超出数组长度
            if (static_cast<uintptr_t>(start + count) <= arr->max_length) {
                auto rawData = arr->GetData();
                if (rawData) {
                    // rawData 是 uintptr_t（字节地址），每个 char16_t 占 2 字节
                    // 必须用 start * sizeof(char16_t) 而非直接 + start（否则偏移量减半）
                    const std::u16string u16(
                        reinterpret_cast<const char16_t*>(rawData + static_cast<uintptr_t>(start) * sizeof(char16_t)),
                        static_cast<size_t>(count));
                    const std::string origText = Misc::ToUTF8(u16);
                    std::string transText;
                    if (TryGetGenericText(origText, &transText)) {
                        UpdateFont(self);
                        TMP_Text_set_text_Orig(self, Il2cppString::New(transText), nullptr);
                        return;
                    }
                    if (Config::textTest) {
                        UpdateFont(self);
                        TMP_Text_set_text_Orig(self, Il2cppString::New("[CA]" + origText), nullptr);
                        return;
                    }
                }
            }
        }
        TMP_Text_SetCharArray_Orig(self, charArray, start, count, mtd);
    }

    DEFINE_HOOK(void, TextField_set_value, (void* self, Il2cppString* value)) {
        if (value) {
            std::string transText;
            if (TryGetGenericText(value->ToString(), &transText)) {
                return TextField_set_value_Orig(self, UnityResolve::UnityType::String::New(transText));
            }
        }
        TextField_set_value_Orig(self, value);
    }

    // 未使用的 Hook
    DEFINE_HOOK(void, EffectGroup_ctor, (void* self, void* mtd)) {
        // auto self_klass = Il2cppUtils::get_class_from_instance(self);
        // Log::DebugFmt("EffectGroup_ctor: self: %s::%s", self_klass->namespaze, self_klass->name);
        EffectGroup_ctor_Orig(self, mtd);
    }

    // 原样返回传入的 ProduceStepType，阻止 SP 被转换成 Normal，日程上才看得到 SP。
    // 不能调 _Orig，调了就是原版行为，SP 会被抹平。
    DEFINE_HOOK(int, ExamExtensions_ExchangeSpToNormal, (int type, void* mtd)) {
        return Config::dbgMode ? type : ExamExtensions_ExchangeSpToNormal_Orig(type, mtd);
    }

    // 用于本地化 MasterDB
    DEFINE_HOOK(void, MessageExtensions_MergeFrom, (void* message, void* span, void* mtd)) {
        MessageExtensions_MergeFrom_Orig(message, span, mtd);
        if (LocalizationActive() && message) {
            EnsureLocalizationData();
            auto ret_klass = Il2cppUtils::get_class_from_instance(message);
            if (ret_klass) {
                // Log::DebugFmt("LocalizeMasterItem: %s", ret_klass->name);
                MasterLocal::LocalizeMasterItem(message, ret_klass->name);
            }
        }
    }

    /*
    // 未使用的 Hook
    DEFINE_HOOK(void, MasterBase_GetAll, (void* self, UnityResolve::UnityType::Array<UnityResolve::UnityType::Byte>* getAllSQL,
            int sqlLength, UnityResolve::UnityType::List<void*>* result, void* predicate, void* comparison, void* mtd)) {
        // result: List<Campus.Common.Proto.Client.Master.*>, 和 query 的表名一致

        MasterBase_GetAll_Orig(self, getAllSQL, sqlLength, result, predicate, comparison, mtd);

        auto data_ptr = reinterpret_cast<std::uint8_t*>(getAllSQL->GetData());
        std::string qS(data_ptr, data_ptr + sqlLength);


        Il2cppUtils::Tools::CSListEditor resultList(result);
        MasterLocal::LocalizeMaster(qS, result);
    }

    void LocalizeFindByKey(void* result, void* self) {
        return;  // 暂时不需要了
        auto self_klass = Il2cppUtils::get_class_from_instance(self);
        Log::DebugFmt("Localize: %s", self_klass->name);  // FeatureLockMaster
        // return;

        if (!result) return;
        auto result_klass = Il2cppUtils::get_class_from_instance(result);
        std::string klassName = result_klass->name;

        auto MasterBase_klass = Il2cppUtils::get_class_from_instance(self);
        auto MasterBase_GetTableName = Il2cppUtils::il2cpp_class_get_method_from_name(MasterBase_klass, "GetTableName", 0);
        if (MasterBase_GetTableName) {
            auto tableName = reinterpret_cast<Il2cppString* (*)(void*, void*)>(MasterBase_GetTableName->methodPointer)(self, MasterBase_GetTableName);
            // Log::DebugFmt("MasterBase_FindByKey: %s", tableName->ToString().c_str());

            if (klassName == "List`1") {
                MasterLocal::LocalizeMaster(result, tableName->ToString());
            }
            else {
                MasterLocal::LocalizeMasterItem(result, tableName->ToString());
            }
        }
    }*/

    DEFINE_HOOK(Il2cppString*, OctoCaching_GetResourceFileName, (void* data, void* method)) {
        auto ret = OctoCaching_GetResourceFileName_Orig(data, method);
        //Log::DebugFmt("OctoCaching_GetResourceFileName: %s", ret->ToString().c_str());
        return ret;
    }

    DEFINE_HOOK(void, OctoResourceLoader_LoadFromCacheOrDownload,
                (void* self, Il2cppString* resourceName, void* onComplete, void* onProgress, void* method)) {

        Log::DebugFmt("OctoResourceLoader_LoadFromCacheOrDownload: %s\n", resourceName->ToString().c_str());

        std::string replaceStr;
        if (LocalizationActive() &&
            Local::GetResourceText(resourceName->ToString(), &replaceStr)) {
            const auto onComplete_klass = Il2cppUtils::get_class_from_instance(onComplete);
            const auto onComplete_invoke_mtd = UnityResolve::Invoke<Il2cppUtils::MethodInfo*>(
                    "il2cpp_class_get_method_from_name", onComplete_klass, "Invoke", 2);
            if (onComplete_invoke_mtd) {
                const auto onComplete_invoke = reinterpret_cast<void (*)(void*, Il2cppString*, void*)>(
                        onComplete_invoke_mtd->methodPointer
                );
                onComplete_invoke(onComplete, UnityResolve::UnityType::String::New(replaceStr), nullptr);
                return;
            }
        }

        return OctoResourceLoader_LoadFromCacheOrDownload_Orig(self, resourceName, onComplete, onProgress, method);
    }

    DEFINE_HOOK(void, OnDownloadProgress_Invoke, (void* self, Il2cppString* name, uint64_t receivedLength, uint64_t contentLength)) {
        Log::DebugFmt("OnDownloadProgress_Invoke: %s, %lu/%lu", name->ToString().c_str(), receivedLength, contentLength);
        OnDownloadProgress_Invoke_Orig(self, name, receivedLength, contentLength);
    }

    // UnHooked
    DEFINE_HOOK(UnityResolve::UnityType::String*, UI_I18n_GetOrDefault, (void* self,
            UnityResolve::UnityType::String* key, UnityResolve::UnityType::String* defaultKey, void* method)) {

        auto ret = UI_I18n_GetOrDefault_Orig(self, key, defaultKey, method);

        // Log::DebugFmt("UI_I18n_GetOrDefault: key: %s, default: %s, result: %s", key->ToString().c_str(), defaultKey->ToString().c_str(), ret->ToString().c_str());

        return ret;
        // return UnityResolve::UnityType::String::New("[I18]" + ret->ToString());
    }

    /*
    DEFINE_HOOK(void*, UserDataManagerBase_get__userIdolCardSkinList, (void* self, void* mtd)) {  // Live默认选择
        auto ret = UserDataManagerBase_get__userIdolCardSkinList_Orig(self, mtd);
        Log::DebugFmt("UserDataManagerBase_get__userIdolCardSkinList: %p", ret);
        return ret;
    }
    DEFINE_HOOK(void*, UserDataManagerBase_get__userCostumeList, (void* self, void* mtd)) {  // 服装选择界面
        auto ret = UserDataManagerBase_get__userCostumeList_Orig(self, mtd);
        Log::DebugFmt("UserDataManagerBase_get__userCostumeList: %p", ret);
        return ret;
    }
    DEFINE_HOOK(void*, UserDataManagerBase_get__userCostumeHeadList, (void* self, void* mtd)) {  // 服装选择界面
        auto ret = UserDataManagerBase_get__userCostumeHeadList_Orig(self, mtd);
        Log::DebugFmt("UserDataManagerBase_get__userCostumeHeadList: %p", ret);
        return ret;
    }*/

    DEFINE_HOOK(bool, UserIdolCardSkinCollection_Exists, (void* self, Il2cppString* id, void* mtd)) { // Live默认选择
        auto ret = UserIdolCardSkinCollection_Exists_Orig(self, id, mtd);
        // Log::DebugFmt("UserIdolCardSkinCollection_Exists: %s, ret: %d", id->ToString().c_str(), ret);
        if (!(Config::dbgMode && Config::unlockAllLive)) return ret;

        if (id) {
            std::string idStr = id->ToString();
            if (idStr.starts_with("music") || idStr.starts_with("i_card-skin")) {  // eg. music-all-kllj-006, i_card-skin-hski-3-002
                return true;
            }
        }
        return ret;
    }

    void* GetMethodPointerByArgCount(const std::string& assemblyName, const std::string& nameSpaceName,
                                     const std::string& className, const std::string& methodName,
                                     size_t argsCount) {
        auto klass = Il2cppUtils::GetClass(assemblyName, nameSpaceName, className);
        if (!klass) return nullptr;

        for (auto method : klass->methods) {
            if (method && method->name == methodName && method->args.size() == argsCount) {
                return method->function;
            }
        }

        Log::ErrorFmt("GetMethodPointerByArgCount error: method %s::%s.%s with %zu args not found.",
                      nameSpaceName.c_str(), className.c_str(), methodName.c_str(), argsCount);
        return nullptr;
    }

#ifdef GKMS_WINDOWS
    DEFINE_HOOK(void, PictureBookLiveThumbnailView_SetReleaseDataAsync, (void* retstr, void* self, void* liveData, Il2cppString* characterId, bool isUnlocked, bool isNew, bool hasLiveSkin, void* ct, void* mtd)) {
        if (Config::dbgMode && Config::unlockAllLive) {
            isUnlocked = true;
            hasLiveSkin = true;
        }
        PictureBookLiveThumbnailView_SetReleaseDataAsync_Orig(retstr, self, liveData, characterId, isUnlocked, isNew, hasLiveSkin, ct, mtd);
    }

    DEFINE_HOOK(void, PictureBookLiveThumbnailView_SetUnReleaseDataAsync, (void* retstr, void* self, void* liveData, bool isExemptLive, Il2cppString* characterId, void* ct, void* mtd)) {
        if (Config::dbgMode && Config::unlockAllLive) {
            isExemptLive = true;
        }
        PictureBookLiveThumbnailView_SetUnReleaseDataAsync_Orig(retstr, self, liveData, isExemptLive, characterId, ct, mtd);
    }

    DEFINE_HOOK(void, PictureBookLiveThumbnailView_SetDataAsync, (void* retstr, void* self, void* liveData, Il2cppString* characterId, bool isReleased, bool isUnlocked, bool isNew, bool hasLiveSkin, void* ct, void* mtd)) {
        // Log::DebugFmt("PictureBookLiveThumbnailView_SetDataAsync: isReleased: %d, isUnlocked: %d, isNew: %d, hasLiveSkin: %d", isReleased, isUnlocked, isNew, hasLiveSkin);
        if (Config::dbgMode && Config::unlockAllLive) {
            isUnlocked = true;
            isReleased = true;
            hasLiveSkin = true;
        }
        PictureBookLiveThumbnailView_SetDataAsync_Orig(retstr, self, liveData, characterId, isReleased, isUnlocked, isNew, hasLiveSkin, ct, mtd);
    }
#else
    DEFINE_HOOK(void, PictureBookLiveThumbnailView_SetReleaseDataAsync, (void* self, void* liveData, Il2cppString* characterId, bool isUnlocked, bool isNew, bool hasLiveSkin, void* ct, void* mtd)) {
        if (Config::dbgMode && Config::unlockAllLive) {
            isUnlocked = true;
            hasLiveSkin = true;
        }
        PictureBookLiveThumbnailView_SetReleaseDataAsync_Orig(self, liveData, characterId, isUnlocked, isNew, hasLiveSkin, ct, mtd);
    }

    DEFINE_HOOK(void, PictureBookLiveThumbnailView_SetUnReleaseDataAsync, (void* self, void* liveData, bool isExemptLive, Il2cppString* characterId, void* ct, void* mtd)) {
        if (Config::dbgMode && Config::unlockAllLive) {
            isExemptLive = true;
        }
        PictureBookLiveThumbnailView_SetUnReleaseDataAsync_Orig(self, liveData, isExemptLive, characterId, ct, mtd);
    }

    DEFINE_HOOK(void, PictureBookLiveThumbnailView_SetDataAsync, (void* self, void* liveData, Il2cppString* characterId, bool isReleased, bool isUnlocked, bool isNew, bool hasLiveSkin, void* ct, void* mtd)) {
        // Log::DebugFmt("PictureBookLiveThumbnailView_SetDataAsync: isReleased: %d, isUnlocked: %d, isNew: %d, hasLiveSkin: %d", isReleased, isUnlocked, isNew, hasLiveSkin);
        if (Config::dbgMode && Config::unlockAllLive) {
            isUnlocked = true;
            isReleased = true;
            hasLiveSkin = true;
        }
        PictureBookLiveThumbnailView_SetDataAsync_Orig(self, liveData, characterId, isReleased, isUnlocked, isNew, hasLiveSkin, ct, mtd);
    }
#endif

    enum class GetIdolIdType {
        MusicId,
        CostumeId,
        CostumeHeadId
    };

    std::vector<std::string> GetIdolMusicIdAll(const std::string& charaNameId = "", GetIdolIdType getType = GetIdolIdType::MusicId) {
        // 传入例: fktn
        // System.Collections.Generic.List`1<valuetype [mscorlib]System.ValueTuple`2<class Campus.Common.Proto.Client.Master.IdolCardSkin, class Campus.Common.Proto.Client.Master.Music>>
        static auto get_IdolCardSkinMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Master", "MasterManager", "get_IdolCardSkinMaster");
        static auto Master_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Master", "IdolCardSkinMaster", "GetAllWithSortByKey");
        static auto IdolCardSkin_get_Id = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "IdolCardSkin", "get_Id");
        static auto IdolCardSkin_get_IdolCardId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "IdolCardSkin", "get_IdolCardId");
        static auto IdolCardSkin_GetMusic = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "IdolCardSkin", "GetMusic");
        static auto IdolCardSkin_get_MusicId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "IdolCardSkin", "get_MusicId");
        static auto IdolCardSkin_get_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "IdolCardSkin", "get_CostumeId");
        static auto IdolCardSkin_get_CostumeHeadId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "IdolCardSkin", "get_CostumeHeadId");
        static auto GetLiveMusics = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.OutGame",
                                                           "PictureBookWindowPresenter", "GetLiveMusics");

        auto idolCardSkinMaster = get_IdolCardSkinMaster->Invoke<void*>(nullptr);  // IdolCardSkinMaster

        std::vector<std::string> ret{};

        if (!idolCardSkinMaster) {
            Log::ErrorFmt("get_IdolCardSkinMaster failed: %p", idolCardSkinMaster);
            return ret;
        }
        // List<IdolCardSkin>
        auto idolCardSkinList = Master_GetAllWithSortByKey->Invoke<UnityResolve::UnityType::List<void*>*>(idolCardSkinMaster, 0x0, nullptr);

        auto idolCardSkins = idolCardSkinList->ToArray()->ToVector();
        const auto checkStartCharaId = "i_card-" + charaNameId;
        // Log::DebugFmt("checkStartCharaId: %s", checkStartCharaId.c_str());

        // origMusics->Clear();
        UnityResolve::Method* idGetFunc = nullptr;
        switch (getType) {
            case GetIdolIdType::MusicId: idGetFunc = IdolCardSkin_get_MusicId;
                break;
            case GetIdolIdType::CostumeId: idGetFunc = IdolCardSkin_get_CostumeId;
                break;
            case GetIdolIdType::CostumeHeadId: idGetFunc = IdolCardSkin_get_CostumeHeadId;
                break;
            default:
                idGetFunc = IdolCardSkin_get_MusicId;
        }

        for (auto i : idolCardSkins) {
            if (!i) continue;
            // auto charaId = IdolCardSkin_get_Id->Invoke<Il2cppString*>(i);
            auto targetId = idGetFunc->Invoke<Il2cppString*>(i);
            auto cardId = IdolCardSkin_get_IdolCardId->Invoke<Il2cppString*>(i)->ToString();
            auto music = IdolCardSkin_GetMusic->Invoke<void*>(i);

            if (charaNameId.empty() || cardId.starts_with(checkStartCharaId)) {
                std::string musicIdStr = targetId->ToString();
                // Log::DebugFmt("Add cardId: %s, musicId: %s", cardId.c_str(), musicIdStr.c_str());
                if (std::find(ret.begin(), ret.end(), musicIdStr) == ret.end()) {
                    ret.emplace_back(musicIdStr);
                }
            }
        }
        return ret;
    }

    void* AddIdsToUserDataCollectionFromMaster(void* origList, std::vector<std::string>& allIds,
                                               UnityResolve::Method* get_CostumeId, UnityResolve::Method* set_CostumeId, UnityResolve::Method* Clone) {
        std::unordered_set<std::string> existIds{};
        Il2cppUtils::Tools::CSListEditor listEditor(origList);
        if (listEditor.get_Count() <= 0) {
            return origList;
        }

        for (auto i : listEditor) {
            auto costumeId = get_CostumeId->Invoke<Il2cppString*>(i);
            if (!costumeId) continue;
            existIds.emplace(costumeId->ToString());
        }

        for (auto& i : allIds) {
            if (i.empty()) continue;
            // Log::DebugFmt("Try add %s", i.c_str());
            if (existIds.contains(i)) continue;

            auto userCostume = Clone->Invoke<void*>(listEditor.get_Item(0));
            set_CostumeId->Invoke<void>(userCostume, Il2cppString::New(i));
            listEditor.Add(userCostume);
        }
        return origList;
    }

    // 把主表全部服装/头部 ID 补进用户服装列表；klassName 决定按 body 还是 head 处理
    void* AugmentUserCostumeList(const std::string& klassName, void* origList) {
        if (klassName == "UserCostumeHeadCollection") {
            static auto UserCostume_Clone = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostumeHead", "Clone");
            static auto UserCostume_get_CostumeHeadId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostumeHead", "get_CostumeHeadId");
            static auto UserCostume_set_CostumeHeadId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostumeHead", "set_CostumeHeadId");

            // 游戏改名/改版本时这些会是 null，直接返回原列表，别带着空指针往下走
            if (!UserCostume_Clone || !UserCostume_get_CostumeHeadId || !UserCostume_set_CostumeHeadId) return origList;

            auto allIds = GetIdolMusicIdAll("", GetIdolIdType::CostumeHeadId);

            // List<Campus.Common.Proto.Client.Transaction.UserCostumeHead>
            return AddIdsToUserDataCollectionFromMaster(origList, allIds, UserCostume_get_CostumeHeadId, UserCostume_set_CostumeHeadId, UserCostume_Clone);
        }
        if (klassName == "UserCostumeCollection") {
            static auto UserCostume_Clone = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostume", "Clone");
            static auto UserCostume_get_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostume", "get_CostumeId");
            static auto UserCostume_set_CostumeId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostume", "set_CostumeId");

            if (!UserCostume_Clone || !UserCostume_get_CostumeId || !UserCostume_set_CostumeId) return origList;

            auto allIds = GetIdolMusicIdAll("", GetIdolIdType::CostumeId);

            // List<Campus.Common.Proto.Client.Transaction.UserCostume>
            return AddIdsToUserDataCollectionFromMaster(origList, allIds, UserCostume_get_CostumeId, UserCostume_set_CostumeId, UserCostume_Clone);
        }
        return origList;
    }

    DEFINE_HOOK(void*, UserCostumeCollection_FindBy, (void* self, void* predicate, void* mtd)) {
        auto ret = UserCostumeCollection_FindBy_Orig(self, predicate, mtd);
        if (!(Config::dbgMode && Config::unlockAllLiveCostume)) return ret;

        auto this_klass = Il2cppUtils::get_class_from_instance(self);

        std::string thisKlassName(this_klass->name);
        // Campus.Common.User::UserCostumeHeadCollection || Campus.Common.User::UserCostumeCollection
        // 两个 class 的 GetAllList 均使用的父类 Qua.UserDataManagement.UserDataCollectionBase`2 的方法，地址一致
        if (thisKlassName == "UserCostumeHeadCollection" || thisKlassName == "UserCostumeCollection") {
            auto getAllListMtd = Il2cppUtils::il2cpp_class_get_method_from_name(this_klass, "GetAllList", 1);
            if (!getAllListMtd) return ret;
            auto getAllList = reinterpret_cast<void* (*)(void*, void*, void*)>(getAllListMtd->methodPointer);
            auto origList = getAllList(self, nullptr, (void*)getAllListMtd);
            return AugmentUserCostumeList(thisKlassName, origList);
        }

        return ret;
    }

    // CostumePhotoGroup 主表收录的全部服装 ID——游戏自己的“可拍摄（已实装）”白名单
    std::unordered_set<std::string> GetPhotoGroupCostumeIds() {
        std::unordered_set<std::string> ret{};
        auto getMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Master", "MasterManager", "get_CostumePhotoGroupMaster");
        auto masterKlass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Common.Master", "CostumePhotoGroupMaster");
        auto getCostumeIds = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "CostumePhotoGroup", "get_CostumeIds");
        if (!getMaster || !masterKlass || !getCostumeIds) return ret;
        auto getAll_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(masterKlass->address, "GetAllWithSortByKey", 1);
        if (!getAll_mtd) return ret;
        auto getAll = reinterpret_cast<UnityResolve::UnityType::List<void*>* (*)(void*, int, void*)>(getAll_mtd->methodPointer);

        auto master = getMaster->Invoke<void*>(nullptr);
        if (!master) return ret;
        auto list = getAll(master, 0, (void*)getAll_mtd);
        if (!list) return ret;
        for (auto group : list->ToArray()->ToVector()) {
            if (!group) continue;  // ToVector 含 List 容量内的 null 槽位
            auto rep = getCostumeIds->Invoke<void*>(group);  // RepeatedField<string>
            if (!rep) continue;
            auto rep_klass = Il2cppUtils::get_class_from_instance(rep);
            static auto count_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(rep_klass, "get_Count", 0);
            static auto item_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(rep_klass, "get_Item", 1);
            if (!count_mtd || !item_mtd) continue;
            // 共享泛型：末尾传 MethodInfo*
            auto getCount = reinterpret_cast<int (*)(void*, void*)>(count_mtd->methodPointer);
            auto getItem = reinterpret_cast<Il2cppString* (*)(void*, int, void*)>(item_mtd->methodPointer);
            int n = getCount(rep, (void*)count_mtd);
            for (int i = 0; i < n; ++i) {
                auto s = getItem(rep, i, (void*)item_mtd);
                if (s) ret.emplace(s->ToString());
            }
        }
        return ret;
    }

    // 主表补充 ID：异色 = 与基础服装（baseIds，IdolCardSkin 来源）共享 colorGroupId 的变体；
    // 另加 CostumePhotoGroup 白名单条目；ViewStartTime 在未来的未实装条目跳过。
    // isHead 时返回这些服装引用的头部 ID
    std::vector<std::string> GetMasterCostumeIdsAll(bool isHead, const std::unordered_set<std::string>& baseIds) {
        std::vector<std::string> ret{};
        auto getMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Master", "MasterManager", "get_CostumeMaster");
        auto masterKlass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Common.Master", "CostumeMaster");
        auto getId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "Costume", "get_Id");
        auto getHeadId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "Costume", "get_CostumeHeadId");
        auto getDefaultHeadId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "Costume", "get_DefaultCostumeHeadId");
        auto getColorGroupId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "Costume", "get_CostumeColorGroupId");
        auto getViewStartTime = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "Costume", "get_ViewStartTime");
        if (!getMaster || !masterKlass || !getId || !getHeadId || !getDefaultHeadId || !getColorGroupId || !getViewStartTime) return ret;
        // 取 1 参重载 GetAllWithSortByKey(sortType)，末尾补隐藏 MethodInfo*
        auto getAll_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(masterKlass->address, "GetAllWithSortByKey", 1);
        if (!getAll_mtd) return ret;
        auto getAll = reinterpret_cast<UnityResolve::UnityType::List<void*>* (*)(void*, int, void*)>(getAll_mtd->methodPointer);

        auto photoAllowed = GetPhotoGroupCostumeIds();
        if (photoAllowed.empty()) return ret;

        auto master = getMaster->Invoke<void*>(nullptr);
        if (!master) return ret;
        auto list = getAll(master, 0, (void*)getAll_mtd);
        if (!list) return ret;
        auto items = list->ToArray()->ToVector();

        // 第一遍：基础服装（IdolCardSkin 来源）与拍摄白名单条目的颜色组 → 允许其全部异色变体
        std::unordered_set<std::string> allowedColorGroups{};
        for (auto item : items) {
            if (!item) continue;  // ToVector 含 List 容量内的 null 槽位
            auto id = getId->Invoke<Il2cppString*>(item);
            if (!id) continue;
            auto idStr = id->ToString();
            if (!photoAllowed.contains(idStr) && !baseIds.contains(idStr)) continue;
            auto cg = getColorGroupId->Invoke<Il2cppString*>(item);
            if (cg && !cg->ToString().empty()) allowedColorGroups.emplace(cg->ToString());
        }

        const auto nowSec = (int64_t)time(nullptr);
        std::unordered_set<std::string> seen{};
        int skippedFuture = 0;
        for (auto item : items) {
            if (!item) continue;
            auto id = getId->Invoke<Il2cppString*>(item);
            if (!id) continue;
            bool allowed = photoAllowed.contains(id->ToString());
            auto viewStart = getViewStartTime->Invoke<int64_t>(item);
            if (viewStart > 4000000000LL) viewStart /= 1000;  // 毫秒 → 秒

            if (!allowed) {
                auto cg = getColorGroupId->Invoke<Il2cppString*>(item);
                allowed = cg && !cg->ToString().empty() && allowedColorGroups.contains(cg->ToString());
            }
            // 独立活动/商店服（如水手服泳装）：无颜色组但有已到期的公开时间
            if (!allowed) allowed = viewStart > 0 && viewStart <= nowSec;
            if (!allowed) continue;

            // 未实装（公开时间在未来）的条目没有资源，进拍摄页会黑屏卡死
            if (viewStart > nowSec) { ++skippedFuture; continue; }

            if (!isHead) {
                if (seen.emplace(id->ToString()).second) ret.emplace_back(id->ToString());
                continue;
            }
            for (auto headGetter : { getHeadId, getDefaultHeadId }) {
                auto headId = headGetter->Invoke<Il2cppString*>(item);
                if (!headId) continue;
                auto headIdStr = headId->ToString();
                if (!headIdStr.empty() && seen.emplace(headIdStr).second) ret.emplace_back(headIdStr);
            }
        }
        if (skippedFuture) Log::InfoFmt("GetMasterCostumeIdsAll(isHead=%d): skipped future entries=%d", isHead, skippedFuture);
        return ret;
    }

    // 把缺失的主表服装/头部克隆成用户记录，直接 Add 进集合本体；
    // 这样 GetAll()（字典 Values 视图，拍摄页 CreateItemModels 用它）等所有读取路径都能看到
    bool AddMissingCostumesToCollection(void* self, const std::string& klassName) {
        UnityResolve::Method *Clone, *getId, *setId;
        GetIdolIdType idType;
        if (klassName == "UserCostumeHeadCollection") {
            static auto head_Clone = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostumeHead", "Clone");
            static auto head_getId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostumeHead", "get_CostumeHeadId");
            static auto head_setId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostumeHead", "set_CostumeHeadId");
            Clone = head_Clone; getId = head_getId; setId = head_setId;
            idType = GetIdolIdType::CostumeHeadId;
        }
        else if (klassName == "UserCostumeCollection") {
            static auto body_Clone = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostume", "Clone");
            static auto body_getId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostume", "get_CostumeId");
            static auto body_setId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Transaction", "UserCostume", "set_CostumeId");
            Clone = body_Clone; getId = body_getId; setId = body_setId;
            idType = GetIdolIdType::CostumeId;
        }
        else return false;
        if (!Clone || !getId || !setId) return false;

        auto collectionKlass = Il2cppUtils::get_class_from_instance(self);
        if (!collectionKlass || klassName != collectionKlass->name) return false;
        // 共享泛型方法必须使用实际运行时集合类型的 MethodInfo，并传入末尾隐藏参数。
        auto getAllListMtd = Il2cppUtils::il2cpp_class_get_method_from_name(collectionKlass, "GetAllList", 1);
        auto addMtd = Il2cppUtils::il2cpp_class_get_method_from_name(collectionKlass, "Add", 1);
        if (!getAllListMtd || !addMtd) return false;
        auto getAllList = reinterpret_cast<void* (*)(void*, void*, void*)>(getAllListMtd->methodPointer);
        auto collectionAdd = reinterpret_cast<void (*)(void*, void*, void*)>(addMtd->methodPointer);
        // Exists 用于 Add 前复核：字典里可能存在 GetAllList 看不到的 key，
        // 重复 Add 会在 native 栈上抛托管异常，直接崩
        auto existsMtd = Il2cppUtils::il2cpp_class_get_method_from_name(collectionKlass, "Exists", 1);
        auto collectionExists = existsMtd
            ? reinterpret_cast<bool (*)(void*, Il2cppString*, void*)>(existsMtd->methodPointer)
            : nullptr;

        auto list = getAllList(self, nullptr, (void*)getAllListMtd);
        Il2cppUtils::Tools::CSListEditor listEditor(list);
        if (listEditor.get_Count() <= 0) return false;  // 无模板可克隆，数据未加载完，等下次调用

        std::unordered_set<std::string> existIds{};
        for (auto i : listEditor) {
            auto id = getId->Invoke<Il2cppString*>(i);
            if (id) existIds.emplace(id->ToString());
        }

        auto allIds = GetIdolMusicIdAll("", idType);
        // 颜色组锚点始终用 body 服装 ID（head 的异色也是通过 body 行的颜色组关联的）
        auto bodyIds = GetIdolMusicIdAll("", GetIdolIdType::CostumeId);
        std::unordered_set<std::string> baseIds(bodyIds.begin(), bodyIds.end());
        auto masterIds = GetMasterCostumeIdsAll(idType == GetIdolIdType::CostumeHeadId, baseIds);
        allIds.insert(allIds.end(), masterIds.begin(), masterIds.end());
        int added = 0;
        for (auto& id : allIds) {
            // emplace 兼做去重：两个来源有重叠，重复 Add 同一 key 会抛异常
            if (id.empty() || !existIds.emplace(id).second) continue;
            auto idStr = Il2cppString::New(id);
            if (collectionExists && collectionExists(self, idStr, (void*)existsMtd)) continue;
            auto clone = Clone->Invoke<void*>(listEditor.get_Item(0));
            setId->Invoke<void>(clone, idStr);
            collectionAdd(self, clone, (void*)addMtd);
            ++added;
        }
        Log::InfoFmt("Costume collection augment: %s, added=%d", klassName.c_str(), added);
        return true;
    }

    // 集合当前条目数，取不到返回 -1
    int GetUserDataCollectionCount(void* self, void* klass) {
        auto mtd = Il2cppUtils::il2cpp_class_get_method_from_name(klass, "get_Count", 0);
        if (!mtd) return -1;
        return reinterpret_cast<int (*)(void*, void*)>(mtd->methodPointer)(self, (void*)mtd);
    }

    DEFINE_HOOK(void*, UserDataCollection_GetAll, (void* self, void* mtd)) {
        if (Config::dbgMode && Config::unlockAllLiveCostume) {
            auto this_klass = Il2cppUtils::get_class_from_instance(self);
            const char* klassName = this_klass ? this_klass->name : nullptr;
            // GetAll 是父类共享泛型实现，游戏内所有 UserDataCollection 都会命中这里，
            // 属于热路径，先用 strcmp 早退，别构造 std::string
            if (klassName && (strcmp(klassName, "UserCostumeCollection") == 0
                              || strcmp(klassName, "UserCostumeHeadCollection") == 0)) {
                // 按实例指针记住"已补过"，再用条目数复核：Boehm GC 不移动对象，但会复用
                // 已释放的地址，新集合落到旧地址上时条目数对不上，于是重新补一次。
                // 取不到 Count（返回 -1）时退化成"每个指针只补一次"的旧行为。
                static std::unordered_map<void*, int> augmentedCounts{};
                const auto it = augmentedCounts.find(self);
                if (it == augmentedCounts.end() || it->second != GetUserDataCollectionCount(self, this_klass)) {
                    if (AddMissingCostumesToCollection(self, klassName)) {
                        augmentedCounts[self] = GetUserDataCollectionCount(self, this_klass);
                    }
                }
            }
        }
        return UserDataCollection_GetAll_Orig(self, mtd);
    }

    DEFINE_HOOK(bool, PhotographyCostumeSettingListItemModel_get_IsDisabled, (void* self, void* mtd)) {
        return Config::dbgMode && Config::unlockAllLiveCostume
            ? false
            : PhotographyCostumeSettingListItemModel_get_IsDisabled_Orig(self, mtd);
    }

    void* AddPhotographyIdolSkinItems(void* list, const std::string& characterId,
                                      UnityResolve::Method* getItem, UnityResolve::Method* getId) {
        if (!list || characterId.empty() || !getItem || !getId) return list;

        static auto get_IdolCardSkinMaster = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Master", "MasterManager", "get_IdolCardSkinMaster");
        static auto Master_GetAllWithSortByKey = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Master", "IdolCardSkinMaster", "GetAllWithSortByKey");
        static auto IdolCardSkin_get_IdolCardId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "IdolCardSkin", "get_IdolCardId");
        if (!get_IdolCardSkinMaster || !Master_GetAllWithSortByKey || !IdolCardSkin_get_IdolCardId) return list;

        Il2cppUtils::Tools::CSListEditor listEditor(list);
        std::unordered_set<std::string> ids;
        for (auto item : listEditor) {
            auto id = getId->Invoke<Il2cppString*>(item);
            if (id) ids.emplace(id->ToString());
        }

        auto idolCardSkinMaster = get_IdolCardSkinMaster->Invoke<void*>(nullptr);
        auto idolCardSkinList = idolCardSkinMaster
            ? Master_GetAllWithSortByKey->Invoke<UnityResolve::UnityType::List<void*>*>(idolCardSkinMaster, 0, nullptr)
            : nullptr;
        if (!idolCardSkinList) return list;

        int added = 0;
        const auto cardPrefix = "i_card-" + characterId;
        for (auto idolCardSkin : idolCardSkinList->ToArray()->ToVector()) {
            if (!idolCardSkin) continue;  // ToVector 会包含 List 容量内的 null 槽位，与 GetIdolMusicIdAll 一致
            auto idolCardId = IdolCardSkin_get_IdolCardId->Invoke<Il2cppString*>(idolCardSkin);
            if (!idolCardId || !idolCardId->ToString().starts_with(cardPrefix)) continue;

            auto item = getItem->Invoke<void*>(idolCardSkin);
            auto id = item ? getId->Invoke<Il2cppString*>(item) : nullptr;
            if (id && ids.emplace(id->ToString()).second) {
                listEditor.Add(item);
                ++added;
            }
        }
        Log::InfoFmt("Photography add: character=%s, added=%d", characterId.c_str(), added);
        return list;
    }

    DEFINE_HOOK(void*, PhotographyCostumeSettingData_GetCostumes,
                (void* self, Il2cppString* characterId, void* mtd)) {
        auto ret = PhotographyCostumeSettingData_GetCostumes_Orig(self, characterId, mtd);
        if (!(Config::dbgMode && Config::unlockAllLiveCostume)) return ret;

        static auto getItem = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "IdolCardSkin", "GetCostume");
        static auto getId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "Costume", "get_Id");
        return AddPhotographyIdolSkinItems(ret, characterId ? characterId->ToString() : "", getItem, getId);
    }

    DEFINE_HOOK(void*, PhotographyCostumeSettingData_GetCostumeHeads,
                (void* self, Il2cppString* characterId, void* mtd)) {
        auto ret = PhotographyCostumeSettingData_GetCostumeHeads_Orig(self, characterId, mtd);
        if (!(Config::dbgMode && Config::unlockAllLiveCostume)) return ret;

        static auto getItem = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "IdolCardSkin", "GetCostumeHead");
        static auto getId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master", "CostumeHead", "get_Id");
        return AddPhotographyIdolSkinItems(ret, characterId ? characterId->ToString() : "", getItem, getId);
    }

    void* getCompletedUniTask() {
        static auto unitask_klass = Il2cppUtils::GetClass("UniTask.dll", "Cysharp.Threading.Tasks", "UniTask");
        static auto CompletedTask_field = unitask_klass->Get<UnityResolve::Field>("CompletedTask");
        auto ret = UnityResolve::Invoke<void*>("il2cpp_object_new", unitask_klass->address);
        UnityResolve::Invoke<void>("il2cpp_field_static_get_value", CompletedTask_field->address, ret);
        return ret;
    }

#ifdef GKMS_WINDOWS
    // 绕过切歌时的等待以及网络请求
    DEFINE_HOOK(void*, Produce_ViewPictureBookLiveAsync, (void* retstr, Il2cppString* musicId, Il2cppString* characterId,
        void* ct, void* callOption, void* errorHandlerIl, Il2cppString* requestIdForResponseCache, void* mtd)) {

        // Log::DebugFmt("Produce_ViewPictureBookLiveAsync: %s - %s", musicId->ToString().c_str(), characterId->ToString().c_str());
        if (Config::dbgMode && Config::unlockAllLive) return getCompletedUniTask();
        return Produce_ViewPictureBookLiveAsync_Orig(retstr, musicId, characterId, ct, callOption, errorHandlerIl, requestIdForResponseCache, mtd);
    }
#else
    DEFINE_HOOK(void*, Produce_ViewPictureBookLiveAsync, (void* retstr, void* musicId, void* characterId,
        void* ct, void* callOption, void* errorHandlerIl, void* requestIdForResponseCache, void* mtd, void* wenhao)) {

        // Log::DebugFmt("Produce_ViewPictureBookLiveAsync: %s - %s", musicId->ToString().c_str(), characterId->ToString().c_str());
        if (Config::dbgMode && Config::unlockAllLive) return getCompletedUniTask();
        return Produce_ViewPictureBookLiveAsync_Orig(retstr, musicId, characterId, ct, callOption, errorHandlerIl, requestIdForResponseCache, mtd, wenhao);
    }
#endif // GKMS_WINDOWS


    void* PictureBookWindowPresenter_instance = nullptr;
    std::string PictureBookWindowPresenter_charaId;
    DEFINE_HOOK(void*, PictureBookWindowPresenter_GetLiveMusics, (void* self, Il2cppString* charaId, void* mtd)) {
        // Log::DebugFmt("GetLiveMusics: %s", charaId->ToString().c_str());

        if (Config::dbgMode && Config::unlockAllLive) {
            PictureBookWindowPresenter_instance = self;
            PictureBookWindowPresenter_charaId = charaId ? charaId->ToString() : "";
            Log::DebugFmt("GetLiveMusics passthrough: self: %p, charaId: %s",
                          self, PictureBookWindowPresenter_charaId.c_str());
        }

        return PictureBookWindowPresenter_GetLiveMusics_Orig(self, charaId, mtd);
    }

    DEFINE_HOOK(void, PictureBookLiveSelectScreenModel_ctor, (void* self, void* transitionParam, UnityResolve::UnityType::List<void*>* musics, void* mtd)) {
        // Log::DebugFmt("PictureBookLiveSelectScreenModel_ctor");

        if (Config::dbgMode && Config::unlockAllLive) {
            static auto GetLiveMusics = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.OutGame",
                                                               "PictureBookWindowPresenter", "GetLiveMusics");
            if (PictureBookWindowPresenter_instance && !PictureBookWindowPresenter_charaId.empty()) {
                auto fullMusics = GetLiveMusics->Invoke<UnityResolve::UnityType::List<void*>*>(PictureBookWindowPresenter_instance,
                                                               Il2cppString::New(PictureBookWindowPresenter_charaId));
                return PictureBookLiveSelectScreenModel_ctor_Orig(self, transitionParam, fullMusics, mtd);
            }
        }

        return PictureBookLiveSelectScreenModel_ctor_Orig(self, transitionParam, musics, mtd);
    }

    bool needRestoreHides = false;
    DEFINE_HOOK(void*, PictureBookLiveSelectScreenPresenter_MoveLiveScene, (void* self, void* produceLive, bool isPlayCharacterFocusCamera, void* mtd)) {
        needRestoreHides = false;
        // Log::InfoFmt("MoveLiveScene: characterId: %s, idolCardId: %s, costumeId: %s, costumeHeadId: %s,",
        //              characterId->ToString().c_str(), idolCardId->ToString().c_str(), costumeId->ToString().c_str(), costumeHeadId->ToString().c_str());

        /*
         characterId: hski, costumeId: hski-cstm-0002, costumeHeadId: costume_head_hski-cstm-0002,
         characterId: shro, costumeId: shro-cstm-0006, costumeHeadId: costume_head_shro-cstm-0006,
         */
        /*
        if (Config::dbgMode && Config::enableLiveCustomeDress) {
            // 修改 LiveFixedData_GetCharacter 可以更改 Loading 角色和演唱者名字，而不变更实际登台人
            return PictureBookLiveSelectScreenPresenter_MoveLiveScene_Orig(self, produceLive, characterId, idolCardId,
                                                                           Config::liveCustomeCostumeId.empty() ? costumeId : Il2cppString::New(Config::liveCustomeCostumeId),
                                                                           Config::liveCustomeHeadId.empty() ? costumeHeadId : Il2cppString::New(Config::liveCustomeHeadId),
                                                                           mtd);
        }
         */
        // return PictureBookLiveSelectScreenPresenter_MoveLiveScene_Orig(self, produceLive, characterId, idolCardId, costumeId, costumeHeadId, mtd);
        return PictureBookLiveSelectScreenPresenter_MoveLiveScene_Orig(self, produceLive, isPlayCharacterFocusCamera, mtd);
    }

    // 进入 Live 时歌词默认显示：仅对每个 Presenter 实例的第一次设置强制 true，
    // 之后（玩家的手动开关）原样透传，不再覆盖关闭操作。
    std::unordered_set<void*> g_lyricsInitialForced{};

    DEFINE_HOOK(void, LiveSceneModel_set_IsLyricsActive, (void* self, bool value, void* mtd)) {
#ifdef GKMS_WINDOWS
        gakumas::vr::NoteLiveSceneModel(self);
#endif
        return LiveSceneModel_set_IsLyricsActive_Orig(self, value, mtd);
    }

    DEFINE_HOOK(void, LiveScenePresenter_SetLyricsActive, (void* self, bool value, void* mtd)) {
#ifdef GKMS_WINDOWS
        gakumas::vr::NoteLiveScenePresenter(self);
#endif
        if (Config::dbgMode && Config::unlockAllLive && self && g_lyricsInitialForced.insert(self).second) {
            value = true;
        }
        return LiveScenePresenter_SetLyricsActive_Orig(self, value, mtd);
    }

#ifdef GKMS_WINDOWS
    // Photo-scene lifecycle (right-A shutter routing). Start caches the
    // presenter; OnFinalize drops it. The Live cache is shared with LivePause;
    // the Photography cache lives in VrPhotoShutter. Targets proven offline in
    // evidence/a-button-photo/ and re-resolved on the live table at install.
    DEFINE_HOOK(void, LiveScenePresenter_Start, (void* self, void* mtd)) {
        gakumas::vr::NoteLiveScenePresenter(self);
        return LiveScenePresenter_Start_Orig(self, mtd);
    }

    DEFINE_HOOK(void, LiveScenePresenter_OnFinalize, (void* self, void* mtd)) {
        gakumas::vr::ForgetLiveScenePresenter(self);
        return LiveScenePresenter_OnFinalize_Orig(self, mtd);
    }

    DEFINE_HOOK(void, PhotographyScenePresenter_Start, (void* self, void* mtd)) {
        gakumas::vr::NotePhotographyScenePresenter(self);
        return PhotographyScenePresenter_Start_Orig(self, mtd);
    }

    DEFINE_HOOK(void, PhotographyScenePresenter_OnFinalize, (void* self, void* mtd)) {
        gakumas::vr::ForgetPhotographyScenePresenter(self);
        return PhotographyScenePresenter_OnFinalize_Orig(self, mtd);
    }

    // stereo.219 hardware: an idol-photography session produced zero
    // PHOTOGRAPHY_SCENE notes while the Live Start hook (same install block)
    // worked, so Start alone is not a reliable anchor for this presenter.
    // Extra anchors: SetEvent / SetPhotoCountView run on the presenter itself;
    // the scene model's IsInitialized setter fires a one-shot presenter scan.
    DEFINE_HOOK(void, PhotographyScenePresenter_SetEvent, (void* self, void* mtd)) {
        gakumas::vr::NotePhotographyScenePresenter(self);
        return PhotographyScenePresenter_SetEvent_Orig(self, mtd);
    }

    DEFINE_HOOK(void, PhotographyScenePresenter_SetPhotoCountView, (void* self, void* mtd)) {
        gakumas::vr::NotePhotographyScenePresenter(self);
        return PhotographyScenePresenter_SetPhotoCountView_Orig(self, mtd);
    }

    DEFINE_HOOK(void, PhotographySceneModel_set_IsInitialized, (void* self, bool value, void* mtd)) {
        gakumas::vr::NotePhotographySceneAnchorEvent();
        return PhotographySceneModel_set_IsInitialized_Orig(self, value, mtd);
    }

    // Ground truth for the "photo taken" toast: both photo scenes play this
    // per-capture thumbnail animation. Invoke success alone lies at the
    // 50-shot limit (OnPhotoButtonAsync returns without capturing).
    // Live's overload carries a by-value Vector3; on MSVC x64 a 12-byte
    // aggregate is passed by pointer, so both forward as opaque pointers.
    DEFINE_HOOK(void, LiveSceneView_PlayCapturePhotoAnimation,
                (void* self, void* data, void* position, void* mtd)) {
        gakumas::vr::NotePhotoCaptured();
        return LiveSceneView_PlayCapturePhotoAnimation_Orig(
            self, data, position, mtd);
    }

    DEFINE_HOOK(void, PhotographyPhotoContentView_PlayCapturePhotoAnimation,
                (void* self, void* data, void* mtd)) {
        gakumas::vr::NotePhotoCaptured();
        return PhotographyPhotoContentView_PlayCapturePhotoAnimation_Orig(
            self, data, mtd);
    }
#endif

    DEFINE_HOOK(void, LiveSceneContentView_SetLyricsTextActive, (void* self, bool value, void* mtd)) {
        return LiveSceneContentView_SetLyricsTextActive_Orig(self, value, mtd);
    }

    // std::string lastMusicId;
#ifdef GKMS_WINDOWS
    DEFINE_HOOK(void*, PictureBookLiveSelectScreenPresenter_OnSelectMusic, (void* retstr, void* self, void* itemModel, void* ct, void* mtd)) {
        // if (!itemModel) return nullptr;
        return PictureBookLiveSelectScreenPresenter_OnSelectMusic_Orig(retstr, self, itemModel, ct, mtd);
    }
#else
    DEFINE_HOOK(void, PictureBookLiveSelectScreenPresenter_OnSelectMusic, (void* self, void* itemModel, void* ct, void* mtd)) {
        /*  // 修改角色后，Live 结束返回时, itemModel 为 null
        Log::DebugFmt("OnSelectMusic itemModel at %p", itemModel);

        static auto GetMusic = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.OutGame",
                                                      "PlaylistMusicContext", "GetMusic");
        static auto GetCurrMusic = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.OutGame.PictureBook",
                                                      "PictureBookLiveSelectMusicListItemModel", "get_Music");
        static auto GetMusicId = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master",
                                                          "Music", "get_Id");

        static auto PictureBookLiveSelectMusicListItemModel_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.OutGame.PictureBook",
                                                                                          "PictureBookLiveSelectMusicListItemModel");
        static auto PictureBookLiveSelectMusicListItemModel_ctor = Il2cppUtils::GetMethod("Assembly-CSharp.dll", "Campus.OutGame.PictureBook",
                                                                                          "PictureBookLiveSelectMusicListItemModel", ".ctor", {"*", "*"});

        if (!itemModel) {
            Log::DebugFmt("OnSelectMusic block", itemModel);
            auto music = GetMusic->Invoke<void*>(lastMusicId);
            auto newItemModel = PictureBookLiveSelectMusicListItemModel_klass->New<void*>();
            PictureBookLiveSelectMusicListItemModel_ctor->Invoke<void>(newItemModel, music, false);

            return PictureBookLiveSelectScreenPresenter_OnSelectMusic_Orig(self, newItemModel, isFirst, mtd);
        }

        if (itemModel) {
            auto currMusic = GetCurrMusic->Invoke<void*>(itemModel);
            auto musicId = GetMusicId->Invoke<Il2cppString*>(currMusic);
            lastMusicId = musicId->ToString();
        }*/
        if (!itemModel) return;
        return PictureBookLiveSelectScreenPresenter_OnSelectMusic_Orig(self, itemModel, ct, mtd);
    }
#endif

    // Two independent paths make actor lighting follow the head under 6DoF:
    //
    //   VL.Rendering.ActorShadowPass          projected actor shadow map
    //     (VLActorShadowGroup blobs on fences, actor self-shadow silhouette)
    //   Campus.Rendering.CampusActorParameterPass  MatCap main light
    //     (UpdateActorCommand(cmd, stack, cameraTransform) + CalcLightVector,
    //      writes _MatCapMainLight — this is what shades hair and accessories
    //      onto the body, and it is not a shadow map at all)
    //
    // Both derive their direction from the rendering camera, so both are
    // bracketed the same way: the pass still runs the game's own algorithm,
    // only the camera transform it reads is pointed back at the pre-HMD
    // Cinemachine pose for the duration of the call. ScriptableRenderContext
    // is a single-pointer struct passed by value; RenderingData is byref.
    template <typename Invoke>
    void RunWithActorShadowAnchor(
        [[maybe_unused]] const char* passName,
        Invoke&& invokeOriginal) {
#ifdef GKMS_WINDOWS
        bool anchored = false;
        try {
            anchored =
                unityStereoRenderer.BeginActorShadowSourceAnchor(passName);
        } catch (...) {
            ReportVrUnityHookException();
        }
        if (anchored) {
            try {
                invokeOriginal();
            } catch (...) {
                unityStereoRenderer.EndActorShadowSourceAnchor();
                throw;
            }
            try {
                unityStereoRenderer.EndActorShadowSourceAnchor();
            } catch (...) {
                ReportVrUnityHookException();
            }
            return;
        }
#endif
        invokeOriginal();
    }

    // ActorShadowPass renders its private RTHandle from the current camera's
    // CullingResults. The live DrawShadow body passes renderingData.cullResults
    // straight to ScriptableRenderContext.DrawRenderers, so the late shot-view
    // patch cannot make caster membership identical between eyes. Cache the
    // left pass data after Setup and let the right Execute reuse the completed
    // left map. This is baseline stereo behavior, independent of the optional
    // source-camera anchor: lock off generates once from the live left eye;
    // lock on generates once from the locked source pose. OnCameraCleanup only
    // disables the receiver keyword; it does not release or clear the RTHandle,
    // so the map remains valid for the next eye.
    void ResetActorShadowLeftReuse() noexcept {
        actorShadowStereoReuse.leftPass = nullptr;
        actorShadowStereoReuse.leftReady = false;
        actorShadowStereoReuse.leftSupportsShadow = false;
        actorShadowStereoReuse.leftDistanceDataReady = false;
    }

    void PrepareActorShadowFeatureCall() noexcept {
        gakumas::vr::perf::SrpSpan span(srpPerformance, "mod.shadow-feature-prepare");
        const std::string_view role = unityStereoRenderer.ClassifyCamera(
            unityStereoRenderer.CurrentCamera());
        if (role == "left") {
            ResetActorShadowLeftReuse();
        }
    }

    void CaptureActorShadowLeftData(void* feature) noexcept {
        gakumas::vr::perf::SrpSpan span(srpPerformance, "mod.shadow-left-capture");
        auto& reuse = actorShadowStereoReuse;
        const std::string_view role = unityStereoRenderer.ClassifyCamera(
            unityStereoRenderer.CurrentCamera());
        if (role != "left" || feature == nullptr || reuse.featurePass == nullptr ||
            reuse.shadowData == nullptr || reuse.shadowBias == nullptr ||
            reuse.supportsShadow == nullptr || reuse.dataSize == 0U ||
            reuse.dataSize > reuse.leftData.size()) {
            return;
        }
        void* pass = Il2cppUtils::ClassGetFieldValue<void*>(
            feature, reuse.featurePass);
        if (pass == nullptr) {
            return;
        }
        // `_supportsShadow` is the left eye's distance/frustum decision, not a
        // validity bit for ActorShadowData. ActorShadowPass is still enqueued
        // and DrawShadow still publishes its receiver constants when this is
        // false. Dropping the capture here made the right eye fall back to an
        // independent UpdateShadowData decision exactly at the fade boundary:
        // left remained at zero while right jumped back to one. Capture and
        // reuse both outcomes so "no shadow" is stereo state too.
        reuse.leftSupportsShadow = Il2cppUtils::ClassGetFieldValue<bool>(
            pass, reuse.supportsShadow);
        std::memcpy(
            reuse.leftData.data(),
            static_cast<const std::byte*>(pass) + reuse.shadowData->offset,
            reuse.dataSize);
        reuse.leftPass = pass;
        reuse.leftReady = true;
        ++reuse.leftCaptures;
        reuse.leftDistanceDataReady = false;
        const auto readDataFloat = [&reuse](const UnityResolve::Field* field,
                                            float& value) {
            const std::int32_t offset = field != nullptr
                ? field->offset - reuse.dataFieldHeader : -1;
            if (offset < 0 ||
                static_cast<std::size_t>(offset) + sizeof(float) >
                    reuse.dataSize) {
                return false;
            }
            std::memcpy(&value,
                        reuse.leftData.data() + offset,
                        sizeof(value));
            return true;
        };
        reuse.leftDistanceDataReady =
            readDataFloat(reuse.startDistance2, reuse.leftStartDistance2) &&
            readDataFloat(reuse.endDistance2, reuse.leftEndDistance2) &&
            readDataFloat(reuse.fade, reuse.leftFade) &&
            readDataFloat(reuse.strength, reuse.leftStrength);
        if (reuse.leftCaptures <= 8U || reuse.leftCaptures % 600U == 0U) {
            std::ostringstream line;
            line << "[VR][shadow] ACTOR_SHADOW_LEFT_CAPTURE role=left"
                 << " pass=" << pass
                 << " bytes=" << reuse.dataSize
                 << " supports=" << (reuse.leftSupportsShadow ? 1 : 0)
                 << " anchor="
                 << (Config::vrActorShadowSourceAnchor ? 1 : 0)
                 << " leftCaptures=" << reuse.leftCaptures;
            if (reuse.leftDistanceDataReady) {
                line << " startDistance2=" << reuse.leftStartDistance2
                     << " endDistance2=" << reuse.leftEndDistance2
                     << " fade=" << reuse.leftFade
                     << " strength=" << reuse.leftStrength;
            }
            static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
        }
    }

    bool ReuseLeftActorShadowForRight(void* pass, void* cmd) noexcept {
        auto& reuse = actorShadowStereoReuse;
        const std::string_view role = unityStereoRenderer.ClassifyCamera(
            unityStereoRenderer.CurrentCamera());
        if (role != "right" || !reuse.leftReady || pass == nullptr ||
            pass != reuse.leftPass ||
            cmd == nullptr || reuse.shadowData == nullptr ||
            reuse.setupReceiver == nullptr || reuse.setKeyword == nullptr ||
            reuse.dataSize == 0U || reuse.dataSize > reuse.leftData.size()) {
            return false;
        }
        Il2cppString* keyword = nullptr;
        UnityResolve::Invoke<void>(
            "il2cpp_field_static_get_value",
            reuse.actorShadowsKeyword->address, &keyword);
        if (keyword == nullptr) {
            return false;
        }
        std::memcpy(
            static_cast<std::byte*>(pass) + reuse.shadowData->offset,
            reuse.leftData.data(), reuse.dataSize);
        Il2cppUtils::ClassSetFieldValue(
            pass, reuse.supportsShadow, reuse.leftSupportsShadow);
        // Match DrawShadow's proven order: enable _ACTOR_SHADOWS, then publish
        // receiver matrices/params. Execute will still bind the RTHandle as the
        // global shadow texture after this hook returns.
        reuse.setKeyword->Invoke<void>(cmd, keyword, true);
        reuse.setupReceiver->Invoke<void>(pass, cmd);
        ++reuse.rightReuses;
        if (reuse.rightReuses <= 8U || reuse.rightReuses % 600U == 0U) {
            std::ostringstream line;
            line << "[VR][shadow] ACTOR_SHADOW_LEFT_REUSE role=right"
                 << " pass=" << pass
                 << " bytes=" << reuse.dataSize
                 << " leftSupports=" << (reuse.leftSupportsShadow ? 1 : 0)
                 << " anchor="
                 << (Config::vrActorShadowSourceAnchor ? 1 : 0)
                 << " leftCaptures=" << reuse.leftCaptures
                 << " rightReuses=" << reuse.rightReuses;
            if (reuse.leftDistanceDataReady) {
                line << " startDistance2=" << reuse.leftStartDistance2
                     << " endDistance2=" << reuse.leftEndDistance2
                     << " fade=" << reuse.leftFade
                     << " strength=" << reuse.leftStrength;
            }
            static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
        }
        return true;
    }

    bool CanReuseLeftActorShadowForRight(void* pass) noexcept {
        const auto& reuse = actorShadowStereoReuse;
        const std::string_view role = unityStereoRenderer.ClassifyCamera(
            unityStereoRenderer.CurrentCamera());
        return role == "right" && reuse.leftReady && pass != nullptr &&
            pass == reuse.leftPass &&
            reuse.dataSize != 0U;
    }

    DEFINE_HOOK(void, ActorShadowPass_Configure,
                (void* self, void* cmd, void* descriptor, void* method)) {
#ifdef GKMS_WINDOWS
        bool preserveLeftMap = false;
        try {
            preserveLeftMap = CanReuseLeftActorShadowForRight(self);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        ActorShadowPass_Configure_Orig(self, cmd, descriptor, method);
#ifdef GKMS_WINDOWS
        if (!preserveLeftMap) {
            return;
        }
        try {
            // ConfigureTarget still has to run for the right pass. Merely
            // skipping Configure was insufficient: ScriptableRenderPass keeps
            // m_ClearFlag on the pass object, so the left eye's All flag could
            // still make ScriptableRenderer clear the shared RT before right
            // Execute. Override the dump-proven base field after the original
            // Configure has refreshed the target. Use the exact public API;
            // the field readback both verifies it and provides a safe fallback
            // if runtime invocation ever stops matching this Unity build.
            auto& reuse = actorShadowStereoReuse;
            constexpr std::int32_t kClearFlagNone = 0;
            reuse.configureClear->RuntimeInvoke<void>(
                self, kClearFlagNone,
                UnityResolve::UnityType::Color(0.0F, 0.0F, 0.0F, 0.0F));
            std::int32_t clearFlag =
                Il2cppUtils::ClassGetFieldValue<std::int32_t>(
                    self, reuse.clearFlag);
            if (clearFlag != kClearFlagNone) {
                Il2cppUtils::ClassSetFieldValue<std::int32_t>(
                    self, reuse.clearFlag, kClearFlagNone);
                clearFlag = Il2cppUtils::ClassGetFieldValue<std::int32_t>(
                    self, reuse.clearFlag);
            }
            ++reuse.rightClearPreserves;
            if (reuse.rightClearPreserves <= 8U ||
                reuse.rightClearPreserves % 600U == 0U) {
                std::ostringstream line;
                line << "[VR][shadow] ACTOR_SHADOW_RIGHT_PRESERVE"
                     << " role=right clearFlag=" << clearFlag
                     << " clearOffset=" << reuse.clearFlag->offset
                     << " preserves=" << reuse.rightClearPreserves;
                static_cast<void>(gakumas::vr::WriteVrLog(line.str()));
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, ActorShadowPass_DrawShadow,
                (void* self, void* cmd, void* context, void* renderingData,
                 void* method)) {
#ifdef GKMS_WINDOWS
        try {
            if (ReuseLeftActorShadowForRight(self, cmd)) {
                return;
            }
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        ActorShadowPass_DrawShadow_Orig(
            self, cmd, context, renderingData, method);
#ifdef GKMS_WINDOWS
        try {
            LogFpHeadActorShadowTag(self);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, ActorShadowPass_Execute,
                (void* self, void* context, void* renderingData, void* method)) {
        // Execute still runs (census + DrawShadow), but the projected
        // matrix is already final here. Keep the transform-swap bracket
        // so any remaining camera-transform reads inside DrawShadow see
        // the shot; the cameraData view patch lives on AddRenderPasses.
        RunWithActorShadowAnchor("actor-shadow", [&] {
            ActorShadowPass_Execute_Orig(
                self, context, renderingData, method);
        });
    }

    // The un-inlinable ancestor of Setup/UpdateShadowData: AddRenderPasses
    // is virtual (slot 7) and URP invokes it polymorphically while walking
    // the renderer-feature list, so LTCG cannot fold it into a caller.
    // .94/.95: Setup itself is inlined into this body (trampoline on the
    // Setup entry never fired); this is the only live hook point that
    // sees cameraData before UpdateShadowData writes _WorldToActorShadow.
    DEFINE_HOOK(void, DrawActorShadowPass_AddRenderPasses,
                (void* self, void* renderer, void* renderingData,
                 void* method)) {
        PrepareActorShadowFeatureCall();
        RunWithActorShadowAnchor("actor-shadow-add", [&] {
#ifdef GKMS_WINDOWS
            bool viewPatched = false;
            try {
                viewPatched = unityStereoRenderer.BeginActorShadowViewPatch(
                    renderingData);
            } catch (...) {
                ReportVrUnityHookException();
            }
            try {
                gakumas::vr::perf::SrpSpan original(srpPerformance, "feature.actor-shadow-original");
                DrawActorShadowPass_AddRenderPasses_Orig(
                    self, renderer, renderingData, method);
            } catch (...) {
                if (viewPatched) {
                    unityStereoRenderer.EndActorShadowViewPatch(renderingData);
                }
                throw;
            }
            if (viewPatched) {
                try {
                    unityStereoRenderer.EndActorShadowViewPatch(renderingData);
                } catch (...) {
                    ReportVrUnityHookException();
                }
            }
            CaptureActorShadowLeftData(self);
#else
            DrawActorShadowPass_AddRenderPasses_Orig(
                self, renderer, renderingData, method);
#endif
        });
    }

    DEFINE_HOOK(void, CampusActorParameterPass_Execute,
                (void* self, void* context, void* renderingData, void* method)) {
#ifdef GKMS_WINDOWS
        // `.179`: rebuild the official complete _OutlineParam for the
        // current camera from this pass's live settings + focal curve. The
        // stored per-role snapshot is what the eye/Grip material writes put
        // back, replacing the `.176`-condemned fixed-w synthetic vector.
        if (Config::vrRuntimeStartupEnabled) {
            try {
                unityStereoRenderer.CaptureOfficialOutlineVector(self);
            } catch (...) {
                ReportVrUnityHookException();
            }
        }
        // Ungated forensic heartbeat (2 s), before any anchor/owner gate: the
        // .90 log left mutually impossible readings behind (census starved
        // while the compensation counter in the same call path kept rising),
        // so this dumps the raw shared state from the hook entry itself.
        {
            static std::atomic<std::uint64_t> heartbeatLastMs{0};
            const std::uint64_t nowMs = GetTickCount64();
            std::uint64_t last = heartbeatLastMs.load(std::memory_order_relaxed);
            if (nowMs - last >= 2000ULL &&
                heartbeatLastMs.compare_exchange_strong(
                    last, nowMs, std::memory_order_relaxed)) {
                try {
                    unityStereoRenderer.LogShadowHeartbeat();
                } catch (...) {
                    ReportVrUnityHookException();
                }
            }
        }
#endif
        RunWithActorShadowAnchor("campus-actor-param", [&] {
            CampusActorParameterPass_Execute_Orig(
                self, context, renderingData, method);
#ifdef GKMS_WINDOWS
            // The pass just recorded the authored view-space MatCap lights
            // into this context. Rim compensate still needs the projected-
            // shadow bracket's saved head pose; authored rim (toon lock)
            // can run without that bracket.
            try {
                unityStereoRenderer.ApplyActorMatcapCompensation(
                    context, renderingData);
            } catch (...) {
                ReportVrUnityHookException();
            }
#endif
        });
    }

    DEFINE_HOOK(void, MobPenlight_UpdatePenlightParams,
                (void* self, void* context, void* camera, void* method)) {
        MobPenlight_UpdatePenlightParams_Orig(self, context, camera, method);
#ifdef GKMS_WINDOWS
        if (!Config::vrRuntimeStartupEnabled) {
            return;
        }
        try {
            gakumas::vr::AfterOfficialPenlightCamera(self);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, CrowdSystem_RenderCrowd,
                (void* self, void* commandBuffer, int eventType,
                 void* method)) {
        CrowdSystem_RenderCrowd_Orig(
            self, commandBuffer, eventType, method);
#ifdef GKMS_WINDOWS
        if (!Config::vrRuntimeStartupEnabled) {
            return;
        }
        // The original crowd was already drawn. Omit only our extra hands
        // from photo/source cameras; keep both HMD eyes and their colors intact.
        const bool photoGuard = gakumas::vr::LiveSourcePhotoProtectionActive();
        const bool eyeCamera = unityStereoRenderer.IsEyeCamera(unityStereoRenderer.CurrentCamera());
        static unsigned photoHandLogMask = 0;
        if (!photoGuard) photoHandLogMask = 0;
        const unsigned photoHandBit = eyeCamera ? 2U : 1U;
        if (photoGuard && (photoHandLogMask & photoHandBit) == 0U) {
            photoHandLogMask |= photoHandBit;
            static_cast<void>(gakumas::vr::WriteVrLog(eyeCamera
                ? "[VR][photo] AUTO_PHOTO_HANDS camera=eye action=keep"
                : "[VR][photo] AUTO_PHOTO_HANDS camera=non-eye action=skip"));
        }
        if (photoGuard && !eyeCamera) {
            return;
        }
        try {
            gakumas::vr::AfterOfficialCrowdRender(
                self, commandBuffer, eventType);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, MaterialInfo_ctor,
                (void* self, void* material, void* renderer, int materialIndex,
                 void* instancedMaterial, void* method)) {
        MaterialInfo_ctor_Orig(
            self, material, renderer, materialIndex, instancedMaterial, method);
#ifdef GKMS_WINDOWS
        // The live VL.Core.MaterialInfo constructor exposes the exact Material
        // selected for this Renderer slot. Prefer the instanced material when
        // present; the constructor stores it as the modifiable/rendered copy.
        if (!Config::vrRuntimeStartupEnabled) {
            return;
        }
        try {
            unityStereoRenderer.QueueActorOutlineMaterial(
                instancedMaterial != nullptr ? instancedMaterial : material);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
    }

    DEFINE_HOOK(void, VLDeferredPass_RenderActor,
                 (void* self, void* context, void* renderingData, void* method)) {
#ifdef GKMS_WINDOWS
        // RenderActor is the last proven managed boundary before actor GBuffer
        // draws. Its dump/live shape has no Renderer/Material argument, so use
        // it only to consume a one-shot, content-ready material discovery.
        try {
            unityStereoRenderer.PrepareActorOutlineMaterialsForCurrentDraw();
        } catch (...) {
            ReportVrUnityHookException();
        }
        // After URP SetupCameraProperties + SetCameraMatrices, immediately
        // before actor GBuffer draws. Parameter-pass cam-pos writes are
        // already overwritten by then (`.139`–`.142`); `.143`'s transform
        // poke + Setup pair is deferred past Submit and self-overwritten.
        // Begin uploads the shot cam pos for the actor draws, End restores
        // the eye value so later passes keep the true camera.
        bool locked = false;
        try {
            locked = unityStereoRenderer.BeginActorMatcapCamPosLock(
                context, renderingData);
        } catch (...) {
            ReportVrUnityHookException();
        }
#endif
        {
            gakumas::vr::perf::SrpSpan original(srpPerformance, "draw.actor-original");
            VLDeferredPass_RenderActor_Orig(self, context, renderingData, method);
        }
#ifdef GKMS_WINDOWS
        if (locked) {
            try {
                unityStereoRenderer.EndActorMatcapCamPosLock(
                    context, renderingData);
            } catch (...) {
                ReportVrUnityHookException();
            }
        }
#endif
    }

    DEFINE_HOOK(void, VLActorParameterPass_Execute,
                (void* self, void* context, void* renderingData, void* method)) {
        RunWithActorShadowAnchor("vl-actor-param", [&] {
            VLActorParameterPass_Execute_Orig(
                self, context, renderingData, method);
#ifdef GKMS_WINDOWS
            // Same MatCap re-record as the campus pass: scenes that use the
            // generic VL parameter pass (instead of the campus one) would
            // otherwise never receive the compensation.
            try {
                unityStereoRenderer.ApplyActorMatcapCompensation(
                    context, renderingData);
            } catch (...) {
                ReportVrUnityHookException();
            }
#endif
        });
    }

    DEFINE_HOOK(bool, VLDOF_IsActive, (void* self)) {
        if (IsLocalifyFreeCameraEnabled()) return false;
        const bool originalActive = VLDOF_IsActive_Orig(self);
#ifdef GKMS_WINDOWS
        if (unityStereoRenderer.ShouldSuppressDepthOfField(self, originalActive)) {
            return false;
        }
#endif
        return originalActive;
    }

    DEFINE_HOOK(bool, URPDOF_IsActive, (void* self)) {
        if (IsLocalifyFreeCameraEnabled()) return false;
        const bool originalActive = URPDOF_IsActive_Orig(self);
#ifdef GKMS_WINDOWS
        if (unityStereoRenderer.ShouldSuppressDepthOfField(self, originalActive)) {
            return false;
        }
#endif
        return originalActive;
    }

    DEFINE_HOOK(void, CampusQualityManager_set_TargetFrameRate, (void* self, float value)) {
        // Log::InfoFmt("CampusQualityManager_set_TargetFrameRate: %f", value);
        const auto configFps = Config::targetFrameRate;
        CampusQualityManager_set_TargetFrameRate_Orig(self, configFps == 0 ? value : (float)configFps);
    }

    DEFINE_HOOK(void, CampusQualityManager_ApplySetting, (void* self, int qualitySettingsLevel, int maxBufferPixel, float renderScale, int volumeIndex)) {
        if (Config::targetFrameRate != 0) {
            CampusQualityManager_set_TargetFrameRate_Orig(self, Config::targetFrameRate);
        }
        if (Config::useCustomeGraphicSettings) {
            static auto SetReflectionQuality = Il2cppUtils::GetMethod("campus-submodule.Runtime.dll", "Campus.Common",
                                                                      "CampusQualityManager", "SetReflectionQuality");
            static auto SetLODQuality = Il2cppUtils::GetMethod("campus-submodule.Runtime.dll", "Campus.Common",
                                                               "CampusQualityManager", "SetLODQuality");

            static auto Enum_GetValues = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Enum", "GetValues");

            static auto QualityLevel_klass = Il2cppUtils::GetClass("campus-submodule.Runtime.dll", "", "QualityLevel");

            static auto values = Enum_GetValues->Invoke<UnityResolve::UnityType::Array<int>*>(QualityLevel_klass->GetType())->ToVector();
            if (values.empty()) {
                values = {0x0, 0xa, 0x14, 0x1e, 0x28, 0x64};
            }
            if (Config::lodQualityLevel >= values.size()) Config::lodQualityLevel = values.size() - 1;
            if (Config::reflectionQualityLevel >= values.size()) Config::reflectionQualityLevel = values.size() - 1;

            SetLODQuality->Invoke<void>(self, values[Config::lodQualityLevel]);
            SetReflectionQuality->Invoke<void>(self, values[Config::reflectionQualityLevel]);

            qualitySettingsLevel = Config::qualitySettingsLevel;
            maxBufferPixel = Config::maxBufferPixel;
            renderScale = Config::renderScale;
            volumeIndex = Config::volumeIndex;

            Log::ShowToastFmt("ApplySetting\nqualityLevel: %d, maxBufferPixel: %d\nenderScale: %f, volumeIndex: %d\nLODQualityLv: %d, ReflectionLv: %d",
                              qualitySettingsLevel, maxBufferPixel, renderScale, volumeIndex, Config::lodQualityLevel, Config::reflectionQualityLevel);
        }

        CampusQualityManager_ApplySetting_Orig(self, qualitySettingsLevel, maxBufferPixel, renderScale, volumeIndex);
    }

    DEFINE_HOOK(void, UIManager_UpdateRenderTarget, (UnityResolve::UnityType::Vector2 ratio, void* mtd)) {
        // const auto resolution = GetResolution();
        // Log::DebugFmt("UIManager_UpdateRenderTarget: %f, %f", ratio.x, ratio.y);
        return UIManager_UpdateRenderTarget_Orig(ratio, mtd);
    }

    DEFINE_HOOK(void, VLSRPCameraController_UpdateRenderTarget, (void* self, int width, int height, bool forceAlpha, void* method)) {
        // const auto resolution = GetResolution();
        // Log::DebugFmt("VLSRPCameraController_UpdateRenderTarget: %d, %d", width, height);
        return VLSRPCameraController_UpdateRenderTarget_Orig(self, width, height, forceAlpha, method);
    }

    DEFINE_HOOK(void*, VLUtility_GetLimitedResolution, (int32_t screenWidth, int32_t screenHeight,
            UnityResolve::UnityType::Vector2 aspectRatio, int32_t maxBufferPixel, float bufferScale, bool firstCall)) {

        if (Config::useCustomeGraphicSettings && (Config::renderScale > 1.0f)) {
            screenWidth *= Config::renderScale;
            screenHeight *= Config::renderScale;
        }
        //Log::DebugFmt("VLUtility_GetLimitedResolution: %d, %d, %f, %f", screenWidth, screenHeight, aspectRatio.x, aspectRatio.y);
        return VLUtility_GetLimitedResolution_Orig(screenWidth, screenHeight, aspectRatio, maxBufferPixel, bufferScale, firstCall);
    }


    DEFINE_HOOK(void, CampusActorModelParts_OnRegisterBone, (void* self, Il2cppString** name, UnityResolve::UnityType::Transform* bone)) {
        CampusActorModelParts_OnRegisterBone_Orig(self, name, bone);
        // Log::DebugFmt("CampusActorModelParts_OnRegisterBone: %s, %p", (*name)->ToString().c_str(), bone);
    }

    bool InitBodyParts() {
        static auto isInit = false;
        if (isInit) return true;

        const auto Enum_GetValues = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Enum", "GetValues");
        const auto Enum_GetNames = Il2cppUtils::GetMethod("mscorlib.dll", "System", "Enum", "GetNames");

        const auto HumanBodyBones_klass = Il2cppUtils::GetClass(
                "UnityEngine.AnimationModule.dll", "UnityEngine", "HumanBodyBones");

        const auto values = Enum_GetValues->Invoke<UnityResolve::UnityType::Array<int>*>(HumanBodyBones_klass->GetType())->ToVector();
        const auto names = Enum_GetNames->Invoke<UnityResolve::UnityType::Array<Il2cppString*>*>(HumanBodyBones_klass->GetType())->ToVector();
        if (values.size() != names.size()) {
            Log::ErrorFmt("InitBodyParts Error: values count: %ld, names count: %ld", values.size(), names.size());
            return false;
        }

        std::vector<std::string> namesVec{};
        namesVec.reserve(names.size());
        for (auto i :names) {
            namesVec.push_back(i->ToString());
        }
        GKCamera::bodyPartsEnum = Misc::CSEnum(namesVec, values);
        GKCamera::bodyPartsEnum.SetIndex(GKCamera::bodyPartsEnum.GetValueByName("Head"));
        isInit = true;
        return true;
    }

    // SEH-guarded m_CachedPtr probe. The cached hidden-head shells can be
    // GC-collected while we hold them (our DLL statics are not GC roots), so
    // even reading the wrapper must tolerate a freed page.
    bool IsManagedShellAlive(void* instance) noexcept {
        if (instance == nullptr) {
            return false;
        }
        __try {
            if (*static_cast<void**>(instance) == nullptr) {
                return false;
            }
            return reinterpret_cast<UnityResolve::UnityType::UnityObject*>(instance)
                       ->m_CachedPtr != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // FP head skip (.287): keep face/hair GameObjects active and
    // Renderers enabled so VL skinning stays posed and the official
    // ActorShadowPass.DrawRenderers (same camera cull) still sees a
    // head. Hide wearer color/depth only via Material.SetShaderPassEnabled
    // on instanced get_materials, using CampusActorShader.Pass GBuffer
    // and DepthOnly names. Do not disable ShadowCaster. Do not
    // SetActive(false). Do not write Renderer.enabled / layer mask
    // (.282/.285/.286 killed VL shells and/or left the blob headless).
    // Restore pass enables on leave-FP / retarget.
    // See evidence/fp-head-hide/.
    constexpr int kFpHeadColorSkipPassCount = 5;
    constexpr const char* kFpHeadColorSkipPassNames[kFpHeadColorSkipPassCount] = {
        "UniversalGBufferActor",
        "UniversalGBufferActorHair",
        "UniversalGBufferOutline",
        "DepthOnly",
        "DepthOnlyPass2",
    };

    struct FpHeadSlot {
        Il2CppGCHandle goHandle = nullptr;
        int rendererCount = 0;
        int materialCount = 0;
    };

    struct FpHeadSkipApi {
        UnityResolve::Method* getComponentsInChildren = nullptr;
        UnityResolve::Method* getMaterials = nullptr;
        UnityResolve::Method* getMaterial = nullptr;
        UnityResolve::Method* setShaderPassEnabled = nullptr;
        UnityResolve::Method* getShaderPassEnabled = nullptr;
        UnityResolve::Method* shaderTagIdCtor = nullptr;
        UnityResolve::Field* actorShadowTag = nullptr;
        UnityResolve::Field* preDepthTag = nullptr;
        void* rendererType = nullptr;
        Il2CppGCHandle passNameHandles[kFpHeadColorSkipPassCount]{};
        Il2cppString* passNames[kFpHeadColorSkipPassCount]{};
        bool resolveLogged = false;
        bool shadowTagLogged = false;

        [[nodiscard]] bool Ready() const noexcept {
            return getComponentsInChildren != nullptr &&
                getMaterials != nullptr &&
                setShaderPassEnabled != nullptr &&
                rendererType != nullptr &&
                passNames[0] != nullptr;
        }
    };

    FpHeadSlot fpHeadFaceSlot{};
    FpHeadSlot fpHeadHairSlot{};
    FpHeadSkipApi fpHeadSkipApi{};
    bool fpHeadColorSkipApplied = false;

    UnityResolve::Method* FindExactInstanceMethod(
        UnityResolve::Class* klass,
        std::string_view name,
        std::string_view returnType,
        const std::initializer_list<std::string_view>& argTypes) {
        if (klass == nullptr) {
            return nullptr;
        }
        UnityResolve::Method* found = nullptr;
        for (auto* candidate : klass->methods) {
            if (candidate == nullptr || candidate->name != name ||
                candidate->static_function ||
                candidate->address == nullptr ||
                candidate->return_type == nullptr ||
                candidate->return_type->name != returnType ||
                candidate->args.size() != argTypes.size()) {
                continue;
            }
            bool exact = true;
            std::size_t index = 0;
            for (const auto argType : argTypes) {
                if (candidate->args[index] == nullptr ||
                    candidate->args[index]->pType == nullptr ||
                    std::string_view(candidate->args[index]->pType->name) !=
                        argType) {
                    exact = false;
                    break;
                }
                ++index;
            }
            if (!exact) {
                continue;
            }
            if (found != nullptr) {
                return nullptr;
            }
            found = candidate;
        }
        return found;
    }

    UnityResolve::Method* FindUniqueInstanceMethod(
        UnityResolve::Class* klass,
        std::string_view name,
        const std::initializer_list<std::string_view>& argTypes) {
        if (klass == nullptr) {
            return nullptr;
        }
        UnityResolve::Method* found = nullptr;
        for (auto* candidate : klass->methods) {
            if (candidate == nullptr || candidate->name != name ||
                candidate->static_function ||
                candidate->address == nullptr ||
                candidate->args.size() != argTypes.size()) {
                continue;
            }
            bool exact = true;
            std::size_t index = 0;
            for (const auto argType : argTypes) {
                if (candidate->args[index] == nullptr ||
                    candidate->args[index]->pType == nullptr ||
                    std::string_view(candidate->args[index]->pType->name) !=
                        argType) {
                    exact = false;
                    break;
                }
                ++index;
            }
            if (!exact) {
                continue;
            }
            if (found != nullptr) {
                return nullptr;
            }
            found = candidate;
        }
        return found;
    }

    bool EnsureFpHeadSkipApi() noexcept {
        if (fpHeadSkipApi.Ready()) {
            return true;
        }
        auto* gameObjectClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
        auto* rendererClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer");
        auto* materialClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Material");
        auto* shaderTagClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll",
            "UnityEngine.Rendering",
            "ShaderTagId");
        auto* actorShadowPassClass = Il2cppUtils::GetClass(
            "vl-unity.Runtime.dll", "VL.Rendering", "ActorShadowPass");
        auto* preDepthClass = Il2cppUtils::GetClass(
            "vl-unity.Runtime.dll", "VL.Rendering", "VLPreDepthIDPass");
        fpHeadSkipApi.getComponentsInChildren = FindExactInstanceMethod(
            gameObjectClass,
            "GetComponentsInChildren",
            "UnityEngine.Component[]",
            {"System.Type", "System.Boolean"});
        if (fpHeadSkipApi.getComponentsInChildren == nullptr) {
            // Some IL2CPP dumps spell the array as Component[] without
            // the UnityEngine prefix. Args are the public contract.
            UnityResolve::Method* fallback = nullptr;
            if (gameObjectClass != nullptr) {
                for (auto* candidate : gameObjectClass->methods) {
                    if (candidate == nullptr ||
                        candidate->name != "GetComponentsInChildren" ||
                        candidate->static_function ||
                        candidate->args.size() != 2U ||
                        candidate->args[0] == nullptr ||
                        candidate->args[0]->pType == nullptr ||
                        candidate->args[1] == nullptr ||
                        candidate->args[1]->pType == nullptr ||
                        std::string_view(candidate->args[0]->pType->name) !=
                            "System.Type" ||
                        std::string_view(candidate->args[1]->pType->name) !=
                            "System.Boolean") {
                        continue;
                    }
                    if (fallback != nullptr) {
                        fallback = nullptr;
                        break;
                    }
                    fallback = candidate;
                }
            }
            fpHeadSkipApi.getComponentsInChildren = fallback;
        }
        fpHeadSkipApi.getMaterials = FindExactInstanceMethod(
            rendererClass,
            "get_materials",
            "UnityEngine.Material[]",
            {});
        if (fpHeadSkipApi.getMaterials == nullptr) {
            fpHeadSkipApi.getMaterials = FindUniqueInstanceMethod(
                rendererClass, "get_materials", {});
        }
        fpHeadSkipApi.getMaterial = FindExactInstanceMethod(
            rendererClass, "get_material", "UnityEngine.Material", {});
        if (fpHeadSkipApi.getMaterial == nullptr) {
            fpHeadSkipApi.getMaterial = FindUniqueInstanceMethod(
                rendererClass, "get_material", {});
        }
        fpHeadSkipApi.setShaderPassEnabled = FindExactInstanceMethod(
            materialClass,
            "SetShaderPassEnabled",
            "System.Void",
            {"System.String", "System.Boolean"});
        if (fpHeadSkipApi.setShaderPassEnabled == nullptr) {
            fpHeadSkipApi.setShaderPassEnabled = FindUniqueInstanceMethod(
                materialClass,
                "SetShaderPassEnabled",
                {"System.String", "System.Boolean"});
        }
        fpHeadSkipApi.getShaderPassEnabled = FindExactInstanceMethod(
            materialClass,
            "GetShaderPassEnabled",
            "System.Boolean",
            {"System.String"});
        fpHeadSkipApi.shaderTagIdCtor = FindExactInstanceMethod(
            shaderTagClass, ".ctor", "System.Void", {"System.String"});
        if (rendererClass != nullptr) {
            fpHeadSkipApi.rendererType = rendererClass->GetType();
        }
        if (actorShadowPassClass != nullptr) {
            fpHeadSkipApi.actorShadowTag =
                actorShadowPassClass->Get<UnityResolve::Field>("_shaderTag");
        }
        if (preDepthClass != nullptr) {
            fpHeadSkipApi.preDepthTag =
                preDepthClass->Get<UnityResolve::Field>("_depthIDShaderTagId");
        }
        for (int i = 0; i < kFpHeadColorSkipPassCount; ++i) {
            if (fpHeadSkipApi.passNames[i] != nullptr) {
                continue;
            }
            auto* name = Il2cppString::New(kFpHeadColorSkipPassNames[i]);
            if (name == nullptr) {
                continue;
            }
            fpHeadSkipApi.passNameHandles[i] =
                UnityResolve::Invoke<Il2CppGCHandle>(
                    "il2cpp_gchandle_new", name, false);
            fpHeadSkipApi.passNames[i] = name;
        }
        if (!fpHeadSkipApi.resolveLogged) {
            fpHeadSkipApi.resolveLogged = true;
            int named = 0;
            for (int i = 0; i < kFpHeadColorSkipPassCount; ++i) {
                if (fpHeadSkipApi.passNames[i] != nullptr) {
                    ++named;
                }
            }
            std::ostringstream stream;
            stream << "[VR][camera] FREECAM_HEAD_SKIP_API ready="
                   << (fpHeadSkipApi.Ready() ? 1 : 0)
                   << " getComp="
                   << (fpHeadSkipApi.getComponentsInChildren != nullptr ? 1 : 0)
                   << " getMats="
                   << (fpHeadSkipApi.getMaterials != nullptr ? 1 : 0)
                   << " getMat="
                   << (fpHeadSkipApi.getMaterial != nullptr ? 1 : 0)
                   << " setPass="
                   << (fpHeadSkipApi.setShaderPassEnabled != nullptr ? 1 : 0)
                   << " getPass="
                   << (fpHeadSkipApi.getShaderPassEnabled != nullptr ? 1 : 0)
                   << " type="
                   << (fpHeadSkipApi.rendererType != nullptr ? 1 : 0)
                   << " names=" << named
                   << " shadowTag="
                   << (fpHeadSkipApi.actorShadowTag != nullptr ? 1 : 0)
                   << " predepthTag="
                   << (fpHeadSkipApi.preDepthTag != nullptr ? 1 : 0);
            static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
        }
        return fpHeadSkipApi.Ready();
    }

    UnityResolve::UnityType::GameObject* ResolveFpHeadGo(
        const FpHeadSlot& slot) noexcept {
        if (slot.goHandle == nullptr) {
            return nullptr;
        }
        const auto target =
            UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", slot.goHandle);
        if (!IsManagedShellAlive(target)) {
            return nullptr;
        }
        return static_cast<UnityResolve::UnityType::GameObject*>(target);
    }

    void ReleaseFpHeadHandle(Il2CppGCHandle& handle) noexcept {
        if (handle != nullptr) {
            UnityResolve::Invoke<void>(
                "il2cpp_gchandle_free", std::exchange(handle, nullptr));
        }
    }

    void ReleaseFpHeadSlot(FpHeadSlot& slot) noexcept {
        slot.rendererCount = 0;
        slot.materialCount = 0;
        ReleaseFpHeadHandle(slot.goHandle);
    }

    void WriteFpHeadMaterialPasses(void* material, bool hidden) {
        for (int i = 0; i < kFpHeadColorSkipPassCount; ++i) {
            auto* name = fpHeadSkipApi.passNames[i];
            if (name == nullptr) {
                continue;
            }
            fpHeadSkipApi.setShaderPassEnabled->Invoke<void>(
                material,
                name,
                !hidden,
                fpHeadSkipApi.setShaderPassEnabled->address);
        }
    }

    void WriteFpHeadRendererPasses(
        void* renderer, bool hidden, int& wrote, int& materials) {
        bool wroteAny = false;
        if (fpHeadSkipApi.getMaterials != nullptr) {
            try {
                auto* array =
                    fpHeadSkipApi.getMaterials
                        ->Invoke<UnityResolve::UnityType::Array<void*>*>(
                            renderer, fpHeadSkipApi.getMaterials->address);
                if (array != nullptr) {
                    const int count = static_cast<int>(array->max_length);
                    for (int i = 0; i < count; ++i) {
                        void* material = array->At(static_cast<unsigned>(i));
                        if (!IsManagedShellAlive(material)) {
                            continue;
                        }
                        WriteFpHeadMaterialPasses(material, hidden);
                        ++materials;
                        wroteAny = true;
                    }
                }
            } catch (...) {
            }
        }
        if (wroteAny) {
            ++wrote;
            return;
        }
        if (fpHeadSkipApi.getMaterial == nullptr) {
            return;
        }
        try {
            void* material = fpHeadSkipApi.getMaterial->Invoke<void*>(
                renderer, fpHeadSkipApi.getMaterial->address);
            if (!IsManagedShellAlive(material)) {
                return;
            }
            WriteFpHeadMaterialPasses(material, hidden);
            ++materials;
            ++wrote;
        } catch (...) {
        }
    }

    void WriteFpHeadSlot(
        FpHeadSlot& slot, bool hidden, int& wrote, int& dead) noexcept {
        if (!EnsureFpHeadSkipApi()) {
            return;
        }
        auto* go = ResolveFpHeadGo(slot);
        if (go == nullptr) {
            if (slot.goHandle != nullptr) {
                ++dead;
            }
            return;
        }
        auto* array = fpHeadSkipApi.getComponentsInChildren
            ->Invoke<UnityResolve::UnityType::Array<void*>*>(
                go, fpHeadSkipApi.rendererType, true);
        if (array == nullptr) {
            ++dead;
            return;
        }
        slot.rendererCount = 0;
        slot.materialCount = 0;
        const int count = static_cast<int>(array->max_length);
        for (int i = 0; i < count; ++i) {
            void* renderer = array->At(static_cast<unsigned>(i));
            if (!IsManagedShellAlive(renderer)) {
                ++dead;
                continue;
            }
            ++slot.rendererCount;
            const int wroteBefore = wrote;
            WriteFpHeadRendererPasses(
                renderer, hidden, wrote, slot.materialCount);
            if (wrote == wroteBefore) {
                ++dead;
            }
        }
    }

    void LogFpHeadSkip(
        const char* action,
        const char* reason,
        bool hidden,
        int wrote,
        int dead) noexcept {
        static bool lastHidden = false;
        static std::string lastAction;
        static std::string lastReason;
        static std::int64_t lastNs = 0;
        const std::int64_t nowNs =
            gakumas::vr::pose::MonotonicNowNanoseconds();
        const bool changed = hidden != lastHidden ||
            lastAction != action || lastReason != reason;
        if (!changed && nowNs - lastNs < 1'000'000'000LL) {
            return;
        }
        lastHidden = hidden;
        lastAction = action;
        lastReason = reason;
        lastNs = nowNs;
        std::ostringstream stream;
        stream << "[VR][camera] FREECAM_HEAD_SKIP action=" << action
               << " reason=" << reason
               << " hidden=" << (hidden ? 1 : 0)
               << " wrote=" << wrote
               << " dead=" << dead
               << " face=" << fpHeadFaceSlot.rendererCount
               << " hair=" << fpHeadHairSlot.rendererCount
               << " mats="
               << (fpHeadFaceSlot.materialCount + fpHeadHairSlot.materialCount)
               << " applied=" << (fpHeadColorSkipApplied ? 1 : 0);
        static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
    }

    bool FpHeadSkipHasTargets() noexcept {
        return fpHeadFaceSlot.goHandle != nullptr ||
            fpHeadHairSlot.goHandle != nullptr;
    }

    void SetFpHeadColorSkip(bool hidden, const char* reason) noexcept {
        if (!FpHeadSkipHasTargets()) {
            return;
        }
        if (!hidden && !fpHeadColorSkipApplied) {
            LogFpHeadSkip("hold", reason, hidden, 0, 0);
            return;
        }
        const bool firstHide = hidden && !fpHeadColorSkipApplied;
        int wrote = 0;
        int dead = 0;
        WriteFpHeadSlot(fpHeadFaceSlot, hidden, wrote, dead);
        WriteFpHeadSlot(fpHeadHairSlot, hidden, wrote, dead);
        fpHeadColorSkipApplied = hidden;
        LogFpHeadSkip(
            hidden ? (firstHide ? "apply" : "refresh") : "restore",
            reason,
            hidden,
            wrote,
            dead);
    }

    std::int32_t MakeFpHeadShaderTagId(const char* name) noexcept {
        if (fpHeadSkipApi.shaderTagIdCtor == nullptr || name == nullptr) {
            return 0;
        }
        struct ShaderTagIdValue {
            std::int32_t id;
        };
        ShaderTagIdValue tag{};
        try {
            fpHeadSkipApi.shaderTagIdCtor->Invoke<void>(
                &tag,
                Il2cppString::New(name),
                fpHeadSkipApi.shaderTagIdCtor->address);
        } catch (...) {
            return 0;
        }
        return tag.id;
    }

    void LogFpHeadActorShadowTag(void* pass) noexcept {
        if (fpHeadSkipApi.shadowTagLogged || pass == nullptr) {
            return;
        }
        fpHeadSkipApi.shadowTagLogged = true;
        if (!EnsureFpHeadSkipApi()) {
            return;
        }
        std::int32_t live = 0;
        std::int32_t predepth = 0;
        int haveLive = 0;
        int havePre = 0;
        if (fpHeadSkipApi.actorShadowTag != nullptr &&
            !fpHeadSkipApi.actorShadowTag->static_field &&
            fpHeadSkipApi.actorShadowTag->offset >= 0) {
            live = *reinterpret_cast<std::int32_t*>(
                static_cast<std::byte*>(pass) +
                fpHeadSkipApi.actorShadowTag->offset);
            haveLive = 1;
        }
        if (fpHeadSkipApi.preDepthTag != nullptr &&
            fpHeadSkipApi.preDepthTag->static_field) {
            fpHeadSkipApi.preDepthTag->GetValue(&predepth);
            havePre = 1;
        }
        const std::int32_t caster = MakeFpHeadShaderTagId("ShadowCaster");
        const std::int32_t gbuffer =
            MakeFpHeadShaderTagId("UniversalGBufferActor");
        const std::int32_t hair =
            MakeFpHeadShaderTagId("UniversalGBufferActorHair");
        const std::int32_t depth = MakeFpHeadShaderTagId("DepthOnly");
        std::ostringstream stream;
        stream << "[VR][camera] FREECAM_HEAD_SKIP action=shadow-tag"
               << " live=" << live
               << " haveLive=" << haveLive
               << " caster=" << caster
               << " gbuffer=" << gbuffer
               << " hair=" << hair
               << " depth=" << depth
               << " matchCaster=" << (haveLive && live == caster ? 1 : 0)
               << " matchGbuffer=" << (haveLive && live == gbuffer ? 1 : 0)
               << " matchHair=" << (haveLive && live == hair ? 1 : 0)
               << " matchDepth=" << (haveLive && live == depth ? 1 : 0)
               << " predepth=" << predepth
               << " havePre=" << havePre
               << " preDepthOnly=" << (havePre && predepth == depth ? 1 : 0)
               << " preGbuffer=" << (havePre && predepth == gbuffer ? 1 : 0);
        static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
    }

    void PublishFpHeadSlot(
        FpHeadSlot& slot, UnityResolve::UnityType::GameObject* obj) noexcept {
        SetFpHeadColorSkip(false, "retarget");
        ReleaseFpHeadSlot(slot);
        if (obj == nullptr || !IsNativeObjectAlive(obj)) {
            return;
        }
        // Keep the GO active so VLActorFaceModel does not OnDisable /
        // Dispose. Color skip is SetShaderPassEnabled, not SetActive.
        obj->SetActive(true);
        slot.goHandle = UnityResolve::Invoke<Il2CppGCHandle>(
            "il2cpp_gchandle_new", obj, false);
        std::ostringstream stream;
        stream << "[VR][camera] FREECAM_HEAD_SKIP action=publish"
               << " face=" << fpHeadFaceSlot.rendererCount
               << " hair=" << fpHeadHairSlot.rendererCount;
        static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
    }

    void HideHead(UnityResolve::UnityType::GameObject* obj, const bool isFace) {
        // Face/hair objects outlive frames and character switches. A raw
        // static pointer is not a GC root (stereo.213). Root the GO and
        // re-resolve materials on every write.
        auto& slot = isFace ? fpHeadFaceSlot : fpHeadHairSlot;
        const auto lastGo = ResolveFpHeadGo(slot);
        const auto isFirstPerson =
            GKCamera::GetCameraMode() == GKCamera::CameraMode::FIRST_PERSON;

        if (isFirstPerson && obj) {
            if (obj == lastGo) {
                SetFpHeadColorSkip(true, "late-update");
                return;
            }
            PublishFpHeadSlot(slot, obj);
            SetFpHeadColorSkip(true, "publish");
        } else {
            SetFpHeadColorSkip(false, "leave-fp");
            if (lastGo != nullptr && IsNativeObjectAlive(lastGo)) {
                lastGo->SetActive(true);
            }
            ReleaseFpHeadSlot(slot);
        }
    }

    DEFINE_HOOK(void, CampusActorController_LateUpdate, (void* self, void* mtd)) {
        static auto CampusActorController_klass = Il2cppUtils::GetClass("campus-submodule.Runtime.dll",
                                                                        "Campus.Common", "CampusActorController");
        static auto rootBody_field = CampusActorController_klass->Get<UnityResolve::Field>("_rootBody");
        static auto parentKlass = UnityResolve::Invoke<void*>("il2cpp_class_get_parent", CampusActorController_klass->address);

        // The VR free camera needs the same bone sampling as the upstream
        // FOLLOW/FIRST_PERSON modes even though Config::enableFreeCamera
        // stays false in VR builds. The GKCamera mode is mirrored from the
        // VR mode every frame, so the FREE check below covers both worlds.
        // Toon lighting needs a hip/chest census only for the explicit
        // free-cam player/headset look-at modes. Baseline left-eye sharing and
        // the source-rig lock target do not sample actors.
        const bool vrBoneAnchorWanted =
            gakumas::vr::camera::IsVrFreeCameraBoneAnchorWanted();
        const bool vrToonCensusWanted =
            Config::vrRuntimeStartupEnabled &&
            Config::vrActorToonSourceAnchor &&
            Config::vrToonFollowRef != Config::kVrToonFollowRefSource &&
            gakumas::vr::camera::IsVrFreeCameraLocomotionActive();
        const bool wantFollowWork =
            IsLocalifyFreeCameraEnabled() || vrBoneAnchorWanted;
        const bool freeMode =
            GKCamera::GetCameraMode() == GKCamera::CameraMode::FREE;
        if (!wantFollowWork && !vrToonCensusWanted) {
            if (needRestoreHides) {
                needRestoreHides = false;
                HideHead(nullptr, false);
                HideHead(nullptr, true);
            }
            return CampusActorController_LateUpdate_Orig(self, mtd);
        }

        static auto GetHumanBodyBoneTransform_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(parentKlass, "GetHumanBodyBoneTransform", 1);
        static auto GetHumanBodyBoneTransform = GetHumanBodyBoneTransform_mtd
            ? reinterpret_cast<UnityResolve::UnityType::Transform* (*)(void*, int)>(
                GetHumanBodyBoneTransform_mtd->methodPointer)
            : nullptr;
        static auto get_index_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(CampusActorController_klass->address, "get_index", 0);
        static auto get_Index = get_index_mtd ? reinterpret_cast<int (*)(void*)>(
                get_index_mtd->methodPointer) : [](void*){return 0;};

        const auto currIndex = get_Index(self);

        static auto humanBodyBoneMap_field =
            UnityResolve::Invoke<Il2cppUtils::FieldInfo*>(
                "il2cpp_class_get_field_from_name",
                CampusActorController_klass->address, "_humanBodyBoneMap");
        static bool fieldMissLogged = false;
        if (humanBodyBoneMap_field == nullptr && !fieldMissLogged) {
            fieldMissLogged = true;
            static_cast<void>(gakumas::vr::WriteVrLog(
                "[VR][camera] FREECAM_BONES_FIELD_MISSING "
                "_humanBodyBoneMap not found; skipping readiness gate"));
        }
        const bool bonesReady = humanBodyBoneMap_field
            ? Il2cppUtils::ClassGetFieldValue<void*>(
                  self, humanBodyBoneMap_field) != nullptr
            : true;

        if (vrToonCensusWanted && bonesReady &&
            GetHumanBodyBoneTransform != nullptr) {
            constexpr int kChestBone = 8;
            constexpr int kHipsBone = 0;
            auto* toonTrans = GetHumanBodyBoneTransform(self, kChestBone);
            if (toonTrans == nullptr) {
                toonTrans = GetHumanBodyBoneTransform(self, kHipsBone);
            }
            if (toonTrans != nullptr) {
                const auto pos = toonTrans->GetPosition();
                const auto fwd = toonTrans->GetForward();
                unityStereoRenderer.NoteToonActorSample(
                    currIndex,
                    {pos.x, pos.y, pos.z},
                    {fwd.x, fwd.y, fwd.z});
            }
        }

        if (!wantFollowWork || freeMode) {
            if (needRestoreHides) {
                needRestoreHides = false;
                HideHead(nullptr, false);
                HideHead(nullptr, true);
            }
            return CampusActorController_LateUpdate_Orig(self, mtd);
        }

        if (vrBoneAnchorWanted) {
            const std::int64_t nowNs =
                gakumas::vr::pose::MonotonicNowNanoseconds();
            RecordVrCameraActorIndex(currIndex, nowNs);

            // 1 Hz probe: proves on hardware whether this hook runs in a
            // scene at all and which indices exist vs the follow target.
            static std::int64_t lastProbeNanoseconds = 0;
            if (nowNs - lastProbeNanoseconds > 1'000'000'000LL) {
                lastProbeNanoseconds = nowNs;
                int seen[kVrCameraSeenActorCapacity];
                const std::size_t seenCount =
                    CollectFreshVrCameraActorIndices(nowNs, seen);
                std::ostringstream stream;
                stream << "[VR][camera] FREECAM_ANCHOR_PROBE follow="
                       << GKCamera::followCharaIndex
                       << " anchorFresh=" << (IsVrCameraAnchorFresh(nowNs) ? 1 : 0)
                       << " seen=";
                for (std::size_t i = 0; i < seenCount; ++i) {
                    if (i > 0) stream << ',';
                    stream << seen[i];
                }
                if (seenCount == 0) stream << '-';
                static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
            }
        }

        if (currIndex == GKCamera::followCharaIndex) {
            static auto initPartsSuccess = InitBodyParts();
            static auto headBodyId = initPartsSuccess ? GKCamera::bodyPartsEnum.GetValueByName("Head") : 0xA;
            const auto isFirstPerson = GKCamera::GetCameraMode() == GKCamera::CameraMode::FIRST_PERSON;

            // Live loading spawns actors whose LateUpdate runs before
            // InitializeBones. Calling GetHumanBodyBoneTransform then makes
            // the game log "InitializeBonesを呼び出して下さい" every frame and
            // crashed the stereo.216 Live run (Crash_2026-08-29_184706589).
            // VLDefaultActorController._humanBodyBoneMap is null until
            // InitializeBones fills it, so gate the query on that field.
            if (!bonesReady) {
                cacheTrans = nullptr;
                if (vrBoneAnchorWanted) {
                    static std::int64_t lastNotReadyNanoseconds = 0;
                    const std::int64_t nowNs =
                        gakumas::vr::pose::MonotonicNowNanoseconds();
                    if (nowNs - lastNotReadyNanoseconds > 1'000'000'000LL) {
                        lastNotReadyNanoseconds = nowNs;
                        std::ostringstream stream;
                        stream << "[VR][camera] FREECAM_BONES_NOT_READY index="
                               << currIndex;
                        static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
                    }
                }
                return CampusActorController_LateUpdate_Orig(self, mtd);
            }

            // The VR FOLLOW bone lives in an atomic owned by VrFreeCamera
            // (menu / Y-button switchable); the upstream keyboard path keeps
            // using bodyPartsEnum.
            const int followBoneId = vrBoneAnchorWanted
                ? gakumas::vr::camera::ReadVrFreeCameraFollowBone()
                : GKCamera::bodyPartsEnum.GetCurrent().second;
            auto targetTrans = GetHumanBodyBoneTransform != nullptr
                ? GetHumanBodyBoneTransform(
                      self, isFirstPerson ? headBodyId : followBoneId)
                : nullptr;

            if (targetTrans) {
                cacheTrans = targetTrans;
                cacheRotation = cacheTrans->GetRotation();
                cachePosition = cacheTrans->GetPosition();
                cacheForward = cacheTrans->GetForward();
                cacheLookAt = cacheTrans->GetPosition() + cacheTrans->GetForward() * 3;

                if (vrBoneAnchorWanted) {
                    // Same thread as ApplyVrHeadPose: plain writes suffice.
                    // Only the position/forward/rotation values leave this
                    // frame; the transform pointer itself is never handed to
                    // the rig, so a scene unload cannot leave a dangling
                    // reference there.
                    vrCameraAnchorPosition = cachePosition;
                    vrCameraAnchorForward = cacheForward;
                    vrCameraAnchorRotation = cacheRotation;
                    vrCameraAnchorTimeNanoseconds =
                        gakumas::vr::pose::MonotonicNowNanoseconds();
                }

                auto rootBody = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(self, rootBody_field);
                auto rootModel = rootBody->GetParent();
                auto rootModelChildCount = rootModel->GetChildCount();
                for (int i = 0; i < rootModelChildCount; i++) {
                    auto rootChild = rootModel->GetChild(i);
                    const auto childName = rootChild->GetName();
                    if (childName == "Root_Face") {
                        for (int n = 0; n < rootChild->GetChildCount(); n++) {
                            auto vLSkinningRenderer = rootChild->GetChild(n);
                            if (vLSkinningRenderer->GetName() == "VLSkinningRenderer") {
                                HideHead(vLSkinningRenderer->GetGameObject(), true);
                                needRestoreHides = true;
                            }
                        }
                    }
                    else if (childName == "Root_Hair") {
                        HideHead(rootChild->GetGameObject(), false);
                        needRestoreHides = true;
                    }
                }
            }
            else {
                cacheTrans = nullptr;
                if (vrBoneAnchorWanted) {
                    // Matched index but no bone: distinct failure signature
                    // (rate-limited) so a Live where the hook runs yet the
                    // bone lookup fails is distinguishable from a dead index.
                    static std::int64_t lastNullNanoseconds = 0;
                    const std::int64_t nowNs =
                        gakumas::vr::pose::MonotonicNowNanoseconds();
                    if (nowNs - lastNullNanoseconds > 1'000'000'000LL) {
                        lastNullNanoseconds = nowNs;
                        std::ostringstream stream;
                        stream << "[VR][camera] FREECAM_ANCHOR_NULL_TRANS index="
                               << currIndex;
                        static_cast<void>(gakumas::vr::WriteVrLog(stream.str()));
                    }
                }
            }

        }

        CampusActorController_LateUpdate_Orig(self, mtd);
    }

    // Grip transparency (.225): _needsClear/_clearColor only act on the
    // DrawFrameBuffer branch of UIRenderPass.Execute, and AddRenderPasses
    // rewrites DrawFrameBuffer on every enqueue. The prefix forces pass0
    // onto that branch while transparency is armed; disarmed it is a no-op.
    DEFINE_HOOK(void, UIRenderPass_Execute,
                (void* self, void* context, void* renderingData, void* mtd)) {
        gakumas::vr::perf::SrpSpan policy(srpPerformance, "mod.ui-policy-enter");
        gakumas::vr::UnityStereoRenderer::GripUiPassExecState saved{};
        const bool modified =
            gakumas::vr::GripUiPassExecuteEnter(self, saved);
        void* camera = unityStereoRenderer.CurrentCamera();
        policy.Stop();
        gakumas::vr::GripBlurSourceScope blurScope(self,
            modified && camera && !unityStereoRenderer.IsEyeCamera(camera));
        {
            gakumas::vr::perf::SrpSpan original(srpPerformance, "ui.execute-original");
            UIRenderPass_Execute_Orig(self, context, renderingData, mtd);
        }
        if (modified) {
            gakumas::vr::perf::SrpSpan restore(srpPerformance, "mod.ui-policy-restore");
            gakumas::vr::GripUiPassExecuteExit(self, saved);
        }
    }

    DEFINE_HOOK(bool, PlatformInformation_get_IsAndroid, ()) {
        if (Config::loginAsIOS) {
            return false;
        }
        // Log::DebugFmt("PlatformInformation_get_IsAndroid: 0x%x", ret);
        return PlatformInformation_get_IsAndroid_Orig();
    }

    DEFINE_HOOK(bool, PlatformInformation_get_IsIOS, ()) {
        if (Config::loginAsIOS) {
            return true;
        }
        // Log::DebugFmt("PlatformInformation_get_IsIOS: 0x%x", ret);
        return PlatformInformation_get_IsIOS_Orig();
    }

    DEFINE_HOOK(Il2cppString*, ApiBase_GetPlatformString, (void* self, void* mtd)) {
        if (Config::loginAsIOS) {
            return Il2cppString::New("iOS");
        }
        // Log::DebugFmt("ApiBase_GetPlatformString: %s", ret->ToString().c_str());
        return ApiBase_GetPlatformString_Orig(self, mtd);
    }

    void ProcessApiBase(void* self) {
        static void* processedIOS = nullptr;

        if (Config::loginAsIOS) {
            if (self == processedIOS) return;

            static auto ApiBase_klass = Il2cppUtils::get_class_from_instance(self);
            static auto platform_field = UnityResolve::Invoke<Il2cppUtils::FieldInfo*>("il2cpp_class_get_field_from_name", ApiBase_klass, "_platform");
             auto platform = Il2cppUtils::ClassGetFieldValue<Il2cppString*>(self, platform_field);
             Log::DebugFmt("ProcessApiBase platform: %s", platform ? platform->ToString().c_str() : "null");
             if (platform) {
                 const auto origPlatform = platform->ToString();
                 if (origPlatform != "iOS") {
                     Il2cppUtils::ClassSetFieldValue(self, platform_field, Il2cppString::New("iOS"));
                     processedIOS = self;
                 }
             }
             else {
                 Il2cppUtils::ClassSetFieldValue(self, platform_field, Il2cppString::New("iOS"));
                 processedIOS = self;
             }
        }
        else {
            if (processedIOS) {
                Log::DebugFmt("Restore API");
                static auto ApiBase_klass = Il2cppUtils::get_class_from_instance(self);
                static auto platform_field = UnityResolve::Invoke<Il2cppUtils::FieldInfo*>("il2cpp_class_get_field_from_name", ApiBase_klass, "_platform");
#ifdef GKMS_WINDOWS
                Il2cppUtils::ClassSetFieldValue(self, platform_field, Il2cppString::New("dmm"));
#else
                Il2cppUtils::ClassSetFieldValue(self, platform_field, Il2cppString::New("Android"));
#endif
                processedIOS = nullptr;
            }
        }
    }

    DEFINE_HOOK(void, ApiBase_ctor, (void* self, void* mtd)) {
        ApiBase_ctor_Orig(self, mtd);
        ProcessApiBase(self);
    }

    DEFINE_HOOK(void*, ApiBase_get_Instance, (void* mtd)) {
        auto ret = ApiBase_get_Instance_Orig(mtd);
        if (ret) {
            ProcessApiBase(ret);
        }
        return ret;
    }

#ifdef GKMS_WINDOWS
    // DMM Only
    DEFINE_HOOK(void*, WindowHandle_SetWindowLong, (int32_t nIndex, intptr_t dwNewLong, void* mtd)) {
        if (GakumasLocal::Config::dmmUnlockSize) {
            // Log::DebugFmt("WindowHandle_SetWindowLong: %d, %p\n", nIndex, dwNewLong);

            if (nIndex == GWLP_WNDPROC) {
                return 0;
            }
        }

		return WindowHandle_SetWindowLong_Orig(nIndex, dwNewLong, mtd);
    }

    // DMM Only
	void SetResolution(int width, int height, bool fullscreen) {
		static auto Screen_SetResolution = reinterpret_cast<void (*)(UINT, UINT, UINT, void*)>(
            Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Screen::SetResolution_Injected(System.Int32,System.Int32,UnityEngine.FullScreenMode,UnityEngine.RefreshRate&)"));

        int64_t v8[3];
        v8[0] = 0x100000000LL;
		Screen_SetResolution(width, height, 2 * !fullscreen + 1, v8);
	}

    // DMM Only
    DEFINE_HOOK(void, WindowManager_ApplyOrientationSettings, (int orientation, void* method)) {
        if (!GakumasLocal::Config::dmmUnlockSize) return WindowManager_ApplyOrientationSettings_Orig(orientation, method);

        static auto get_Height = reinterpret_cast<int (*)()>(Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Screen::get_height()"));
        static auto get_Width = reinterpret_cast<int (*)()>(Il2cppUtils::il2cpp_resolve_icall("UnityEngine.Screen::get_width()"));

		static auto lastWidth = -1;
		static auto lastHeight = -1;

		const auto currWidth = get_Width();
		const auto currHeight = get_Height();

        if (lastWidth == -1) {
			lastWidth = currWidth;
			lastHeight = currHeight;
            return;
		}

		const bool lastIsPortrait = lastWidth < lastHeight;
		const bool currIsPortrait = currWidth < currHeight;
        if (lastIsPortrait == currIsPortrait) {
            lastWidth = currWidth;
            lastHeight = currHeight;
            return;
        }

		SetResolution(lastWidth, lastHeight, false);
		lastWidth = currWidth;
		lastHeight = currHeight;

		Log::DebugFmt("WindowManager_ApplyOrientationSettings: %d (%d, %d)\n", orientation, get_Width(), get_Height());
    }

    // DMM Only
    DEFINE_HOOK(void, AspectRatioHandler_NudgeWindow, (void* method)) {
		if (!GakumasLocal::Config::dmmUnlockSize) return AspectRatioHandler_NudgeWindow_Orig(method);
		// printf("AspectRatioHandler_NudgeWindow\n");
    }
#endif

    void UpdateSwingBreastBonesData(void* initializeData) {
        if (!Config::enableBreastParam) return;
        static auto CampusActorAnimationInitializeData_klass = Il2cppUtils::GetClass("campus-submodule.Runtime.dll", "ActorAnimation",
                                                                                     "CampusActorAnimationInitializeData");
        static auto ActorSwingBreastBone_klass = Il2cppUtils::GetClass("ActorAnimation.Runtime.dll", "ActorAnimation",
                                                                       "ActorSwingBreastBone");
        static auto LimitInfo_klass = Il2cppUtils::GetClass("ActorAnimation.Runtime.dll", "ActorAnimation",
                                                            "LimitInfo");

        static auto Data_swingBreastBones_field = CampusActorAnimationInitializeData_klass->Get<UnityResolve::Field>("swingBreastBones");
        static auto damping_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("damping");
        static auto stiffness_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("stiffness");
        static auto spring_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("spring");
        static auto pendulum_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("pendulum");
        static auto pendulumRange_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("pendulumRange");
        static auto average_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("average");
        static auto rootWeight_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("rootWeight");
        static auto useArmCorrection_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("useArmCorrection");
        static auto isDirty_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("<isDirty>k__BackingField");
        static auto leftBreast_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("leftBreast");
        static auto rightBreast_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("rightBreast");
        static auto leftBreastEnd_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("leftBreastEnd");
        static auto rightBreastEnd_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("rightBreastEnd");
        static auto limitInfo_field = ActorSwingBreastBone_klass->Get<UnityResolve::Field>("limitInfo");

        static auto limitInfo_useLimit_field = LimitInfo_klass->Get<UnityResolve::Field>("useLimit");
        static auto limitInfo_axisX_field = LimitInfo_klass->Get<UnityResolve::Field>("axisX");
        static auto limitInfo_axisY_field = LimitInfo_klass->Get<UnityResolve::Field>("axisY");
        static auto limitInfo_axisZ_field = LimitInfo_klass->Get<UnityResolve::Field>("axisZ");

        auto swingBreastBones = Il2cppUtils::ClassGetFieldValue
                <UnityResolve::UnityType::List<UnityResolve::UnityType::MonoBehaviour*>*>(initializeData, Data_swingBreastBones_field);

        auto boneArr = swingBreastBones->ToArray();
        for (int i = 0; i < boneArr->max_length; i++) {
            auto bone = boneArr->At(i);
            if (!bone) continue;

            auto damping = Il2cppUtils::ClassGetFieldValue<float>(bone, damping_field);
            auto stiffness = Il2cppUtils::ClassGetFieldValue<float>(bone, stiffness_field);
            auto spring = Il2cppUtils::ClassGetFieldValue<float>(bone, spring_field);
            auto pendulum = Il2cppUtils::ClassGetFieldValue<float>(bone, pendulum_field);
            auto pendulumRange = Il2cppUtils::ClassGetFieldValue<float>(bone, pendulumRange_field);
            auto average = Il2cppUtils::ClassGetFieldValue<float>(bone, average_field);
            auto rootWeight = Il2cppUtils::ClassGetFieldValue<float>(bone, rootWeight_field);
            auto useArmCorrection = Il2cppUtils::ClassGetFieldValue<bool>(bone, useArmCorrection_field);
            auto isDirty = Il2cppUtils::ClassGetFieldValue<bool>(bone, isDirty_field);

            auto limitInfo = Il2cppUtils::ClassGetFieldValue<void*>(bone, limitInfo_field);
            auto useLimit = Il2cppUtils::ClassGetFieldValue<int>(limitInfo, limitInfo_useLimit_field);

            if (Config::bUseScale) {
                auto leftBreast = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(bone, leftBreast_field);
                auto rightBreast = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(bone, rightBreast_field);
                auto leftBreastEnd = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(bone, leftBreastEnd_field);
                auto rightBreastEnd = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Transform*>(bone, rightBreastEnd_field);

                const auto setScale = UnityResolve::UnityType::Vector3(Config::bScale, Config::bScale, Config::bScale);
                leftBreast->SetLocalScale(setScale);
                rightBreast->SetLocalScale(setScale);
                leftBreastEnd->SetLocalScale(setScale);
                rightBreastEnd->SetLocalScale(setScale);
            }

            Log::DebugFmt("orig bone: damping: %f, stiffness: %f, spring: %f, pendulum: %f, "
                          "pendulumRange: %f, average: %f, rootWeight: %f, useLimit: %d, useArmCorrection: %d, isDirty: %d",
                          damping, stiffness, spring, pendulum, pendulumRange, average, rootWeight, useLimit, useArmCorrection, isDirty);
            if (!Config::bUseLimit) {
                Il2cppUtils::ClassSetFieldValue(limitInfo, limitInfo_useLimit_field, 0);
            }
            else {
                Il2cppUtils::ClassSetFieldValue(limitInfo, limitInfo_useLimit_field, 1);
                auto axisX = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Vector2Int>(limitInfo, limitInfo_axisX_field);
                auto axisY = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Vector2Int>(limitInfo, limitInfo_axisY_field);
                auto axisZ = Il2cppUtils::ClassGetFieldValue<UnityResolve::UnityType::Vector2Int>(limitInfo, limitInfo_axisZ_field);
                axisX.m_X *= Config::bLimitXx;
                axisX.m_Y *= Config::bLimitXy;
                axisY.m_X *= Config::bLimitYx;
                axisY.m_Y *= Config::bLimitYy;
                axisZ.m_X *= Config::bLimitZx;
                axisZ.m_Y *= Config::bLimitZy;
                Il2cppUtils::ClassSetFieldValue(limitInfo, limitInfo_axisX_field, axisX);
                Il2cppUtils::ClassSetFieldValue(limitInfo, limitInfo_axisY_field, axisY);
                Il2cppUtils::ClassSetFieldValue(limitInfo, limitInfo_axisZ_field, axisZ);

            }

            Il2cppUtils::ClassSetFieldValue(bone, damping_field, Config::bDamping);
            Il2cppUtils::ClassSetFieldValue(bone, stiffness_field, Config::bStiffness);
            Il2cppUtils::ClassSetFieldValue(bone, spring_field, Config::bSpring);
            Il2cppUtils::ClassSetFieldValue(bone, pendulum_field, Config::bPendulum);
            Il2cppUtils::ClassSetFieldValue(bone, pendulumRange_field, Config::bPendulumRange);
            Il2cppUtils::ClassSetFieldValue(bone, average_field, Config::bAverage);
            Il2cppUtils::ClassSetFieldValue(bone, rootWeight_field, Config::bRootWeight);
            Il2cppUtils::ClassSetFieldValue(bone, useArmCorrection_field, Config::bUseArmCorrection);
            // Il2cppUtils::ClassSetFieldValue(bone, isDirty_field, Config::bIsDirty);
        }
        // Log::DebugFmt("\n");
    }

    DEFINE_HOOK(void, CampusActorAnimation_Setup, (void* self, void* rootTrans, void* initializeData)) {
        UpdateSwingBreastBonesData(initializeData);
        return CampusActorAnimation_Setup_Orig(self, rootTrans, initializeData);
    }

/*
    std::map<std::string, std::pair<uintptr_t, void*>> findByKeyHookAddress{};
    void* FindByKeyHooks(void* self, void* key, void* mtd) {
        auto self_klass = Il2cppUtils::get_class_from_instance(self);

        if (auto it = findByKeyHookAddress.find(self_klass->name); it != findByKeyHookAddress.end()) {
            Log::DebugFmt("FindByKeyHooks Call cache: %s, %p, %p", self_klass->name, it->second.first, it->second.second);
            return reinterpret_cast<decltype(FindByKeyHooks)*>(it->second.second)(self, key, mtd);
        }
        Log::DebugFmt("FindByKeyHooks not in cache: %s", self_klass->name);

        auto FindByKey_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(self_klass, "FindByKey", 1);
        for (auto& [k, v] : findByKeyHookAddress) {
            if (FindByKey_mtd->methodPointer == v.first) {
                findByKeyHookAddress.emplace(self_klass->name, std::make_pair(FindByKey_mtd->methodPointer, v.second));
                Log::DebugFmt("FindByKeyHooks add to cache: %s", self_klass->name);
                return reinterpret_cast<decltype(FindByKeyHooks)*>(v.second)(self, key, mtd);
            }
        }

        Log::ErrorFmt("FindByKeyHooks not found hook: %s", self_klass->name);
        return SHADOWHOOK_CALL_PREV(FindByKeyHooks, self, key, mtd);
    }

    static inline std::vector<void(*)(HookInstaller* hookInstaller)> g_registerMasterFindByKeyHookFuncs;

#define DEF_AND_ADD_MASTER_FINDBYKEY_HOOK(name)                                \
    using name##_FindByKey_Type = void* (*)(void* self, void* key, void* idx, void* mtd); \
    inline name##_FindByKey_Type name##_FindByKey_Addr = nullptr;              \
    inline void* name##_FindByKey_Orig = nullptr;                              \
    inline void* name##_FindByKey_Hook(void* self, void* key, void* idx, void* mtd) {     \
        auto result = reinterpret_cast<decltype(name##_FindByKey_Hook)*>(      \
            name##_FindByKey_Orig)(self, key, idx, mtd);                            \
        LocalizeFindByKey(result, self);                                       \
        return result;                                                         \
    }                                                                          \
    inline void name##_RegisterHook(HookInstaller* hookInstaller) {            \
        auto klass = Il2cppUtils::GetClass(                                    \
            "Assembly-CSharp.dll", "Campus.Common.Master", #name);             \
        auto mtd = Il2cppUtils::il2cpp_class_get_method_from_name(             \
            klass->address, "GetData", 2);                                   \
        ADD_HOOK(name##_FindByKey, mtd->methodPointer);                        \
    }                                                                          \
    struct name##_RegisterHookPusher {                                         \
        name##_RegisterHookPusher() {                                          \
            g_registerMasterFindByKeyHookFuncs.push_back(&name##_RegisterHook);\
        }                                                                      \
    } g_##name##_RegisterHookPusherInst;

    DEF_AND_ADD_MASTER_FINDBYKEY_HOOK(AchievementMaster)
    DEF_AND_ADD_MASTER_FINDBYKEY_HOOK(ProduceSkillMaster)
    DEF_AND_ADD_MASTER_FINDBYKEY_HOOK(FeatureLockMaster)
    DEF_AND_ADD_MASTER_FINDBYKEY_HOOK(ProduceCardMaster)

    // 安装 DEF_AND_ADD_MASTER_FINDBYKEY_HOOK 的 hook
    void InitMasterHooks(HookInstaller* hookInstaller) {
        for (auto& func : g_registerMasterFindByKeyHookFuncs) {
            func(hookInstaller);
        }
    }
*/

    void StartInjectFunctions() {
        const auto hookInstaller = Plugin::GetInstance().GetHookInstaller();

#ifdef GKMS_WINDOWS
        auto il2cpp_module = GetModuleHandleW(L"GameAssembly.dll");
        if (!il2cpp_module) {
            Log::Error("GameAssembly.dll not loaded.");
            return;
        }
        UnityResolve::Init(il2cpp_module, UnityResolve::Mode::Il2Cpp, Config::lazyInit);
        if (Config::vrRuntimeStartupEnabled && Config::enableFreeCamera &&
            Config::vrDiagnosticsStartupEnabled) {
            Log::Info(
                "VR_CAMERA_OWNERSHIP owner=vr localifyFreeCamera=suppressed");
        }
        if (Config::enabled) {
            GakumasLocal::WinHooks::Keyboard::InstallWndProcHook();
        }
#else
        UnityResolve::Init(xdl_open(hookInstaller->m_il2cppLibraryPath.c_str(), RTLD_NOW),
            UnityResolve::Mode::Il2Cpp, Config::lazyInit);
#endif

        if (Config::enabled) {
        // Extra font/texture bundle is armed on demand from
        // EnsureExtraAssetBundle when vrLocalizeText is on. Do not
        // LoadFromFile on PatchWorker (.312).
        // Translation and legacy Localify features are deliberately isolated
        // from the VR camera core below.
        // Texture replacement is currently isolated to CanvasRenderer.SetTexture.
        //
        // Unity 6 compatibility warning:
        // The resolver helpers below may return either a native icall address or
        // a managed IL2CPP methodPointer fallback. These two targets do not share
        // the same ABI: a managed methodPointer normally has a trailing MethodInfo*
        // argument, while the current hook declarations use the icall-style
        // signatures. Do not simply uncomment these ADD_HOOK calls on Unity 6.
        // Before restoring them, either:
        //   1. make each resolver return only a verified icall address; or
        //   2. return ABI metadata and install a separate managed hook signature
        //      that preserves and forwards the trailing MethodInfo* argument.
        // ADD_HOOK(AssetBundle_LoadAsset, ResolveAssetBundleLoadAssetHookAddress());
        // ADD_HOOK(AssetBundle_LoadAssetAsync, ResolveAssetBundleLoadAssetAsyncHookAddress());
        // ADD_HOOK(AssetBundleRequest_GetResult, ResolveAssetBundleRequestResultHookAddress());
        // ADD_HOOK(AssetBundleRequest_get_asset, ResolveAssetBundleRequestAssetHookAddress());
        // ADD_HOOK(AssetBundleRequest_get_allAssets, ResolveAssetBundleRequestAllAssetsHookAddress());
        // ADD_HOOK(Resources_Load, ResolveResourcesLoadHookAddress());
        // ADD_HOOK(Sprite_get_texture, ResolveSpriteGetTextureHookAddress());
        // ADD_HOOK(Image_set_sprite, Il2cppUtils::GetMethodPointer("UnityEngine.UI.dll", "UnityEngine.UI", "Image", "set_sprite"));
        // ADD_HOOK(Image_set_overrideSprite, Il2cppUtils::GetMethodPointer("UnityEngine.UI.dll", "UnityEngine.UI", "Image", "set_overrideSprite"));
        ADD_HOOK(CanvasRenderer_SetTexture, Il2cppUtils::GetMethodPointer("UnityEngine.UIModule.dll", "UnityEngine", "CanvasRenderer", "SetTexture", {"UnityEngine.Texture"}));
        // ADD_HOOK(SpriteRenderer_set_sprite, Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine", "SpriteRenderer", "set_sprite"));

        ADD_HOOK(I18nHelper_SetUpI18n, Il2cppUtils::GetMethodPointer("quaunity-ui.Runtime.dll", "Qua.UI",
                                                                     "I18nHelper", "SetUpI18n"));
        ADD_HOOK(I18nHelper_SetValue, Il2cppUtils::GetMethodPointer("quaunity-ui.Runtime.dll", "Qua.UI",
                                                                     "I18n", "SetValue"));

        //ADD_HOOK(UI_I18n_GetOrDefault, Il2cppUtils::GetMethodPointer("quaunity-ui.Runtime.dll", "Qua.UI",
        //                                                             "I18n", "GetOrDefault"));

        ADD_HOOK(TextMeshProUGUI_Awake, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
                                                                      "TextMeshProUGUI", "Awake"));

        ADD_HOOK(TMP_Text_set_text, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
                                                                  "TMP_Text", "set_text"));
        const auto tmpSetText1 = Il2cppUtils::GetMethodPointer(
            "Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "SetText",
            {"System.String"});
        const auto tmpSetText2 = Il2cppUtils::GetMethodPointer(
            "Unity.TextMeshPro.dll", "TMPro", "TMP_Text", "SetText",
            {"System.String", "System.Boolean"});
        ADD_HOOK(TMP_Text_SetText_1, tmpSetText1);
        ADD_HOOK(TMP_Text_PopulateTextBackingArray, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
                                                                  "TMP_Text", "PopulateTextBackingArray",
                                                                  {"System.String", "System.Int32", "System.Int32"}));
        if (tmpSetText2 && tmpSetText2 != tmpSetText1) {
            ADD_HOOK(TMP_Text_SetText_2, tmpSetText2);
        } else {
            Log::Info(
                "TMP_Text_SetText_2 skipped: same address as SetText_1");
        }

        ADD_HOOK(TextField_set_value, Il2cppUtils::GetMethodPointer("UnityEngine.UIElementsModule.dll", "UnityEngine.UIElements",
                                                                  "TextField", "set_value"));

        // Legacy UnityEngine.UI.Text hook
        {
            auto uiTextPtr = Il2cppUtils::GetMethodPointer("UnityEngine.UI.dll", "UnityEngine.UI",
                                                           "Text", "set_text");
            if (uiTextPtr) {
                ADD_HOOK(UIText_set_text, uiTextPtr);
            }
            else {
                Log::InfoFmt("UIText_set_text: method not found, legacy UI.Text hook skipped.");
            }
        }

        ADD_HOOK(TMP_Text_SetCharArray, Il2cppUtils::GetMethodPointer("Unity.TextMeshPro.dll", "TMPro",
            "TMP_Text", "SetCharArray", {"System.Char[]", "System.Int32", "System.Int32"}));
        /* SQL 查询相关函数，不好用
        // 下面是 byte[] u8 string 转 std::string 的例子
        auto query = reinterpret_cast<UnityResolve::UnityType::Array<UnityResolve::UnityType::Byte>*>(mtd);
        auto data_ptr = reinterpret_cast<std::uint8_t*>(query->GetData());
        std::string qS(data_ptr, data_ptr + lastLength);

        ADD_HOOK(PreparedStatement_ExecuteQuery, Il2cppUtils::GetMethodPointer("quaunity-master-manager.Runtime.dll", "Qua.Master.SQLite",
                                                                               "PreparedStatement", "ExecuteQuery", {"System.String"}));
        ADD_HOOK(PreparedStatement_ExecuteQuery_u8, Il2cppUtils::GetMethodPointer("quaunity-master-manager.Runtime.dll", "Qua.Master.SQLite",
                                                                                  "PreparedStatement", "ExecuteQuery", {"*", "*"}));
        ADD_HOOK(PreparedStatement_FinalizeStatement, Il2cppUtils::GetMethodPointer("quaunity-master-manager.Runtime.dll", "Qua.Master.SQLite",
                                                                                  "PreparedStatement", "FinalizeStatement"));
       */

        // ADD_HOOK(EffectGroup_ctor, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.Common.Proto.Client.Master",
        //                                                          "EffectGroup", ".ctor"));

        ADD_HOOK(MessageExtensions_MergeFrom, Il2cppUtils::GetMethodPointer("Google.Protobuf.dll", "Google.Protobuf",
                                                                            "MessageExtensions", "MergeFrom", {"Google.Protobuf.IMessage", "System.ReadOnlySpan<System.Byte>"}));

        ADD_HOOK(ExamExtensions_ExchangeSpToNormal,
                 Il2cppUtils::GetMethodPointer(
                     "Assembly-CSharp.dll", "Campus.InGame", "ExamExtensions",
                     "ExchangeSpToNormal",
                     { "Campus.Common.Proto.Client.Enums.ProduceStepType" }));

        /* // 此 block 为 MasterBase 相关的 hook，后来发现它们最后都会调用 MessageExtensions.MergeFrom 进行构造，遂停用。现留档以备用
        // ADD_HOOK(MasterBase_GetAll, Il2cppUtils::GetMethodPointer("quaunity-master-manager.Runtime.dll", "Qua.Master",
        //                                                          "MasterBase`2", "GetAll", {"*", "*", "*", "*", "*"}));

        // 安装 DEF_AND_ADD_MASTER_FINDBYKEY_HOOK 的 hook
        InitMasterHooks(hookInstaller);

        auto AchievementMaster_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Common.Master", "AchievementMaster");
        auto AchievementMaster_GetAll_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(AchievementMaster_klass->address, "GetAll", 5);
        // auto AchievementMaster_FindByKey_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(AchievementMaster_klass->address, "FindByKey", 1);
        // Log::DebugFmt("AchievementMaster_GetAll_mtd at %p", AchievementMaster_GetAll_mtd);
        ADD_HOOK(MasterBase_GetAll, AchievementMaster_GetAll_mtd->methodPointer);
        */

        ADD_HOOK(OctoCaching_GetResourceFileName, Il2cppUtils::GetMethodPointer("Octo.dll", "Octo.Caching",
                                                                     "OctoCaching", "GetResourceFileName"));

        ADD_HOOK(OctoResourceLoader_LoadFromCacheOrDownload,
                 Il2cppUtils::GetMethodPointer("Octo.dll", "Octo.Loader",
                                               "OctoResourceLoader", "LoadFromCacheOrDownload",
                                               {"System.String", "System.Action<System.String,Octo.LoadError>", "Octo.OnDownloadProgress"}));

        ADD_HOOK(OnDownloadProgress_Invoke,
                 Il2cppUtils::GetMethodPointer("Octo.dll", "Octo",
                                               "OnDownloadProgress", "Invoke"));

        /*
        auto UserDataManager_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Common.User",
                                                           "UserDataManager");
        if (UserDataManager_klass) {
            auto UserDataManagerBase_klass = UnityResolve::Invoke<Il2cppUtils::Il2CppClassHead*>("il2cpp_class_get_parent", UserDataManager_klass->address);
            if (UserDataManagerBase_klass) {
                auto get_userIdolCardSkinList_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(UserDataManagerBase_klass, "get__userIdolCardSkinList", 0);
                if (get_userIdolCardSkinList_mtd) {
                    ADD_HOOK(UserDataManagerBase_get__userIdolCardSkinList, get_userIdolCardSkinList_mtd->methodPointer);
                }
                auto get_userCostumeList_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(UserDataManagerBase_klass, "get__userCostumeList", 0);
                if (get_userCostumeList_mtd) {
                    ADD_HOOK(UserDataManagerBase_get__userCostumeList, get_userCostumeList_mtd->methodPointer);
                }
                auto get_userCostumeHeadList_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(UserDataManagerBase_klass, "get__userCostumeHeadList", 0);
                if (get_userCostumeHeadList_mtd) {
                    ADD_HOOK(UserDataManagerBase_get__userCostumeHeadList, get_userCostumeHeadList_mtd->methodPointer);
                }
            }
        }*/

        auto UserIdolCardSkinCollection_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Common.User",
                                                                      "UserIdolCardSkinCollection");
        auto UserIdolCardSkinCollection_Exists_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(UserIdolCardSkinCollection_klass->address, "Exists", 1);
        if (UserIdolCardSkinCollection_Exists_mtd) {
            ADD_HOOK(UserIdolCardSkinCollection_Exists, UserIdolCardSkinCollection_Exists_mtd->methodPointer);
        }

        auto UserCostumeCollection_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Common.User",
                                                                      "UserCostumeCollection");
        auto UserCostumeCollection_FindBy_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                UserCostumeCollection_klass->address, "FindBy", 1);
        if (UserCostumeCollection_FindBy_mtd) {
            ADD_HOOK(UserCostumeCollection_FindBy, UserCostumeCollection_FindBy_mtd->methodPointer);
        }

        auto UserCostumeCollection_GetAll_mtd = Il2cppUtils::il2cpp_class_get_method_from_name(
                UserCostumeCollection_klass->address, "GetAll", 0);
        if (UserCostumeCollection_GetAll_mtd) {
            ADD_HOOK(UserDataCollection_GetAll, UserCostumeCollection_GetAll_mtd->methodPointer);
        }

        ADD_HOOK(PhotographyCostumeSettingListItemModel_get_IsDisabled,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.Photography",
                                               "PhotographyCostumeSettingListItemModel", "get_IsDisabled"));
        ADD_HOOK(PhotographyCostumeSettingData_GetCostumes,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.Photography",
                                               "PhotographyCostumeSettingData", "GetCostumes"));
        ADD_HOOK(PhotographyCostumeSettingData_GetCostumeHeads,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.Photography",
                                               "PhotographyCostumeSettingData", "GetCostumeHeads"));

        // 双端
        ADD_HOOK(PictureBookLiveThumbnailView_SetReleaseDataAsync,
            GetMethodPointerByArgCount("Assembly-CSharp.dll", "Campus.OutGame.PictureBook",
                "PictureBookLiveThumbnailView", "SetReleaseDataAsync", 6));
        ADD_HOOK(PictureBookLiveThumbnailView_SetUnReleaseDataAsync,
            GetMethodPointerByArgCount("Assembly-CSharp.dll", "Campus.OutGame.PictureBook",
                "PictureBookLiveThumbnailView", "SetUnReleaseDataAsync", 4));

        ADD_HOOK(PictureBookWindowPresenter_GetLiveMusics,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.OutGame",
                                               "PictureBookWindowPresenter", "GetLiveMusics"));

#ifdef GKMS_WINDOWS
        // 跳过切歌Loading，安卓端会崩溃
        // Disabled after game updates because this async method signature is update-sensitive.
        // ADD_HOOK(Produce_ViewPictureBookLiveAsync,
        //     Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "",
        //         "Produce", "ViewPictureBookLiveAsync"));
#endif
        // ADD_HOOK(PictureBookLiveSelectScreenModel_ctor,
        //          Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.OutGame",
        //                                        "PictureBookLiveSelectScreenModel", ".ctor"));

        // ADD_HOOK(PictureBookLiveSelectScreenPresenter_MoveLiveScene,
        //          Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.OutGame",
        //                                        "PictureBookLiveSelectScreenPresenter", "MoveLiveScene"));

        // 双端
        ADD_HOOK(LiveSceneModel_set_IsLyricsActive,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.Live",
                                               "LiveSceneModel", "set_IsLyricsActive"));

        ADD_HOOK(LiveScenePresenter_SetLyricsActive,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.Live",
                                               "LiveScenePresenter", "SetLyricsActive", { "*" }));

        ADD_HOOK(LiveSceneContentView_SetLyricsTextActive,
                 Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.Live",
                                               "LiveSceneContentView", "SetLyricsTextActive", { "*" }));

        // ADD_HOOK(PictureBookLiveSelectScreenPresenter_OnSelectMusic,
        //     Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.OutGame",
        //         "PictureBookLiveSelectScreenPresenter", "OnSelectMusicAsync"));

        ADD_HOOK(VLDOF_IsActive,
                 Il2cppUtils::GetMethodPointer("Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering",
                                               "VLDOF", "IsActive"));
        ADD_HOOK(URPDOF_IsActive,
                 Il2cppUtils::GetMethodPointer("Unity.RenderPipelines.Universal.Runtime.dll",
                                               "UnityEngine.Rendering.Universal",
                                               "DepthOfField", "IsActive"));

        ADD_HOOK(CampusQualityManager_ApplySetting,
                 Il2cppUtils::GetMethodPointer("campus-submodule.Runtime.dll", "Campus.Common",
                                               "CampusQualityManager", "ApplySetting"));

        ADD_HOOK(UIManager_UpdateRenderTarget,
                 Il2cppUtils::GetMethodPointer("ADV.Runtime.dll", "Campus.ADV",
                                               "UIManager", "UpdateRenderTarget"));
        ADD_HOOK(VLSRPCameraController_UpdateRenderTarget,
                 Il2cppUtils::GetMethodPointer("vl-unity.Runtime.dll", "VL.Rendering",
                                               "VLSRPCameraController", "UpdateRenderTarget",
                                               {"*", "*", "*"}));

        ADD_HOOK(VLUtility_GetLimitedResolution,
                 Il2cppUtils::GetMethodPointer("vl-unity.Runtime.dll", "VL",
                                               "VLUtility", "GetLimitedResolution",
                                               {"*", "*", "*", "*", "*", "*"}));

        ADD_HOOK(CampusActorModelParts_OnRegisterBone,
                 Il2cppUtils::GetMethodPointer("campus-submodule.Runtime.dll", "Campus.Common",
                                               "CampusActorModelParts", "OnRegisterBone"));

        ADD_HOOK(PlatformInformation_get_IsAndroid, Il2cppUtils::GetMethodPointer("Firebase.Platform.dll", "Firebase.Platform",
                                                                         "PlatformInformation", "get_IsAndroid"));
        ADD_HOOK(PlatformInformation_get_IsIOS, Il2cppUtils::GetMethodPointer("Firebase.Platform.dll", "Firebase.Platform",
                                                                                  "PlatformInformation", "get_IsIOS"));

        auto api_klass = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Common.Network", "Api");
        if (api_klass) {
            // Qua.Network.ApiBase
            auto api_parent = UnityResolve::Invoke<Il2cppUtils::Il2CppClassHead*>("il2cpp_class_get_parent", api_klass->address);
            if (api_parent) {
                // Log::DebugFmt("api_parent at %p, name: %s::%s", api_parent, api_parent->namespaze, api_parent->name);
                ADD_HOOK(ApiBase_GetPlatformString, Il2cppUtils::il2cpp_class_get_method_pointer_from_name(api_parent, "GetPlatformString", 0));
                ADD_HOOK(ApiBase_ctor, Il2cppUtils::il2cpp_class_get_method_pointer_from_name(api_parent, ".ctor", 0));
                ADD_HOOK(ApiBase_get_Instance, Il2cppUtils::il2cpp_class_get_method_pointer_from_name(api_parent, "get_Instance", 0));
            }
        }

        /*
        static auto CampusActorController_klass = Il2cppUtils::GetClass("campus-submodule.Runtime.dll",
                                                                        "Campus.Common", "CampusActorController");
        for (const auto& i : CampusActorController_klass->methods) {
            Log::DebugFmt("CampusActorController.%s at %p", i->name.c_str(), i->function);
        }*/

        ADD_HOOK(CampusActorAnimation_Setup,
                 Il2cppUtils::GetMethodPointer("campus-submodule.Runtime.dll", "Campus.Common",
                                               "CampusActorAnimation", "Setup"));

        ADD_HOOK(CampusQualityManager_set_TargetFrameRate,
                 Il2cppUtils::GetMethodPointer("campus-submodule.Runtime.dll", "Campus.Common",
                                               "CampusQualityManager", "set_TargetFrameRate"));

        // Unity 6 no longer exposes these two methods through the old icall
        // names in this player. Resolve their managed IL2CPP methodPointer
        // through GetMethod() and use hook signatures with a trailing MethodInfo*.
        const auto Internal_LogException_Method = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll",
            "UnityEngine",
            "DebugLogHandler",
            "Internal_LogException",
            { "System.Exception", "UnityEngine.Object" }
        );
        ADD_HOOK(
            Internal_LogException,
            Internal_LogException_Method
                ? Internal_LogException_Method->function
                : nullptr
        );

        const auto Internal_Log_Method = Il2cppUtils::GetMethod(
            "UnityEngine.CoreModule.dll",
            "UnityEngine",
            "DebugLogHandler",
            "Internal_Log",
            {
                "UnityEngine.LogType",
                "UnityEngine.LogOption",
                "System.String",
                "UnityEngine.Object"
            }
        );
        ADD_HOOK(
            Internal_Log,
            Internal_Log_Method
                ? Internal_Log_Method->function
                : nullptr
        );

        // 双端
        ADD_HOOK(InternalSetOrientationAsync,
            Il2cppUtils::GetMethodPointer("campus-submodule.Runtime.dll", "Campus.Common",
                "ScreenOrientationControllerBase", "InternalSetOrientationAsync"));
        }

        // Keep the VR-only observation milestone deliberately smaller than the
        // legacy translation/free-camera surface. Managed FOV hooks and
        // Transform setters are not required to discover cameras, and their ABI
        // or side effects would add risk to a read-only hardware probe.
        if (Config::enabled) {
            ADD_HOOK(Unity_set_position_Injected, Il2cppUtils::il2cpp_resolve_icall(
                    "UnityEngine.Transform::set_position_Injected(UnityEngine.Vector3&)"));
            ADD_HOOK(Unity_set_rotation_Injected, Il2cppUtils::il2cpp_resolve_icall(
                    "UnityEngine.Transform::set_rotation_Injected(UnityEngine.Quaternion&)"));
            ADD_HOOK(Unity_SetPositionAndRotation_Injected, Il2cppUtils::il2cpp_resolve_icall(
                    "UnityEngine.Transform::SetPositionAndRotation_Injected(UnityEngine.Vector3&,UnityEngine.Quaternion&)"));
            ADD_HOOK(Unity_set_localPosition_Injected, Il2cppUtils::il2cpp_resolve_icall(
                    "UnityEngine.Transform::set_localPosition_Injected(UnityEngine.Vector3&)"));
            ADD_HOOK(Component_get_transform, Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                                                                            "Component", "get_transform"));
            ADD_HOOK(Unity_get_fieldOfView, Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                                                                          "Camera", "get_fieldOfView"));
            ADD_HOOK(Unity_set_fieldOfView, Il2cppUtils::GetMethodPointer("UnityEngine.CoreModule.dll", "UnityEngine",
                                                                          "Camera", "set_fieldOfView"));
            ADD_HOOK(Unity_set_targetFrameRate, Il2cppUtils::il2cpp_resolve_icall(
                    "UnityEngine.Application::set_targetFrameRate(System.Int32)"));
        }

#ifdef GKMS_WINDOWS
        if (Config::enabled) {
        ADD_HOOK(WindowHandle_SetWindowLong, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.Common.StandAloneWindow",
            "WindowHandle", "SetWindowLong"));
        ADD_HOOK(WindowManager_ApplyOrientationSettings, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.Common.StandAloneWindow",
            "WindowManager", "ApplyOrientationSettings"));
        ADD_HOOK(AspectRatioHandler_NudgeWindow, Il2cppUtils::GetMethodPointer("Assembly-CSharp.dll", "Campus.Common.StandAloneWindow",
            "AspectRatioHandler", "NudgeWindow"));

        if (GakumasLocal::Config::dmmUnlockSize) {
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::seconds(3));

                const auto currentProcessId = GetCurrentProcessId();
                HWND hWnd = nullptr;
                HWND candidate = nullptr;

                while ((candidate = FindWindowExW(
                            nullptr,
                            candidate,
                            L"UnityWndClass",
                            nullptr
                        )) != nullptr) {
                    DWORD windowProcessId = 0;
                    GetWindowThreadProcessId(
                        candidate,
                        &windowProcessId
                    );

                    if (windowProcessId == currentProcessId) {
                        hWnd = candidate;
                        break;
                    }
                }

                if (!hWnd) {
                    Log::Error(
                        "DMM unlock size failed: Unity window not found."
                    );
                    return;
                }

                SetLastError(ERROR_SUCCESS);

                auto style = GetWindowLongPtrW(
                    hWnd,
                    GWL_STYLE
                );

                if (style == 0 &&
                    GetLastError() != ERROR_SUCCESS) {
                    Log::ErrorFmt(
                        "DMM unlock size failed: "
                        "GetWindowLongPtrW error=%lu",
                        GetLastError()
                    );
                    return;
                }

                style |= WS_THICKFRAME |
                         WS_MAXIMIZEBOX;

                SetLastError(ERROR_SUCCESS);

                const auto previousStyle =
                    SetWindowLongPtrW(
                        hWnd,
                        GWL_STYLE,
                        style
                    );

                if (previousStyle == 0 &&
                    GetLastError() != ERROR_SUCCESS) {
                    Log::ErrorFmt(
                        "DMM unlock size failed: "
                        "SetWindowLongPtrW error=%lu",
                        GetLastError()
                    );
                    return;
                }

                if (!SetWindowPos(
                        hWnd,
                        nullptr,
                        0,
                        0,
                        0,
                        0,
                        SWP_NOMOVE |
                        SWP_NOSIZE |
                        SWP_NOZORDER |
                        SWP_NOACTIVATE |
                        SWP_FRAMECHANGED
                    )) {
                    Log::ErrorFmt(
                        "DMM unlock size failed: "
                        "SetWindowPos error=%lu",
                        GetLastError()
                    );
                    return;
                }

                Log::Info(
                    "DMM window size unlocked."
                );
            }).detach();
        }

		GkmsResourceUpdate::CheckUpdateFromAPI(false);
        }
#endif

#ifdef GKMS_WINDOWS
        const bool vrRuntime = IsVrUnityRuntimeEnabled();
#else
        constexpr bool vrRuntime = false;
#endif
        if (Config::enabled || vrRuntime) {
            // The VR free camera samples FOLLOW/FIRST_PERSON bone anchors in
            // this hook, so it must install even when the translation layer
            // (Config::enabled) is off — the VR package ships enabled=false.
            ADD_HOOK(CampusActorController_LateUpdate,
                     Il2cppUtils::GetMethodPointer("campus-submodule.Runtime.dll", "Campus.Common",
                                                   "CampusActorController", "LateUpdate"));

            // Grip transparency Execute-branch fixup (.225); live class
            // proven in campus-submodule.Runtime.dll by the .224 run.
            auto* uiExecute = Il2cppUtils::GetMethodPointer("campus-submodule.Runtime.dll", "Campus.Common.UIRenderer",
                                                           "UIRenderPass", "Execute");
            if (Config::vrDiagnosticsStartupEnabled) {
                auto* pass = Il2cppUtils::GetClass("campus-submodule.Runtime.dll", "Campus.Common.UIRenderer", "UIRenderPass");
                if (pass) for (const auto* m : pass->methods) {
                    if (m && (m->name == "Execute" || m->name == "DrawBlur" || m->name == "OnDrawBaseUI")) {
                        const auto label = std::string("UIRenderPass.pre-hook.") + m->name;
                        DumpSrpBytes(label.c_str(), m->function, 8192);
                    }
                }
            }
            ADD_HOOK(UIRenderPass_Execute, uiExecute);

            const auto* cameraMainMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Camera", "get_main");
            const bool cameraMainShape = cameraMainMethod != nullptr &&
                cameraMainMethod->static_function && cameraMainMethod->args.empty();
            ADD_HOOK(Camera_get_main,
                     cameraMainShape ? cameraMainMethod->function : nullptr);

            const auto* cinemachineMethod = Il2cppUtils::GetMethod(
                "Cinemachine.dll", "Cinemachine", "CinemachineBrain",
                "PushStateToUnityCamera");
            const bool cinemachineShape = cinemachineMethod != nullptr &&
                !cinemachineMethod->static_function &&
                cinemachineMethod->args.size() == 1U &&
                cinemachineMethod->args.front() != nullptr &&
                cinemachineMethod->args.front()->pType != nullptr &&
                cinemachineMethod->args.front()->pType->name.find("CameraState") !=
                    std::string::npos;
            ADD_HOOK(CinemachineBrain_PushStateToUnityCamera,
                     cinemachineShape ? cinemachineMethod->function : nullptr);

#ifdef GKMS_WINDOWS
            if (vrRuntime) {
                gakumas::vr::InstallLiveAutoPhotoProtection();
                gakumas::vr::InstallGripBlurSource();
                gakumas::vr::input::InstallUnityAnalogScrollHook();
                gakumas::vr::input::InstallUnityPointerInput();
                if (Config::vrDiagnosticsStartupEnabled) {
                    gakumas::vr::input::InstallScrollInputDiagnosticHooks();
                    gakumas::vr::InstallGripTransparencyTrace();
                }
                // Photo-scene lifecycle for the right-A shutter: must install
                // under the frozen VR runtime gate (the VR package ships
                // Config::enabled=false, so translation-block hooks never run).
                ADD_HOOK(LiveScenePresenter_Start,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Live",
                             "LiveScenePresenter", "Start"));
                ADD_HOOK(LiveScenePresenter_OnFinalize,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Live",
                             "LiveScenePresenter", "OnFinalize"));
                ADD_HOOK(PhotographyScenePresenter_Start,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographyScenePresenter", "Start"));
                ADD_HOOK(PhotographyScenePresenter_OnFinalize,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographyScenePresenter", "OnFinalize"));
                ADD_HOOK(PhotographyScenePresenter_SetEvent,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographyScenePresenter", "SetEvent"));
                ADD_HOOK(PhotographyScenePresenter_SetPhotoCountView,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographyScenePresenter", "SetPhotoCountView"));
                ADD_HOOK(PhotographySceneModel_set_IsInitialized,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographySceneModel", "set_IsInitialized",
                             { "*" }));
                ADD_HOOK(LiveSceneView_PlayCapturePhotoAnimation,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Live",
                             "LiveSceneView", "PlayCapturePhotoAnimation",
                             { "*", "*" }));
                ADD_HOOK(PhotographyPhotoContentView_PlayCapturePhotoAnimation,
                         Il2cppUtils::GetMethodPointer(
                             "Assembly-CSharp.dll", "Campus.Photography",
                             "PhotographyPhotoContentView",
                             "PlayCapturePhotoAnimation", { "*" }));
                // Failure-visible install proof: the ADD_HOOK macro skips a
                // null resolve silently, which stereo.219 hardware could not
                // distinguish from a hook that never fires.
                {
                    std::ostringstream photoHooks;
                    photoHooks
                        << "[VR][photo] HOOK_INSTALL liveStart="
                        << (LiveScenePresenter_Start_Orig != nullptr ? 1 : 0)
                        << " liveFinalize="
                        << (LiveScenePresenter_OnFinalize_Orig != nullptr ? 1 : 0)
                        << " photoStart="
                        << (PhotographyScenePresenter_Start_Orig != nullptr ? 1 : 0)
                        << " photoFinalize="
                        << (PhotographyScenePresenter_OnFinalize_Orig != nullptr ? 1 : 0)
                        << " photoSetEvent="
                        << (PhotographyScenePresenter_SetEvent_Orig != nullptr ? 1 : 0)
                        << " photoCountView="
                        << (PhotographyScenePresenter_SetPhotoCountView_Orig != nullptr ? 1 : 0)
                        << " photoModelInit="
                        << (PhotographySceneModel_set_IsInitialized_Orig != nullptr ? 1 : 0)
                        << " liveCaptureAnim="
                        << (LiveSceneView_PlayCapturePhotoAnimation_Orig != nullptr ? 1 : 0)
                        << " photoCaptureAnim="
                        << (PhotographyPhotoContentView_PlayCapturePhotoAnimation_Orig != nullptr ? 1 : 0);
                    static_cast<void>(
                        gakumas::vr::WriteVrLog(photoHooks.str()));
                }
                LogProduceTransitionLayoutForDiagnostics();
                EnsureProFlareProjectionLayout();
                ADD_HOOK(
                    ProFlare_UpdateElementJobData,
                    proFlareProjectionLayout.ready &&
                            proFlareProjectionLayout.updateElementJobData != nullptr
                        ? proFlareProjectionLayout.updateElementJobData->function
                        : nullptr);
                ADD_HOOK(
                    ProFlareBatchForSRPData_ScheduleFlares,
                    proFlareProjectionLayout.ready &&
                            proFlareProjectionLayout.scheduleFlares != nullptr
                        ? proFlareProjectionLayout.scheduleFlares->function
                        : nullptr);
                const bool proFlareReplacementReady =
                    gakumas::vr::camera::ProjectionEquivalentProFlareReady(
                        proFlareProjectionLayout.ready,
                        ProFlareBatchForSRPData_ScheduleFlares_Orig != nullptr,
                        ProFlare_UpdateElementJobData_Orig != nullptr);
                unityStereoRenderer.SetProjectionEquivalentProFlareReady(
                    proFlareReplacementReady);
                std::ostringstream proFlareState;
                proFlareState
                    << "[VR][fov] PRO_FLARE_REPLACEMENT_STATE ready="
                    << (proFlareReplacementReady ? 1 : 0)
                    << " layout="
                    << (proFlareProjectionLayout.ready ? 1 : 0)
                    << " scheduleHook="
                    << (ProFlareBatchForSRPData_ScheduleFlares_Orig != nullptr
                            ? 1 : 0)
                    << " elementHook="
                    << (ProFlare_UpdateElementJobData_Orig != nullptr ? 1 : 0)
                    << " failureVisible="
                    << (proFlareReplacementReady ? 0 : 1);
                static_cast<void>(
                    gakumas::vr::WriteVrLog(proFlareState.str()));
            }
#endif

            const auto contextBoundaryShape = [](const UnityResolve::Method* method) {
                return method != nullptr && method->static_function &&
                    method->return_type != nullptr &&
                    method->return_type->name == "System.Void" &&
                    method->function != nullptr && method->address != nullptr &&
                    method->args.size() == 2U && method->args[0] != nullptr &&
                    method->args[0]->pType != nullptr &&
                    method->args[0]->pType->name ==
                        "UnityEngine.Rendering.ScriptableRenderContext" &&
                    method->args[1] != nullptr &&
                    method->args[1]->pType != nullptr &&
                    method->args[1]->pType->name.find(
                        "System.Collections.Generic.List") != std::string::npos &&
                    method->args[1]->pType->name.find("UnityEngine.Camera") !=
                        std::string::npos;
            };
            const auto* beginContextRenderingMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipelineManager", "BeginContextRendering");
            ADD_HOOK(
                BeginContextRendering,
                contextBoundaryShape(beginContextRenderingMethod)
                    ? beginContextRenderingMethod->function
                    : nullptr);
            const auto* endContextRenderingMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipelineManager", "EndContextRendering");
            ADD_HOOK(
                EndContextRendering,
                contextBoundaryShape(endContextRenderingMethod)
                    ? endContextRenderingMethod->function
                    : nullptr);

            const auto cameraRenderingShape =
                [](const UnityResolve::Method* method) {
                    return method != nullptr && method->static_function &&
                        method->args.size() == 2U &&
                        method->args[0] != nullptr &&
                        method->args[0]->pType != nullptr &&
                        method->args[0]->pType->name.find(
                            "ScriptableRenderContext") != std::string::npos &&
                        method->args[1] != nullptr &&
                        method->args[1]->pType != nullptr &&
                        method->args[1]->pType->name.find("Camera") !=
                            std::string::npos;
                };
            const auto* beginCameraRenderingMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipeline", "BeginCameraRendering");
            ADD_HOOK(
                BeginCameraRendering,
                cameraRenderingShape(beginCameraRenderingMethod)
                    ? beginCameraRenderingMethod->function
                    : nullptr);
            const auto* endCameraRenderingMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipeline", "EndCameraRendering");
            ADD_HOOK(EndCameraRendering,
                      cameraRenderingShape(endCameraRenderingMethod)
                          ? endCameraRenderingMethod->function
                          : nullptr);
            const auto* beginCameraRenderingManagerMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipelineManager", "BeginCameraRendering");
            ADD_HOOK(
                BeginCameraRenderingManager,
                cameraRenderingShape(beginCameraRenderingManagerMethod)
                    ? beginCameraRenderingManagerMethod->function
                    : nullptr);

            const auto* doRenderLoopMethod = Il2cppUtils::GetMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderPipelineManager", "DoRenderLoop_Internal");
            const bool doRenderLoopShape = doRenderLoopMethod != nullptr &&
                doRenderLoopMethod->static_function &&
                doRenderLoopMethod->return_type != nullptr &&
                doRenderLoopMethod->return_type->name == "System.Void" &&
                doRenderLoopMethod->function != nullptr &&
                doRenderLoopMethod->address != nullptr &&
                doRenderLoopMethod->args.size() == 3U &&
                doRenderLoopMethod->args[0] != nullptr &&
                doRenderLoopMethod->args[0]->pType != nullptr &&
                doRenderLoopMethod->args[0]->pType->name ==
                    "UnityEngine.Rendering.RenderPipelineAsset" &&
                doRenderLoopMethod->args[1] != nullptr &&
                doRenderLoopMethod->args[1]->pType != nullptr &&
                doRenderLoopMethod->args[1]->pType->name == "System.IntPtr" &&
                doRenderLoopMethod->args[2] != nullptr &&
                doRenderLoopMethod->args[2]->pType != nullptr &&
                doRenderLoopMethod->args[2]->pType->name == "UnityEngine.Object";
            ADD_HOOK(
                DoRenderLoopInternal,
                doRenderLoopShape ? doRenderLoopMethod->function : nullptr);

            if (Config::vrDiagnosticsStartupEnabled) {
                auto* contextClass = Il2cppUtils::GetClass(
                    "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                    "ScriptableRenderContext");
                // Persist the actual class table (including misses) before any
                // diagnostic hook. No historical RVA or guessed field offset.
                if (contextClass != nullptr) {
                    for (const auto* m : contextClass->methods) {
                        if (m == nullptr) continue;
                        std::ostringstream line;
                        line << "[VR][perf] SRP_PERF_METHOD type=ScriptableRenderContext name="
                             << m->name << " static=" << m->static_function
                             << " return=" << (m->return_type ? m->return_type->name : "?")
                             << " function=" << m->function << " info=" << m->address;
                        for (std::size_t i = 0; i < m->args.size(); ++i) {
                            line << " arg" << i << '='
                                 << (m->args[i] && m->args[i]->pType ? m->args[i]->pType->name : "?");
                        }
                        WriteUnityCameraDiagnosticEvent(line.str());
                    }
                    for (const auto* f : contextClass->fields) {
                        if (f == nullptr) continue;
                        std::ostringstream line;
                        line << "[VR][perf] SRP_PERF_FIELD type=ScriptableRenderContext name="
                             << f->name << " offset=" << f->offset;
                        WriteUnityCameraDiagnosticEvent(line.str());
                    }
                }
                auto exact = [&](const char* name,
                                 std::initializer_list<const char*> args,
                                 bool isStatic = true) -> void* {
                    UnityResolve::Method* found = nullptr;
                    if (contextClass == nullptr) return nullptr;
                    for (auto* m : contextClass->methods) {
                        if (!m || m->name != name || m->static_function != isStatic ||
                            !m->function || !m->address || !m->return_type ||
                            m->return_type->name != "System.Void" || m->args.size() != args.size()) continue;
                        bool match = true;
                        std::size_t i = 0;
                        for (const char* arg : args) {
                            if (!m->args[i] || !m->args[i]->pType || m->args[i]->pType->name != arg) match = false;
                            ++i;
                        }
                        if (!match) continue;
                        if (found) return nullptr;
                        found = m;
                    }
                    if (found) DumpSrpBytes(name, found->function, 256);
                    return found ? found->function : nullptr;
                };
                ADD_HOOK(SrpPerfCull, exact("Internal_Cull_Injected", {
                    "UnityEngine.Rendering.ScriptableCullingParameters&",
                    "UnityEngine.Rendering.ScriptableRenderContext&", "System.IntPtr"}));
                // Current metadata has instance Submit_Internal(), with no
                // Submit_Internal_Injected. Forward the opaque value-type this
                // unchanged; the hook never reads/unboxes it or invokes it itself.
                ADD_HOOK(SrpPerfSubmit, exact("Submit_Internal", {}, false));
                ADD_HOOK(SrpPerfExecuteCommandBuffer, exact("ExecuteCommandBuffer_Internal_Injected", {
                    "UnityEngine.Rendering.ScriptableRenderContext&", "System.IntPtr"}));
                const auto ga = GetModuleHandleW(L"GameAssembly.dll");
                const auto unity = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"UnityPlayer.dll"));
                using ResolveIcall = void* (*)(const char*);
                auto resolveIcall = reinterpret_cast<ResolveIcall>(GetProcAddress(ga, "il2cpp_resolve_icall"));
                char bases[192]{};
                std::snprintf(bases, sizeof(bases),
                    "[VR][perf] SRP_PERF_MODULES GameAssembly=0x%llx UnityPlayer=0x%llx",
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ga)),
                    static_cast<unsigned long long>(unity));
                WriteUnityCameraDiagnosticEvent(bases);
                // Exact current file/PDB identity and ABI bytes are persisted
                // in evidence/srp-performance-dev398. A missing or changed
                // binding is a diagnostic miss, never a guessed RVA hook.
                auto native = [&](const char* signature, std::uintptr_t rva,
                                  const char* expected, std::size_t size,
                                  bool liveWrapperReady) -> void* {
                    if (!liveWrapperReady || !resolveIcall || !unity) return nullptr;
                    void* target = resolveIcall(signature);
                    DumpSrpBytes(signature, target, 256);
                    IMAGE_DOS_HEADER dos{};
                    IMAGE_NT_HEADERS64 nt{};
                    std::array<unsigned char, 16> code{};
                    const bool imageOk = TryCopyNativeBytes(reinterpret_cast<void*>(unity), &dos, sizeof(dos)) &&
                        dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 4096 &&
                        TryCopyNativeBytes(reinterpret_cast<void*>(unity + dos.e_lfanew), &nt, sizeof(nt)) &&
                        nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.TimeDateStamp == 0x6a1fcfac &&
                        nt.OptionalHeader.SizeOfImage == 0x21b6000;
                    const bool ok = imageOk && reinterpret_cast<std::uintptr_t>(target) == unity + rva &&
                        size <= code.size() && TryCopyNativeBytes(target, code.data(), size) &&
                        std::memcmp(code.data(), expected, size) == 0;
                    char record[512]{};
                    std::snprintf(record, sizeof(record),
                        "[VR][perf] SRP_PERF_BIND signature=%s target=0x%llx rva=0x%llx imageOk=%d bytesOk=%d",
                        signature, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(target)),
                        static_cast<unsigned long long>(rva), imageOk, ok);
                    WriteUnityCameraDiagnosticEvent(record);
                    return ok ? target : nullptr;
                };
                ADD_HOOK(SrpNativeCull, native("UnityEngine.Rendering.ScriptableRenderContext::Internal_Cull_Injected",
                    0xe5fa0, "\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xec\x20\x48", 16,
                    SrpPerfCull_Orig != nullptr));
                ADD_HOOK(SrpNativeSubmit, native("UnityEngine.Rendering.ScriptableRenderContext::Submit_Internal",
                    0xe6210, "\x48\x8b\x09\xe9\xf8\x06\x41\x00", 8,
                    SrpPerfSubmit_Orig != nullptr));
                ADD_HOOK(SrpNativeCommand, native("UnityEngine.Rendering.ScriptableRenderContext::ExecuteCommandBuffer_Internal_Injected",
                    0xe64d0, "\x48\x83\xec\x58\x48\x89\x5c\x24\x60\x48\x8d\x05\x8a\x12\xa5\x01", 16,
                    SrpPerfExecuteCommandBuffer_Orig != nullptr));
                // Capture verified classes' real member tables and bounded
                // current method bytes to avoid another extract-only run.
                for (const auto& type : {
                        std::array<const char*, 3>{"Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal", "ScriptableRenderer"},
                        std::array<const char*, 3>{"Unity.RenderPipelines.Universal.Runtime.dll", "UnityEngine.Rendering.Universal", "UniversalRenderPipeline"},
                        std::array<const char*, 3>{"Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLSRPRenderer"},
                        std::array<const char*, 3>{"campus-submodule.Runtime.dll", "Campus.Common.UIRenderer", "UIRenderPass"},
                        std::array<const char*, 3>{"UnityEngine.CoreModule.dll", "UnityEngine", "Resources"},
                        std::array<const char*, 3>{"UnityEngine.CoreModule.dll", "UnityEngine", "ResourcesAPI"},
                        std::array<const char*, 3>{"UnityEngine.CoreModule.dll", "UnityEngine", "ResourcesAPIInternal"},
                        std::array<const char*, 3>{"UnityEngine.CoreModule.dll", "UnityEngine", "Object"}}) {
                    auto* klass = Il2cppUtils::GetClass(type[0], type[1], type[2]);
                    if (!klass) continue;
                    for (const auto* m : klass->methods) {
                        if (!m) continue;
                        std::ostringstream record;
                        record.imbue(std::locale::classic());
                        record << "[VR][perf] SRP_PERF_CLASS_METHOD type=" << type[2] << " name=" << m->name
                               << " static=" << m->static_function << " return=" << (m->return_type ? m->return_type->name : "?")
                               << " function=" << m->function << " info=" << m->address;
                        for (std::size_t i = 0; i < m->args.size(); ++i)
                            record << " arg" << i << '=' << (m->args[i] && m->args[i]->pType ? m->args[i]->pType->name : "?");
                        WriteUnityCameraDiagnosticEvent(record.str());
                        if (std::string_view(type[0]) == "UnityEngine.CoreModule.dll") {
                            // dev.405: read-only evidence for the remaining native
                            // inventory cost. Do not invoke or hook these entries.
                            if (m->name == "FindObjectsOfTypeAll" || m->name == "FindObjectsByType" ||
                                m->name == "FindObjectsOfType" || m->name == "get_ActiveAPI" ||
                                m->name == "get_overrideAPI") {
                                const auto label = std::string(type[2]) + "." + m->name;
                                DumpSrpBytes(label.c_str(), m->function, 512);
                            }
                            continue;
                        }
                        if (std::string_view(type[2]) == "UIRenderPass") continue; // pre-hook bytes already captured
                        if (m->name == "RenderSingleCamera" || m->name == "RenderCameraStack" ||
                            m->name == "Execute" || m->name == "ExecuteRenderPass" || m->name == "InternalStartRendering" ||
                            m->name == "InternalFinishRendering" || m->name == "InitializeRenderingData" ||
                            m->name == "AddRenderPasses" || m->name == "OnPreCullRenderPasses" ||
                            m->name == "OnDrawBaseUI" || m->name == "DrawBlur" ||
                            m->name == "Setup" || m->name == "SetupLights" || m->name == "SetupCullingParameters") {
                            const auto label = std::string(type[2]) + "." + m->name;
                            DumpSrpBytes(label.c_str(), m->function, 8192);
                        }
                    }
                    for (const auto* f : klass->fields) {
                        if (!f) continue;
                        std::ostringstream record;
                        record.imbue(std::locale::classic());
                        record << "[VR][perf] SRP_PERF_CLASS_FIELD type=" << type[2] << " name=" << f->name
                               << " offset=" << f->offset;
                        WriteUnityCameraDiagnosticEvent(record.str());
                    }
                }
                // New observation hooks are installed only after the class
                // census above. Reject missing/ambiguous signatures and shared
                // bodies; never hook a name-only match or a historical RVA.
                auto stage = [&](const char* assembly, const char* space, const char* type,
                                 const char* name, bool isStatic,
                                 std::initializer_list<const char*> args) -> void* {
                    auto* klass = Il2cppUtils::GetClass(assembly, space, type);
                    UnityResolve::Method* found = nullptr;
                    if (klass) for (auto* m : klass->methods) {
                        if (!m || m->name != name || m->static_function != isStatic ||
                            !m->function || !m->address || !m->return_type ||
                            m->return_type->name != "System.Void" || m->args.size() != args.size()) continue;
                        bool match = true;
                        std::size_t i = 0;
                        for (const char* arg : args) {
                            if (!m->args[i] || !m->args[i]->pType || m->args[i]->pType->name != arg) match = false;
                            ++i;
                        }
                        if (!match) continue;
                        if (found) { found = nullptr; break; }
                        found = m;
                    }
                    if (found) for (const auto* m : klass->methods) {
                        if (m && m != found && m->function == found->function) { found = nullptr; break; }
                    }
                    std::ostringstream record;
                    record.imbue(std::locale::classic());
                    record << "[VR][perf] SRP_PERF_BIND stage=" << type << '.' << name
                           << " exact=" << (found != nullptr) << " target=" << (found ? found->function : nullptr);
                    WriteUnityCameraDiagnosticEvent(record.str());
                    return found ? found->function : nullptr;
                };
                ADD_HOOK(SrpInitializeRenderingData, stage("Unity.RenderPipelines.Universal.Runtime.dll",
                    "UnityEngine.Rendering.Universal", "UniversalRenderPipeline", "InitializeRenderingData", true,
                    {"UnityEngine.Rendering.Universal.UniversalRenderPipelineAsset", "UnityEngine.Rendering.Universal.CameraData&",
                     "UnityEngine.Rendering.CullingResults&", "UnityEngine.Rendering.CommandBuffer", "UnityEngine.Rendering.Universal.RenderingData&"}));
                ADD_HOOK(SrpAddRenderPasses, stage("Unity.RenderPipelines.Universal.Runtime.dll",
                    "UnityEngine.Rendering.Universal", "ScriptableRenderer", "AddRenderPasses", false,
                    {"UnityEngine.Rendering.Universal.RenderingData&"}));
                ADD_HOOK(SrpPreCullPasses, stage("Unity.RenderPipelines.Universal.Runtime.dll",
                    "UnityEngine.Rendering.Universal", "ScriptableRenderer", "OnPreCullRenderPasses", false,
                    {"UnityEngine.Rendering.Universal.CameraData&"}));
                ADD_HOOK(SrpUiDrawBase, stage("campus-submodule.Runtime.dll", "Campus.Common.UIRenderer",
                    "UIRenderPass", "OnDrawBaseUI", false,
                    {"UnityEngine.Rendering.ScriptableRenderContext", "UnityEngine.Rendering.Universal.RenderingData&"}));
                std::ostringstream line;
                line << "[VR][perf] SRP_PERF_READY cull=" << (SrpPerfCull_Orig != nullptr)
                     << " submit=" << (SrpPerfSubmit_Orig != nullptr)
                     << " command=" << (SrpPerfExecuteCommandBuffer_Orig != nullptr)
                     << " nativeCull=" << (SrpNativeCull_Orig != nullptr)
                     << " nativeSubmit=" << (SrpNativeSubmit_Orig != nullptr)
                     << " nativeCommand=" << (SrpNativeCommand_Orig != nullptr)
                     << " initializeData=" << (SrpInitializeRenderingData_Orig != nullptr)
                     << " addPasses=" << (SrpAddRenderPasses_Orig != nullptr)
                     << " preCullPasses=" << (SrpPreCullPasses_Orig != nullptr)
                     << " uiDrawBase=" << (SrpUiDrawBase_Orig != nullptr)
                     << " burstLoops=4 periodMs=2000 clock=cpu-wall gpuTiming=0";
                WriteUnityCameraDiagnosticEvent(line.str());
            }

            // Direct owned-eye render-path census. This hooks the exact URP
            // dispatcher that receives every classic ScriptableRenderPass,
            // rather than selecting a component/material by a guessed name.
            // If this player takes the RenderGraph/ExecuteFast route instead,
            // the eye callback will report an empty queue and that is itself
            // the next concrete boundary.
            auto* scriptableRendererClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll",
                "UnityEngine.Rendering.Universal", "ScriptableRenderer");
            UnityResolve::Method* executeRenderPassMethod = nullptr;
            if (scriptableRendererClass != nullptr) {
                for (auto* candidate : scriptableRendererClass->methods) {
                    if (candidate == nullptr ||
                        candidate->name != "ExecuteRenderPass" ||
                        candidate->static_function ||
                        candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != 3U ||
                        candidate->args[0] == nullptr ||
                        candidate->args[0]->pType == nullptr ||
                        candidate->args[0]->pType->name !=
                            "UnityEngine.Rendering.ScriptableRenderContext" ||
                        candidate->args[1] == nullptr ||
                        candidate->args[1]->pType == nullptr ||
                        candidate->args[1]->pType->name !=
                            "UnityEngine.Rendering.Universal.ScriptableRenderPass" ||
                        candidate->args[2] == nullptr ||
                        candidate->args[2]->pType == nullptr ||
                        candidate->args[2]->pType->name !=
                            "UnityEngine.Rendering.Universal.RenderingData&") {
                        continue;
                    }
                    if (executeRenderPassMethod != nullptr) {
                        executeRenderPassMethod = nullptr;
                        break;
                    }
                    executeRenderPassMethod = candidate;
                }
            }
            auto* scriptableRenderPassClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll",
                "UnityEngine.Rendering.Universal", "ScriptableRenderPass");
            if (scriptableRenderPassClass != nullptr) {
                for (auto* candidate : scriptableRenderPassClass->methods) {
                    if (candidate == nullptr ||
                        candidate->name != "get_renderPassEvent" ||
                        candidate->static_function ||
                        candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name !=
                            "UnityEngine.Rendering.Universal.RenderPassEvent" ||
                        !candidate->args.empty()) {
                        continue;
                    }
                    if (eyePassEventGetter != nullptr) {
                        eyePassEventGetter = nullptr;
                        break;
                    }
                    eyePassEventGetter = candidate;
                }
            }

            // Census the real full-screen raster boundary reached by the
            // currently dispatched eye ScriptableRenderPass. Blitter funnels
            // its texture overloads through these exact private helpers, so
            // material/shader/pass plus the captured GameAssembly call stack
            // tells us which authored stage actually submitted pixels.
            auto* blitterClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Core.Runtime.dll",
                "UnityEngine.Rendering", "Blitter");
            const auto findExactBlitterPrimitive = [](
                    UnityResolve::Class* klass,
                    std::string_view methodName) -> UnityResolve::Method* {
                if (klass == nullptr) {
                    return nullptr;
                }
                constexpr std::array<std::string_view, 3> expectedArgs = {
                    "UnityEngine.Rendering.CommandBuffer",
                    "UnityEngine.Material",
                    "System.Int32",
                };
                UnityResolve::Method* exact = nullptr;
                for (auto* candidate : klass->methods) {
                    if (candidate == nullptr || candidate->name != methodName ||
                        !candidate->static_function || candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != expectedArgs.size()) {
                        continue;
                    }
                    bool argsMatch = true;
                    for (std::size_t index = 0U; index < expectedArgs.size(); ++index) {
                        if (candidate->args[index] == nullptr ||
                            candidate->args[index]->pType == nullptr ||
                            std::string_view(candidate->args[index]->pType->name) !=
                                expectedArgs[index]) {
                            argsMatch = false;
                            break;
                        }
                    }
                    if (!argsMatch) {
                        continue;
                    }
                    if (exact != nullptr) {
                        return nullptr;
                    }
                    exact = candidate;
                }
                return exact;
            };
            auto* blitterDrawTriangle = findExactBlitterPrimitive(
                blitterClass, "DrawTriangle");
            auto* blitterDrawQuad = findExactBlitterPrimitive(
                blitterClass, "DrawQuad");

            auto* commandBufferClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll",
                "UnityEngine.Rendering", "CommandBuffer");
            const auto findExactInstanceVoid = [](
                    UnityResolve::Class* klass,
                    std::string_view methodName,
                    const auto& expectedArgs) -> UnityResolve::Method* {
                if (klass == nullptr) {
                    return nullptr;
                }
                UnityResolve::Method* exact = nullptr;
                for (auto* candidate : klass->methods) {
                    if (candidate == nullptr || candidate->name != methodName ||
                        candidate->static_function || candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != expectedArgs.size()) {
                        continue;
                    }
                    bool argsMatch = true;
                    for (std::size_t index = 0U; index < expectedArgs.size(); ++index) {
                        if (candidate->args[index] == nullptr ||
                            candidate->args[index]->pType == nullptr ||
                            std::string_view(candidate->args[index]->pType->name) !=
                                expectedArgs[index]) {
                            argsMatch = false;
                            break;
                        }
                    }
                    if (!argsMatch) {
                        continue;
                    }
                    if (exact != nullptr) {
                        return nullptr;
                    }
                    exact = candidate;
                }
                return exact;
            };
            // The current metadata contains exactly one instance-void method at
            // each ActorShadow name/arity below. Runtime parameter names and ref
            // value-type spellings are diagnostic only: both proved unstable in
            // `.277`/`.278` despite the methods remaining in the live table.
            const auto findUniqueInstanceVoidByArity = [](
                    UnityResolve::Class* klass,
                    std::string_view methodName,
                    std::size_t expectedArgCount) -> UnityResolve::Method* {
                if (klass == nullptr) {
                    return nullptr;
                }
                UnityResolve::Method* exact = nullptr;
                for (auto* candidate : klass->methods) {
                    if (candidate == nullptr || candidate->name != methodName ||
                        candidate->static_function || candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != expectedArgCount) {
                        continue;
                    }
                    if (exact != nullptr) {
                        return nullptr;
                    }
                    exact = candidate;
                }
                return exact;
            };
            constexpr std::array<std::string_view, 7> drawProceduralArgs = {
                "UnityEngine.Matrix4x4",
                "UnityEngine.Material",
                "System.Int32",
                "UnityEngine.MeshTopology",
                "System.Int32",
                "System.Int32",
                "UnityEngine.MaterialPropertyBlock",
            };
            constexpr std::array<std::string_view, 4> commandBlitArgs = {
                "UnityEngine.Rendering.RenderTargetIdentifier",
                "UnityEngine.Rendering.RenderTargetIdentifier",
                "UnityEngine.Material",
                "System.Int32",
            };
            auto* commandDrawProcedural = findExactInstanceVoid(
                commandBufferClass, "DrawProcedural", drawProceduralArgs);
            auto* commandBlit = findExactInstanceVoid(
                commandBufferClass, "Blit", commandBlitArgs);

            auto* materialClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Material");
            if (materialClass != nullptr) {
                for (auto* candidate : materialClass->methods) {
                    if (candidate == nullptr || candidate->name != "get_shader" ||
                        candidate->static_function || candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "UnityEngine.Shader" ||
                        !candidate->args.empty()) {
                        continue;
                    }
                    if (eyeMaterialGetShader != nullptr) {
                        eyeMaterialGetShader = nullptr;
                        break;
                    }
                    eyeMaterialGetShader = candidate;
                }
            }

            // `.149` captured this exact late pass on hardware. Resolve its
            // concrete Execute override and every inspected field from the
            // live metadata table before the eyes-only A/B is allowed to run.
            auto* renderObjectsPassClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll",
                "UnityEngine.Experimental.Rendering.Universal",
                "RenderObjectsPass");
            eyeRenderObjectsPassFields.passClass = renderObjectsPassClass;
            const auto findInstanceField = [](
                    UnityResolve::Class* klass,
                    std::string_view name) -> UnityResolve::Field* {
                if (klass == nullptr) {
                    return nullptr;
                }
                UnityResolve::Field* match = nullptr;
                for (auto* field : klass->fields) {
                    if (field == nullptr || field->name != name ||
                        field->static_field || field->offset < 0) {
                        continue;
                    }
                    if (match != nullptr) {
                        return nullptr;
                    }
                    match = field;
                }
                return match;
            };
            eyeRenderObjectsPassFields.renderQueueType = findInstanceField(
                renderObjectsPassClass, "renderQueueType");
            eyeRenderObjectsPassFields.filteringSettings = findInstanceField(
                renderObjectsPassClass, "m_FilteringSettings");
            eyeRenderObjectsPassFields.cameraSettings = findInstanceField(
                renderObjectsPassClass, "m_CameraSettings");
            eyeRenderObjectsPassFields.profilerTag = findInstanceField(
                renderObjectsPassClass, "m_ProfilerTag");
            eyeRenderObjectsPassFields.overrideMaterial = findInstanceField(
                renderObjectsPassClass, "<overrideMaterial>k__BackingField");
            eyeRenderObjectsPassFields.overrideMaterialPassIndex =
                findInstanceField(
                    renderObjectsPassClass,
                    "<overrideMaterialPassIndex>k__BackingField");
            eyeRenderObjectsPassFields.overrideShader = findInstanceField(
                renderObjectsPassClass, "<overrideShader>k__BackingField");
            eyeRenderObjectsPassFields.overrideShaderPassIndex =
                findInstanceField(
                    renderObjectsPassClass,
                    "<overrideShaderPassIndex>k__BackingField");

            auto* filteringSettingsClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "FilteringSettings");
            eyeRenderObjectsPassFields.filteringClass = filteringSettingsClass;
            eyeRenderObjectsPassFields.renderQueueRange = findInstanceField(
                filteringSettingsClass, "m_RenderQueueRange");
            eyeRenderObjectsPassFields.layerMask = findInstanceField(
                filteringSettingsClass, "m_LayerMask");
            eyeRenderObjectsPassFields.renderingLayerMask = findInstanceField(
                filteringSettingsClass, "m_RenderingLayerMask");
            eyeRenderObjectsPassFields.excludeMotionVectorObjects =
                findInstanceField(
                    filteringSettingsClass, "m_ExcludeMotionVectorObjects");

            auto* renderQueueRangeClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
                "RenderQueueRange");
            eyeRenderObjectsPassFields.renderQueueRangeClass =
                renderQueueRangeClass;
            eyeRenderObjectsPassFields.lowerBound = findInstanceField(
                renderQueueRangeClass, "m_LowerBound");
            eyeRenderObjectsPassFields.upperBound = findInstanceField(
                renderQueueRangeClass, "m_UpperBound");

            UnityResolve::Method* renderObjectsExecuteMethod = nullptr;
            if (renderObjectsPassClass != nullptr) {
                for (auto* candidate : renderObjectsPassClass->methods) {
                    if (candidate == nullptr || candidate->name != "Execute" ||
                        candidate->static_function || candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != 2U ||
                        candidate->args[0] == nullptr ||
                        candidate->args[0]->pType == nullptr ||
                        candidate->args[0]->pType->name !=
                            "UnityEngine.Rendering.ScriptableRenderContext" ||
                        candidate->args[1] == nullptr ||
                        candidate->args[1]->pType == nullptr ||
                        candidate->args[1]->pType->name !=
                            "UnityEngine.Rendering.Universal.RenderingData&") {
                        continue;
                    }
                    if (renderObjectsExecuteMethod != nullptr) {
                        renderObjectsExecuteMethod = nullptr;
                        break;
                    }
                    renderObjectsExecuteMethod = candidate;
                }
            }
            // SMAA T2x functionally copies the shared motion-vector RTHandle
            // immediately after each eye's verified post-process pass. TSCMAA
            // reuses this proven GPU-ordered boundary. The hook therefore
            // follows the ordinary frozen VR runtime gate; all remaining
            // pass/full-screen census hooks stay diagnostic.
            if (Config::vrRuntimeStartupEnabled) {
                ADD_HOOK(
                    ScriptableRenderer_ExecuteRenderPass,
                    executeRenderPassMethod != nullptr
                        ? executeRenderPassMethod->function
                        : nullptr);
            }
            if (Config::vrDiagnosticsStartupEnabled) {
                ADD_HOOK(
                    Blitter_DrawTriangle,
                    blitterDrawTriangle != nullptr
                        ? blitterDrawTriangle->function
                        : nullptr);
                ADD_HOOK(
                    Blitter_DrawQuad,
                    blitterDrawQuad != nullptr ? blitterDrawQuad->function : nullptr);
                ADD_HOOK(
                    CommandBuffer_DrawProcedural,
                    commandDrawProcedural != nullptr
                        ? commandDrawProcedural->function
                        : nullptr);
                eyeCommandDrawProceduralHookReady.store(
                    CommandBuffer_DrawProcedural_Orig != nullptr,
                    std::memory_order_release);
                ADD_HOOK(
                    CommandBuffer_Blit,
                    commandBlit != nullptr ? commandBlit->function : nullptr);
#ifdef GKMS_WINDOWS
                {
                std::ostringstream traceApi;
                traceApi << "[VR][eye-pass] EYE_PASS_TRACE_API execute="
                         << (executeRenderPassMethod != nullptr ? 1 : 0)
                         << " eventGetter=" << (eyePassEventGetter != nullptr ? 1 : 0)
                         << " executeFn="
                         << (executeRenderPassMethod != nullptr
                                 ? executeRenderPassMethod->function
                                 : nullptr)
                         << " executeInfo="
                         << (executeRenderPassMethod != nullptr
                                 ? executeRenderPassMethod->address
                                 : nullptr)
                         << " eventFn="
                         << (eyePassEventGetter != nullptr
                                 ? eyePassEventGetter->function
                                 : nullptr);
                WriteUnityCameraDiagnosticEvent(traceApi.str());
                WriteUnityCameraDiagnosticEvent(
                    "[VR][eye-pass] EYE_PASS_TRACE_METHOD signature=\"instance "
                    "System.Void UnityEngine.Rendering.Universal."
                    "ScriptableRenderer.ExecuteRenderPass("
                    "UnityEngine.Rendering.ScriptableRenderContext,"
                    "UnityEngine.Rendering.Universal.ScriptableRenderPass,"
                    "UnityEngine.Rendering.Universal.RenderingData&)\"");

                std::ostringstream fullscreenApi;
                fullscreenApi
                    << "[VR][eye-pass] EYE_FULLSCREEN_API triangle="
                    << (blitterDrawTriangle != nullptr ? 1 : 0)
                    << " quad=" << (blitterDrawQuad != nullptr ? 1 : 0)
                    << " procedural=" << (commandDrawProcedural != nullptr ? 1 : 0)
                    << " blit=" << (commandBlit != nullptr ? 1 : 0)
                    << " getShader=" << (eyeMaterialGetShader != nullptr ? 1 : 0)
                    << " triangleFn="
                    << (blitterDrawTriangle != nullptr
                            ? blitterDrawTriangle->function
                            : nullptr)
                    << " triangleInfo="
                    << (blitterDrawTriangle != nullptr
                            ? blitterDrawTriangle->address
                            : nullptr)
                    << " quadFn="
                    << (blitterDrawQuad != nullptr
                            ? blitterDrawQuad->function
                            : nullptr)
                    << " quadInfo="
                    << (blitterDrawQuad != nullptr
                            ? blitterDrawQuad->address
                            : nullptr)
                    << " proceduralFn="
                    << (commandDrawProcedural != nullptr
                            ? commandDrawProcedural->function
                            : nullptr)
                    << " proceduralInfo="
                    << (commandDrawProcedural != nullptr
                            ? commandDrawProcedural->address
                            : nullptr)
                    << " blitFn="
                    << (commandBlit != nullptr
                            ? commandBlit->function
                            : nullptr)
                    << " blitInfo="
                    << (commandBlit != nullptr
                            ? commandBlit->address
                            : nullptr);
                WriteUnityCameraDiagnosticEvent(fullscreenApi.str());
                WriteUnityCameraDiagnosticEvent(
                    "[VR][eye-pass] EYE_FULLSCREEN_METHOD signatures=\"static "
                    "System.Void UnityEngine.Rendering.Blitter.DrawTriangle|"
                    "DrawQuad(UnityEngine.Rendering.CommandBuffer,"
                    "UnityEngine.Material,System.Int32); instance System.Void "
                    "UnityEngine.Rendering.CommandBuffer.DrawProcedural("
                    "UnityEngine.Matrix4x4,UnityEngine.Material,System.Int32,"
                    "UnityEngine.MeshTopology,System.Int32,System.Int32,"
                    "UnityEngine.MaterialPropertyBlock); Blit("
                    "UnityEngine.Rendering.RenderTargetIdentifier,"
                    "UnityEngine.Rendering.RenderTargetIdentifier,"
                    "UnityEngine.Material,System.Int32)\"");

                const bool inspectedFieldsReady =
                    eyeRenderObjectsPassFields.Ready();
                std::ostringstream renderObjectsApi;
                renderObjectsApi
                    << "[VR][eye-pass] EYE_RENDER_OBJECTS_API class="
                    << (renderObjectsPassClass != nullptr ? 1 : 0)
                    << " execute="
                    << (renderObjectsExecuteMethod != nullptr ? 1 : 0)
                    << " fields=" << (inspectedFieldsReady ? 1 : 0)
                    << " executeFn="
                    << (renderObjectsExecuteMethod != nullptr
                            ? renderObjectsExecuteMethod->function
                            : nullptr)
                    << " executeInfo="
                    << (renderObjectsExecuteMethod != nullptr
                            ? renderObjectsExecuteMethod->address
                            : nullptr);
                WriteUnityCameraDiagnosticEvent(renderObjectsApi.str());

                const auto logLiveFields = [](UnityResolve::Class* klass) {
                    if (klass == nullptr) {
                        return;
                    }
                    for (const auto* field : klass->fields) {
                        if (field == nullptr) {
                            continue;
                        }
                        std::ostringstream line;
                        line << "[VR][eye-pass] EYE_RENDER_OBJECTS_FIELD owner="
                             << klass->namespaze << '.' << klass->name
                             << " name=\"" << field->name << "\" type=\""
                             << (field->type != nullptr
                                     ? field->type->name
                                     : std::string("?"))
                             << "\" offset=0x" << std::hex << field->offset
                             << std::dec
                             << " static=" << (field->static_field ? 1 : 0);
                        WriteUnityCameraDiagnosticEvent(line.str());
                    }
                };
                logLiveFields(renderObjectsPassClass);
                logLiveFields(filteringSettingsClass);
                logLiveFields(renderQueueRangeClass);
                }
#endif
            }

            // OverlayCanvas skip installs independently of diagnostics.
            ADD_HOOK(
                RenderObjectsPass_Execute,
                renderObjectsExecuteMethod != nullptr
                    ? renderObjectsExecuteMethod->function
                    : nullptr);
#ifdef GKMS_WINDOWS
            {
                std::ostringstream hookLine;
                hookLine << "[VR][eye-pass] EYE_RENDER_OBJECTS_HOOK ready="
                         << (renderObjectsExecuteMethod != nullptr &&
                                     eyePassEventGetter != nullptr
                                 ? 1
                                 : 0)
                         << " installed="
                         << (RenderObjectsPass_Execute_Orig != nullptr ? 1 : 0)
                         << " eventGetter="
                         << (eyePassEventGetter != nullptr ? 1 : 0)
                         << " signature=System.Void Execute("
                         << "UnityEngine.Rendering.ScriptableRenderContext,"
                         << "UnityEngine.Rendering.Universal.RenderingData&)";
                static_cast<void>(gakumas::vr::WriteVrLog(hookLine.str()));
            }
#endif

            UnityResolve::Method* computeAndRenderFlaresMethod = nullptr;
            if (auto* proFlareAssembly =
                    UnityResolve::Get("ProFlare.Runtime.dll")) {
                auto* renderingSystem =
                    proFlareAssembly->Get("ProFlareRenderingSystem", "");
                if (renderingSystem == nullptr) {
                    renderingSystem =
                        proFlareAssembly->Get("ProFlareRenderingSystem");
                }
                if (renderingSystem != nullptr) {
                    for (auto* candidate : renderingSystem->methods) {
                        if (candidate == nullptr ||
                            candidate->name != "ComputeAndRenderFlares" ||
                            candidate->static_function ||
                            candidate->function == nullptr ||
                            candidate->address == nullptr ||
                            candidate->return_type == nullptr ||
                            candidate->return_type->name != "System.Void" ||
                            candidate->args.size() != 2U ||
                            candidate->args[0] == nullptr ||
                            candidate->args[0]->pType == nullptr ||
                            candidate->args[0]->pType->name.find(
                                "CommandBuffer") == std::string::npos ||
                            candidate->args[1] == nullptr ||
                            candidate->args[1]->pType == nullptr ||
                            candidate->args[1]->pType->name.find("Camera") ==
                                std::string::npos ||
                            candidate->args[1]->pType->name.find(
                                "Cinemachine") != std::string::npos) {
                            continue;
                        }
                        if (computeAndRenderFlaresMethod != nullptr) {
                            computeAndRenderFlaresMethod = nullptr;
                            break;
                        }
                        computeAndRenderFlaresMethod = candidate;
                    }
                }
            }
            ADD_HOOK(
                ProFlareRenderingSystem_ComputeAndRenderFlares,
                computeAndRenderFlaresMethod != nullptr
                    ? computeAndRenderFlaresMethod->function
                    : nullptr);
#ifdef GKMS_WINDOWS
            {
                std::ostringstream flareHook;
                flareHook << "[VR][fov] PRO_FLARE_RENDER_HOOK ready="
                          << (computeAndRenderFlaresMethod != nullptr ? 1 : 0)
                          << " installed="
                          << (ProFlareRenderingSystem_ComputeAndRenderFlares_Orig !=
                                      nullptr
                                  ? 1
                                  : 0)
                          << " signature=System.Void ComputeAndRenderFlares("
                          << "UnityEngine.Rendering.CommandBuffer,"
                          << "UnityEngine.Camera)";
                static_cast<void>(gakumas::vr::WriteVrLog(flareHook.str()));
            }
#endif

            // Actor-lighting passes all override
            // ScriptableRenderPass.Execute(ScriptableRenderContext,
            // ref RenderingData). Accept only that exact shape, resolved on
            // the live table, and let a miss disable the anchor for that pass
            // instead of guessing an address.
            const auto resolveActorPassExecute =
                [](const char* assembly, const char* nameSpace,
                   const char* className) -> void* {
                    auto* klass =
                        Il2cppUtils::GetClass(assembly, nameSpace, className);
                    if (klass == nullptr) {
                        return nullptr;
                    }
                    for (const auto* candidate : klass->methods) {
                        if (candidate == nullptr ||
                            candidate->name != "Execute" ||
                            candidate->static_function ||
                            candidate->function == nullptr ||
                            candidate->return_type == nullptr ||
                            candidate->return_type->name != "System.Void" ||
                            candidate->args.size() != 2U ||
                            candidate->args[0] == nullptr ||
                            candidate->args[0]->pType == nullptr ||
                            candidate->args[0]->pType->name !=
                                "UnityEngine.Rendering.ScriptableRenderContext" ||
                            candidate->args[1] == nullptr ||
                            candidate->args[1]->pType == nullptr ||
                            candidate->args[1]->pType->name.find(
                                "RenderingData") == std::string::npos) {
                            continue;
                        }
                        return candidate->function;
                    }
                    return nullptr;
                };
            ADD_HOOK(
                ActorShadowPass_Execute,
                resolveActorPassExecute(
                    "vl-unity.Runtime.dll", "VL.Rendering", "ActorShadowPass"));
            auto* actorShadowPassClass = Il2cppUtils::GetClass(
                "vl-unity.Runtime.dll", "VL.Rendering", "ActorShadowPass");
            auto* drawActorShadowFeatureClass = Il2cppUtils::GetClass(
                "vl-unity.Runtime.dll", "VL.Rendering", "DrawActorShadowPass");
            actorShadowStereoReuse.featurePass = findInstanceField(
                drawActorShadowFeatureClass, "_pass");
            actorShadowStereoReuse.shadowData = findInstanceField(
                actorShadowPassClass, "_actorShadowData");
            actorShadowStereoReuse.shadowBias = findInstanceField(
                actorShadowPassClass, "_actorShadowBias");
            actorShadowStereoReuse.supportsShadow = findInstanceField(
                actorShadowPassClass, "_supportsShadow");
            auto* actorShadowScriptableRenderPassClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Universal.Runtime.dll",
                "UnityEngine.Rendering.Universal", "ScriptableRenderPass");
            actorShadowStereoReuse.clearFlag = findInstanceField(
                actorShadowScriptableRenderPassClass, "m_ClearFlag");
            if (actorShadowScriptableRenderPassClass != nullptr) {
                constexpr std::array<std::string_view, 2> clearArgs = {
                    "UnityEngine.Rendering.ClearFlag",
                    "UnityEngine.Color",
                };
                for (auto* candidate :
                     actorShadowScriptableRenderPassClass->methods) {
                    if (candidate == nullptr ||
                        candidate->name != "ConfigureClear" ||
                        candidate->static_function ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != clearArgs.size()) {
                        continue;
                    }
                    bool exact = true;
                    for (std::size_t index = 0U;
                         index < clearArgs.size(); ++index) {
                        if (candidate->args[index] == nullptr ||
                            candidate->args[index]->pType == nullptr ||
                            std::string_view(candidate->args[index]->pType->name) !=
                                clearArgs[index]) {
                            exact = false;
                            break;
                        }
                    }
                    if (!exact ||
                        actorShadowStereoReuse.configureClear != nullptr) {
                        actorShadowStereoReuse.configureClear = nullptr;
                        break;
                    }
                    actorShadowStereoReuse.configureClear = candidate;
                }
            }
            auto* actorShadowDataClass = Il2cppUtils::GetClass(
                "vl-unity.Runtime.dll", "", "ActorShadowData");
            if (actorShadowDataClass != nullptr &&
                actorShadowDataClass->address != nullptr &&
                UnityResolve::Invoke<bool>(
                    "il2cpp_class_is_valuetype",
                    actorShadowDataClass->address)) {
                actorShadowStereoReuse.dataFieldHeader =
                    static_cast<std::int32_t>(2U * sizeof(void*));
            }
            actorShadowStereoReuse.startDistance2 = findInstanceField(
                actorShadowDataClass, "startDistance2");
            actorShadowStereoReuse.endDistance2 = findInstanceField(
                actorShadowDataClass, "endDistance2");
            actorShadowStereoReuse.fade = findInstanceField(
                actorShadowDataClass, "fade");
            actorShadowStereoReuse.strength = findInstanceField(
                actorShadowDataClass, "strength");
            std::uint32_t actorShadowsKeywordFlags = 0U;
            if (actorShadowPassClass != nullptr) {
                for (auto* field : actorShadowPassClass->fields) {
                    if (field != nullptr && field->address != nullptr &&
                        field->name == "_ACTOR_SHADOWS" &&
                        field->type != nullptr &&
                        field->type->name == "System.String") {
                        const std::uint32_t flags =
                            UnityResolve::Invoke<std::uint32_t>(
                                "il2cpp_field_get_flags", field->address);
                        constexpr std::uint32_t kFieldAttributeStatic = 0x10U;
                        if ((flags & kFieldAttributeStatic) == 0U) {
                            continue;
                        }
                        if (actorShadowStereoReuse.actorShadowsKeyword != nullptr) {
                            actorShadowStereoReuse.actorShadowsKeyword = nullptr;
                            actorShadowsKeywordFlags = 0U;
                            break;
                        }
                        actorShadowStereoReuse.actorShadowsKeyword = field;
                        actorShadowsKeywordFlags = flags;
                    }
                }
            }
            if (actorShadowStereoReuse.shadowData != nullptr &&
                actorShadowStereoReuse.shadowBias != nullptr &&
                actorShadowStereoReuse.shadowBias->offset >
                    actorShadowStereoReuse.shadowData->offset) {
                actorShadowStereoReuse.dataSize = static_cast<std::size_t>(
                    actorShadowStereoReuse.shadowBias->offset -
                    actorShadowStereoReuse.shadowData->offset);
            }
            actorShadowStereoReuse.setupReceiver =
                findUniqueInstanceVoidByArity(
                actorShadowPassClass,
                "SetupActorShadowReceiverConstants",
                1U);
            UnityResolve::Method* actorShadowDrawMethod = nullptr;
            UnityResolve::Method* actorShadowConfigureMethod = nullptr;
            if (actorShadowPassClass != nullptr) {
                actorShadowDrawMethod = findUniqueInstanceVoidByArity(
                    actorShadowPassClass, "DrawShadow", 3U);
                actorShadowConfigureMethod = findUniqueInstanceVoidByArity(
                    actorShadowPassClass, "Configure", 2U);
            }
            auto* coreUtilsClass = Il2cppUtils::GetClass(
                "Unity.RenderPipelines.Core.Runtime.dll",
                "UnityEngine.Rendering", "CoreUtils");
            if (coreUtilsClass != nullptr) {
                constexpr std::array<std::string_view, 3> keywordArgs = {
                    "UnityEngine.Rendering.CommandBuffer",
                    "System.String",
                    "System.Boolean",
                };
                UnityResolve::Method* exactKeyword = nullptr;
                for (auto* candidate : coreUtilsClass->methods) {
                    if (candidate == nullptr || candidate->name != "SetKeyword" ||
                        !candidate->static_function || candidate->function == nullptr ||
                        candidate->address == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != keywordArgs.size()) {
                        continue;
                    }
                    bool exact = true;
                    for (std::size_t i = 0; i < keywordArgs.size(); ++i) {
                        if (candidate->args[i] == nullptr ||
                            candidate->args[i]->pType == nullptr ||
                            std::string_view(candidate->args[i]->pType->name) !=
                                keywordArgs[i]) {
                            exact = false;
                            break;
                        }
                    }
                    if (!exact) {
                        continue;
                    }
                    if (exactKeyword != nullptr) {
                        exactKeyword = nullptr;
                        break;
                    }
                    exactKeyword = candidate;
                }
                actorShadowStereoReuse.setKeyword = exactKeyword;
            }
            const bool actorShadowReuseReady =
                actorShadowStereoReuse.featurePass != nullptr &&
                actorShadowStereoReuse.shadowData != nullptr &&
                actorShadowStereoReuse.shadowBias != nullptr &&
                actorShadowStereoReuse.supportsShadow != nullptr &&
                actorShadowStereoReuse.clearFlag != nullptr &&
                actorShadowStereoReuse.configureClear != nullptr &&
                actorShadowStereoReuse.actorShadowsKeyword != nullptr &&
                actorShadowStereoReuse.setupReceiver != nullptr &&
                actorShadowStereoReuse.setKeyword != nullptr &&
                actorShadowDrawMethod != nullptr &&
                actorShadowConfigureMethod != nullptr &&
                actorShadowStereoReuse.dataSize == 0x11CU;
            ADD_HOOK(
                ActorShadowPass_Configure,
                actorShadowReuseReady && actorShadowConfigureMethod != nullptr
                    ? actorShadowConfigureMethod->function : nullptr);
            ADD_HOOK(
                ActorShadowPass_DrawShadow,
                actorShadowReuseReady && actorShadowDrawMethod != nullptr
                    ? actorShadowDrawMethod->function : nullptr);
            // Virtual (slot 7) — called through the vtable by URP's feature
            // walk, so it always has a live function body even when LTCG
            // inlines Setup/UpdateShadowData into it. The .94 Setup trampoline
            // never entered; .95 hardware confirmed this is the live path.
            const auto resolveDrawActorShadowAddRenderPasses = []() -> void* {
                auto* klass = Il2cppUtils::GetClass(
                    "vl-unity.Runtime.dll", "VL.Rendering",
                    "DrawActorShadowPass");
                if (klass == nullptr) {
                    return nullptr;
                }
                for (const auto* candidate : klass->methods) {
                    if (candidate == nullptr ||
                        candidate->name != "AddRenderPasses" ||
                        candidate->static_function ||
                        candidate->function == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != 2U ||
                        candidate->args[1] == nullptr ||
                        candidate->args[1]->pType == nullptr ||
                        candidate->args[1]->pType->name.find(
                            "RenderingData") == std::string::npos) {
                        continue;
                    }
                    return candidate->function;
                }
                return nullptr;
            };
            const void* addRenderPassesAddr =
                resolveDrawActorShadowAddRenderPasses();
            ADD_HOOK(
                DrawActorShadowPass_AddRenderPasses,
                const_cast<void*>(addRenderPassesAddr));
#ifdef GKMS_WINDOWS
            // ADD_HOOK only reports through printf (no console on the game
            // process), so mirror the resolve results into vr.log; the
            // .94 run could not distinguish "resolver missed" from "hook
            // installed but the body was never entered".
            {
                const auto logMethodCandidates = [](UnityResolve::Class* klass) {
                    if (klass == nullptr) {
                        return;
                    }
                    for (const auto* candidate : klass->methods) {
                        if (candidate == nullptr ||
                            (candidate->name != "Configure" &&
                             candidate->name != "DrawShadow" &&
                             candidate->name !=
                                 "SetupActorShadowReceiverConstants")) {
                            continue;
                        }
                        std::ostringstream methodStream;
                        methodStream
                            << "[VR][shadow] ACTOR_SHADOW_METHOD_TABLE"
                            << " name=" << candidate->name
                            << " function=" << candidate->function
                            << " info=" << candidate->address
                            << " static=" << (candidate->static_function ? 1 : 0)
                            << " return="
                            << (candidate->return_type != nullptr
                                    ? candidate->return_type->name : "?")
                            << " argc=" << candidate->args.size();
                        for (std::size_t index = 0U;
                             index < candidate->args.size(); ++index) {
                            const auto* arg = candidate->args[index];
                            methodStream << " arg" << index << "="
                                         << (arg != nullptr ? arg->name : "?")
                                         << ":"
                                         << (arg != nullptr && arg->pType != nullptr
                                                 ? arg->pType->name : "?");
                        }
                        static_cast<void>(gakumas::vr::WriteVrLog(
                            methodStream.str()));
                    }
                };
                logMethodCandidates(actorShadowPassClass);
                std::ostringstream resolveStream;
                resolveStream << "[VR][shadow] SHADOW_HOOK_RESOLVE"
                              << " addRenderPasses=" << addRenderPassesAddr
                              << " drawShadow="
                              << (actorShadowDrawMethod != nullptr
                                      ? actorShadowDrawMethod->function : nullptr)
                              << " configure="
                              << (actorShadowConfigureMethod != nullptr
                                      ? actorShadowConfigureMethod->function : nullptr)
                              << " reuseReady="
                              << (actorShadowReuseReady ? 1 : 0)
                              << " prereq="
                              << (actorShadowStereoReuse.featurePass != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.shadowData != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.shadowBias != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.supportsShadow != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.clearFlag != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.configureClear != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.actorShadowsKeyword != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.setupReceiver != nullptr ? 1 : 0)
                              << (actorShadowStereoReuse.setKeyword != nullptr ? 1 : 0)
                              << (actorShadowDrawMethod != nullptr ? 1 : 0)
                              << (actorShadowConfigureMethod != nullptr ? 1 : 0)
                              << " dataOffset="
                              << (actorShadowStereoReuse.shadowData != nullptr
                                      ? actorShadowStereoReuse.shadowData->offset : -1)
                              << " dataSize=" << actorShadowStereoReuse.dataSize
                              << " clearOffset="
                              << (actorShadowStereoReuse.clearFlag != nullptr
                                      ? actorShadowStereoReuse.clearFlag->offset : -1)
                              << " configureClear="
                              << (actorShadowStereoReuse.configureClear != nullptr
                                      ? actorShadowStereoReuse.configureClear->function
                                      : nullptr)
                              << " dataFieldHeader="
                              << actorShadowStereoReuse.dataFieldHeader
                              << " keywordOffset="
                              << (actorShadowStereoReuse.actorShadowsKeyword != nullptr
                                      ? actorShadowStereoReuse.actorShadowsKeyword->offset : -1)
                              << " keywordFlags=" << actorShadowsKeywordFlags
                              << " distanceOffsets="
                              << (actorShadowStereoReuse.startDistance2 != nullptr
                                      ? actorShadowStereoReuse.startDistance2->offset -
                                            actorShadowStereoReuse.dataFieldHeader : -1)
                              << ","
                              << (actorShadowStereoReuse.endDistance2 != nullptr
                                      ? actorShadowStereoReuse.endDistance2->offset -
                                            actorShadowStereoReuse.dataFieldHeader : -1)
                              << ","
                              << (actorShadowStereoReuse.fade != nullptr
                                      ? actorShadowStereoReuse.fade->offset -
                                            actorShadowStereoReuse.dataFieldHeader : -1)
                              << ","
                              << (actorShadowStereoReuse.strength != nullptr
                                      ? actorShadowStereoReuse.strength->offset -
                                            actorShadowStereoReuse.dataFieldHeader : -1);
                static_cast<void>(
                    gakumas::vr::WriteVrLog(resolveStream.str()));
            }
#endif
            // The MatCap main light — hair/accessory shading on the body.
            ADD_HOOK(
                CampusActorParameterPass_Execute,
                resolveActorPassExecute(
                    "campus-submodule.Runtime.dll", "Campus.Rendering",
                    "CampusActorParameterPass"));
            ADD_HOOK(
                VLActorParameterPass_Execute,
                resolveActorPassExecute(
                    "Unity.RenderPipelines.Universal.Runtime.dll",
                    "VL.Rendering", "VLActorParameterPass"));
            const auto resolveMaterialInfoConstructor = []() -> void* {
                auto* klass = Il2cppUtils::GetClass(
                    "vl-unity.Runtime.dll", "VL.Core", "MaterialInfo");
                if (klass == nullptr) {
                    return nullptr;
                }
                static constexpr const char* kArgs[] = {
                    "UnityEngine.Material", "UnityEngine.Renderer",
                    "System.Int32", "UnityEngine.Material",
                };
                for (const auto* candidate : klass->methods) {
                    if (candidate == nullptr || candidate->name != ".ctor" ||
                        candidate->static_function ||
                        candidate->function == nullptr ||
                        candidate->return_type == nullptr ||
                        candidate->return_type->name != "System.Void" ||
                        candidate->args.size() != std::size(kArgs)) {
                        continue;
                    }
                    bool exact = true;
                    for (std::size_t index = 0; index < std::size(kArgs);
                         ++index) {
                        exact = exact && candidate->args[index] != nullptr &&
                            candidate->args[index]->pType != nullptr &&
                            candidate->args[index]->pType->name == kArgs[index];
                    }
                    if (exact) {
                        return candidate->function;
                    }
                }
                return nullptr;
            };
            const auto resolveUpdatePenlightParams = []() -> void* {
                static constexpr const char* kAssemblies[] = {
                    "Assembly-CSharp.dll",
                    "campus-submodule.Runtime.dll",
                };
                for (const char* assembly : kAssemblies) {
                    auto* klass = Il2cppUtils::GetClass(
                        assembly, "Campus.MobAudience",
                        "MobAudiencePenlightController");
                    if (klass == nullptr) {
                        continue;
                    }
                    for (const auto* candidate : klass->methods) {
                        if (candidate == nullptr ||
                            candidate->name != "UpdatePenlightParams" ||
                            candidate->static_function ||
                            candidate->function == nullptr ||
                            candidate->return_type == nullptr ||
                            candidate->return_type->name != "System.Void" ||
                            candidate->args.size() != 2U ||
                            candidate->args[0] == nullptr ||
                            candidate->args[0]->pType == nullptr ||
                            candidate->args[1] == nullptr ||
                            candidate->args[1]->pType == nullptr) {
                            continue;
                        }
                        const auto& contextType =
                            candidate->args[0]->pType->name;
                        const auto& cameraType =
                            candidate->args[1]->pType->name;
                        if (contextType.find("ScriptableRenderContext") !=
                                std::string::npos &&
                            cameraType.find("Camera") != std::string::npos) {
                            return candidate->function;
                        }
                    }
                }
                return nullptr;
            };
            std::string crowdRenderSignature = "-";
            const auto resolveCrowdRender =
                [&crowdRenderSignature]() -> void* {
                static constexpr const char* kAssemblies[] = {
                    "Assembly-CSharp.dll",
                    "campus-submodule.Runtime.dll",
                };
                for (const char* assembly : kAssemblies) {
                    auto* klass = Il2cppUtils::GetClass(
                        assembly, "Campus.Crowd", "CrowdSystem");
                    if (klass == nullptr) {
                        continue;
                    }
                    for (const auto* candidate : klass->methods) {
                        if (candidate == nullptr ||
                            candidate->name != "RenderCrowd" ||
                            candidate->return_type == nullptr) {
                            continue;
                        }
                        std::ostringstream signature;
                        signature << candidate->return_type->name
                                  << " RenderCrowd(";
                        for (std::size_t index = 0;
                             index < candidate->args.size(); ++index) {
                            if (index != 0U) {
                                signature << ",";
                            }
                            signature << (candidate->args[index] != nullptr &&
                                    candidate->args[index]->pType != nullptr
                                ? candidate->args[index]->pType->name
                                : "-");
                        }
                        signature << ")";
                        crowdRenderSignature = signature.str();

                        if (candidate->static_function ||
                            candidate->function == nullptr ||
                            candidate->return_type->name != "System.Void" ||
                            candidate->args.size() != 2U ||
                            candidate->args[0] == nullptr ||
                            candidate->args[0]->pType == nullptr ||
                            candidate->args[1] == nullptr ||
                            candidate->args[1]->pType == nullptr) {
                            continue;
                        }
                        const auto& commandBufferType =
                            candidate->args[0]->pType->name;
                        const auto& eventType =
                            candidate->args[1]->pType->name;
                        const bool exactEvent =
                            eventType == "CrowdSystem.EventType" ||
                            eventType ==
                                "Campus.Crowd.CrowdSystem.EventType";
                        if (commandBufferType ==
                                "UnityEngine.Rendering.CommandBuffer" &&
                            exactEvent) {
                            return candidate->function;
                        }
                    }
                }
                return nullptr;
            };
            const void* materialInfoCtorAddr = nullptr;
            const void* updatePenlightParamsAddr = nullptr;
            const void* crowdRenderAddr = nullptr;
            if (vrRuntime) {
                materialInfoCtorAddr = resolveMaterialInfoConstructor();
                ADD_HOOK(
                    MaterialInfo_ctor,
                    const_cast<void*>(materialInfoCtorAddr));
                updatePenlightParamsAddr = resolveUpdatePenlightParams();
                ADD_HOOK(
                    MobPenlight_UpdatePenlightParams,
                    const_cast<void*>(updatePenlightParamsAddr));
                crowdRenderAddr = resolveCrowdRender();
                ADD_HOOK(
                    CrowdSystem_RenderCrowd,
                    const_cast<void*>(crowdRenderAddr));
            }
#ifdef GKMS_WINDOWS
            if (vrRuntime) {
                static_cast<void>(gakumas::vr::WriteVrLog(
                    std::string(
                        "[VR][stereo] EYE_OUTLINE_CAPTURE_HOOK resolved=") +
                    (materialInfoCtorAddr != nullptr ? "1" : "0")));
                static_cast<void>(gakumas::vr::WriteVrLog(
                    std::string(
                        "[VR][stereo] HAND_GLOW hook UpdatePenlightParams=") +
                    (updatePenlightParamsAddr != nullptr ? "1" : "0")));
                static_cast<void>(gakumas::vr::WriteVrLog(
                    std::string(
                        "[VR][stereo] HAND_GLOW crowd hook RenderCrowd=") +
                    (crowdRenderAddr != nullptr ? "1" : "0") +
                    " signature=" + crowdRenderSignature));
            }
#endif
            const auto resolveDeferredRenderActor = []() -> void* {
                static constexpr const char* kAssemblies[] = {
                    "Unity.RenderPipelines.Universal.Runtime.dll",
                    "vl-unity.Runtime.dll",
                };
                for (const char* assembly : kAssemblies) {
                    auto* klass = Il2cppUtils::GetClass(
                        assembly, "VL.Rendering", "VLDeferredPass");
                    if (klass == nullptr) {
                        continue;
                    }
                    for (const auto* candidate : klass->methods) {
                        if (candidate == nullptr ||
                            candidate->name != "RenderActor" ||
                            candidate->static_function ||
                            candidate->function == nullptr ||
                            candidate->return_type == nullptr ||
                            candidate->return_type->name != "System.Void" ||
                            candidate->args.size() != 2U ||
                            candidate->args[0] == nullptr ||
                            candidate->args[0]->pType == nullptr ||
                            candidate->args[0]->pType->name !=
                                "UnityEngine.Rendering.ScriptableRenderContext" ||
                            candidate->args[1] == nullptr ||
                            candidate->args[1]->pType == nullptr ||
                            candidate->args[1]->pType->name.find(
                                "RenderingData") == std::string::npos) {
                            continue;
                        }
                        return candidate->function;
                    }
                }
                return nullptr;
            };
            const void* renderActorAddr = resolveDeferredRenderActor();
            ADD_HOOK(
                VLDeferredPass_RenderActor,
                const_cast<void*>(renderActorAddr));
#ifdef GKMS_WINDOWS
            {
                std::ostringstream resolveStream;
                resolveStream << "[VR][shadow] MATCAP_GBUFFER_HOOK_RESOLVE"
                              << " renderActor=" << renderActorAddr;
                static_cast<void>(
                    gakumas::vr::WriteVrLog(resolveStream.str()));
            }
#endif
        }

#ifdef GKMS_WINDOWS
        if (vrRuntime) {
            unityCameraMainHookReady.store(
                Camera_get_main_Orig != nullptr,
                std::memory_order_relaxed);
            unityCinemachineHookReady.store(
                CinemachineBrain_PushStateToUnityCamera_Orig != nullptr,
                std::memory_order_relaxed);
            unityCameraRenderHookReady.store(
                EndCameraRendering_Orig != nullptr,
                std::memory_order_relaxed);
            unityRenderPipelineGuardReady.store(
                DoRenderLoopInternal_Orig != nullptr,
                std::memory_order_release);
            unityCameraDiagnosticHooksConfigured.store(true, std::memory_order_release);
            EnsureUnityCameraDiagnosticMarker();
        }
#endif

    }
    // 77 2640 5000

    DEFINE_HOOK(int, il2cpp_init, (const char* domain_name)) {
#ifndef GKMS_WINDOWS
        const auto ret = il2cpp_init_Orig(domain_name);
#else
        const auto ret = 0;
#endif
        // InjectFunctions();

        if (!Config::isConfigInit) {
            Log::Error("Core initialization requested before config was loaded.");
            return ret;
        }
        if (!Config::enabled &&
            (!Config::vrRuntimeStartupEnabled || Config::vrNativeOnly)) {
            Log::Info("Unity hook layer is disabled for this configuration.");
            return ret;
        }

        Log::InfoFmt(
            "Start core init (localify=%d, localizeText=%d, "
            "vrRuntimeStartup=%d, vrDiagnosticsStartup=%d, nativeOnly=%d)...",
            Config::enabled,
            Config::vrLocalizeText,
            Config::vrRuntimeStartupEnabled,
            Config::vrDiagnosticsStartupEnabled,
            Config::vrNativeOnly);

        if (Config::lazyInit) {
            UnityResolveProgress::startInit = true;
            UnityResolveProgress::assembliesProgress.total = 2;
            UnityResolveProgress::assembliesProgress.current = 1;
            UnityResolveProgress::classProgress.total = 43;
            UnityResolveProgress::classProgress.current = 0;
        }

        StartInjectFunctions();
        if (Config::enabled) {
            GKCamera::initCameraSettings();
        }

        if (Config::lazyInit) {
            UnityResolveProgress::assembliesProgress.current = 2;
            UnityResolveProgress::classProgress.total = 1;
            UnityResolveProgress::classProgress.current = 0;
        }

        if (Config::enabled) {
            EnsureLocalizationData();
        }

        UnityResolveProgress::startInit = false;

        Log::Info("Core init finished.");
        return ret;
    }
}


namespace GakumasLocal::Hook {
    void Install() {
        const auto hookInstaller = Plugin::GetInstance().GetHookInstaller();

        Log::Info("Installing hook");

#ifndef GKMS_WINDOWS
        ADD_HOOK(HookMain::il2cpp_init,
            Plugin::GetInstance().GetHookInstaller()->LookupSymbol("il2cpp_init"));
#else
        HookMain::il2cpp_init_Hook(nullptr);
#endif


        Log::Info("Hook installed");
    }
}
