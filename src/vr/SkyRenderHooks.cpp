#include "SkyRenderHooks.hpp"

#include "UnityStereoRenderer.hpp"
#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "../deps/UnityResolve/UnityResolve.hpp"
#include "../hooks/HookManager.hpp"

#include <Windows.h>
#include <d3d11.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace gakumas::vr {
namespace {

constexpr std::uint32_t kSamplesPerRole = 16U;
constexpr std::uint32_t kBurstSamples = 8U;
constexpr std::uint32_t kLaterStride = 30U;

void* g_pendingPass = nullptr;
bool g_apiDumped = false;
bool g_lookupMissLogged = false;
bool g_relatedDumped = false;
bool g_hooksInstalled = false;
bool g_matrixApiDumped = false;
bool g_fieldsReady = false;
bool g_runtimeLayoutMissLogged = false;
std::int32_t g_cameraDataOffset = -1;

struct CameraDataFields {
    std::int32_t view = -1;
    std::int32_t proj = -1;
    std::int32_t jitter = -1;
    std::int32_t camera = -1;
    std::int32_t targetTex = -1;
    std::int32_t pixelRect = -1;
    std::int32_t useOverride = -1;
    std::int32_t screenSize = -1;
    std::int32_t screenBias = -1;
    std::int32_t pixelW = -1;
    std::int32_t pixelH = -1;
    std::int32_t aspect = -1;
    std::int32_t scale = -1;
    std::int32_t xrRendering = -1;
    std::int32_t antialiasing = -1;
    std::int32_t resetHistory = -1;
    std::int32_t taaPersistent = -1;
    std::int32_t motionPersistent = -1;
    std::int32_t taaSettings = -1;
};

CameraDataFields g_fields{};
float g_sourceView[16]{};
float g_sourceProj[16]{};
bool g_haveSourceMatrices = false;

struct EyeViewStash {
    bool ready = false;
    float matrix[16]{};
    float view[16]{};
    float invpMid[3]{};
    float fovCamMid[3]{};
    float fovP02Mid[3]{};
    float midX = 0.0F;
    float midY = 0.0F;
    float fov = 0.0F;
    float p02 = 0.0F;
    float p12 = 0.0F;
    int pixW = 0;
    int pixH = 0;
};

constexpr bool kWriteViewDir = false;
constexpr bool kSkipEyeSkyPass = false;
constexpr bool kForceEyeSolidClear = false;
constexpr bool kRecenterSkyDraw = false;
constexpr bool kReplaceComputeViewDir = true;

// Trivial IL2CPP/MSVC x64 layout mirrors (no C++ constructors).
struct Il2CppVec2 {
    float x;
    float y;
};
struct Il2CppVec4 {
    float x;
    float y;
    float z;
    float w;
};
struct Il2CppMat4 {
    float m[16];
};

std::array<EyeViewStash, 2> g_eyeStash{};
std::array<std::uint32_t, 2> g_writeLogs{};
std::array<std::uint32_t, 2> g_hookLogs{};
std::array<std::uint32_t, 2> g_inLogs{};
int g_viewDirNameId = 0;
bool g_writeFaulted = false;
bool g_setMatrixHookInstalled = false;
bool g_skyMatDumped = false;
bool g_skyExpFaulted = false;
std::uint32_t g_anySetLogs = 0;
std::array<std::uint32_t, 2> g_expLogs{};
std::array<std::uint32_t, 2> g_skipLogs{};
std::array<std::uint32_t, 2> g_clearLogs{};
std::array<std::uint32_t, 2> g_drawLogs{};
using SetMatFn = void (*)(void*, int, void*, void*);
using GlobalSetFn = void (*)(int, void*, void*);
using DrawMeshFn = void (*)(void*, void*, void*, void*, int, int, void*, void*);
SetMatFn g_setMatrixOrig = nullptr;
SetMatFn g_matSetOrig = nullptr;
SetMatFn g_cmdSetOrig = nullptr;
GlobalSetFn g_globalSetOrig = nullptr;
DrawMeshFn g_drawMeshOrig = nullptr;
using ComputeCamFn = Il2CppMat4* (__fastcall*)(
    Il2CppMat4*, void*, const Il2CppVec4*, Il2CppMat4*, Il2CppMat4*, void*);
using ComputeFovFn = Il2CppMat4* (__fastcall*)(
    Il2CppMat4*, float, Il2CppVec2, const Il2CppVec4*, const Il2CppMat4*, bool,
    float, bool, void*);
ComputeCamFn g_computeCamOrig = nullptr;
ComputeFovFn g_computeFovOrig = nullptr;
bool g_skyboxHookInstalled = false;
bool g_drawMeshHookInstalled = false;
bool g_computeHooksInstalled = false;
bool g_inEyeSkyExecute = false;
std::array<std::uint32_t, 2> g_computeLogs{};

float ReadFloat(void* instance, std::int32_t offset) noexcept;
int ReadInt(void* instance, std::int32_t offset) noexcept;
void* ReadPointer(void* instance, std::int32_t offset) noexcept;
bool ReadMatrix(void* instance, std::int32_t offset, float out[16]) noexcept;
void LogInheritedFields(void* klassAddress, const char* tag) noexcept;
void LogIncomingViewDir(const char* via, int nameId, void* matrix) noexcept;
void DumpSkyMaterial(void* pass) noexcept;
std::array<std::uint32_t, 4> g_samples{};
std::array<std::uint32_t, 4> g_interesting{};
bool g_taaTypesDumped = false;
bool g_taaPersistLogged = false;
std::uint32_t g_taaInputBurst = 0;
std::uint32_t g_taaInputStride = 0;
UnityStereoRenderer* g_renderer = nullptr;

struct SmaaT2xProbeApi {
    bool attempted = false;
    bool ready = false;
    UnityResolve::Class* postProcessPass = nullptr;
    UnityResolve::Class* vlPostProcessPass = nullptr;
    UnityResolve::Class* motionVectorPass = nullptr;
    UnityResolve::Class* vlMotionVectorPass = nullptr;
    UnityResolve::Class* rtHandle = nullptr;
    UnityResolve::Class* texture = nullptr;
    UnityResolve::Field* motionVectors = nullptr;
    UnityResolve::Method* getRt = nullptr;
    UnityResolve::Method* getNativeTexturePtr = nullptr;
};

SmaaT2xProbeApi g_smaaT2xProbeApi{};
std::array<std::uint32_t, 2> g_smaaT2xProbeSamples{};
std::array<void*, 2> g_smaaT2xLastRtHandles{};
std::array<void*, 2> g_smaaT2xLastRenderTextures{};
std::array<void*, 2> g_smaaT2xLastNativePointers{};
std::array<void*, 2> g_smaaT2xLastD3dResources{};
std::uint64_t g_smaaT2xPassSerial = 0U;

using PassFn = void (*)(void*, void*, void*, void*);
PassFn g_executeOrig = nullptr;
PassFn g_drawOrig = nullptr;
PassFn g_skyboxOrig = nullptr;

void Log(std::string_view message) noexcept {
    WriteVrLog(message);
}

std::string TypeName(const UnityResolve::Type* type) noexcept {
    if (type == nullptr || type->name.empty()) {
        return "?";
    }
    return type->name;
}

UnityResolve::Class* FindClass(
    const char* assemblyName, const char* ns, const char* name) noexcept {
    auto* assembly = UnityResolve::Get(assemblyName);
    return assembly != nullptr ? assembly->Get(name, ns) : nullptr;
}

std::int32_t FindInstanceFieldOffset(
    UnityResolve::Class* klass, std::string_view name) noexcept {
    if (klass == nullptr) {
        return -1;
    }
    for (auto* field : klass->fields) {
        if (field != nullptr && !field->static_field && field->name == name) {
            return field->offset;
        }
    }
    return -1;
}

bool ResolveSkyRuntimeDataLayout() noexcept {
    if (g_fieldsReady && g_cameraDataOffset >= 16) {
        return true;
    }

    auto* renderingData = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "RenderingData");
    auto* cameraData = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "CameraData");
    CameraDataFields resolved{};
    const std::int32_t cameraDataOffset =
        FindInstanceFieldOffset(renderingData, "cameraData");
    resolved.view = FindInstanceFieldOffset(cameraData, "m_ViewMatrix");
    resolved.proj = FindInstanceFieldOffset(cameraData, "m_ProjectionMatrix");
    resolved.jitter = FindInstanceFieldOffset(cameraData, "m_JitterMatrix");
    resolved.camera = FindInstanceFieldOffset(cameraData, "camera");
    resolved.targetTex = FindInstanceFieldOffset(cameraData, "targetTexture");
    resolved.pixelRect = FindInstanceFieldOffset(cameraData, "pixelRect");
    resolved.useOverride =
        FindInstanceFieldOffset(cameraData, "useScreenCoordOverride");
    resolved.screenSize =
        FindInstanceFieldOffset(cameraData, "screenSizeOverride");
    resolved.screenBias =
        FindInstanceFieldOffset(cameraData, "screenCoordScaleBias");
    resolved.xrRendering = FindInstanceFieldOffset(cameraData, "xrRendering");
    resolved.pixelW = FindInstanceFieldOffset(cameraData, "pixelWidth");
    resolved.pixelH = FindInstanceFieldOffset(cameraData, "pixelHeight");
    resolved.aspect = FindInstanceFieldOffset(cameraData, "aspectRatio");
    resolved.scale = FindInstanceFieldOffset(cameraData, "renderScale");
    resolved.antialiasing = FindInstanceFieldOffset(cameraData, "antialiasing");
    resolved.resetHistory = FindInstanceFieldOffset(cameraData, "resetHistory");
    resolved.taaPersistent =
        FindInstanceFieldOffset(cameraData, "taaPersistentData");
    resolved.motionPersistent =
        FindInstanceFieldOffset(cameraData, "motionVectorsPersistentData");
    resolved.taaSettings = FindInstanceFieldOffset(cameraData, "taaSettings");

    const bool ready = renderingData != nullptr && cameraData != nullptr &&
        cameraDataOffset >= 16 && resolved.view >= 16 && resolved.proj >= 16 &&
        resolved.camera >= 16 && resolved.pixelW >= 16 &&
        resolved.pixelH >= 16 && resolved.antialiasing >= 16;
    if (!ready) {
        if (!g_runtimeLayoutMissLogged) {
            g_runtimeLayoutMissLogged = true;
            Log(std::string("[VR][sky] SKY_RUNTIME_LAYOUT ready=0 renderingData=") +
                (renderingData != nullptr ? "1" : "0") + " cameraData=" +
                (cameraData != nullptr ? "1" : "0") + " camDataOff=" +
                std::to_string(cameraDataOffset) + " view=" +
                std::to_string(resolved.view) + " proj=" +
                std::to_string(resolved.proj) + " camera=" +
                std::to_string(resolved.camera) + " pix=" +
                std::to_string(resolved.pixelW) + "x" +
                std::to_string(resolved.pixelH) + " aa=" +
                std::to_string(resolved.antialiasing));
        }
        return false;
    }

    g_cameraDataOffset = cameraDataOffset;
    g_fields = resolved;
    g_fieldsReady = true;
    Log("[VR][sky] SKY_RUNTIME_LAYOUT ready=1 camDataOff=" +
        std::to_string(g_cameraDataOffset) + " view=" +
        std::to_string(g_fields.view) + " proj=" +
        std::to_string(g_fields.proj) + " camera=" +
        std::to_string(g_fields.camera) + " pix=" +
        std::to_string(g_fields.pixelW) + "x" +
        std::to_string(g_fields.pixelH) + " aa=" +
        std::to_string(g_fields.antialiasing) + " resetHistory=" +
        std::to_string(g_fields.resetHistory) + " taa=" +
        std::to_string(g_fields.taaPersistent) + " motion=" +
        std::to_string(g_fields.motionPersistent) + " taaSettings=" +
        std::to_string(g_fields.taaSettings));
    return true;
}

UnityResolve::Class* FindSkyPass() noexcept {
    auto* found = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering",
        "VLSkyPass");
    if (found == nullptr) {
        found = FindClass("vl-unity.Runtime.dll", "VL.Rendering", "VLSkyPass");
    }
    return found;
}

void DumpClass(
    UnityResolve::Class* klass, const char* tag, std::uint32_t methodCap,
    std::uint32_t fieldCap) noexcept {
    if (klass == nullptr) {
        Log(std::string("[VR][sky] ") + tag + " found=0");
        return;
    }
    Log(std::string("[VR][sky] ") + tag + " ns=" + klass->namespaze +
        " name=" + klass->name + " methods=" +
        std::to_string(klass->methods.size()) + " fields=" +
        std::to_string(klass->fields.size()));
    std::uint32_t loggedMethods = 0;
    for (auto* method : klass->methods) {
        if (method == nullptr || loggedMethods >= methodCap) {
            continue;
        }
        ++loggedMethods;
        std::ostringstream stream;
        stream << "[VR][sky] " << tag << "_METHOD name=" << method->name
               << " static=" << (method->static_function ? "1" : "0")
               << " ret=" << TypeName(method->return_type) << " args=";
        for (std::size_t index = 0; index < method->args.size(); ++index) {
            if (index != 0U) {
                stream << ",";
            }
            const auto* arg = method->args[index];
            stream << (arg != nullptr ? TypeName(arg->pType) : "?");
        }
        Log(stream.str());
    }
    std::uint32_t loggedFields = 0;
    for (auto* field : klass->fields) {
        if (field == nullptr || loggedFields >= fieldCap) {
            continue;
        }
        ++loggedFields;
        Log(std::string("[VR][sky] ") + tag + "_FIELD name=" + field->name +
            " type=" + TypeName(field->type) +
            " offset=" + std::to_string(field->offset) +
            " static=" + (field->static_field ? "1" : "0"));
    }
}

UnityResolve::Method* FindExactInstanceZeroArg(
    UnityResolve::Class* klass,
    std::string_view name,
    std::string_view returnType) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    UnityResolve::Method* exact = nullptr;
    for (auto* candidate : klass->methods) {
        if (candidate == nullptr || candidate->name != name ||
            candidate->static_function || !candidate->args.empty() ||
            candidate->function == nullptr || candidate->address == nullptr ||
            candidate->return_type == nullptr ||
            std::string_view(candidate->return_type->name) != returnType) {
            continue;
        }
        if (exact != nullptr) {
            return nullptr;
        }
        exact = candidate;
    }
    return exact;
}

UnityResolve::Field* FindExactInstanceField(
    UnityResolve::Class* klass,
    std::string_view name,
    std::string_view type) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    UnityResolve::Field* exact = nullptr;
    for (auto* candidate : klass->fields) {
        if (candidate == nullptr || candidate->name != name ||
            candidate->static_field || candidate->offset < 16 ||
            candidate->type == nullptr ||
            std::string_view(candidate->type->name) != type) {
            continue;
        }
        if (exact != nullptr) {
            return nullptr;
        }
        exact = candidate;
    }
    return exact;
}

UnityResolve::Class* FindVlClass(
    const char* nameSpace,
    const char* name) noexcept {
    auto* found = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll", nameSpace, name);
    if (found == nullptr) {
        found = FindClass("vl-unity.Runtime.dll", nameSpace, name);
    }
    return found;
}

void* RuntimeInvokeZeroArg(
    UnityResolve::Method* method,
    void* instance,
    bool unboxPointer) noexcept {
    if (method == nullptr || method->address == nullptr || instance == nullptr) {
        return nullptr;
    }
    using RuntimeInvoke = void* (*)(void*, void*, void**, void**);
    using ObjectUnbox = void* (*)(void*);
    static const auto runtimeInvoke = reinterpret_cast<RuntimeInvoke>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_runtime_invoke"));
    static const auto objectUnbox = reinterpret_cast<ObjectUnbox>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_object_unbox"));
    if (runtimeInvoke == nullptr || (unboxPointer && objectUnbox == nullptr)) {
        return nullptr;
    }
    void* exception = nullptr;
    void* result = nullptr;
    __try {
        result = runtimeInvoke(method->address, instance, nullptr, &exception);
        if (exception != nullptr || result == nullptr) {
            return nullptr;
        }
        if (!unboxPointer) {
            return result;
        }
        void* value = objectUnbox(result);
        return value != nullptr ? *reinterpret_cast<void**>(value) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool IsClassOrSubclass(void* instanceClass, void* expectedClass) noexcept {
    if (instanceClass == nullptr || expectedClass == nullptr) {
        return false;
    }
    using GetParent = void* (*)(void*);
    static const auto getParent = reinterpret_cast<GetParent>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_class_get_parent"));
    if (getParent == nullptr) {
        return false;
    }
    void* current = instanceClass;
    for (std::uint32_t depth = 0U; current != nullptr && depth < 32U; ++depth) {
        if (current == expectedClass) {
            return true;
        }
        __try {
            current = getParent(current);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    return false;
}

const char* ReadLiveClassNamePart(void* klass, bool nameSpace) noexcept {
    using GetName = const char* (*)(void*);
    static const auto getName = reinterpret_cast<GetName>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_class_get_name"));
    static const auto getNamespace = reinterpret_cast<GetName>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_class_get_namespace"));
    const auto getter = nameSpace ? getNamespace : getName;
    if (klass == nullptr || getter == nullptr) {
        return nullptr;
    }
    __try {
        return getter(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::string LiveClassName(void* klass) noexcept {
    if (klass == nullptr) {
        return "?";
    }
    const char* name = ReadLiveClassNamePart(klass, false);
    const char* nameSpace = ReadLiveClassNamePart(klass, true);
    std::string result = nameSpace != nullptr ? nameSpace : "";
    if (!result.empty()) {
        result.push_back('.');
    }
    result += name != nullptr ? name : "?";
    return result;
}

void ResolveSmaaT2xProbeApiOnce() noexcept {
    if (g_smaaT2xProbeApi.attempted ||
        (!GakumasLocal::Config::vrRuntimeStartupEnabled &&
         !GakumasLocal::Config::vrDiagnosticsStartupEnabled)) {
        return;
    }
    g_smaaT2xProbeApi.attempted = true;
    auto& api = g_smaaT2xProbeApi;
    api.postProcessPass = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "PostProcessPass");
    api.vlPostProcessPass = FindVlClass(
        "VL.Rendering.Internal", "VLPostProcessPass");
    api.motionVectorPass = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "MotionVectorRenderPass");
    api.vlMotionVectorPass = FindVlClass(
        "VL.Rendering", "VLMotionVectorRenderPass");
    api.rtHandle = FindClass(
        "Unity.RenderPipelines.Core.Runtime.dll",
        "UnityEngine.Rendering", "RTHandle");
    api.texture = FindClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Texture");
    api.motionVectors = FindExactInstanceField(
        api.postProcessPass, "m_MotionVectors",
        "UnityEngine.Rendering.RTHandle");
    api.getRt = FindExactInstanceZeroArg(
        api.rtHandle, "get_rt", "UnityEngine.RenderTexture");
    api.getNativeTexturePtr = FindExactInstanceZeroArg(
        api.texture, "GetNativeTexturePtr", "System.IntPtr");
    // The VL types are part of the census, but their absence must not suppress
    // the core URP PostProcessPass observation that tells us why they moved.
    api.ready = api.postProcessPass != nullptr && api.rtHandle != nullptr &&
        api.texture != nullptr && api.motionVectors != nullptr &&
        api.getRt != nullptr && api.getNativeTexturePtr != nullptr;

    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        DumpClass(api.postProcessPass, "SMAA_T2X_POST_PROCESS", 128U, 128U);
        DumpClass(api.vlPostProcessPass, "SMAA_T2X_VL_POST_PROCESS", 128U, 128U);
        DumpClass(api.motionVectorPass, "SMAA_T2X_MOTION_PASS", 96U, 96U);
        DumpClass(api.vlMotionVectorPass, "SMAA_T2X_VL_MOTION_PASS", 96U, 96U);
        DumpClass(api.rtHandle, "SMAA_T2X_RT_HANDLE", 96U, 96U);
        if (api.postProcessPass != nullptr) {
            LogInheritedFields(api.postProcessPass->address, "SMAA_T2X_POST_INHERITED");
        }
        if (api.vlPostProcessPass != nullptr) {
            LogInheritedFields(
                api.vlPostProcessPass->address, "SMAA_T2X_VL_POST_INHERITED");
        }
        if (api.motionVectorPass != nullptr) {
            LogInheritedFields(
                api.motionVectorPass->address, "SMAA_T2X_MOTION_INHERITED");
        }
        if (api.vlMotionVectorPass != nullptr) {
            LogInheritedFields(
                api.vlMotionVectorPass->address, "SMAA_T2X_VL_MOTION_INHERITED");
        }
    }

    std::ostringstream line;
    line << "[VR][smaa-t2x] SMAA_T2X_API ready=" << (api.ready ? 1 : 0)
         << " post=" << (api.postProcessPass != nullptr ? 1 : 0)
         << " vlPost=" << (api.vlPostProcessPass != nullptr ? 1 : 0)
         << " motion=" << (api.motionVectorPass != nullptr ? 1 : 0)
         << " vlMotion=" << (api.vlMotionVectorPass != nullptr ? 1 : 0)
         << " rtHandle=" << (api.rtHandle != nullptr ? 1 : 0)
         << " motionField=" << (api.motionVectors != nullptr ? 1 : 0)
         << " motionOffset="
         << (api.motionVectors != nullptr ? api.motionVectors->offset : -1)
         << " getRt=" << (api.getRt != nullptr ? 1 : 0)
         << " getNative=" << (api.getNativeTexturePtr != nullptr ? 1 : 0)
         << " getRtFn="
         << (api.getRt != nullptr ? api.getRt->function : nullptr)
         << " getRtInfo="
         << (api.getRt != nullptr ? api.getRt->address : nullptr)
         << " getNativeFn="
         << (api.getNativeTexturePtr != nullptr
                 ? api.getNativeTexturePtr->function
                 : nullptr)
         << " getNativeInfo="
         << (api.getNativeTexturePtr != nullptr
                 ? api.getNativeTexturePtr->address
                 : nullptr);
    Log(line.str());
}

struct SmaaT2xNativeDescription {
    bool ready = false;
    void* identity = nullptr;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t format = 0U;
    std::uint32_t samples = 0U;
};

SmaaT2xNativeDescription DescribeSmaaT2xNativeTexture(
    void* nativePointer) noexcept {
    SmaaT2xNativeDescription result{};
    if (nativePointer == nullptr) {
        return result;
    }
    ID3D11Texture2D* texture = nullptr;
    IUnknown* identity = nullptr;
    __try {
        if (FAILED(reinterpret_cast<IUnknown*>(nativePointer)->QueryInterface(
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&texture))) || texture == nullptr) {
            return result;
        }
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (SUCCEEDED(texture->QueryInterface(
                __uuidof(IUnknown), reinterpret_cast<void**>(&identity))) &&
            identity != nullptr) {
            result.identity = identity;
        } else {
            result.identity = texture;
        }
        result.ready = true;
        result.width = description.Width;
        result.height = description.Height;
        result.format = static_cast<std::uint32_t>(description.Format);
        result.samples = description.SampleDesc.Count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = {};
    }
    if (identity != nullptr) {
        identity->Release();
    }
    if (texture != nullptr) {
        texture->Release();
    }
    return result;
}

void DumpTaaTypesOnce() noexcept;
void InstallVlPostProcessHooks() noexcept;

void DumpMatrixApiOnce() noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    if (g_matrixApiDumped) {
        return;
    }
    g_matrixApiDumped = true;
    const std::tuple<const char*, const char*, const char*> types[] = {
        {"UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock"},
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Material"},
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Shader"},
        {"UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "CommandBuffer"},
        {"UnityEngine.CoreModule.dll", "UnityEngine", "Camera"},
    };
    for (const auto& type : types) {
        auto* klass = FindClass(
            std::get<0>(type), std::get<1>(type), std::get<2>(type));
        if (klass == nullptr) {
            Log(std::string("[VR][sky] MATRIX_API name=") + std::get<2>(type) +
                " found=0");
            continue;
        }
        std::uint32_t logged = 0;
        for (auto* method : klass->methods) {
            if (method == nullptr || logged >= 16U) {
                continue;
            }
            if (method->name.find("Matrix") == std::string::npos &&
                method->name.find("projection") == std::string::npos &&
                method->name.find("worldToCamera") == std::string::npos &&
                method->name.find("DrawMesh") == std::string::npos &&
                method->name.find("Blit") == std::string::npos &&
                method->name.find("SetView") == std::string::npos) {
                continue;
            }
            ++logged;
            std::ostringstream stream;
            stream << "[VR][sky] MATRIX_API type=" << std::get<2>(type)
                   << " name=" << method->name
                   << " static=" << (method->static_function ? "1" : "0")
                   << " args=" << method->args.size();
            if (method->name.find("SetMatrix") != std::string::npos ||
                method->name.find("DrawMesh") != std::string::npos) {
                stream << " types=";
                for (std::size_t index = 0; index < method->args.size(); ++index) {
                    if (index != 0U) {
                        stream << ",";
                    }
                    const auto* arg = method->args[index];
                    stream << (arg != nullptr ? TypeName(arg->pType) : "?");
                }
            }
            Log(stream.str());
        }
        if (std::string_view(std::get<2>(type)) == "MaterialPropertyBlock") {
            DumpClass(klass, "MPB", 8U, 16U);
        }
        if (std::string_view(std::get<2>(type)) == "CommandBuffer") {
            DumpClass(klass, "CMDBUF_DRAW", 32U, 4U);
        }
    }
}

void DumpRelatedSkyTypes() noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    if (g_relatedDumped) {
        return;
    }
    g_relatedDumped = true;
    const char* assemblies[] = {
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "vl-unity.Runtime.dll",
        "Unity.RenderPipelines.Core.Runtime.dll",
    };
    const std::string_view needles[] = {
        "SkyPass", "Skybox", "VLSky", "VLCloud", "CloudLayer", "CloudData",
        "DrawCloud",
    };
    for (const char* assemblyName : assemblies) {
        auto* assembly = UnityResolve::Get(assemblyName);
        Log(std::string("[VR][sky] SKY_PASS_ASSEMBLY name=") + assemblyName +
            " found=" + (assembly != nullptr ? "1" : "0"));
        if (assembly == nullptr) {
            continue;
        }
        std::uint32_t loggedTypes = 0;
        for (auto* klass : assembly->classes) {
            if (klass == nullptr || loggedTypes >= 48U) {
                continue;
            }
            bool interesting = false;
            for (const auto needle : needles) {
                if (klass->name.find(needle) != std::string::npos) {
                    interesting = true;
                    break;
                }
            }
            if (!interesting) {
                continue;
            }
            ++loggedTypes;
            Log(std::string("[VR][sky] SKY_PASS_TYPE assembly=") + assemblyName +
                " ns=" + klass->namespaze + " name=" + klass->name);
        }
    }
    const std::pair<const char*, const char*> extras[] = {
        {"VL.Rendering", "VLCloud"},
        {"VL.Rendering", "CloudData"},
        {"VL.Rendering", "CloudLayer"},
        {"VL.Rendering", "CloudDataParameter"},
        {"UnityEngine.Rendering", "VolumeParameter"},
        {"UnityEngine.Rendering", "FloatParameter"},
        {"UnityEngine.Rendering", "ColorParameter"},
        {"UnityEngine.Rendering.Universal", "DrawSkyboxPass"},
        {"UnityEngine.Rendering.Universal", "RenderingData"},
        {"UnityEngine.Rendering.Universal", "CameraData"},
        {"UnityEngine.Rendering.Universal", "UniversalCameraData"},
        {"UnityEngine.Rendering", "RTHandle"},
    };
    for (const auto& extra : extras) {
        auto* found = FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll", extra.first,
            extra.second);
        if (found == nullptr) {
            found = FindClass("vl-unity.Runtime.dll", extra.first, extra.second);
        }
        if (found == nullptr) {
            found = FindClass(
                "Unity.RenderPipelines.Core.Runtime.dll", extra.first,
                extra.second);
        }
        const std::string_view extraName = extra.second;
        const bool wide = extraName == "CameraData" || extraName == "CloudLayer" ||
            extraName == "CloudData" || extraName == "VLCloud" ||
            extraName == "UniversalCameraData";
        DumpClass(found, extra.second, wide ? 24U : 16U, wide ? 96U : 16U);
        if (found != nullptr && found->address != nullptr &&
            (extraName == "CloudDataParameter" || extraName == "VolumeParameter" ||
             extraName == "FloatParameter" || extraName == "ColorParameter" ||
             extraName == "CloudLayer")) {
            LogInheritedFields(
                found->address, (std::string(extra.second) + "_INHERITED").c_str());
        }
    }
    DumpMatrixApiOnce();
    DumpTaaTypesOnce();
}

void DumpSkyPassApiOnce() noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    DumpRelatedSkyTypes();
    const char* assemblies[] = {
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "vl-unity.Runtime.dll",
    };
    auto* klass = FindSkyPass();
    if (klass == nullptr) {
        if (!g_lookupMissLogged) {
            for (const char* assemblyName : assemblies) {
                auto* found =
                    FindClass(assemblyName, "VL.Rendering", "VLSkyPass");
                Log(std::string("[VR][sky] SKY_PASS_LOOKUP assembly=") +
                    assemblyName +
                    " found=" + (found != nullptr ? "1" : "0"));
            }
            Log("[VR][sky] SKY_PASS_LOOKUP found=0");
            g_lookupMissLogged = true;
        }
        return;
    }
    if (g_apiDumped) {
        return;
    }
    g_apiDumped = true;
    for (const char* assemblyName : assemblies) {
        auto* found = FindClass(assemblyName, "VL.Rendering", "VLSkyPass");
        Log(std::string("[VR][sky] SKY_PASS_LOOKUP assembly=") + assemblyName +
            " found=" + (found != nullptr ? "1" : "0"));
    }
    DumpClass(klass, "SKY_PASS", 32U, 32U);
}

int RoleIndex(const char* role) noexcept {
    if (role == nullptr) {
        return 3;
    }
    if (std::string_view(role) == "source") {
        return 0;
    }
    if (std::string_view(role) == "left") {
        return 1;
    }
    if (std::string_view(role) == "right") {
        return 2;
    }
    return 3;
}

UnityResolve::Method* FindExecute(UnityResolve::Class* klass) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    UnityResolve::Method* fallback = nullptr;
    for (auto* method : klass->methods) {
        if (method == nullptr || method->static_function ||
            method->name != "Execute" || method->function == nullptr) {
            continue;
        }
        if (method->args.size() == 2U && method->args[0] != nullptr &&
            method->args[1] != nullptr && method->args[0]->pType != nullptr &&
            method->args[1]->pType != nullptr &&
            method->args[0]->pType->name.find("ScriptableRenderContext") !=
                std::string::npos &&
            method->args[1]->pType->name.find("RenderingData") !=
                std::string::npos) {
            return method;
        }
        if (fallback == nullptr && method->args.size() == 2U) {
            fallback = method;
        }
    }
    return fallback;
}

UnityResolve::Method* FindNamedIntArg(
    UnityResolve::Class* klass, const char* name) noexcept {
    if (klass == nullptr || name == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method == nullptr || method->function == nullptr ||
            method->name != name || method->args.size() != 1U ||
            method->args[0] == nullptr || method->args[0]->pType == nullptr) {
            continue;
        }
        const std::string type = TypeName(method->args[0]->pType);
        if (type.find("Int32") != std::string::npos ||
            type.find("System.Int32") != std::string::npos) {
            return method;
        }
    }
    return nullptr;
}

UnityResolve::Method* FindNamed(
    UnityResolve::Class* klass, const char* name) noexcept {
    if (klass == nullptr || name == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method != nullptr && method->name == name &&
            method->function != nullptr) {
            return method;
        }
    }
    return klass->Get<UnityResolve::Method>(name);
}

void LogNamedMethodShapes(
    UnityResolve::Class* klass,
    std::string_view name,
    std::string_view tag) noexcept {
    std::uint32_t count = 0U;
    if (klass != nullptr) {
        for (auto* method : klass->methods) {
            if (method == nullptr || method->name != name) {
                continue;
            }
            ++count;
            std::ostringstream line;
            line << "[VR][sky] " << tag << " candidate=" << count
                 << " static=" << (method->static_function ? 1 : 0)
                 << " ret=" << TypeName(method->return_type) << " args=";
            for (std::size_t index = 0U; index < method->args.size(); ++index) {
                if (index != 0U) {
                    line << ',';
                }
                const auto* arg = method->args[index];
                line << (arg != nullptr ? TypeName(arg->pType) : "?");
            }
            line << " function=0x" << std::hex
                 << reinterpret_cast<std::uintptr_t>(method->function)
                 << " methodInfo=0x"
                 << reinterpret_cast<std::uintptr_t>(method->address)
                 << std::dec;
            Log(line.str());
        }
    }
    if (count == 0U) {
        Log(std::string("[VR][sky] ") + std::string(tag) + " candidate=0");
    }
}

UnityResolve::Method* FindExactVlTextureBlur(
    UnityResolve::Class* klass) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    constexpr std::array<std::string_view, 3> expectedArgs = {
        "UnityEngine.Rendering.CommandBuffer",
        "UnityEngine.Rendering.RTHandle",
        "UnityEngine.Rendering.RTHandle",
    };
    UnityResolve::Method* exact = nullptr;
    for (auto* candidate : klass->methods) {
        if (candidate == nullptr || candidate->name != "DoVLTextureBlur" ||
            candidate->static_function || candidate->function == nullptr ||
            candidate->address == nullptr || candidate->return_type == nullptr ||
            candidate->return_type->name != "System.Boolean" ||
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
}

UnityResolve::Method* FindExactVlDrawFlare(
    UnityResolve::Class* klass) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    constexpr std::array<std::string_view, 5> expectedArgs = {
        "UnityEngine.Rendering.CommandBuffer",
        "UnityEngine.Material",
        "VL.Rendering.FlareSetting",
        "System.Single",
        "System.Single",
    };
    UnityResolve::Method* exact = nullptr;
    for (auto* candidate : klass->methods) {
        if (candidate == nullptr || candidate->name != "DrawFlare" ||
            candidate->static_function || candidate->function == nullptr ||
            candidate->address == nullptr || candidate->return_type == nullptr ||
            candidate->return_type->name != "System.Boolean" ||
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
}

UnityResolve::Method* FindExactStaticVoid(
    UnityResolve::Class* klass,
    std::string_view name,
    std::initializer_list<std::string_view> expectedArgs) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    UnityResolve::Method* exact = nullptr;
    for (auto* candidate : klass->methods) {
        if (candidate == nullptr || candidate->name != name ||
            !candidate->static_function || candidate->function == nullptr ||
            candidate->address == nullptr || candidate->return_type == nullptr ||
            candidate->return_type->name != "System.Void" ||
            candidate->args.size() != expectedArgs.size()) {
            continue;
        }
        bool argsMatch = true;
        auto expected = expectedArgs.begin();
        for (std::size_t index = 0U; index < candidate->args.size();
             ++index, ++expected) {
            if (candidate->args[index] == nullptr ||
                candidate->args[index]->pType == nullptr ||
                std::string_view(candidate->args[index]->pType->name) !=
                    *expected) {
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
}

std::string CameraName(void* camera) noexcept {
    if (camera == nullptr) {
        return "-";
    }
    auto* typed = reinterpret_cast<UnityResolve::UnityType::Camera*>(camera);
    auto* gameObject = typed->GetGameObject();
    if (gameObject == nullptr) {
        return "-";
    }
    const std::string name = gameObject->GetName();
    return name.empty() ? "-" : name;
}

int InvokeInt(UnityResolve::Class* klass, void* instance, const char* name) noexcept {
    auto* method = FindNamed(klass, name);
    if (method == nullptr || instance == nullptr) {
        return 0;
    }
    return method->Invoke<int>(instance);
}

float InvokeFloat(UnityResolve::Class* klass, void* instance, const char* name) noexcept {
    auto* method = FindNamed(klass, name);
    if (method == nullptr || instance == nullptr) {
        return 0.0F;
    }
    return method->Invoke<float>(instance);
}

void* InvokePtr(UnityResolve::Class* klass, void* instance, const char* name) noexcept {
    auto* method = FindNamed(klass, name);
    if (method == nullptr) {
        return nullptr;
    }
    if (method->static_function) {
        return method->Invoke<void*>();
    }
    if (instance == nullptr) {
        return nullptr;
    }
    return method->Invoke<void*>(instance);
}

bool InvokeVec2Raw(
    void* function, void* self, void* argument, void* methodInfo,
    float* x, float* y) noexcept {
    using Fn = UnityResolve::UnityType::Vector2 (*)(void*, void*, void*);
    if (function == nullptr || x == nullptr || y == nullptr) {
        return false;
    }
    __try {
        const auto size = reinterpret_cast<Fn>(function)(self, argument, methodInfo);
        *x = size.x;
        *y = size.y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InvokeVec2IntRaw(
    void* function, void* self, void* argument, void* methodInfo,
    int* x, int* y) noexcept {
    using Fn = UnityResolve::UnityType::Vector2Int (*)(void*, void*, void*);
    if (function == nullptr || x == nullptr || y == nullptr) {
        return false;
    }
    __try {
        const auto size = reinterpret_cast<Fn>(function)(self, argument, methodInfo);
        *x = size.m_X;
        *y = size.m_Y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InvokeVec4Raw(
    void* function, void* self, void* argument, void* methodInfo,
    float* x, float* y) noexcept {
    using Fn = UnityResolve::UnityType::Vector4 (*)(void*, void*, void*);
    if (function == nullptr || x == nullptr || y == nullptr) {
        return false;
    }
    __try {
        const auto size = reinterpret_cast<Fn>(function)(self, argument, methodInfo);
        *x = size.x;
        *y = size.y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void DescribeScaledSize(
    UnityResolve::Class* skyPass,
    std::ostringstream& stream) noexcept {
    auto* method = FindNamed(skyPass, "GetScaledScreenSize");
    if (method == nullptr) {
        stream << " scaled=?";
        return;
    }
    stream << " scaled=? static=" << (method->static_function ? "1" : "0")
           << " camDataOff=" << g_cameraDataOffset
           << " ret=" << TypeName(method->return_type);
}

void* AddBytes(void* base, std::int32_t offset) noexcept {
    if (base == nullptr || offset < 0) {
        return nullptr;
    }
    return reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(base) +
        static_cast<std::uintptr_t>(offset));
}

CameraDataFields Unboxed(const CameraDataFields& src) noexcept {
    CameraDataFields out = src;
    auto shift = [](std::int32_t value) noexcept {
        return value >= 16 ? static_cast<std::int32_t>(value - 16) : value;
    };
    out.view = shift(src.view);
    out.proj = shift(src.proj);
    out.jitter = shift(src.jitter);
    out.camera = shift(src.camera);
    out.targetTex = shift(src.targetTex);
    out.pixelRect = shift(src.pixelRect);
    out.useOverride = shift(src.useOverride);
    out.screenSize = shift(src.screenSize);
    out.screenBias = shift(src.screenBias);
    out.xrRendering = src.xrRendering >= 16 ? src.xrRendering - 16 : src.xrRendering;
    out.pixelW = shift(src.pixelW);
    out.pixelH = shift(src.pixelH);
    out.aspect = shift(src.aspect);
    out.scale = shift(src.scale);
    out.antialiasing = shift(src.antialiasing);
    out.resetHistory = shift(src.resetHistory);
    out.taaPersistent = shift(src.taaPersistent);
    out.motionPersistent = shift(src.motionPersistent);
    out.taaSettings = shift(src.taaSettings);
    return out;
}

void AppendCsv16(std::ostringstream& stream, const float matrix[16]) noexcept {
    for (int index = 0; index < 16; ++index) {
        if (index != 0) {
            stream << ",";
        }
        stream << matrix[index];
    }
}

void RecoverViewPosition(const float view[16], float out[3]) noexcept {
    const float tx = view[12];
    const float ty = view[13];
    const float tz = view[14];
    out[0] = -(view[0] * tx + view[1] * ty + view[2] * tz);
    out[1] = -(view[4] * tx + view[5] * ty + view[6] * tz);
    out[2] = -(view[8] * tx + view[9] * ty + view[10] * tz);
}

bool MatricesClose(const float a[16], const float b[16], float eps) noexcept {
    for (int index = 0; index < 16; ++index) {
        if (std::fabs(a[index] - b[index]) > eps) {
            return false;
        }
    }
    return true;
}

bool FieldNameMatches(
    const char* name, std::initializer_list<const char*> names) noexcept {
    if (name == nullptr) {
        return false;
    }
    for (const char* want : names) {
        if (want != nullptr && std::strcmp(name, want) == 0) {
            return true;
        }
    }
    return false;
}

bool FieldTypeContains(
    const char* typeName, std::initializer_list<const char*> typeNames) noexcept {
    if (typeNames.size() == 0U) {
        return true;
    }
    if (typeName == nullptr) {
        return false;
    }
    const std::string_view have(typeName);
    for (const char* want : typeNames) {
        if (want != nullptr && have.find(want) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

std::int32_t FindFieldOffsetOnIl2CppClass(
    void* klassAddress,
    std::initializer_list<const char*> names,
    std::initializer_list<const char*> typeNames) noexcept {
    void* current = klassAddress;
    for (int depth = 0; current != nullptr && depth < 8; ++depth) {
        void* iter = nullptr;
        void* field = nullptr;
        while ((field = UnityResolve::Invoke<void*>(
                    "il2cpp_class_get_fields", current, &iter)) != nullptr) {
            const char* name = UnityResolve::Invoke<const char*>(
                "il2cpp_field_get_name", field);
            const int offset =
                UnityResolve::Invoke<int>("il2cpp_field_get_offset", field);
            if (!FieldNameMatches(name, names) || offset <= 0) {
                continue;
            }
            void* type = UnityResolve::Invoke<void*>(
                "il2cpp_field_get_type", field);
            const char* typeName = type != nullptr
                ? UnityResolve::Invoke<const char*>(
                      "il2cpp_type_get_name", type)
                : nullptr;
            if (FieldTypeContains(typeName, typeNames)) {
                return offset;
            }
        }
        current = UnityResolve::Invoke<void*>(
            "il2cpp_class_get_parent", current);
    }
    return -1;
}

void LogInheritedFields(void* klassAddress, const char* tag) noexcept {
    if (klassAddress == nullptr || tag == nullptr) {
        return;
    }
    void* current = klassAddress;
    for (int depth = 0; current != nullptr && depth < 8; ++depth) {
        const char* className = UnityResolve::Invoke<const char*>(
            "il2cpp_class_get_name", current);
        void* iter = nullptr;
        void* field = nullptr;
        std::uint32_t logged = 0;
        while ((field = UnityResolve::Invoke<void*>(
                    "il2cpp_class_get_fields", current, &iter)) != nullptr &&
               logged < 16U) {
            const char* name = UnityResolve::Invoke<const char*>(
                "il2cpp_field_get_name", field);
            const int offset =
                UnityResolve::Invoke<int>("il2cpp_field_get_offset", field);
            void* type = UnityResolve::Invoke<void*>(
                "il2cpp_field_get_type", field);
            const char* typeName = type != nullptr
                ? UnityResolve::Invoke<const char*>(
                      "il2cpp_type_get_name", type)
                : "?";
            ++logged;
            Log(std::string("[VR][sky] ") + tag +
                " class=" + (className != nullptr ? className : "?") +
                " name=" + (name != nullptr ? name : "?") +
                " type=" + (typeName != nullptr ? typeName : "?") +
                " offset=" + std::to_string(offset));
        }
        current = UnityResolve::Invoke<void*>(
            "il2cpp_class_get_parent", current);
    }
}

std::int32_t ResolveVolumeField(
    void* parameter,
    std::initializer_list<const char*> names,
    std::initializer_list<const char*> typeNames) noexcept {
    if (parameter == nullptr) {
        return -1;
    }
    void* klass = ReadPointer(parameter, 0);
    if (klass == nullptr) {
        return -1;
    }
    return FindFieldOffsetOnIl2CppClass(klass, names, typeNames);
}

void* VolumeValueObject(void* parameter) noexcept {
    const std::int32_t offset = ResolveVolumeField(
        parameter, {"m_Value", "value"}, {});
    return offset >= 0 ? ReadPointer(parameter, offset) : nullptr;
}

float VolumeValueFloat(void* parameter, bool* ok) noexcept {
    const std::int32_t offset = ResolveVolumeField(
        parameter, {"m_Value", "value"}, {"System.Single", "Single"});
    if (offset < 0) {
        if (ok != nullptr) {
            *ok = false;
        }
        return 0.0F;
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return ReadFloat(parameter, offset);
}

int VolumeValueBool(void* parameter) noexcept {
    const std::int32_t offset = ResolveVolumeField(
        parameter, {"m_Value", "value"}, {"System.Boolean", "Boolean"});
    return offset >= 0 ? (ReadInt(parameter, offset) & 0xFF) : -1;
}

int VolumeOverride(void* parameter) noexcept {
    const std::int32_t offset = ResolveVolumeField(
        parameter, {"m_OverrideState", "overrideState"},
        {"System.Boolean", "Boolean"});
    return offset >= 0 ? (ReadInt(parameter, offset) & 0xFF) : -1;
}

int VolumeComponentActive(void* component) noexcept {
    if (component == nullptr) {
        return -1;
    }
    const std::int32_t offset = FindFieldOffsetOnIl2CppClass(
        ReadPointer(component, 0), {"active", "m_Active"},
        {"System.Boolean", "Boolean"});
    return offset >= 0 ? (ReadInt(component, offset) & 0xFF) : -1;
}

bool VolumeValueFloats(
    void* parameter, float* out, std::size_t count) noexcept {
    if (parameter == nullptr || out == nullptr || count == 0U) {
        return false;
    }
    const std::int32_t offset = ResolveVolumeField(
        parameter, {"m_Value", "value"}, {});
    if (offset < 0) {
        return false;
    }
    __try {
        std::memcpy(out, AddBytes(parameter, offset), count * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void MulMat4(const float a[16], const float b[16], float c[16]) noexcept {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            c[col * 4 + row] = sum;
        }
    }
}

void Transpose4(const float in[16], float out[16]) noexcept {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            out[col * 4 + row] = in[row * 4 + col];
        }
    }
}

void MulMatVec4(const float m[16], const float v[4], float out[4]) noexcept {
    for (int row = 0; row < 4; ++row) {
        out[row] = m[0 * 4 + row] * v[0] + m[1 * 4 + row] * v[1] +
            m[2 * 4 + row] * v[2] + m[3 * 4 + row] * v[3];
    }
}

bool Invert4(const float m[16], float out[16]) noexcept {
    const float a00 = m[0], a01 = m[4], a02 = m[8], a03 = m[12];
    const float a10 = m[1], a11 = m[5], a12 = m[9], a13 = m[13];
    const float a20 = m[2], a21 = m[6], a22 = m[10], a23 = m[14];
    const float a30 = m[3], a31 = m[7], a32 = m[11], a33 = m[15];
    const float b00 = a00 * a11 - a01 * a10;
    const float b01 = a00 * a12 - a02 * a10;
    const float b02 = a00 * a13 - a03 * a10;
    const float b03 = a01 * a12 - a02 * a11;
    const float b04 = a01 * a13 - a03 * a11;
    const float b05 = a02 * a13 - a03 * a12;
    const float b06 = a20 * a31 - a21 * a30;
    const float b07 = a20 * a32 - a22 * a30;
    const float b08 = a20 * a33 - a23 * a30;
    const float b09 = a21 * a32 - a22 * a31;
    const float b10 = a21 * a33 - a23 * a31;
    const float b11 = a22 * a33 - a23 * a32;
    const float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 -
        b04 * b07 + b05 * b06;
    if (!std::isfinite(det) || std::fabs(det) < 1.0e-12F) {
        return false;
    }
    const float invDet = 1.0F / det;
    out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * invDet;
    out[1] = (-a10 * b11 + a12 * b08 - a13 * b07) * invDet;
    out[2] = (a10 * b10 - a11 * b08 + a13 * b06) * invDet;
    out[3] = (-a10 * b09 + a11 * b07 - a12 * b06) * invDet;
    out[4] = (-a01 * b11 + a02 * b10 - a03 * b09) * invDet;
    out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * invDet;
    out[6] = (-a00 * b10 + a01 * b08 - a03 * b06) * invDet;
    out[7] = (a00 * b09 - a01 * b07 + a02 * b06) * invDet;
    out[8] = (a31 * b05 - a32 * b04 + a33 * b03) * invDet;
    out[9] = (-a30 * b05 + a32 * b02 - a33 * b01) * invDet;
    out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * invDet;
    out[11] = (-a30 * b03 + a31 * b01 - a32 * b00) * invDet;
    out[12] = (-a21 * b05 + a22 * b04 - a23 * b03) * invDet;
    out[13] = (a20 * b05 - a22 * b02 + a23 * b01) * invDet;
    out[14] = (-a20 * b04 + a21 * b02 - a23 * b00) * invDet;
    out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * invDet;
    return true;
}

void Normalize3(float v[3]) noexcept {
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1.0e-8F) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

void RotateInverseView(const float view[16], const float dir[3], float out[3]) noexcept {
    out[0] = view[0] * dir[0] + view[1] * dir[1] + view[2] * dir[2];
    out[1] = view[4] * dir[0] + view[5] * dir[1] + view[6] * dir[2];
    out[2] = view[8] * dir[0] + view[9] * dir[1] + view[10] * dir[2];
}

void BuildHdrpViewRayMatrix(
    float verticalFovRadians,
    float lensShiftX,
    float lensShiftY,
    float width,
    float height,
    float aspect,
    const float worldToView[16],
    bool flipY,
    float out[16]) noexcept {
    const float invW = width > 0.0F ? 1.0F / width : 0.0F;
    const float invH = height > 0.0F ? 1.0F / height : 0.0F;
    const float usedAspect = aspect > 0.0F ? aspect : (invH > 0.0F ? width * invH : 1.0F);
    const float tanHalfVert = std::tan(0.5F * verticalFovRadians);
    float m21 = (1.0F - 2.0F * lensShiftY) * tanHalfVert;
    float m11 = -2.0F * invH * tanHalfVert;
    const float m20 = (1.0F - 2.0F * lensShiftX) * tanHalfVert * usedAspect;
    const float m00 = -2.0F * invW * tanHalfVert * usedAspect;
    if (flipY) {
        m11 = -m11;
        m21 = -m21;
    }
    float raster[16] = {
        m00, 0.0F, 0.0F, 0.0F,
        0.0F, m11, 0.0F, 0.0F,
        m20, m21, -1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
    };
    float view[16];
    for (int index = 0; index < 16; ++index) {
        view[index] = worldToView[index];
    }
    view[12] = 0.0F;
    view[13] = 0.0F;
    view[14] = 0.0F;
    view[15] = 1.0F;
    view[2] = -view[2];
    view[6] = -view[6];
    view[10] = -view[10];
    view[14] = -view[14];
    float viewT[16];
    float mul[16];
    Transpose4(view, viewT);
    MulMat4(viewT, raster, mul);
    Transpose4(mul, out);
}

void EvalHdrpRay(
    const float matrix[16], float pixelX, float pixelY, float out[3]) noexcept {
    out[0] = pixelX * matrix[0] + pixelY * matrix[1] + matrix[2];
    out[1] = pixelX * matrix[4] + pixelY * matrix[5] + matrix[6];
    out[2] = pixelX * matrix[8] + pixelY * matrix[9] + matrix[10];
    Normalize3(out);
}

bool UnprojectViewRay(
    const float view[16],
    const float proj[16],
    float ndcX,
    float ndcY,
    float clipZ,
    float out[3]) noexcept {
    float invP[16];
    if (!Invert4(proj, invP)) {
        return false;
    }
    const float clip[4] = {ndcX, ndcY, clipZ, 1.0F};
    float viewH[4];
    MulMatVec4(invP, clip, viewH);
    if (std::fabs(viewH[3]) > 1.0e-8F) {
        viewH[0] /= viewH[3];
        viewH[1] /= viewH[3];
        viewH[2] /= viewH[3];
    }
    const float viewDir[3] = {viewH[0], viewH[1], viewH[2]};
    RotateInverseView(view, viewDir, out);
    Normalize3(out);
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

float AngleDeg(const float a[3], const float b[3]) noexcept {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (dot > 1.0F) {
        dot = 1.0F;
    }
    if (dot < -1.0F) {
        dot = -1.0F;
    }
    return std::acos(dot) * (180.0F / 3.14159265358979323846F);
}

void AppendDir(std::ostringstream& stream, const char* tag, const float dir[3]) noexcept {
    stream << " " << tag << "=" << dir[0] << "," << dir[1] << "," << dir[2];
}

void LogReconstructedViewRays(
    const char* role,
    float fovDegrees,
    float cameraAspect,
    int pixW,
    int pixH,
    float dataAspect,
    const float view[16],
    const float proj[16]) noexcept {
    if (pixW <= 0 || pixH <= 0) {
        return;
    }
    constexpr float kPi = 3.14159265358979323846F;
    const float width = static_cast<float>(pixW);
    const float height = static_cast<float>(pixH);
    const float p00 = proj[0];
    const float p11 = proj[5];
    const float p02 = proj[8];
    const float p12 = proj[9];
    const float fovFromP11 = p11 > 1.0e-6F
        ? (2.0F * std::atan(1.0F / p11) * (180.0F / kPi))
        : 0.0F;
    const float aspectFromP = (p00 > 1.0e-6F && p11 > 1.0e-6F) ? (p11 / p00) : 0.0F;
    std::ostringstream head;
    head << "[VR][sky] VIEW_RAY role=" << (role != nullptr ? role : "?")
         << " pix=" << pixW << "x" << pixH
         << " fovCam=" << fovDegrees << " fovP11=" << fovFromP11
         << " aspectCam=" << cameraAspect << " aspectData=" << dataAspect
         << " aspectP=" << aspectFromP
         << " p02=" << p02 << " p12=" << p12
         << " expectScaled=" << width << "," << height << ","
         << (1.0F / width) << "," << (1.0F / height);
    Log(head.str());

    const float midX = 0.5F * width;
    const float midY = 0.5F * height;
    const struct {
        const char* name;
        float x;
        float y;
    } pixels[] = {
        {"c00", 0.0F, 0.0F},
        {"c10", width, 0.0F},
        {"c01", 0.0F, height},
        {"c11", width, height},
        {"mid", midX, midY},
    };

    auto logInvp = [&](const char* tag, float clipZ, bool flipY) noexcept {
        std::ostringstream stream;
        stream << "[VR][sky] VIEW_RAY role=" << (role != nullptr ? role : "?")
               << " src=" << tag;
        bool any = false;
        float mid[3]{};
        for (const auto& pixel : pixels) {
            float ndcX = 2.0F * ((pixel.x + 0.5F) / width) - 1.0F;
            float ndcY = 2.0F * ((pixel.y + 0.5F) / height) - 1.0F;
            if (flipY) {
                ndcY = -ndcY;
            }
            float dir[3]{};
            if (!UnprojectViewRay(view, proj, ndcX, ndcY, clipZ, dir)) {
                continue;
            }
            any = true;
            AppendDir(stream, pixel.name, dir);
            if (std::string_view(pixel.name) == "mid") {
                mid[0] = dir[0];
                mid[1] = dir[1];
                mid[2] = dir[2];
            }
        }
        if (any) {
            Log(stream.str());
        }
        return std::array<float, 3>{mid[0], mid[1], mid[2]};
    };

    const auto invp = logInvp("invp", 1.0F, false);
    logInvp("invpY", 1.0F, true);

    auto logFov = [&](const char* tag, float fovRad, float aspect, float shiftX,
                      float shiftY, bool flipY) noexcept {
        float matrix[16]{};
        BuildHdrpViewRayMatrix(
            fovRad, shiftX, shiftY, width, height, aspect, view, flipY, matrix);
        std::ostringstream stream;
        stream << "[VR][sky] VIEW_RAY role=" << (role != nullptr ? role : "?")
               << " src=" << tag;
        float mid[3]{};
        for (const auto& pixel : pixels) {
            float dir[3]{};
            EvalHdrpRay(matrix, pixel.x, pixel.y, dir);
            dir[0] = -dir[0];
            dir[1] = -dir[1];
            dir[2] = -dir[2];
            AppendDir(stream, pixel.name, dir);
            if (std::string_view(pixel.name) == "mid") {
                mid[0] = dir[0];
                mid[1] = dir[1];
                mid[2] = dir[2];
            }
        }
        if (std::isfinite(invp[0])) {
            const float midArr[3] = {mid[0], mid[1], mid[2]};
            const float invpArr[3] = {invp[0], invp[1], invp[2]};
            stream << " dMidInvp=" << AngleDeg(midArr, invpArr);
        }
        Log(stream.str());
    };

    const float fovRad = fovDegrees * (kPi / 180.0F);
    logFov("fovCam", fovRad, cameraAspect, 0.0F, 0.0F, false);
    logFov("fovPix", fovRad, dataAspect, 0.0F, 0.0F, false);
    logFov("fovP02", fovRad, cameraAspect, 0.5F * p02, 0.5F * p12, false);
    logFov("fovP02f", fovRad, cameraAspect, p02, p12, false);
}

void LogCameraDataLine(
    const char* role,
    void* cameraData,
    const CameraDataFields& fields,
    void* currentCamera,
    void* targetTexture,
    int expectW,
    int expectH,
    float fovDegrees,
    float cameraAspect) noexcept {
    if (cameraData == nullptr) {
        return;
    }
    void* dataCamera = ReadPointer(cameraData, fields.camera);
    void* dataTarget = ReadPointer(cameraData, fields.targetTex);
    float view[16]{};
    float proj[16]{};
    float jitter[16]{};
    const bool haveView = ReadMatrix(cameraData, fields.view, view);
    const bool haveProj = ReadMatrix(cameraData, fields.proj, proj);
    const bool haveJitter = ReadMatrix(cameraData, fields.jitter, jitter);
    const int pixW = ReadInt(cameraData, fields.pixelW);
    const int pixH = ReadInt(cameraData, fields.pixelH);
    std::ostringstream stream;
    stream << "[VR][sky] CAMERA_DATA role=" << (role != nullptr ? role : "?")
           << " layout=unbox"
           << " camMatch="
           << (currentCamera != nullptr && dataCamera == currentCamera ? "1"
                                                                      : "0")
           << " tgtMatch="
           << (targetTexture != nullptr && dataTarget == targetTexture ? "1"
                                                                      : "0")
           << " pix=" << pixW << "x" << pixH
           << " pixMatch="
           << (expectW > 0 && pixW == expectW && pixH == expectH ? "1" : "0")
           << " aspect=" << ReadFloat(cameraData, fields.aspect)
           << " scale=" << ReadFloat(cameraData, fields.scale)
           << " ov=" << (ReadInt(cameraData, fields.useOverride) & 0xFF)
           << " xr="
           << (fields.xrRendering >= 0
                   ? (ReadInt(cameraData, fields.xrRendering) & 0xFF)
                   : -1)
           << " rect=" << ReadFloat(cameraData, fields.pixelRect) << ","
           << ReadFloat(cameraData, fields.pixelRect + 4) << ","
           << ReadFloat(cameraData, fields.pixelRect + 8) << ","
           << ReadFloat(cameraData, fields.pixelRect + 12)
           << " screen=" << ReadFloat(cameraData, fields.screenSize) << ","
           << ReadFloat(cameraData, fields.screenSize + 4) << ","
           << ReadFloat(cameraData, fields.screenSize + 8) << ","
           << ReadFloat(cameraData, fields.screenSize + 12)
           << " bias=" << ReadFloat(cameraData, fields.screenBias) << ","
           << ReadFloat(cameraData, fields.screenBias + 4) << ","
           << ReadFloat(cameraData, fields.screenBias + 8) << ","
           << ReadFloat(cameraData, fields.screenBias + 12);
    if (haveProj) {
        stream << " p00=" << proj[0] << " p02=" << proj[8]
               << " p11=" << proj[5] << " p12=" << proj[9]
               << " p22=" << proj[10] << " p23=" << proj[11];
    }
    if (haveView) {
        float fromView[3]{};
        RecoverViewPosition(view, fromView);
        stream << " v00=" << view[0] << " v03=" << view[12]
               << " v13=" << view[13] << " v23=" << view[14]
               << " v11=" << view[5] << " v22=" << view[10]
               << " fromView=" << fromView[0] << "," << fromView[1] << ","
               << fromView[2];
    }
    if (haveJitter) {
        stream << " j00=" << jitter[0] << " j02=" << jitter[8]
               << " j11=" << jitter[5];
    }
    if (fields.antialiasing >= 0) {
        stream << " camDataAA=" << ReadInt(cameraData, fields.antialiasing);
    }
    if (fields.resetHistory >= 0) {
        stream << " camDataResetHistory="
               << (ReadInt(cameraData, fields.resetHistory) & 0xFF);
    }
    if (fields.taaPersistent >= 0) {
        stream << " camDataTaa=0x" << std::hex
               << reinterpret_cast<std::uintptr_t>(
                      ReadPointer(cameraData, fields.taaPersistent))
               << std::dec;
    }
    if (fields.motionPersistent >= 0) {
        stream << " camDataMotion=0x" << std::hex
               << reinterpret_cast<std::uintptr_t>(
                      ReadPointer(cameraData, fields.motionPersistent))
               << std::dec;
    }
    if (fields.taaSettings >= 0) {
        stream << " camDataTaaQ=" << ReadInt(cameraData, fields.taaSettings)
               << " camDataTaaJS="
               << ReadFloat(cameraData, fields.taaSettings + 8)
               << " camDataTaaResetFrames="
               << ReadInt(cameraData, fields.taaSettings + 24)
               << " camDataTaaJitterOff="
               << ReadInt(cameraData, fields.taaSettings + 28);
    }
    Log(stream.str());
    if (haveView) {
        std::ostringstream viewLine;
        viewLine << "[VR][sky] CAMERA_VIEW role="
                 << (role != nullptr ? role : "?") << " m=";
        AppendCsv16(viewLine, view);
        Log(viewLine.str());
    }
    if (haveProj) {
        std::ostringstream projLine;
        projLine << "[VR][sky] CAMERA_PROJ role="
                 << (role != nullptr ? role : "?") << " m=";
        AppendCsv16(projLine, proj);
        Log(projLine.str());
    }
    if (haveJitter) {
        std::ostringstream jitterLine;
        jitterLine << "[VR][sky] CAMERA_JITTER role="
                   << (role != nullptr ? role : "?") << " m=";
        AppendCsv16(jitterLine, jitter);
        Log(jitterLine.str());
    }
    if (haveView && haveProj) {
        LogReconstructedViewRays(
            role, fovDegrees, cameraAspect, pixW, pixH,
            ReadFloat(cameraData, fields.aspect), view, proj);
    }
    const auto roleName = std::string_view(role != nullptr ? role : "");
    if (roleName == "source" && haveView && haveProj) {
        for (int index = 0; index < 16; ++index) {
            g_sourceView[index] = view[index];
            g_sourceProj[index] = proj[index];
        }
        g_haveSourceMatrices = true;
    } else if (g_haveSourceMatrices && haveView && haveProj &&
        (roleName == "left" || roleName == "right")) {
        std::ostringstream diff;
        diff << "[VR][sky] CAMERA_DIFF role=" << role
             << " viewEq="
             << (MatricesClose(view, g_sourceView, 0.0001F) ? "1" : "0")
             << " projEq="
             << (MatricesClose(proj, g_sourceProj, 0.0001F) ? "1" : "0")
             << " dv03=" << (view[12] - g_sourceView[12])
             << " dv13=" << (view[13] - g_sourceView[13])
             << " dv23=" << (view[14] - g_sourceView[14])
             << " dp02=" << (proj[8] - g_sourceProj[8])
             << " dp12=" << (proj[9] - g_sourceProj[9]);
        Log(diff.str());
    }
}

// .39 proved dump offsets include a 16-byte boxed header. .40 reads only
// the unboxed layout and logs XR offset / view translation over time.
void LogEmbeddedCameraData(
    void* renderingData,
    void* currentCamera,
    void* targetTexture,
    const char* role,
    int expectW,
    int expectH,
    float fovDegrees,
    float cameraAspect) noexcept {
    if (renderingData == nullptr || g_cameraDataOffset < 16) {
        Log("[VR][sky] CAMERA_DATA missing");
        return;
    }
    const CameraDataFields unboxed = Unboxed(g_fields);
    const int unboxBase = g_cameraDataOffset - 16;
    LogCameraDataLine(
        role, AddBytes(renderingData, unboxBase), unboxed, currentCamera,
        targetTexture, expectW, expectH, fovDegrees, cameraAspect);
}

void* ReadPointer(void* instance, std::int32_t offset) noexcept {
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

float ReadFloat(void* instance, std::int32_t offset) noexcept {
    if (instance == nullptr || offset < 0) {
        return 0.0F;
    }
    float value = 0.0F;
    __try {
        value = *reinterpret_cast<float*>(
            reinterpret_cast<std::uintptr_t>(instance) +
            static_cast<std::uintptr_t>(offset));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0F;
    }
    return value;
}

int ReadInt(void* instance, std::int32_t offset) noexcept {
    if (instance == nullptr || offset < 0) {
        return 0;
    }
    int value = 0;
    __try {
        value = *reinterpret_cast<int*>(
            reinterpret_cast<std::uintptr_t>(instance) +
            static_cast<std::uintptr_t>(offset));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return value;
}

bool ReadMatrix(void* instance, std::int32_t offset, float out[16]) noexcept {
    if (instance == nullptr || offset < 0 || out == nullptr) {
        return false;
    }
    __try {
        const auto* src = reinterpret_cast<const float*>(
            reinterpret_cast<std::uintptr_t>(instance) +
            static_cast<std::uintptr_t>(offset));
        for (int index = 0; index < 16; ++index) {
            out[index] = src[index];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyMatrix(
    const UnityResolve::UnityType::Matrix4x4& matrix, float out[16]) noexcept {
    if (out == nullptr) {
        return false;
    }
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            out[row * 4 + column] = matrix.m[row][column];
        }
    }
    return true;
}

bool GetMatrixInjectedRaw(
    void* function, void* object, int nameId, void* methodInfo,
    float out[16]) noexcept {
    using Fn4 = void (*)(
        void*, int, UnityResolve::UnityType::Matrix4x4*, void*);
    using Fn3 = void (*)(void*, int, UnityResolve::UnityType::Matrix4x4*);
    if (function == nullptr || object == nullptr || out == nullptr) {
        return false;
    }
    UnityResolve::UnityType::Matrix4x4 matrix{};
    bool ok = false;
    if (methodInfo != nullptr) {
        __try {
            reinterpret_cast<Fn4>(function)(object, nameId, &matrix, methodInfo);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (!ok) {
        __try {
            reinterpret_cast<Fn3>(function)(object, nameId, &matrix);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    return CopyMatrix(matrix, out);
}

bool GetStaticMatrixInjectedRaw(
    void* function, int nameId, void* methodInfo, float out[16]) noexcept {
    using Fn = void (*)(int, UnityResolve::UnityType::Matrix4x4*, void*);
    using FnNoInfo = void (*)(int, UnityResolve::UnityType::Matrix4x4*);
    if (function == nullptr || out == nullptr) {
        return false;
    }
    UnityResolve::UnityType::Matrix4x4 matrix{};
    bool ok = false;
    if (methodInfo != nullptr) {
        __try {
            reinterpret_cast<Fn>(function)(nameId, &matrix, methodInfo);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (!ok) {
        __try {
            reinterpret_cast<FnNoInfo>(function)(nameId, &matrix);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    return CopyMatrix(matrix, out);
}

bool GetCameraMatrixInjectedRaw(
    void* function, void* camera, void* methodInfo, float out[16]) noexcept {
    using Fn = void (*)(void*, UnityResolve::UnityType::Matrix4x4*, void*);
    using FnNoInfo = void (*)(void*, UnityResolve::UnityType::Matrix4x4*);
    if (function == nullptr || camera == nullptr || out == nullptr) {
        return false;
    }
    UnityResolve::UnityType::Matrix4x4 matrix{};
    bool ok = false;
    if (methodInfo != nullptr) {
        __try {
            reinterpret_cast<Fn>(function)(camera, &matrix, methodInfo);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (!ok) {
        __try {
            reinterpret_cast<FnNoInfo>(function)(camera, &matrix);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    return CopyMatrix(matrix, out);
}

bool TryGetNamedMatrix(
    UnityResolve::Class* klass, void* object, int nameId, float out[16]) noexcept {
    if (klass == nullptr || object == nullptr || nameId == 0 || out == nullptr) {
        return false;
    }
    auto* injected = FindNamed(klass, "GetMatrix_Injected");
    if (injected != nullptr &&
        GetMatrixInjectedRaw(
            injected->function, object, nameId, injected->address, out)) {
        return true;
    }
    auto* getter = FindNamed(klass, "GetMatrix");
    if (getter != nullptr && getter->args.size() == 1U &&
        getter->function != nullptr) {
        return GetMatrixInjectedRaw(
            getter->function, object, nameId, getter->address, out);
    }
    return false;
}

bool TryGetGlobalMatrix(int nameId, float out[16]) noexcept {
    auto* klass = FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Shader");
    if (klass == nullptr || nameId == 0 || out == nullptr) {
        return false;
    }
    auto* injected = FindNamed(klass, "GetGlobalMatrix_Injected");
    if (injected != nullptr &&
        GetStaticMatrixInjectedRaw(
            injected->function, nameId, injected->address, out)) {
        return true;
    }
    auto* getter = FindNamed(klass, "GetGlobalMatrix");
    if (getter != nullptr && getter->static_function &&
        getter->args.size() == 1U && getter->function != nullptr) {
        return GetStaticMatrixInjectedRaw(
            getter->function, nameId, getter->address, out);
    }
    return false;
}

void LogMatrix(const char* tag, const char* source, const float matrix[16]) noexcept {
    std::ostringstream stream;
    stream << "[VR][sky] " << tag << " src=" << source << " m00=" << matrix[0]
           << " m11=" << matrix[5] << " m02=" << matrix[2]
           << " m12=" << matrix[6] << " m20=" << matrix[8]
           << " m21=" << matrix[9] << " m22=" << matrix[10];
    Log(stream.str());
}

void LogCameraMatrices(void* camera, const char* role) noexcept {
    auto* cameraClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
    if (cameraClass == nullptr || camera == nullptr) {
        return;
    }
    const float fov = InvokeFloat(cameraClass, camera, "get_fieldOfView");
    float projection[16]{};
    float view[16]{};
    auto* proj = FindNamed(cameraClass, "get_projectionMatrix_Injected");
    auto* world = FindNamed(cameraClass, "get_worldToCameraMatrix_Injected");
    const bool haveProj = proj != nullptr &&
        GetCameraMatrixInjectedRaw(
            proj->function, camera, proj->address, projection);
    const bool haveView = world != nullptr &&
        GetCameraMatrixInjectedRaw(
            world->function, camera, world->address, view);
    std::ostringstream stream;
    stream << "[VR][sky] CAMERA_MAT role=" << role
           << " fov=" << fov << " proj=" << (haveProj ? "1" : "0")
           << " view=" << (haveView ? "1" : "0");
    if (haveProj) {
        stream << " p00=" << projection[0] << " p11=" << projection[5]
               << " p22=" << projection[10] << " p23=" << projection[11];
    }
    if (haveView) {
        stream << " v00=" << view[0] << " v11=" << view[5]
               << " v22=" << view[10];
    }
    Log(stream.str());
}

void LogReadableFields(
    UnityResolve::Class* klass, void* instance, const char* tag) noexcept {
    if (klass == nullptr || instance == nullptr || tag == nullptr) {
        return;
    }
    std::uint32_t logged = 0;
    for (auto* field : klass->fields) {
        if (field == nullptr || logged >= 16U) {
            continue;
        }
        const std::string type = TypeName(field->type);
        std::ostringstream stream;
        stream << "[VR][sky] " << tag << " name=" << field->name
               << " off=" << field->offset << " type=" << type;
        if (type.find("Single") != std::string::npos) {
            stream << " f=" << ReadFloat(instance, field->offset);
        } else if (type.find("Int32") != std::string::npos ||
            type.find("Boolean") != std::string::npos) {
            stream << " i=" << ReadInt(instance, field->offset);
        } else if (type.find("Vector3") != std::string::npos) {
            stream << " v=" << ReadFloat(instance, field->offset) << ","
                   << ReadFloat(instance, field->offset + 4) << ","
                   << ReadFloat(instance, field->offset + 8);
        } else if (type.find("Vector4") != std::string::npos ||
            type.find("Color") != std::string::npos) {
            stream << " v=" << ReadFloat(instance, field->offset) << ","
                   << ReadFloat(instance, field->offset + 4) << ","
                   << ReadFloat(instance, field->offset + 8) << ","
                   << ReadFloat(instance, field->offset + 12);
        } else if (type.find("Matrix") != std::string::npos) {
            float matrix[16]{};
            if (ReadMatrix(instance, field->offset, matrix)) {
                stream << " p00=" << matrix[0] << " p02=" << matrix[8]
                       << " p11=" << matrix[5];
            }
        } else {
            stream << " ptr="
                   << (ReadPointer(instance, field->offset) != nullptr ? "1"
                                                                      : "0");
        }
        Log(stream.str());
        ++logged;
    }
}

int EyeSlot(const char* role) noexcept {
    if (role == nullptr) {
        return -1;
    }
    if (std::string_view(role) == "left") {
        return 0;
    }
    if (std::string_view(role) == "right") {
        return 1;
    }
    return -1;
}

void CacheViewDirNameId(void* pass) noexcept {
    if (g_viewDirNameId != 0 || pass == nullptr) {
        return;
    }
    auto* klass = FindSkyPass();
    if (klass == nullptr) {
        return;
    }
    const auto* idField = klass->Get<UnityResolve::Field>("_PixelCoordToViewDirWS");
    if (idField == nullptr) {
        return;
    }
    if (TypeName(idField->type).find("Matrix") != std::string::npos) {
        return;
    }
    const int nameId = ReadInt(pass, idField->offset);
    if (nameId != 0) {
        g_viewDirNameId = nameId;
        Log("[VR][sky] VIEW_DIR_ID id=" + std::to_string(nameId));
    }
}

EyeViewStash* CurrentEyeStash() noexcept {
    if (g_renderer == nullptr) {
        return nullptr;
    }
    const int slot = EyeSlot(g_renderer->ClassifyCamera(g_renderer->CurrentCamera()));
    if (slot < 0 || !g_eyeStash[static_cast<std::size_t>(slot)].ready) {
        return nullptr;
    }
    return &g_eyeStash[static_cast<std::size_t>(slot)];
}

bool ShouldReplaceViewDir(int nameId) noexcept {
    if (g_writeFaulted || nameId == 0 || g_viewDirNameId == 0 ||
        nameId != g_viewDirNameId) {
        return false;
    }
    return CurrentEyeStash() != nullptr;
}

bool ShouldLogWrite(std::uint32_t& counter) noexcept {
    ++counter;
    return counter <= kBurstSamples || (counter % kLaterStride) == 0U;
}

void LogSetViewDir(
    const char* via, const char* dest, bool ok, const EyeViewStash& stash) noexcept {
    const char* role = g_renderer != nullptr
        ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
        : "?";
    std::ostringstream stream;
    stream << "[VR][sky] SET_VIEW_DIR role=" << (role != nullptr ? role : "?")
           << " via=" << (via != nullptr ? via : "?")
           << " dest=" << (dest != nullptr ? dest : "?")
           << " ok=" << (ok ? "1" : "0")
           << " fault=" << (g_writeFaulted ? "1" : "0")
           << " id=" << g_viewDirNameId
           << " p02=" << stash.p02 << " p12=" << stash.p12
           << " fov=" << stash.fov
           << " pix=" << stash.pixW << "x" << stash.pixH;
    Log(stream.str());
}

void StashEyeViewDir(void* pass, void* renderingData) noexcept {
    if (g_renderer == nullptr || renderingData == nullptr) {
        return;
    }
    ResolveSkyRuntimeDataLayout();
    DumpSkyPassApiOnce();
    CacheViewDirNameId(pass);
    void* camera = g_renderer->CurrentCamera();
    if (camera == nullptr) {
        auto* cameraClass =
            FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
        camera = InvokePtr(cameraClass, nullptr, "get_current");
    }
    const char* role = g_renderer->ClassifyCamera(camera);
    const int slot = EyeSlot(role);
    if (slot < 0 || g_cameraDataOffset < 16 || !g_fieldsReady) {
        return;
    }
    auto* cameraClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
    const float fov = InvokeFloat(cameraClass, camera, "get_fieldOfView");
    const float aspect = InvokeFloat(cameraClass, camera, "get_aspect");
    const CameraDataFields unboxed = Unboxed(g_fields);
    void* cameraData = AddBytes(renderingData, g_cameraDataOffset - 16);
    float view[16]{};
    float proj[16]{};
    if (!ReadMatrix(cameraData, unboxed.view, view) ||
        !ReadMatrix(cameraData, unboxed.proj, proj)) {
        return;
    }
    const int pixW = ReadInt(cameraData, unboxed.pixelW);
    const int pixH = ReadInt(cameraData, unboxed.pixelH);
    if (pixW < 512 || pixH < 512 || fov <= 1.0F) {
        return;
    }
    constexpr float kPi = 3.14159265358979323846F;
    auto& stash = g_eyeStash[static_cast<std::size_t>(slot)];
    BuildHdrpViewRayMatrix(
        fov * (kPi / 180.0F), 0.5F * proj[8], 0.5F * proj[9],
        static_cast<float>(pixW), static_cast<float>(pixH), aspect, view, false,
        stash.matrix);
    const float width = static_cast<float>(pixW);
    const float height = static_cast<float>(pixH);
    stash.midX = 0.5F * width;
    stash.midY = 0.5F * height;
    const float ndcX = 2.0F * ((stash.midX + 0.5F) / width) - 1.0F;
    const float ndcY = 2.0F * ((stash.midY + 0.5F) / height) - 1.0F;
    UnprojectViewRay(view, proj, ndcX, ndcY, 1.0F, stash.invpMid);
    float fovCamMat[16]{};
    BuildHdrpViewRayMatrix(
        fov * (kPi / 180.0F), 0.0F, 0.0F, width, height, aspect, view, false,
        fovCamMat);
    EvalHdrpRay(fovCamMat, stash.midX, stash.midY, stash.fovCamMid);
    stash.fovCamMid[0] = -stash.fovCamMid[0];
    stash.fovCamMid[1] = -stash.fovCamMid[1];
    stash.fovCamMid[2] = -stash.fovCamMid[2];
    EvalHdrpRay(stash.matrix, stash.midX, stash.midY, stash.fovP02Mid);
    stash.fovP02Mid[0] = -stash.fovP02Mid[0];
    stash.fovP02Mid[1] = -stash.fovP02Mid[1];
    stash.fovP02Mid[2] = -stash.fovP02Mid[2];
    stash.ready = true;
    stash.fov = fov;
    stash.p02 = proj[8];
    stash.p12 = proj[9];
    stash.pixW = pixW;
    stash.pixH = pixH;
    std::memcpy(stash.view, view, sizeof(stash.view));
    DumpSkyMaterial(pass);
}

void* MpbNativePtr(void* block) noexcept {
    if (block == nullptr) {
        return nullptr;
    }
    static std::int32_t offset = -1;
    if (offset < 0) {
        auto* klass = FindClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock");
        if (klass != nullptr) {
            for (auto* field : klass->fields) {
                if (field != nullptr &&
                    (field->name == "m_Ptr" || field->name == "m_NativePtr")) {
                    offset = field->offset;
                    break;
                }
            }
        }
        if (offset < 0) {
            offset = 16;
        }
    }
    return ReadPointer(block, offset);
}

UnityResolve::Method* FindSetMatrixInjected(const char* className) noexcept {
    auto* klass = FindClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", className);
    if (klass == nullptr) {
        return nullptr;
    }
    UnityResolve::Method* fallback = nullptr;
    for (auto* method : klass->methods) {
        if (method == nullptr || method->function == nullptr ||
            method->name != "SetMatrixImpl_Injected") {
            continue;
        }
        if (method->static_function && method->args.size() == 3U) {
            return method;
        }
        if (fallback == nullptr) {
            fallback = method;
        }
    }
    return fallback;
}

UnityResolve::Method* FindSetMatrixInjected() noexcept {
    return FindSetMatrixInjected("MaterialPropertyBlock");
}

bool CallSetMatrixInjected(
    void* function, void* object, int nameId, void* methodInfo,
    const float matrix[16]) noexcept {
    using Fn4 = void (*)(void*, int, const float*, void*);
    using Fn3 = void (*)(void*, int, const float*);
    if (function == nullptr || object == nullptr || matrix == nullptr ||
        g_writeFaulted) {
        return false;
    }
    float local[16]{};
    std::memcpy(local, matrix, sizeof(local));
    bool ok = false;
    __try {
        reinterpret_cast<Fn3>(function)(object, nameId, local);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (ok) {
        return true;
    }
    if (methodInfo != nullptr) {
        __try {
            reinterpret_cast<Fn4>(function)(object, nameId, local, methodInfo);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (!ok) {
        g_writeFaulted = true;
    }
    return ok;
}

std::string ObjectName(void* object) noexcept {
    if (object == nullptr) {
        return "-";
    }
    auto* typed = reinterpret_cast<UnityResolve::UnityType::UnityObject*>(object);
    const std::string name = typed->GetName();
    return name.empty() ? "-" : name;
}

int PassPropertyId(
    UnityResolve::Class* klass, void* pass, const char* name) noexcept {
    if (klass == nullptr || name == nullptr) {
        return 0;
    }
    const auto* field = klass->Get<UnityResolve::Field>(name);
    if (field == nullptr) {
        return 0;
    }
    if (field->offset >= 16 && pass != nullptr && !field->static_field) {
        return ReadInt(pass, field->offset);
    }
    int value = 0;
    if (field->address != nullptr) {
        UnityResolve::Invoke<void, void*, int*>(
            "il2cpp_field_static_get_value", field->address, &value);
    }
    return value;
}

bool CopyMatrixBytes(void* src, float out[16]) noexcept {
    if (src == nullptr || out == nullptr) {
        return false;
    }
    __try {
        std::memcpy(out, src, sizeof(float) * 16U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteMatrixBytes(const float src[16], void* dst) noexcept {
    if (src == nullptr || dst == nullptr) {
        return false;
    }
    __try {
        std::memcpy(dst, src, sizeof(float) * 16U);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* InvokePtrIntRaw(void* function, void* self, int argument) noexcept {
    using Fn = void* (*)(void*, int);
    if (function == nullptr || self == nullptr) {
        return nullptr;
    }
    void* value = nullptr;
    __try {
        value = reinterpret_cast<Fn>(function)(self, argument);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return value;
}

int InvokeIntIntRaw(void* function, void* self, int argument) noexcept {
    using Fn = int (*)(void*, int);
    if (function == nullptr || self == nullptr) {
        return 0;
    }
    int value = 0;
    __try {
        value = reinterpret_cast<Fn>(function)(self, argument);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return value;
}

void InvokeSetFloatRaw(
    void* function, void* self, int nameId, float value) noexcept {
    using Fn = void (*)(void*, int, float);
    if (function == nullptr || self == nullptr) {
        return;
    }
    __try {
        reinterpret_cast<Fn>(function)(self, nameId, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void InvokeSetIntRaw(void* function, void* self, int value) noexcept {
    using Fn = void (*)(void*, int);
    if (function == nullptr || self == nullptr) {
        return;
    }
    __try {
        reinterpret_cast<Fn>(function)(self, value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void InvokeSetColorRaw(void* function, void* self, float color[4]) noexcept {
    using Fn = void (*)(void*, float*);
    if (function == nullptr || self == nullptr || color == nullptr) {
        return;
    }
    __try {
        reinterpret_cast<Fn>(function)(self, color);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

UnityResolve::Method* FindSetFloatInt(UnityResolve::Class* klass) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method == nullptr || method->function == nullptr ||
            method->args.size() != 2U ||
            (method->name != "SetFloat" && method->name != "SetFloatImpl")) {
            continue;
        }
        const std::string a0 = TypeName(method->args[0] != nullptr
            ? method->args[0]->pType : nullptr);
        const std::string a1 = TypeName(method->args[1] != nullptr
            ? method->args[1]->pType : nullptr);
        if ((a0.find("Int32") != std::string::npos) &&
            (a1.find("Single") != std::string::npos)) {
            return method;
        }
    }
    return nullptr;
}

std::string UnityString(void* value) noexcept {
    if (value == nullptr) {
        return "-";
    }
    return reinterpret_cast<UnityResolve::UnityType::String*>(value)->ToString();
}

void LogAnySetMatrix(const char* via, int nameId, void* matrix) noexcept {
    if (g_anySetLogs < 24U) {
        ++g_anySetLogs;
        std::ostringstream stream;
        stream << "[VR][sky] SET_MATRIX_ID via=" << (via != nullptr ? via : "?")
               << " name=" << nameId
               << " viewDir="
               << (g_viewDirNameId != 0 && nameId == g_viewDirNameId ? "1"
                                                                    : "0");
        Log(stream.str());
    }
    LogIncomingViewDir(via, nameId, matrix);
}

void DumpSkyShaderProps(void* shader) noexcept {
    auto* shaderClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Shader");
    if (shaderClass == nullptr || shader == nullptr) {
        return;
    }
    const int count = InvokeInt(shaderClass, shader, "GetPropertyCount");
    auto* getName = FindNamedIntArg(shaderClass, "GetPropertyName");
    auto* getType = FindNamedIntArg(shaderClass, "GetPropertyType");
    Log("[VR][sky] SKY_PROP count=" + std::to_string(count) +
        " shader=" + ObjectName(shader));
    const int cap = count > 32 ? 32 : count;
    for (int index = 0; index < cap; ++index) {
        std::string name = "-";
        int type = -1;
        if (getName != nullptr && getName->function != nullptr) {
            name = UnityString(
                InvokePtrIntRaw(getName->function, shader, index));
        }
        if (getType != nullptr && getType->function != nullptr) {
            type = InvokeIntIntRaw(getType->function, shader, index);
        }
        Log("[VR][sky] SKY_PROP i=" + std::to_string(index) +
            " name=" + name + " type=" + std::to_string(type));
    }
}

float InvokeFloatIntRaw(void* function, void* self, int argument) noexcept {
    using Fn = float (*)(void*, int);
    if (function == nullptr || self == nullptr) {
        return 0.0F;
    }
    float value = 0.0F;
    __try {
        value = reinterpret_cast<Fn>(function)(self, argument);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0F;
    }
    return value;
}

void LogIncomingViewDir(const char* via, int nameId, void* matrix) noexcept {
    if (matrix == nullptr || nameId == 0 || g_viewDirNameId == 0 ||
        nameId != g_viewDirNameId) {
        return;
    }
    EyeViewStash* stash = CurrentEyeStash();
    if (stash == nullptr) {
        return;
    }
    const int slot = EyeSlot(
        g_renderer != nullptr
            ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
            : nullptr);
    if (slot < 0 ||
        !ShouldLogWrite(g_inLogs[static_cast<std::size_t>(slot)])) {
        return;
    }
    float incoming[16]{};
    if (!CopyMatrixBytes(matrix, incoming)) {
        return;
    }
    float mid[3]{};
    EvalHdrpRay(incoming, stash->midX, stash->midY, mid);
    mid[0] = -mid[0];
    mid[1] = -mid[1];
    mid[2] = -mid[2];
    std::ostringstream stream;
    stream << "[VR][sky] IN_VIEW_DIR role="
           << (g_renderer != nullptr
                   ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
                   : "?")
           << " via=" << (via != nullptr ? via : "?")
           << " eqStash="
           << (MatricesClose(incoming, stash->matrix, 0.01F) ? "1" : "0")
           << " dMidStash=" << AngleDeg(mid, stash->fovP02Mid)
           << " dMidInvp=" << AngleDeg(mid, stash->invpMid)
           << " dMidFovCam=" << AngleDeg(mid, stash->fovCamMid)
           << " mid=" << mid[0] << "," << mid[1] << "," << mid[2]
           << " write=" << (kWriteViewDir ? "1" : "0");
    Log(stream.str());
}

void DumpSkyMaterial(void* pass) noexcept {
    if (g_skyMatDumped || pass == nullptr || CurrentEyeStash() == nullptr) {
        return;
    }
    g_skyMatDumped = true;
    auto* klass = FindSkyPass();
    auto* matClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Material");
    auto* texClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Texture");
    const auto* skyMatField =
        klass != nullptr ? klass->Get<UnityResolve::Field>("_skyMaterial")
                         : nullptr;
    void* skyMat = skyMatField != nullptr
        ? ReadPointer(pass, skyMatField->offset)
        : nullptr;
    void* shader = InvokePtr(matClass, skyMat, "get_shader");
    const int mainTexId = PassPropertyId(klass, pass, "_MainTex");
    const int rotationId = PassPropertyId(klass, pass, "_Rotation");
    const int exposureId = PassPropertyId(klass, pass, "_Exposure");
    void* mainTex = nullptr;
    float rotation = 0.0F;
    float exposure = 0.0F;
    auto* getTex = FindNamedIntArg(matClass, "GetTexture");
    auto* getFloat = FindNamedIntArg(matClass, "GetFloat");
    if (getTex != nullptr && skyMat != nullptr && mainTexId != 0) {
        mainTex = InvokePtrIntRaw(getTex->function, skyMat, mainTexId);
    }
    if (getFloat != nullptr && skyMat != nullptr) {
        if (rotationId != 0) {
            rotation = InvokeFloatIntRaw(getFloat->function, skyMat, rotationId);
        }
        if (exposureId != 0) {
            exposure = InvokeFloatIntRaw(getFloat->function, skyMat, exposureId);
        }
    }
    std::ostringstream stream;
    stream << "[VR][sky] SKY_MAT mat=" << (skyMat != nullptr ? "1" : "0")
           << " shader=" << ObjectName(shader)
           << " tex=" << ObjectName(mainTex)
           << " texId=" << mainTexId
           << " dim=" << InvokeInt(texClass, mainTex, "get_dimension")
           << " w=" << InvokeInt(texClass, mainTex, "get_width")
           << " h=" << InvokeInt(texClass, mainTex, "get_height")
           << " rot=" << rotation << " rotId=" << rotationId
           << " exp=" << exposure;
    Log(stream.str());
    DumpSkyShaderProps(shader);
}

bool ApplyEyeSkyExposure(void* pass, bool dim, float* saved) noexcept {
    if (g_skyExpFaulted || pass == nullptr || CurrentEyeStash() == nullptr) {
        return false;
    }
    auto* klass = FindSkyPass();
    auto* matClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Material");
    const auto* skyMatField =
        klass != nullptr ? klass->Get<UnityResolve::Field>("_skyMaterial")
                         : nullptr;
    void* skyMat = skyMatField != nullptr
        ? ReadPointer(pass, skyMatField->offset)
        : nullptr;
    const int exposureId = PassPropertyId(klass, pass, "_Exposure");
    auto* getFloat = FindNamedIntArg(matClass, "GetFloat");
    auto* setFloat = FindSetFloatInt(matClass);
    if (skyMat == nullptr || exposureId == 0 || getFloat == nullptr ||
        getFloat->function == nullptr || setFloat == nullptr ||
        setFloat->function == nullptr) {
        return false;
    }
    if (dim) {
        const float before =
            InvokeFloatIntRaw(getFloat->function, skyMat, exposureId);
        if (saved != nullptr) {
            *saved = before;
        }
        InvokeSetFloatRaw(setFloat->function, skyMat, exposureId, 0.0F);
        const float after =
            InvokeFloatIntRaw(getFloat->function, skyMat, exposureId);
        if (after > 0.05F) {
            g_skyExpFaulted = true;
        }
        const int slot = EyeSlot(
            g_renderer != nullptr
                ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
                : nullptr);
        if (slot >= 0 &&
            ShouldLogWrite(g_expLogs[static_cast<std::size_t>(slot)])) {
            std::ostringstream stream;
            stream << "[VR][sky] SET_SKY_EXP role="
                   << (g_renderer != nullptr
                           ? g_renderer->ClassifyCamera(
                                 g_renderer->CurrentCamera())
                           : "?")
                   << " ok=" << (g_skyExpFaulted ? "0" : "1")
                   << " before=" << before << " after=" << after
                   << " id=" << exposureId;
            Log(stream.str());
        }
        return !g_skyExpFaulted;
    }
    if (saved != nullptr) {
        InvokeSetFloatRaw(setFloat->function, skyMat, exposureId, *saved);
    }
    return true;
}

bool SetViewDirOnBlock(void* block, int nameId, const float matrix[16]) noexcept {
    if (block == nullptr || nameId == 0 || matrix == nullptr || g_writeFaulted) {
        return false;
    }
    auto* injected = FindSetMatrixInjected();
    if (injected == nullptr || injected->function == nullptr) {
        return false;
    }
    void* native = MpbNativePtr(block);
    if (native != nullptr &&
        CallSetMatrixInjected(
            injected->function, native, nameId, injected->address, matrix)) {
        return true;
    }
    return CallSetMatrixInjected(
        injected->function, block, nameId, injected->address, matrix);
}

void WriteStashedViewDir(void* pass) noexcept {
    if (!kWriteViewDir || pass == nullptr || g_writeFaulted) {
        return;
    }
    CacheViewDirNameId(pass);
    EyeViewStash* stash = CurrentEyeStash();
    if (stash == nullptr || g_viewDirNameId == 0) {
        return;
    }
    auto* klass = FindSkyPass();
    if (klass == nullptr) {
        return;
    }
    const auto* cloudBlockField =
        klass->Get<UnityResolve::Field>("_cloudMaterialPropertyBlock");
    const auto* skyBlockField =
        klass->Get<UnityResolve::Field>("_materialPropertyBlock");
    const auto* skyMatField = klass->Get<UnityResolve::Field>("_skyMaterial");
    void* cloudBlock = cloudBlockField != nullptr
        ? ReadPointer(pass, cloudBlockField->offset)
        : nullptr;
    void* skyBlock = skyBlockField != nullptr
        ? ReadPointer(pass, skyBlockField->offset)
        : nullptr;
    void* skyMat = skyMatField != nullptr
        ? ReadPointer(pass, skyMatField->offset)
        : nullptr;
    const bool cloudOk =
        SetViewDirOnBlock(cloudBlock, g_viewDirNameId, stash->matrix);
    const bool skyOk = SetViewDirOnBlock(skyBlock, g_viewDirNameId, stash->matrix);
    bool matOk = false;
    auto* matInjected = FindSetMatrixInjected("Material");
    if (skyMat != nullptr && matInjected != nullptr &&
        matInjected->function != nullptr) {
        matOk = CallSetMatrixInjected(
            matInjected->function, skyMat, g_viewDirNameId,
            matInjected->address, stash->matrix);
    }
    const int slot = EyeSlot(
        g_renderer != nullptr
            ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
            : nullptr);
    if (slot >= 0 &&
        ShouldLogWrite(g_writeLogs[static_cast<std::size_t>(slot)])) {
        LogSetViewDir("draw", "cloud", cloudOk, *stash);
        LogSetViewDir("draw", "sky", skyOk, *stash);
        LogSetViewDir("draw", "mat", matOk, *stash);
    }
}

void SetMatrixDetour(void* native, int nameId, void* matrix, void* method) {
    LogAnySetMatrix("mpb", nameId, matrix);
    if (kWriteViewDir && matrix != nullptr && ShouldReplaceViewDir(nameId)) {
        EyeViewStash* stash = CurrentEyeStash();
        if (stash != nullptr) {
            __try {
                std::memcpy(matrix, stash->matrix, sizeof(stash->matrix));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                g_writeFaulted = true;
            }
            if (!g_writeFaulted) {
                const int slot = EyeSlot(
                    g_renderer != nullptr
                        ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
                        : nullptr);
                if (slot >= 0 &&
                    ShouldLogWrite(g_hookLogs[static_cast<std::size_t>(slot)])) {
                    LogSetViewDir("hook", "inplace", true, *stash);
                }
            }
        }
    }
    if (g_setMatrixOrig != nullptr) {
        g_setMatrixOrig(native, nameId, matrix, method);
    }
}

void MaterialSetMatrixDetour(void* native, int nameId, void* matrix, void* method) {
    LogAnySetMatrix("mat", nameId, matrix);
    if (g_matSetOrig != nullptr) {
        g_matSetOrig(native, nameId, matrix, method);
    }
}

void CmdSetMatrixDetour(void* native, int nameId, void* matrix, void* method) {
    LogAnySetMatrix("cmd", nameId, matrix);
    if (g_cmdSetOrig != nullptr) {
        g_cmdSetOrig(native, nameId, matrix, method);
    }
}

void GlobalSetMatrixDetour(int nameId, void* matrix, void* method) {
    LogAnySetMatrix("global", nameId, matrix);
    if (g_globalSetOrig != nullptr) {
        g_globalSetOrig(nameId, matrix, method);
    }
}

void InstallSetMatrixHook() noexcept {
    if (g_setMatrixHookInstalled) {
        return;
    }
    g_setMatrixHookInstalled = true;
    auto* injected = FindSetMatrixInjected();
    const bool ready = injected != nullptr && injected->function != nullptr &&
        injected->static_function && injected->args.size() == 3U;
    std::ostringstream stream;
    stream << "[VR][sky] SET_MATRIX_HOOK ready=" << (ready ? "1" : "0");
    if (injected != nullptr) {
        stream << " static=" << (injected->static_function ? "1" : "0")
               << " args=" << injected->args.size();
        for (std::size_t index = 0; index < injected->args.size(); ++index) {
            stream << (index == 0U ? " types=" : ",")
                   << (injected->args[index] != nullptr
                           ? TypeName(injected->args[index]->pType)
                           : "?");
        }
    }
    Log(stream.str());
    if (ready) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            injected->function, reinterpret_cast<void*>(&SetMatrixDetour),
            reinterpret_cast<void**>(&g_setMatrixOrig),
            "MaterialPropertyBlock.SetMatrixImpl_Injected");
        Log(std::string("[VR][sky] SET_MATRIX_HOOK installed=") +
            (installed ? "1" : "0"));
    }
    auto* matInjected = FindSetMatrixInjected("Material");
    const bool matReady = matInjected != nullptr &&
        matInjected->function != nullptr && matInjected->static_function &&
        matInjected->args.size() == 3U;
    Log(std::string("[VR][sky] SET_MATRIX_HOOK dest=mat ready=") +
        (matReady ? "1" : "0"));
    if (matReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            matInjected->function,
            reinterpret_cast<void*>(&MaterialSetMatrixDetour),
            reinterpret_cast<void**>(&g_matSetOrig),
            "Material.SetMatrixImpl_Injected");
        Log(std::string("[VR][sky] SET_MATRIX_HOOK dest=mat installed=") +
            (installed ? "1" : "0"));
    }
    auto* shaderClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Shader");
    UnityResolve::Method* global = nullptr;
    if (shaderClass != nullptr) {
        for (auto* method : shaderClass->methods) {
            if (method != nullptr && method->function != nullptr &&
                method->static_function &&
                method->name == "SetGlobalMatrix_Injected") {
                global = method;
                break;
            }
        }
    }
    const bool globalReady = global != nullptr && global->args.size() >= 2U;
    Log(std::string("[VR][sky] SET_MATRIX_HOOK dest=global ready=") +
        (globalReady ? "1" : "0"));
    if (globalReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            global->function, reinterpret_cast<void*>(&GlobalSetMatrixDetour),
            reinterpret_cast<void**>(&g_globalSetOrig),
            "Shader.SetGlobalMatrix_Injected");
        Log(std::string("[VR][sky] SET_MATRIX_HOOK dest=global installed=") +
            (installed ? "1" : "0"));
    }
    auto* cmdClass = FindClass(
        "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "CommandBuffer");
    UnityResolve::Method* cmdInjected = nullptr;
    if (cmdClass != nullptr) {
        for (auto* method : cmdClass->methods) {
            if (method != nullptr && method->function != nullptr &&
                method->name == "SetGlobalMatrix_Injected") {
                cmdInjected = method;
                break;
            }
        }
    }
    const bool cmdReady = cmdInjected != nullptr &&
        cmdInjected->args.size() >= 2U;
    Log(std::string("[VR][sky] SET_MATRIX_HOOK dest=cmd ready=") +
        (cmdReady ? "1" : "0") +
        (cmdInjected != nullptr
             ? " args=" + std::to_string(cmdInjected->args.size())
             : ""));
    if (cmdReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            cmdInjected->function, reinterpret_cast<void*>(&CmdSetMatrixDetour),
            reinterpret_cast<void**>(&g_cmdSetOrig),
            "CommandBuffer.SetGlobalMatrix_Injected");
        Log(std::string("[VR][sky] SET_MATRIX_HOOK dest=cmd installed=") +
            (installed ? "1" : "0"));
    }
}

void LogViewDirMatrix(void* pass) noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    auto* klass = FindSkyPass();
    if (klass == nullptr || pass == nullptr) {
        return;
    }
    const auto* idField = klass->Get<UnityResolve::Field>("_PixelCoordToViewDirWS");
    const auto* cloudBlockField =
        klass->Get<UnityResolve::Field>("_cloudMaterialPropertyBlock");
    const auto* skyBlockField =
        klass->Get<UnityResolve::Field>("_materialPropertyBlock");
    const auto* skyMatField = klass->Get<UnityResolve::Field>("_skyMaterial");
    if (idField == nullptr) {
        Log("[VR][sky] VIEW_DIR missing idField");
        return;
    }
    const std::string idType = TypeName(idField->type);
    if (idType.find("Matrix") != std::string::npos) {
        float matrix[16]{};
        if (ReadMatrix(pass, idField->offset, matrix)) {
            LogMatrix("VIEW_DIR", "field", matrix);
        }
        return;
    }
    const int nameId = ReadInt(pass, idField->offset);
    void* cloudBlock = cloudBlockField != nullptr
        ? ReadPointer(pass, cloudBlockField->offset)
        : nullptr;
    void* skyBlock = skyBlockField != nullptr
        ? ReadPointer(pass, skyBlockField->offset)
        : nullptr;
    void* skyMat = skyMatField != nullptr
        ? ReadPointer(pass, skyMatField->offset)
        : nullptr;
    void* cloudNative = cloudBlock != nullptr ? ReadPointer(cloudBlock, 16) : nullptr;
    void* skyNative = skyBlock != nullptr ? ReadPointer(skyBlock, 16) : nullptr;
    Log("[VR][sky] VIEW_DIR id=" + std::to_string(nameId) +
        " cloudBlock=" + (cloudBlock != nullptr ? "1" : "0") +
        " skyBlock=" + (skyBlock != nullptr ? "1" : "0") +
        " skyMat=" + (skyMat != nullptr ? "1" : "0") +
        " cloudNative=" + (cloudNative != nullptr ? "1" : "0") +
        " skyNative=" + (skyNative != nullptr ? "1" : "0") +
        " note=mpb-native-only");
}

void DumpFilteredFields(
    UnityResolve::Class* klass, const char* tag) noexcept {
    if (klass == nullptr || tag == nullptr) {
        return;
    }
    for (auto* field : klass->fields) {
        if (field == nullptr) {
            continue;
        }
        const auto& name = field->name;
        if (name.find("taa") == std::string::npos &&
            name.find("Taa") == std::string::npos &&
            name.find("TAA") == std::string::npos &&
            name.find("resetHistory") == std::string::npos &&
            name.find("antialiasing") == std::string::npos &&
            name.find("motion") == std::string::npos &&
            name.find("Motion") == std::string::npos &&
            name.find("jitter") == std::string::npos &&
            name.find("Jitter") == std::string::npos) {
            continue;
        }
        Log(std::string("[VR][sky] ") + tag + "_FIELD name=" + name +
            " type=" + TypeName(field->type) +
            " offset=" + std::to_string(field->offset) +
            " static=" + (field->static_field ? "1" : "0"));
    }
}

void DumpTaaTypesOnce() noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    if (g_taaTypesDumped) {
        return;
    }
    g_taaTypesDumped = true;
    const char* assemblies[] = {
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "vl-unity.Runtime.dll",
        "Unity.RenderPipelines.Core.Runtime.dll",
    };
    const std::string_view needles[] = {
        "TemporalAA",
        "TaaPersistent",
        "MotionVectorsPersistent",
        "MotionVectorRender",
        "MotionVectorPass",
    };
    for (const char* assemblyName : assemblies) {
        auto* assembly = UnityResolve::Get(assemblyName);
        if (assembly == nullptr) {
            continue;
        }
        for (auto* klass : assembly->classes) {
            if (klass == nullptr) {
                continue;
            }
            bool interesting = klass->namespaze.find("TemporalAA") !=
                std::string::npos;
            for (const auto needle : needles) {
                if (klass->name.find(needle) != std::string::npos) {
                    interesting = true;
                    break;
                }
            }
            if (!interesting) {
                continue;
            }
            DumpClass(klass, klass->name.c_str(), 64U, 64U);
        }
    }
    DumpFilteredFields(
        FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "UnityEngine.Rendering.Universal",
            "UniversalAdditionalCameraData"),
        "UACD_TAA");
    DumpFilteredFields(
        FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "UnityEngine.Rendering.Universal",
            "CameraData"),
        "CAMDATA_TAA");
    DumpClass(
        FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "UnityEngine.Rendering.Universal",
            "TemporalAA"),
        "TemporalAA",
        64U,
        16U);
    DumpClass(
        FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "UnityEngine.Rendering.Universal.TemporalAA",
            "Settings"),
        "TaaSettings",
        16U,
        16U);
    DumpClass(
        FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "UnityEngine.Rendering.Universal",
            "TaaPersistentData"),
        "TaaPersistentData",
        32U,
        32U);
    DumpClass(
        FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "UnityEngine.Rendering.Universal",
            "MotionVectorsPersistentData"),
        "MotionVectorsPersistentData",
        32U,
        32U);
    if (auto* uacd = FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "UnityEngine.Rendering.Universal",
            "UniversalAdditionalCameraData")) {
        for (auto* field : uacd->fields) {
            if (field == nullptr || field->offset < 140 || field->offset > 256) {
                continue;
            }
            Log(std::string("[VR][sky] UACD_NEAR_FIELD name=") + field->name +
                " type=" + TypeName(field->type) +
                " offset=" + std::to_string(field->offset));
        }
    }
    auto* urp = UnityResolve::Get("Unity.RenderPipelines.Universal.Runtime.dll");
    if (urp != nullptr) {
        for (auto* klass : urp->classes) {
            if (klass == nullptr || klass->name.find("Settings") ==
                std::string::npos) {
                continue;
            }
            bool looksTaa = klass->namespaze.find("TemporalAA") !=
                std::string::npos;
            for (auto* field : klass->fields) {
                if (field != nullptr &&
                    (field->name == "jitterScale" ||
                     field->name == "resetHistoryFrames" ||
                     field->name == "jitterFrameCountOffset")) {
                    looksTaa = true;
                    break;
                }
            }
            if (looksTaa) {
                DumpClass(klass, "TaaSettingsLive", 8U, 16U);
            }
        }
    }
}

using TaaExecuteFn =
    void (*)(void*, void*, void*, void*, void*, void*, void*);
using MvUpdateFn = void (*)(void*, void*, void*);
using JumpFloodFn = void (*)(void*, void*, void*, void*, void*, void*);
using VlMotionBlurFn = bool (*)(void*, void*, void*, void*, void*);
using VlTextureBlurFn = bool (*)(void*, void*, void*, void*, void*);
using VlDrawFlareFn = bool (*)(
    void*, void*, void*, void*, float, float, void*);
// VLMotionVectorRenderPass.DrawObjectMotionVectors(
//   ScriptableRenderContext, ref RenderingData, Camera, Material,
//   CommandBuffer, ref FilteringSettings) + MethodInfo.
using ObjectMvFn =
    void (*)(void*, void*, void*, void*, void*, void*, void*);
// DeferredLights.RenderStencilLights(
//   ScriptableRenderContext, CommandBuffer, ref RenderingData,
//   bool useComplexity) + MethodInfo. Instance method.
using StencilLightsFn =
    void (*)(void*, void*, void*, void*, bool, void*);
// FogUtility.UpdateProperties(CommandBuffer, DepthFog, SphereFog,
// VLDensityFogVolume) + MethodInfo. Static method.
using FogUpdateFn = void (*)(void*, void*, void*, void*, void*);
// FogUtility.RenderDeferredFog(CommandBuffer, Material, DeferredFogPasses)
// + MethodInfo. Static method.
using FogRenderFn = void (*)(void*, void*, std::int32_t, void*);
TaaExecuteFn g_taaExecuteOrig = nullptr;
MvUpdateFn g_mvUpdateOrig = nullptr;
JumpFloodFn g_jumpFloodOrig = nullptr;
VlMotionBlurFn g_vlMotionBlurOrig = nullptr;
VlTextureBlurFn g_vlTextureBlurOrig = nullptr;
VlDrawFlareFn g_vlDrawFlareOrig = nullptr;
ObjectMvFn g_objectMvOrig = nullptr;
StencilLightsFn g_stencilLightsOrig = nullptr;
FogUpdateFn g_fogUpdateOrig = nullptr;
FogRenderFn g_fogRenderOrig = nullptr;
bool g_vlPostProcessHooksInstalled = false;
bool g_fogDiagnosticHooksAttempted = false;
bool g_taaHookFaulted = false;
bool g_fogDiagnosticFaulted = false;
std::uint32_t g_taaPassBurst = 12U;
std::uint32_t g_taaPassStride = 0U;
std::uint32_t g_taaPassNulls = 0U;
std::uint32_t g_mvUpdateBurst = 8U;
std::uint32_t g_jumpFloodBurst = 12U;
std::uint32_t g_jumpFloodStride = 0U;
std::uint32_t g_vlMotionBlurBurst = 12U;
std::uint32_t g_vlMotionBlurStride = 0U;
std::array<std::uint32_t, 4> g_vlTextureBlurLogs{};
std::array<int, 4> g_vlTextureBlurLastAction = [] {
    std::array<int, 4> actions{};
    actions.fill(-1);
    return actions;
}();
std::array<std::array<std::uint32_t, 5>, 4> g_vlFlareLogs{};
std::array<std::array<int, 5>, 4> g_vlFlareLastAction = [] {
    std::array<std::array<int, 5>, 4> actions{};
    for (auto& role : actions) {
        role.fill(-1);
    }
    return actions;
}();
bool g_vlFlareReadFaultLogged = false;
std::uint32_t g_objectMvBurst = 12U;
std::uint32_t g_objectMvStride = 0U;
std::uint32_t g_stencilLightsBurst = 12U;
std::uint32_t g_stencilLightsStride = 0U;
std::array<std::uint32_t, 4> g_fogPropertyLogs{};
std::array<std::uint32_t, 4> g_fogRenderLogs{};

// Lobby fog / character-highlight boundary census (BACKLOG row; static trace
// in evidence/lobby-highlight-boundary/). Fog and VLActorParameter ride the
// same per-camera volume stack, so the census enumerates the registered
// Volume objects to prove whether the lobby boundary is a local Volume
// (isGlobal=0, blendDistance≈0) whose profile also overrides
// VLActorParameter. Diagnostics-only: runs from the FogUpdateDetour, which
// is installed only under vrDiagnosticsStartupEnabled.
struct VolumeCensusState {
    bool attempted = false;
    bool faulted = false;
    UnityResolve::Class* managerClass = nullptr;
    UnityResolve::Class* volumeClass = nullptr;
    std::int32_t managerVolumes = -1;
    std::int32_t volumeIsGlobal = -1;
    std::int32_t volumePriority = -1;
    std::int32_t volumeBlend = -1;
    std::int32_t volumeWeight = -1;
    std::int32_t volumeSharedProfile = -1;
    std::int32_t volumeInternalProfile = -1;
    std::int32_t volumeColliders = -1;
    std::uint64_t lastLogTick = 0;
};
VolumeCensusState g_volumeCensus{};

struct FogFieldLayout {
    std::int32_t density = -1;
    std::int32_t maxAmount = -1;
    std::int32_t color = -1;
    std::int32_t applySkybox = -1;
    std::int32_t position = -1;
    std::int32_t radius = -1;
    std::int32_t spherePosition = -1;
    std::int32_t sphereRadius = -1;
    std::int32_t sphereMaxAmount = -1;
    std::int32_t sphereColor = -1;
};

FogFieldLayout g_depthFogFields{};
FogFieldLayout g_sphereFogFields{};
FogFieldLayout g_legacyFogFields{};

int TextureWidth(void* texture) noexcept {
    if (texture == nullptr) {
        return -1;
    }
    auto* klass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Texture");
    auto* method = FindNamed(klass, "get_width");
    if (method == nullptr) {
        return -1;
    }
    int width = -1;
    __try {
        width = method->Invoke<int>(texture);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        width = -1;
    }
    return width;
}

int TextureHeight(void* texture) noexcept {
    if (texture == nullptr) {
        return -1;
    }
    auto* klass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Texture");
    auto* method = FindNamed(klass, "get_height");
    if (method == nullptr) {
        return -1;
    }
    int height = -1;
    __try {
        height = method->Invoke<int>(texture);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        height = -1;
    }
    return height;
}

void LogTaaCameraDataLine(
    const char* tag, void* cameraData, void* motionVectors) noexcept {
    if (cameraData == nullptr || !g_fieldsReady) {
        return;
    }
    const auto fields = Unboxed(g_fields);
    void* camera = ReadPointer(cameraData, fields.camera);
    const char* role = g_renderer != nullptr
        ? g_renderer->ClassifyCamera(camera)
        : "?";
    const char* mainRole = "?";
    auto* cameraClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
    auto* getMain = FindNamed(cameraClass, "get_main");
    if (getMain != nullptr && getMain->function != nullptr) {
        using GetMainFn = void* (*)(void*);
        void* mainCam = reinterpret_cast<GetMainFn>(getMain->function)(
            getMain->address);
        mainRole = g_renderer != nullptr
            ? g_renderer->ClassifyCamera(mainCam)
            : "?";
    }
    float jitter[16]{};
    float view[16]{};
    float proj[16]{};
    const bool haveJitter = ReadMatrix(cameraData, fields.jitter, jitter);
    const bool haveView = ReadMatrix(cameraData, fields.view, view);
    const bool haveProj = ReadMatrix(cameraData, fields.proj, proj);
    void* persist = ReadPointer(cameraData, fields.taaPersistent);
    void* motionPersist = fields.motionPersistent >= 0
        ? ReadPointer(cameraData, fields.motionPersistent)
        : nullptr;
    std::ostringstream stream;
    stream << "[VR][sky] " << tag
           << " role=" << (role != nullptr ? role : "?")
           << " suppressed="
           << ((g_renderer != nullptr && g_renderer->SourceCameraSuppressed())
                   ? "1"
                   : "0")
           << " main=" << (mainRole != nullptr ? mainRole : "?")
           << " aa=" << ReadInt(cameraData, fields.antialiasing)
           << " pix=" << ReadInt(cameraData, fields.pixelW) << "x"
           << ReadInt(cameraData, fields.pixelH)
           // Jitter is a translation-only matrix: the offsets live in
           // m03/m13 (memory [12]/[13]); [0]/[8]/[5] are always 1/0/1 and
           // told us nothing in earlier probes.
           << " j03=" << (haveJitter ? jitter[12] : 0.0F)
           << " j13=" << (haveJitter ? jitter[13] : 0.0F)
           << " viewP00=" << (haveView ? view[0] : 0.0F)
           << " viewP02=" << (haveView ? view[8] : 0.0F)
           << " projP00=" << (haveProj ? proj[0] : 0.0F)
           << " projP02=" << (haveProj ? proj[8] : 0.0F)
           << " lastAccum=" << (persist != nullptr ? ReadInt(persist, 88) : -1)
           << " mv=" << (motionVectors != nullptr ? "1" : "0")
           << " mvWH=" << TextureWidth(motionVectors) << "x"
           << TextureHeight(motionVectors)
           << " persist=0x" << std::hex
           << reinterpret_cast<std::uintptr_t>(persist)
           << " cam=0x" << reinterpret_cast<std::uintptr_t>(camera)
           << std::dec;
    if (fields.taaSettings >= 0) {
        stream << " taaQ=" << ReadInt(cameraData, fields.taaSettings)
               << " taaJS=" << ReadFloat(cameraData, fields.taaSettings + 8)
               << " taaRF=" << ReadInt(cameraData, fields.taaSettings + 24)
               << " taaJO=" << ReadInt(cameraData, fields.taaSettings + 28);
    }
    if (motionPersist != nullptr) {
        void* curArr = ReadPointer(motionPersist, 16);
        void* prevArr = ReadPointer(motionPersist, 24);
        stream << " mvCurP00="
               << (curArr != nullptr ? ReadFloat(curArr, 32) : 0.0F)
               << " mvPrevP00="
               << (prevArr != nullptr ? ReadFloat(prevArr, 32) : 0.0F);
    }
    Log(stream.str());
}

void TaaExecuteDetour(
    void* cmd,
    void* material,
    void* cameraData,
    void* source,
    void* destination,
    void* motionVectors,
    void* method) noexcept {
    if (!g_taaHookFaulted) {
        __try {
            bool logLine = false;
            if (g_taaPassBurst > 0U) {
                --g_taaPassBurst;
                logLine = true;
            } else if ((++g_taaPassStride % 45U) == 0U) {
                logLine = true;
            } else if (motionVectors == nullptr && g_taaPassNulls < 8U) {
                ++g_taaPassNulls;
                logLine = true;
            }
            if (logLine) {
                LogTaaCameraDataLine("TAA_PASS", cameraData, motionVectors);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_taaHookFaulted = true;
            Log("[VR][sky] TAA_PASS faulted");
        }
    }
    if (g_taaExecuteOrig != nullptr) {
        g_taaExecuteOrig(
            cmd, material, cameraData, source, destination, motionVectors,
            method);
    }
}

// VLPostProcessPass.DoJumpFloodOutline(cmd, source, destination,
// CameraData cameraData). CameraData travels BY VALUE, so the pointer we
// receive is a private per-call copy — mutating it affects only this
// outline pass. The temporal branch inside gates on
// CameraData.IsTemporalAAEnabled and reprojects a distance-field history
// that is a single pass-instance double buffer shared by every camera
// (this+0x5D0/0x5E8), so an eye always reprojects another camera's
// silhouette. Zeroing the copy's antialiasing forces the non-temporal
// branch for that call.
void LogJumpFloodLine(
    const char* role, int aaBefore, bool killed) noexcept {
    Log(std::string("[VR][sky] EYE_JUMPFLOOD role=") +
        (role != nullptr ? role : "?") +
        " suppressed=" +
        ((g_renderer != nullptr && g_renderer->SourceCameraSuppressed())
             ? "1"
             : "0") +
        " aa=" + std::to_string(aaBefore) +
        " killed=" + (killed ? "1" : "0"));
}

void JumpFloodOutlineBody(void* cameraData) noexcept {
    const auto fields = Unboxed(g_fields);
    void* camera = ReadPointer(cameraData, fields.camera);
    const char* role = g_renderer != nullptr
        ? g_renderer->ClassifyCamera(camera)
        : "?";
    const bool isEye = role != nullptr &&
        (std::strcmp(role, "left") == 0 || std::strcmp(role, "right") == 0);
    const int aaBefore = fields.antialiasing >= 0
        ? ReadInt(cameraData, fields.antialiasing)
        : -1;
    bool killed = false;
    if (isEye && GakumasLocal::Config::vrEyeJumpFloodTemporalOff &&
        fields.antialiasing >= 0 && aaBefore != 0) {
        *reinterpret_cast<int*>(
            static_cast<std::uint8_t*>(cameraData) + fields.antialiasing) =
            0;
        killed = true;
    }
    bool logLine = false;
    if (g_jumpFloodBurst > 0U) {
        --g_jumpFloodBurst;
        logLine = true;
    } else if ((++g_jumpFloodStride % 300U) == 0U) {
        logLine = true;
    }
    if (logLine) {
        LogJumpFloodLine(role, aaBefore, killed);
    }
}

void JumpFloodOutlineDetour(
    void* self,
    void* cmd,
    void* source,
    void* destination,
    void* cameraData,
    void* method) noexcept {
    ResolveSkyRuntimeDataLayout();
    if (!g_taaHookFaulted && cameraData != nullptr && g_fieldsReady) {
        __try {
            JumpFloodOutlineBody(cameraData);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_taaHookFaulted = true;
            Log("[VR][sky] EYE_JUMPFLOOD faulted");
        }
    }
    if (g_jumpFloodOrig != nullptr) {
        g_jumpFloodOrig(self, cmd, source, destination, cameraData, method);
    }
}

// VLPostProcessPass.DoVLMotionBlur(cmd, source, destination) -> bool.
// Caller (VLPostProcessPass.Render) does `if (DoVLMotionBlur(...)) Swap();`,
// so returning false without touching destination is exactly the
// IsActive()==false path. The blur reconstructs a velocity texture from the
// camera motion-vector target and smears along it with a discrete
// sampleCount — with dirty velocity that produces the source-off multi
// silhouette trail regardless of AA mode. Kill switch drops the pass for
// every camera (VR should not have authored motion blur anyway).
void VlMotionBlurLog(bool killed) noexcept {
    bool logLine = false;
    if (g_vlMotionBlurBurst > 0U) {
        --g_vlMotionBlurBurst;
        logLine = true;
    } else if ((++g_vlMotionBlurStride % 600U) == 0U) {
        logLine = true;
    }
    if (logLine) {
        Log(std::string("[VR][sky] VL_MOTION_BLUR killed=") +
            (killed ? "1" : "0") + " suppressed=" +
            ((g_renderer != nullptr && g_renderer->SourceCameraSuppressed())
                 ? "1"
                 : "0"));
    }
}

bool VlMotionBlurDetour(
    void* self,
    void* cmd,
    void* source,
    void* destination,
    void* method) noexcept {
    const bool kill = GakumasLocal::Config::vrVlMotionBlurOff;
    VlMotionBlurLog(kill);
    if (kill) {
        return false;
    }
    if (g_vlMotionBlurOrig != nullptr) {
        return g_vlMotionBlurOrig(self, cmd, source, destination, method);
    }
    return false;
}

// VLPostProcessPass.DoVLTextureBlur(cmd, source, destination) -> bool.
// The runtime body reads VLTextureBlur.texture/intensity from the Volume
// stack, writes _TextureBlurTexture/_TextureBlurIntensity, then performs a
// full-screen Blit. VLPostProcessPass.Render swaps source/destination only
// when this method returns true, so false is the authored IsActive()==false
// branch and cannot expose an unwritten destination. Eyes skip the blit
// under the same lens-card toggle as DrawFlare; source / monitor / Grip
// stay authored.
void VlTextureBlurLog(
    const char* role,
    void* camera,
    void* pass,
    bool isEye,
    bool skipped,
    bool originalCalled,
    bool originalActive) noexcept {
    const int roleIndex = RoleIndex(role);
    auto& count = g_vlTextureBlurLogs[static_cast<std::size_t>(roleIndex)];
    auto& lastAction = g_vlTextureBlurLastAction[static_cast<std::size_t>(roleIndex)];
    const int action = skipped ? 1 : 0;
    const bool changed = lastAction != action;
    lastAction = action;
    ++count;
    if (!changed && count > 8U && count % 600U != 0U) {
        return;
    }
    std::ostringstream line;
    line << "[VR][sky] EYE_VL_TEXTURE_BLUR role="
         << (role != nullptr ? role : "?") << " camera=0x" << std::hex
         << reinterpret_cast<std::uintptr_t>(camera) << std::dec
         << " eye=" << (isEye ? 1 : 0)
         << " toggle="
         << (GakumasLocal::Config::vrHideUiTextureOverlay ? 1 : 0)
         << " action=" << (skipped ? "skip" : "execute")
         << " originalCalled=" << (originalCalled ? 1 : 0)
         << " originalActive=";
    if (originalCalled) {
        line << (originalActive ? 1 : 0);
    } else {
        line << '?';
    }
    line << " pass=0x" << std::hex
         << reinterpret_cast<std::uintptr_t>(pass) << std::dec;
    Log(line.str());
}

bool VlTextureBlurDetour(
    void* self,
    void* cmd,
    void* source,
    void* destination,
    void* method) noexcept {
    void* camera = g_renderer != nullptr ? g_renderer->CurrentCamera() : nullptr;
    const bool isEye =
        g_renderer != nullptr && g_renderer->IsEyeCamera(camera);
    const char* role = g_renderer != nullptr
        ? g_renderer->ClassifyCamera(camera)
        : "?";
    const bool skip =
        isEye && GakumasLocal::Config::vrHideUiTextureOverlay;
    if (skip) {
        VlTextureBlurLog(role, camera, self, isEye, true, false, false);
        return false;
    }
    const bool active = g_vlTextureBlurOrig != nullptr
        ? g_vlTextureBlurOrig(self, cmd, source, destination, method)
        : false;
    VlTextureBlurLog(
        role, camera, self, isEye, false, g_vlTextureBlurOrig != nullptr,
        active);
    return active;
}

// dump.cs FlareSetting layout (unboxed): type@0, fixToBaseAspect@4,
// center@8, color0@0x10, color1@0x20, size@0x30; total 0x38 bytes.
struct VlFlareSetting {
    std::int32_t type = 0;
    std::uint8_t fixToBaseAspect = 0U;
    std::uint8_t padding[3]{};
    float center[2]{};
    float color0[4]{};
    float color1[4]{};
    float size[2]{};
};
static_assert(sizeof(VlFlareSetting) == 0x38U);

bool ReadVlFlareSetting(void* raw, VlFlareSetting* out) noexcept {
    if (raw == nullptr || out == nullptr) {
        return false;
    }
    __try {
        std::memcpy(out, raw, sizeof(*out));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void VlFlareLog(
    const char* role,
    void* camera,
    void* pass,
    const VlFlareSetting& flare,
    float baseAspect,
    float aspect,
    bool isEye,
    bool skipped,
    bool originalCalled,
    bool originalDrawn) noexcept {
    const int roleIndex = RoleIndex(role);
    const auto typeIndex = static_cast<std::size_t>(
        std::clamp(flare.type, 0, 4));
    auto& count = g_vlFlareLogs[static_cast<std::size_t>(roleIndex)][typeIndex];
    auto& lastAction =
        g_vlFlareLastAction[static_cast<std::size_t>(roleIndex)][typeIndex];
    const int action = skipped ? 1 : 0;
    const bool changed = lastAction != action;
    lastAction = action;
    ++count;
    if (!changed && count > 8U && count % 600U != 0U) {
        return;
    }
    std::ostringstream line;
    line << "[VR][sky] EYE_VL_FLARE role="
         << (role != nullptr ? role : "?") << " camera=0x" << std::hex
         << reinterpret_cast<std::uintptr_t>(camera) << std::dec
         << " eye=" << (isEye ? 1 : 0)
         << " toggle="
         << (GakumasLocal::Config::vrHideUiTextureOverlay ? 1 : 0)
         << " type=" << flare.type
         << " fixAspect=" << static_cast<int>(flare.fixToBaseAspect)
         << " center=" << flare.center[0] << ',' << flare.center[1]
         << " color0=" << flare.color0[0] << ',' << flare.color0[1] << ','
         << flare.color0[2] << ',' << flare.color0[3]
         << " color1=" << flare.color1[0] << ',' << flare.color1[1] << ','
         << flare.color1[2] << ',' << flare.color1[3]
         << " size=" << flare.size[0] << ',' << flare.size[1]
         << " baseAspect=" << baseAspect << " aspect=" << aspect
         << " action=" << (skipped ? "skip" : "execute")
         << " originalCalled=" << (originalCalled ? 1 : 0)
         << " originalDrawn=";
    if (originalCalled) {
        line << (originalDrawn ? 1 : 0);
    } else {
        line << '?';
    }
    line << " pass=0x" << std::hex
         << reinterpret_cast<std::uintptr_t>(pass) << std::dec;
    Log(line.str());
}

bool VlDrawFlareDetour(
    void* self,
    void* cmd,
    void* flareMaterial,
    void* flareRaw,
    float baseAspect,
    float aspect,
    void* method) noexcept {
    VlFlareSetting flare{};
    if (!ReadVlFlareSetting(flareRaw, &flare)) {
        if (!g_vlFlareReadFaultLogged) {
            g_vlFlareReadFaultLogged = true;
            Log("[VR][sky] EYE_VL_FLARE readFault=1 action=execute");
        }
        return g_vlDrawFlareOrig != nullptr
            ? g_vlDrawFlareOrig(
                  self, cmd, flareMaterial, flareRaw, baseAspect, aspect,
                  method)
            : false;
    }
    void* camera = g_renderer != nullptr ? g_renderer->CurrentCamera() : nullptr;
    const bool isEye =
        g_renderer != nullptr && g_renderer->IsEyeCamera(camera);
    const char* role = g_renderer != nullptr
        ? g_renderer->ClassifyCamera(camera)
        : "?";
    constexpr std::int32_t firstDrawableType = 1;
    constexpr std::int32_t lastDrawableType = 4;
    const bool drawable = flare.type >= firstDrawableType &&
        flare.type <= lastDrawableType;
    const bool skip = isEye && drawable &&
        GakumasLocal::Config::vrHideUiTextureOverlay;
    if (skip) {
        VlFlareLog(
            role, camera, self, flare, baseAspect, aspect, isEye, true, false,
            false);
        return false;
    }
    const bool drawn = g_vlDrawFlareOrig != nullptr
        ? g_vlDrawFlareOrig(
              self, cmd, flareMaterial, flareRaw, baseAspect, aspect, method)
        : false;
    if (drawable || flare.type == 0) {
        VlFlareLog(
            role, camera, self, flare, baseAspect, aspect, isEye, false,
            g_vlDrawFlareOrig != nullptr, drawn);
    }
    return drawn;
}

// VLMotionVectorRenderPass.DrawObjectMotionVectors draws skinned /
// outlined object velocity on top of the camera-motion full-screen pass.
// Camera MVs come from per-eye MVPD (.108: clean). Object MVs use Unity's
// previous object-to-world / previous-bone buffers. .112's managed
// Camera.get_main override did not refresh those (native / earlier-in-
// frame gate). Skipping this draw leaves camera MVs only — TAA can still
// cancel Halton jitter; characters lose object velocity.
void ObjectMvLog(const char* role, bool skipped) noexcept {
    bool logLine = false;
    if (g_objectMvBurst > 0U) {
        --g_objectMvBurst;
        logLine = true;
    } else if ((++g_objectMvStride % 300U) == 0U) {
        logLine = true;
    }
    if (logLine) {
        Log(std::string("[VR][sky] OBJECT_MV role=") +
            (role != nullptr ? role : "?") +
            " skipped=" + (skipped ? "1" : "0") +
            " suppressed=" +
            ((g_renderer != nullptr && g_renderer->SourceCameraSuppressed())
                 ? "1"
                 : "0"));
    }
}

void ObjectMvDetour(
    void* context,
    void* renderingData,
    void* camera,
    void* objectMaterial,
    void* cmd,
    void* filteringSettings,
    void* method) noexcept {
    const char* role = g_renderer != nullptr
        ? g_renderer->ClassifyCamera(camera)
        : "?";
    const bool isEye = role != nullptr &&
        (std::strcmp(role, "left") == 0 || std::strcmp(role, "right") == 0);
    const bool skip =
        isEye && GakumasLocal::Config::vrEyeObjectMotionVectorsOff;
    ObjectMvLog(role, skip);
    if (skip) {
        return;
    }
    if (g_objectMvOrig != nullptr) {
        g_objectMvOrig(
            context, renderingData, camera, objectMaterial, cmd,
            filteringSettings, method);
    }
}

// DeferredLights.RenderStencilLights draws the VLSRP stencil-deferred
// punctual lights (stage spot / point volumes) after the actor GBuffer.
// That stage reconstructs the view vector per camera from
// _CameraDepthTexture / _CameraNormalsTexture, so its specular follows
// each eye — the one term the .131–.134 Campus-global zeroes never
// touched. Eyes-only skip: Grip keeps the authored look for comparison.
void StencilLightsLog(const char* role, bool skipped) noexcept {
    bool logLine = false;
    if (g_stencilLightsBurst > 0U) {
        --g_stencilLightsBurst;
        logLine = true;
    } else if ((++g_stencilLightsStride % 300U) == 0U) {
        logLine = true;
    }
    if (logLine) {
        Log(std::string("[VR][sky] DEFERRED_STENCIL role=") +
            (role != nullptr ? role : "?") +
            " skipped=" + (skipped ? "1" : "0"));
    }
}

void StencilLightsDetour(
    void* self,
    void* context,
    void* cmd,
    void* renderingData,
    bool useComplexity,
    void* method) noexcept {
    const char* role = g_renderer != nullptr
        ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
        : "?";
    const bool isEye = role != nullptr &&
        (std::strcmp(role, "left") == 0 || std::strcmp(role, "right") == 0);
    const bool skip =
        isEye && GakumasLocal::Config::vrEyeDeferredStencilOff;
    StencilLightsLog(role, skip);
    if (skip) {
        return;
    }
    if (g_stencilLightsOrig != nullptr) {
        g_stencilLightsOrig(
            self, context, cmd, renderingData, useComplexity, method);
    }
}

void AppendFogFloat(
    std::ostringstream& line,
    std::string_view label,
    void* component,
    std::int32_t fieldOffset) noexcept {
    line << ' ' << label << '=';
    void* parameter = fieldOffset >= 0
        ? ReadPointer(component, fieldOffset)
        : nullptr;
    bool ok = false;
    const float value = VolumeValueFloat(parameter, &ok);
    if (!ok) {
        line << '?';
        return;
    }
    line << value << "/ov=" << VolumeOverride(parameter);
}

void AppendFogBool(
    std::ostringstream& line,
    std::string_view label,
    void* component,
    std::int32_t fieldOffset) noexcept {
    line << ' ' << label << '=';
    void* parameter = fieldOffset >= 0
        ? ReadPointer(component, fieldOffset)
        : nullptr;
    const int value = VolumeValueBool(parameter);
    if (value < 0) {
        line << '?';
        return;
    }
    line << value << "/ov=" << VolumeOverride(parameter);
}

void AppendFogVector(
    std::ostringstream& line,
    std::string_view label,
    void* component,
    std::int32_t fieldOffset,
    std::size_t count) noexcept {
    line << ' ' << label << '=';
    void* parameter = fieldOffset >= 0
        ? ReadPointer(component, fieldOffset)
        : nullptr;
    float values[4]{};
    if (count == 0U || count > std::size(values) ||
        !VolumeValueFloats(parameter, values, count)) {
        line << '?';
        return;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        if (index != 0U) {
            line << ',';
        }
        line << values[index];
    }
    line << "/ov=" << VolumeOverride(parameter);
}

void LogFogFieldLayout(
    std::string_view tag, const FogFieldLayout& fields) noexcept {
    std::ostringstream line;
    line << "[VR][fog] FOG_LAYOUT component=" << tag
         << " density=" << fields.density
         << " maxAmount=" << fields.maxAmount
         << " color=" << fields.color
         << " applySkybox=" << fields.applySkybox
         << " position=" << fields.position
         << " radius=" << fields.radius
         << " spherePosition=" << fields.spherePosition
         << " sphereRadius=" << fields.sphereRadius
         << " sphereMaxAmount=" << fields.sphereMaxAmount
         << " sphereColor=" << fields.sphereColor;
    Log(line.str());
}

FogFieldLayout ResolveFogFields(UnityResolve::Class* klass) noexcept {
    FogFieldLayout fields{};
    fields.density = FindInstanceFieldOffset(klass, "density");
    fields.maxAmount = FindInstanceFieldOffset(klass, "maxAmount");
    fields.color = FindInstanceFieldOffset(klass, "color");
    fields.applySkybox = FindInstanceFieldOffset(klass, "applySkybox");
    fields.position = FindInstanceFieldOffset(klass, "position");
    fields.radius = FindInstanceFieldOffset(klass, "radius");
    fields.spherePosition = FindInstanceFieldOffset(klass, "spherePosition");
    fields.sphereRadius = FindInstanceFieldOffset(klass, "sphereRadius");
    fields.sphereMaxAmount =
        FindInstanceFieldOffset(klass, "sphereMaxAmount");
    fields.sphereColor = FindInstanceFieldOffset(klass, "sphereColor");
    return fields;
}

bool ShouldLogFog(std::array<std::uint32_t, 4>& counts, const char* role) noexcept {
    auto& count = counts[static_cast<std::size_t>(RoleIndex(role))];
    ++count;
    return count <= 12U || (count % 120U) == 0U;
}

void LogFogProperties(
    void* cmd, void* depthFog, void* sphereFog, void* legacyFog) noexcept {
    if (g_renderer == nullptr) {
        return;
    }
    void* camera = g_renderer->CurrentCamera();
    const char* role = g_renderer->ClassifyCamera(camera);
    if (!ShouldLogFog(g_fogPropertyLogs, role)) {
        return;
    }
    const auto pose = g_renderer->ReadCurrentCameraPoseForDiagnostics();
    std::ostringstream line;
    line << std::fixed << std::setprecision(5)
         << "[VR][fog] FOG_PROPERTIES role="
         << (role != nullptr ? role : "?")
         << " camera=0x" << std::hex
         << reinterpret_cast<std::uintptr_t>(camera)
         << " cmd=0x" << reinterpret_cast<std::uintptr_t>(cmd)
         << " depth=0x" << reinterpret_cast<std::uintptr_t>(depthFog)
         << " sphere=0x" << reinterpret_cast<std::uintptr_t>(sphereFog)
         << " legacy=0x" << reinterpret_cast<std::uintptr_t>(legacyFog)
         << std::dec
         << " camPos=" << pose.position.x << ',' << pose.position.y << ','
         << pose.position.z
         << " camRot=" << pose.orientation.x << ',' << pose.orientation.y << ','
         << pose.orientation.z << ',' << pose.orientation.w
         << " depthActive=" << VolumeComponentActive(depthFog);
    AppendFogFloat(
        line, "depthDensity", depthFog, g_depthFogFields.density);
    AppendFogFloat(
        line, "depthMax", depthFog, g_depthFogFields.maxAmount);
    AppendFogVector(line, "depthColor", depthFog, g_depthFogFields.color, 4U);
    AppendFogBool(
        line, "depthSky", depthFog, g_depthFogFields.applySkybox);
    line << " sphereActive=" << VolumeComponentActive(sphereFog);
    AppendFogFloat(
        line, "sphereDensity", sphereFog, g_sphereFogFields.density);
    AppendFogFloat(
        line, "sphereMax", sphereFog, g_sphereFogFields.maxAmount);
    AppendFogVector(
        line, "sphereColor", sphereFog, g_sphereFogFields.color, 4U);
    AppendFogVector(
        line, "spherePos", sphereFog, g_sphereFogFields.position, 3U);
    AppendFogFloat(
        line, "sphereRadius", sphereFog, g_sphereFogFields.radius);
    line << " legacyActive=" << VolumeComponentActive(legacyFog);
    AppendFogFloat(
        line, "legacyDensity", legacyFog, g_legacyFogFields.density);
    AppendFogFloat(
        line, "legacyMax", legacyFog, g_legacyFogFields.maxAmount);
    AppendFogVector(
        line, "legacyColor", legacyFog, g_legacyFogFields.color, 4U);
    AppendFogVector(
        line, "legacySpherePos", legacyFog,
        g_legacyFogFields.spherePosition, 3U);
    AppendFogFloat(
        line, "legacySphereRadius", legacyFog,
        g_legacyFogFields.sphereRadius);
    AppendFogFloat(
        line, "legacySphereMax", legacyFog,
        g_legacyFogFields.sphereMaxAmount);
    AppendFogVector(
        line, "legacySphereColor", legacyFog,
        g_legacyFogFields.sphereColor, 4U);
    Log(line.str());
}

bool ReadManagedList(
    void* list, void** itemsOut, std::int32_t* sizeOut) noexcept {
    if (list == nullptr || itemsOut == nullptr || sizeOut == nullptr) {
        return false;
    }
    void* listKlass = ReadPointer(list, 0);
    const std::int32_t itemsOffset =
        FindFieldOffsetOnIl2CppClass(listKlass, {"_items"}, {});
    const std::int32_t sizeOffset = FindFieldOffsetOnIl2CppClass(
        listKlass, {"_size"}, {"System.Int32", "Int32"});
    if (itemsOffset < 0 || sizeOffset < 0) {
        return false;
    }
    *itemsOut = ReadPointer(list, itemsOffset);
    *sizeOut = ReadInt(list, sizeOffset);
    return *itemsOut != nullptr && *sizeOut >= 0;
}

// Il2CppArray on x64: klass +0x0, monitor +0x8, bounds +0x10,
// max_length +0x18, data +0x20.
void* ReadManagedArrayElement(void* array, std::int32_t index) noexcept {
    if (array == nullptr || index < 0) {
        return nullptr;
    }
    const std::int32_t length = ReadInt(array, 0x18);
    if (index >= length) {
        return nullptr;
    }
    return ReadPointer(array, 0x20 + index * 8);
}

std::string ComponentGoName(void* behaviour) noexcept {
    if (behaviour == nullptr) {
        return "-";
    }
    auto* typed =
        reinterpret_cast<UnityResolve::UnityType::Component*>(behaviour);
    auto* gameObject = typed->GetGameObject();
    if (gameObject == nullptr) {
        return "-";
    }
    const std::string name = gameObject->GetName();
    return name.empty() ? "-" : name;
}

void AppendCensusParameter(
    std::ostringstream& line,
    std::string_view label,
    void* component,
    const char* fieldName,
    std::size_t floatCount) noexcept {
    const std::int32_t offset = FindFieldOffsetOnIl2CppClass(
        ReadPointer(component, 0), {fieldName}, {});
    if (offset < 0) {
        return;
    }
    void* parameter = ReadPointer(component, offset);
    if (parameter == nullptr) {
        return;
    }
    line << ',' << label << '=';
    float values[4]{};
    if (floatCount > 0U && floatCount <= std::size(values) &&
        VolumeValueFloats(parameter, values, floatCount)) {
        for (std::size_t index = 0U; index < floatCount; ++index) {
            if (index != 0U) {
                line << '|';
            }
            line << values[index];
        }
    } else if (floatCount == 0U) {
        // Enum-backed parameter (e.g. LightSpaceTypeParameter): first int.
        const std::int32_t valueOffset = ResolveVolumeField(
            parameter, {"m_Value", "value"}, {});
        line << (valueOffset >= 0 ? ReadInt(parameter, valueOffset) : -1);
    } else {
        line << '?';
    }
    line << "/ov=" << VolumeOverride(parameter);
}

void AppendCensusProfile(std::ostringstream& line, void* profile) noexcept {
    line << " profile=0x" << std::hex
         << reinterpret_cast<std::uintptr_t>(profile) << std::dec;
    if (profile == nullptr) {
        return;
    }
    const std::int32_t componentsOffset = FindFieldOffsetOnIl2CppClass(
        ReadPointer(profile, 0), {"components"}, {});
    void* items = nullptr;
    std::int32_t size = 0;
    if (componentsOffset < 0 ||
        !ReadManagedList(
            ReadPointer(profile, componentsOffset), &items, &size)) {
        line << " comps=?";
        return;
    }
    line << " comps=";
    const std::int32_t cap = size < 12 ? size : 12;
    for (std::int32_t index = 0; index < cap; ++index) {
        void* component = ReadManagedArrayElement(items, index);
        if (component == nullptr) {
            continue;
        }
        if (index != 0) {
            line << ' ';
        }
        const char* shortName =
            ReadLiveClassNamePart(ReadPointer(component, 0), false);
        const std::string componentName =
            shortName != nullptr ? shortName : "?";
        line << componentName << "(a=" << VolumeComponentActive(component);
        if (componentName == "SphereFog") {
            AppendCensusParameter(line, "d", component, "density", 1U);
            AppendCensusParameter(line, "max", component, "maxAmount", 1U);
            AppendCensusParameter(line, "pos", component, "position", 3U);
            AppendCensusParameter(line, "r", component, "radius", 1U);
        } else if (componentName == "DepthFog") {
            AppendCensusParameter(line, "d", component, "density", 1U);
            AppendCensusParameter(line, "max", component, "maxAmount", 1U);
        } else if (componentName == "VLActorParameter") {
            AppendCensusParameter(
                line, "space", component, "mainLightSpace", 0U);
            AppendCensusParameter(
                line, "mla", component, "mainLightAngle", 2U);
            AppendCensusParameter(line, "rim", component, "rimAngle", 2U);
            AppendCensusParameter(
                line, "lightColor", component, "lightColor", 4U);
        }
        line << ')';
    }
    if (size > cap) {
        line << " …+" << (size - cap);
    }
}

// Worker with C++ locals; the SEH guard lives in the caller.
void RunVolumeCensus() noexcept {
    auto& census = g_volumeCensus;
    const std::uint64_t now = GetTickCount64();
    if (census.lastLogTick != 0U && now - census.lastLogTick < 5000ULL) {
        return;
    }
    census.lastLogTick = now;
    if (!census.attempted) {
        census.attempted = true;
        census.managerClass = FindClass(
            "Unity.RenderPipelines.Core.Runtime.dll",
            "UnityEngine.Rendering", "VolumeManager");
        census.volumeClass = FindClass(
            "Unity.RenderPipelines.Core.Runtime.dll",
            "UnityEngine.Rendering", "Volume");
        census.managerVolumes =
            FindInstanceFieldOffset(census.managerClass, "m_Volumes");
        census.volumeIsGlobal =
            FindInstanceFieldOffset(census.volumeClass, "m_IsGlobal");
        census.volumePriority =
            FindInstanceFieldOffset(census.volumeClass, "priority");
        census.volumeBlend =
            FindInstanceFieldOffset(census.volumeClass, "blendDistance");
        census.volumeWeight =
            FindInstanceFieldOffset(census.volumeClass, "weight");
        census.volumeSharedProfile =
            FindInstanceFieldOffset(census.volumeClass, "sharedProfile");
        census.volumeInternalProfile =
            FindInstanceFieldOffset(census.volumeClass, "m_InternalProfile");
        census.volumeColliders =
            FindInstanceFieldOffset(census.volumeClass, "m_Colliders");
        std::ostringstream ready;
        ready << "[VR][fog] VOLUME_CENSUS_READY manager="
              << (census.managerClass != nullptr ? 1 : 0)
              << " volume=" << (census.volumeClass != nullptr ? 1 : 0)
              << " volumes=" << census.managerVolumes
              << " isGlobal=" << census.volumeIsGlobal
              << " priority=" << census.volumePriority
              << " blend=" << census.volumeBlend
              << " weight=" << census.volumeWeight
              << " sharedProfile=" << census.volumeSharedProfile
              << " internalProfile=" << census.volumeInternalProfile
              << " colliders=" << census.volumeColliders;
        Log(ready.str());
    }
    if (census.managerClass == nullptr || census.managerVolumes < 0) {
        return;
    }
    void* manager = InvokePtr(census.managerClass, nullptr, "get_instance");
    if (manager == nullptr) {
        return;
    }
    void* items = nullptr;
    std::int32_t size = 0;
    if (!ReadManagedList(
            ReadPointer(manager, census.managerVolumes), &items, &size)) {
        Log("[VR][fog] VOLUME_CENSUS_BEGIN count=? list=unreadable");
        return;
    }
    {
        const char* role = g_renderer != nullptr
            ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
            : "?";
        std::ostringstream begin;
        begin << "[VR][fog] VOLUME_CENSUS_BEGIN count=" << size
              << " role=" << (role != nullptr ? role : "?");
        if (g_renderer != nullptr) {
            const auto pose =
                g_renderer->ReadCurrentCameraPoseForDiagnostics();
            begin << std::fixed << std::setprecision(3)
                  << " camPos=" << pose.position.x << ','
                  << pose.position.y << ',' << pose.position.z;
        }
        Log(begin.str());
    }
    // `.228` capped at 16 and the lobby registers 17 volumes, so idx=16 was
    // never logged (suspected SphereFogVolume). 32 covers every scene seen.
    const std::int32_t cap = size < 32 ? size : 32;
    for (std::int32_t index = 0; index < cap; ++index) {
        void* volume = ReadManagedArrayElement(items, index);
        if (volume == nullptr) {
            continue;
        }
        std::ostringstream line;
        line << std::fixed << std::setprecision(3)
             << "[VR][fog] VOLUME_CENSUS idx=" << index << '/' << size
             << " vol=0x" << std::hex
             << reinterpret_cast<std::uintptr_t>(volume) << std::dec
             << " class=" << LiveClassName(ReadPointer(volume, 0))
             << " go=" << ComponentGoName(volume);
        auto* component =
            reinterpret_cast<UnityResolve::UnityType::Component*>(volume);
        auto* transform = component->GetTransform();
        if (transform != nullptr) {
            const auto position = transform->GetPosition();
            line << " pos=" << position.x << ',' << position.y << ','
                 << position.z;
        }
        if (census.volumeIsGlobal >= 0) {
            line << " global="
                 << (ReadInt(volume, census.volumeIsGlobal) & 0xFF);
        }
        if (census.volumePriority >= 0) {
            line << " priority=" << ReadFloat(volume, census.volumePriority);
        }
        if (census.volumeBlend >= 0) {
            line << " blend=" << ReadFloat(volume, census.volumeBlend);
        }
        if (census.volumeWeight >= 0) {
            line << " weight=" << ReadFloat(volume, census.volumeWeight);
        }
        std::int32_t colliderCount = -1;
        if (census.volumeColliders >= 0) {
            void* colliderItems = nullptr;
            std::int32_t colliderSize = 0;
            if (ReadManagedList(
                    ReadPointer(volume, census.volumeColliders),
                    &colliderItems, &colliderSize)) {
                colliderCount = colliderSize;
            }
        }
        line << " colliders=" << colliderCount;
        void* profile = census.volumeInternalProfile >= 0
            ? ReadPointer(volume, census.volumeInternalProfile)
            : nullptr;
        if (profile == nullptr && census.volumeSharedProfile >= 0) {
            profile = ReadPointer(volume, census.volumeSharedProfile);
        }
        AppendCensusProfile(line, profile);
        Log(line.str());
    }
}

void FogUpdateDetour(
    void* cmd,
    void* depthFog,
    void* sphereFog,
    void* legacyFog,
    void* method) noexcept {
    if (!g_fogDiagnosticFaulted) {
        __try {
            LogFogProperties(cmd, depthFog, sphereFog, legacyFog);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_fogDiagnosticFaulted = true;
            Log("[VR][fog] FOG_PROPERTIES faulted=1 action=forward");
        }
    }
    if (!g_volumeCensus.faulted) {
        __try {
            RunVolumeCensus();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_volumeCensus.faulted = true;
            Log("[VR][fog] VOLUME_CENSUS faulted=1 action=skip");
        }
    }
    if (g_fogUpdateOrig != nullptr) {
        g_fogUpdateOrig(cmd, depthFog, sphereFog, legacyFog, method);
    }
}

void LogFogRender(void* cmd, void* material, std::int32_t pass) noexcept {
    const char* role =
        g_renderer->ClassifyCamera(g_renderer->CurrentCamera());
    if (!ShouldLogFog(g_fogRenderLogs, role)) {
        return;
    }
    std::ostringstream line;
    line << "[VR][fog] FOG_RENDER role="
         << (role != nullptr ? role : "?")
         << " pass=" << pass
         << " name="
         << (pass == 0 ? "Full"
             : pass == 1 ? "ActorOnly"
             : pass == 2 ? "Downscale"
                         : "?")
         << " cmd=0x" << std::hex
         << reinterpret_cast<std::uintptr_t>(cmd)
         << " material=0x" << reinterpret_cast<std::uintptr_t>(material)
         << std::dec;
    Log(line.str());
}

void FogRenderDetour(
    void* cmd, void* material, std::int32_t pass, void* method) noexcept {
    if (g_renderer != nullptr && !g_fogDiagnosticFaulted) {
        __try {
            LogFogRender(cmd, material, pass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_fogDiagnosticFaulted = true;
            Log("[VR][fog] FOG_RENDER faulted=1 action=forward");
        }
    }
    if (g_fogRenderOrig != nullptr) {
        g_fogRenderOrig(cmd, material, pass, method);
    }
}

void InstallFogDiagnosticHooks() noexcept {
    if (g_fogDiagnosticHooksAttempted ||
        !GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    g_fogDiagnosticHooksAttempted = true;
    const char* assemblyName = "Unity.RenderPipelines.Universal.Runtime.dll";
    auto* fogUtility = FindClass(
        assemblyName, "VL.Rendering", "FogUtility");
    if (fogUtility == nullptr) {
        assemblyName = "vl-unity.Runtime.dll";
        fogUtility = FindClass(assemblyName, "VL.Rendering", "FogUtility");
    }
    auto* depthFog = FindClass(assemblyName, "VL.Rendering", "DepthFog");
    auto* sphereFog = FindClass(assemblyName, "VL.Rendering", "SphereFog");
    auto* legacyFog =
        FindClass(assemblyName, "VL.Rendering", "VLDensityFogVolume");
    auto* campusSphere = FindClass(
        "campus-submodule.Runtime.dll", "Campus.Rendering",
        "SphereFogVolume");
    Log(std::string("[VR][fog] FOG_LOOKUP assembly=") + assemblyName +
        " utility=" + (fogUtility != nullptr ? "1" : "0") +
        " depth=" + (depthFog != nullptr ? "1" : "0") +
        " sphere=" + (sphereFog != nullptr ? "1" : "0") +
        " legacy=" + (legacyFog != nullptr ? "1" : "0") +
        " campusSphere=" + (campusSphere != nullptr ? "1" : "0"));
    DumpClass(fogUtility, "FOG_UTILITY_LIVE", 24U, 24U);
    DumpClass(depthFog, "DEPTH_FOG_LIVE", 16U, 16U);
    DumpClass(sphereFog, "SPHERE_FOG_LIVE", 16U, 16U);
    DumpClass(legacyFog, "VL_DENSITY_FOG_LIVE", 16U, 16U);
    DumpClass(campusSphere, "CAMPUS_SPHERE_FOG_VOLUME_LIVE", 16U, 16U);
    g_depthFogFields = ResolveFogFields(depthFog);
    g_sphereFogFields = ResolveFogFields(sphereFog);
    g_legacyFogFields = ResolveFogFields(legacyFog);
    LogFogFieldLayout("DepthFog", g_depthFogFields);
    LogFogFieldLayout("SphereFog", g_sphereFogFields);
    LogFogFieldLayout("VLDensityFogVolume", g_legacyFogFields);
    LogNamedMethodShapes(
        fogUtility, "UpdateProperties", "FOG_UPDATE_METHOD");
    LogNamedMethodShapes(
        fogUtility, "RenderDeferredFog", "FOG_RENDER_METHOD");
    auto* update = FindExactStaticVoid(
        fogUtility, "UpdateProperties",
        {"UnityEngine.Rendering.CommandBuffer", "VL.Rendering.DepthFog",
         "VL.Rendering.SphereFog", "VL.Rendering.VLDensityFogVolume"});
    auto* render = FindExactStaticVoid(
        fogUtility, "RenderDeferredFog",
        {"UnityEngine.Rendering.CommandBuffer", "UnityEngine.Material",
         "VL.Rendering.DeferredFogPasses"});
    Log(std::string("[VR][fog] FOG_UPDATE_HOOK ready=") +
        (update != nullptr ? "1" : "0") +
        " signature=System.Void UpdateProperties(CommandBuffer,DepthFog," +
        "SphereFog,VLDensityFogVolume)");
    if (update != nullptr) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            update->function, reinterpret_cast<void*>(&FogUpdateDetour),
            reinterpret_cast<void**>(&g_fogUpdateOrig),
            "FogUtility.UpdateProperties");
        Log(std::string("[VR][fog] FOG_UPDATE_HOOK installed=") +
            (installed ? "1" : "0"));
    }
    Log(std::string("[VR][fog] FOG_RENDER_HOOK ready=") +
        (render != nullptr ? "1" : "0") +
        " signature=System.Void RenderDeferredFog(CommandBuffer,Material," +
        "DeferredFogPasses)");
    if (render != nullptr) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            render->function, reinterpret_cast<void*>(&FogRenderDetour),
            reinterpret_cast<void**>(&g_fogRenderOrig),
            "FogUtility.RenderDeferredFog");
        Log(std::string("[VR][fog] FOG_RENDER_HOOK installed=") +
            (installed ? "1" : "0"));
    }
}

void MvUpdateDetour(void* self, void* cameraData, void* method) noexcept {
    bool correctionRequired = false;
    bool correctionReady = true;
    if (g_renderer != nullptr && cameraData != nullptr && g_fieldsReady) {
        const CameraDataFields fields = Unboxed(g_fields);
        void* camera = ReadPointer(cameraData, fields.camera);
        float* projection = fields.proj >= 0
            ? reinterpret_cast<float*>(AddBytes(cameraData, fields.proj))
            : nullptr;
        correctionReady = g_renderer->CorrectSmaaT2xMotionHistoryProjection(
            camera, projection, &correctionRequired);
    }
    if (!g_taaHookFaulted && g_mvUpdateBurst > 0U) {
        --g_mvUpdateBurst;
        __try {
            LogTaaCameraDataLine("TAA_MV_UPDATE", cameraData, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_taaHookFaulted = true;
            Log("[VR][sky] TAA_MV_UPDATE faulted");
        }
    }
    if (g_mvUpdateOrig != nullptr) {
        g_mvUpdateOrig(self, cameraData, method);
    }
    // A failed correction leaves the renderer's per-eye pair bit false, so the
    // verified post-process capture withholds this jittered pair.
    if (correctionRequired && !correctionReady) {
        Log("[VR][smaa-t2x] SMAA_T2X_FAULT mv-history-update "
            "action=withhold-on-capture");
    }
}

void InstallVlPostProcessHooks() noexcept {
    if (g_vlPostProcessHooksInstalled) {
        return;
    }
    g_vlPostProcessHooksInstalled = true;
    Log(std::string("[VR][sky] VL_POST_RUNTIME_HOOKS_INSTALL diagnostics=") +
        (GakumasLocal::Config::vrDiagnosticsStartupEnabled ? "1" : "0"));
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        auto* temporalAa = FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "UnityEngine.Rendering.Universal",
            "TemporalAA");
        auto* execute = FindNamed(temporalAa, "ExecutePass");
        const bool executeReady =
            execute != nullptr && execute->function != nullptr &&
            execute->static_function;
        Log(std::string("[VR][sky] TAA_PASS_HOOK ready=") +
            (executeReady ? "1" : "0") +
            " args=" +
            (execute != nullptr ? std::to_string(execute->args.size()) : "-"));
        if (executeReady) {
            const bool installed = GakumasVR::Hooks::CreateAndEnable(
                execute->function, reinterpret_cast<void*>(&TaaExecuteDetour),
                reinterpret_cast<void**>(&g_taaExecuteOrig),
                "TemporalAA.ExecutePass");
            Log(std::string("[VR][sky] TAA_PASS_HOOK installed=") +
                (installed ? "1" : "0"));
        }
    }
    // Functional T2x correction uses the same exact live method proven by the
    // earlier diagnostic hook. Install it on ordinary VR startup so selecting
    // T2x from the menu never depends on vrDiagnosticsStartupEnabled.
    auto* motion = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal",
        "MotionVectorsPersistentData");
    auto* update = FindNamed(motion, "Update");
    const bool updateReady =
        update != nullptr && update->function != nullptr &&
        !update->static_function && update->args.size() == 1U;
    Log(std::string("[VR][smaa-t2x] SMAA_T2X_MV_HISTORY_HOOK ready=") +
        (updateReady ? "1" : "0") +
        " args=" +
        (update != nullptr ? std::to_string(update->args.size()) : "-"));
    if (updateReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            update->function, reinterpret_cast<void*>(&MvUpdateDetour),
            reinterpret_cast<void**>(&g_mvUpdateOrig),
            "MotionVectorsPersistentData.Update");
        Log(std::string("[VR][smaa-t2x] SMAA_T2X_MV_HISTORY_HOOK installed=") +
            (installed ? "1" : "0"));
    }
    // dump.cs: internal class VLPostProcessPass lives in
    // VL.Rendering.Internal (the .109 lookup used VL.Rendering and never
    // found it -> EYE_JUMPFLOOD_HOOK ready=0).
    const char* vlPostAssembly =
        "Unity.RenderPipelines.Universal.Runtime.dll";
    auto* vlPost = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "VL.Rendering.Internal", "VLPostProcessPass");
    if (vlPost == nullptr) {
        vlPostAssembly = "vl-unity.Runtime.dll";
        vlPost = FindClass(
            "vl-unity.Runtime.dll", "VL.Rendering.Internal",
            "VLPostProcessPass");
    }
    if (vlPost == nullptr) {
        vlPostAssembly = "Unity.RenderPipelines.Universal.Runtime.dll";
        vlPost = FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "VL.Rendering", "VLPostProcessPass");
    }
    auto* jumpFlood = FindNamed(vlPost, "DoJumpFloodOutline");
    const bool jumpFloodReady =
        jumpFlood != nullptr && jumpFlood->function != nullptr &&
        !jumpFlood->static_function && jumpFlood->args.size() == 4U;
    Log(std::string("[VR][sky] EYE_JUMPFLOOD_HOOK ready=") +
        (jumpFloodReady ? "1" : "0") +
        " args=" +
        (jumpFlood != nullptr ? std::to_string(jumpFlood->args.size())
                              : "-"));
    if (jumpFloodReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            jumpFlood->function,
            reinterpret_cast<void*>(&JumpFloodOutlineDetour),
            reinterpret_cast<void**>(&g_jumpFloodOrig),
            "VLPostProcessPass.DoJumpFloodOutline");
        Log(std::string("[VR][sky] EYE_JUMPFLOOD_HOOK installed=") +
            (installed ? "1" : "0"));
    }
    auto* vlMotionBlur = FindNamed(vlPost, "DoVLMotionBlur");
    const bool vlMotionBlurReady =
        vlMotionBlur != nullptr && vlMotionBlur->function != nullptr &&
        !vlMotionBlur->static_function && vlMotionBlur->args.size() == 3U;
    Log(std::string("[VR][sky] VL_MOTION_BLUR_HOOK ready=") +
        (vlMotionBlurReady ? "1" : "0") +
        " args=" +
        (vlMotionBlur != nullptr
             ? std::to_string(vlMotionBlur->args.size())
             : "-"));
    if (vlMotionBlurReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            vlMotionBlur->function,
            reinterpret_cast<void*>(&VlMotionBlurDetour),
            reinterpret_cast<void**>(&g_vlMotionBlurOrig),
            "VLPostProcessPass.DoVLMotionBlur");
        Log(std::string("[VR][sky] VL_MOTION_BLUR_HOOK installed=") +
            (installed ? "1" : "0"));
    }
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        LogNamedMethodShapes(
            vlPost, "DoVLTextureBlur", "EYE_VL_TEXTURE_BLUR_METHOD");
        LogNamedMethodShapes(vlPost, "DrawFlare", "EYE_VL_FLARE_METHOD");
    }
    auto* vlTextureBlur = FindExactVlTextureBlur(vlPost);
    const bool vlTextureBlurReady = vlTextureBlur != nullptr;
    Log(std::string("[VR][sky] EYE_VL_TEXTURE_BLUR_HOOK ready=") +
        (vlTextureBlurReady ? "1" : "0") + " assembly=" +
        vlPostAssembly +
        " signature=System.Boolean DoVLTextureBlur(" +
        "UnityEngine.Rendering.CommandBuffer," +
        "UnityEngine.Rendering.RTHandle," +
        "UnityEngine.Rendering.RTHandle)");
    if (vlTextureBlurReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            vlTextureBlur->function,
            reinterpret_cast<void*>(&VlTextureBlurDetour),
            reinterpret_cast<void**>(&g_vlTextureBlurOrig),
            "VLPostProcessPass.DoVLTextureBlur");
        Log(std::string("[VR][sky] EYE_VL_TEXTURE_BLUR_HOOK installed=") +
            (installed ? "1" : "0"));
    }
    auto* vlDrawFlare = FindExactVlDrawFlare(vlPost);
    const bool vlDrawFlareReady = vlDrawFlare != nullptr;
    Log(std::string("[VR][sky] EYE_VL_FLARE_HOOK ready=") +
        (vlDrawFlareReady ? "1" : "0") + " assembly=" + vlPostAssembly +
        " signature=System.Boolean DrawFlare(" +
        "UnityEngine.Rendering.CommandBuffer,UnityEngine.Material," +
        "VL.Rendering.FlareSetting,System.Single,System.Single)");
    if (vlDrawFlareReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            vlDrawFlare->function,
            reinterpret_cast<void*>(&VlDrawFlareDetour),
            reinterpret_cast<void**>(&g_vlDrawFlareOrig),
            "VLPostProcessPass.DrawFlare");
        Log(std::string("[VR][sky] EYE_VL_FLARE_HOOK installed=") +
            (installed ? "1" : "0"));
    }
    auto* vlMotion = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "VL.Rendering", "VLMotionVectorRenderPass");
    if (vlMotion == nullptr) {
        vlMotion = FindClass(
            "vl-unity.Runtime.dll", "VL.Rendering",
            "VLMotionVectorRenderPass");
    }
    auto* drawObjectMv = FindNamed(vlMotion, "DrawObjectMotionVectors");
    const bool objectMvReady =
        drawObjectMv != nullptr && drawObjectMv->function != nullptr &&
        drawObjectMv->static_function && drawObjectMv->args.size() == 6U;
    std::string objectMvArgs = "-";
    if (drawObjectMv != nullptr) {
        objectMvArgs = std::to_string(drawObjectMv->args.size());
        for (std::size_t index = 0; index < drawObjectMv->args.size();
             ++index) {
            const auto* arg = drawObjectMv->args[index];
            objectMvArgs += (index == 0U ? " " : ",");
            objectMvArgs += arg != nullptr ? TypeName(arg->pType) : "?";
        }
    }
    Log(std::string("[VR][sky] OBJECT_MV_HOOK ready=") +
        (objectMvReady ? "1" : "0") + " args=" + objectMvArgs);
    if (objectMvReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            drawObjectMv->function,
            reinterpret_cast<void*>(&ObjectMvDetour),
            reinterpret_cast<void**>(&g_objectMvOrig),
            "VLMotionVectorRenderPass.DrawObjectMotionVectors");
        Log(std::string("[VR][sky] OBJECT_MV_HOOK installed=") +
            (installed ? "1" : "0"));
    }
    auto* deferredLights = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal.Internal", "DeferredLights");
    auto* stencilLights = FindNamed(deferredLights, "RenderStencilLights");
    const bool stencilReady =
        stencilLights != nullptr && stencilLights->function != nullptr &&
        !stencilLights->static_function && stencilLights->args.size() == 4U;
    std::string stencilArgs = "-";
    if (stencilLights != nullptr) {
        stencilArgs = std::to_string(stencilLights->args.size());
        for (std::size_t index = 0; index < stencilLights->args.size();
             ++index) {
            const auto* arg = stencilLights->args[index];
            stencilArgs += (index == 0U ? " " : ",");
            stencilArgs += arg != nullptr ? TypeName(arg->pType) : "?";
        }
    }
    Log(std::string("[VR][sky] DEFERRED_STENCIL_HOOK ready=") +
        (stencilReady ? "1" : "0") + " args=" + stencilArgs);
    if (stencilReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            stencilLights->function,
            reinterpret_cast<void*>(&StencilLightsDetour),
            reinterpret_cast<void**>(&g_stencilLightsOrig),
            "DeferredLights.RenderStencilLights");
        Log(std::string("[VR][sky] DEFERRED_STENCIL_HOOK installed=") +
            (installed ? "1" : "0"));
    }
}

int InvokePropertyToIDRaw(
    void* function, void* managed, void* methodInfo) noexcept {
    using Fn2 = int (*)(void*, void*);
    using Fn1 = int (*)(void*);
    int id = 0;
    if (function == nullptr || managed == nullptr) {
        return 0;
    }
    if (methodInfo != nullptr) {
        __try {
            id = reinterpret_cast<Fn2>(function)(managed, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            id = 0;
        }
    }
    if (id == 0) {
        __try {
            id = reinterpret_cast<Fn1>(function)(managed);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            id = 0;
        }
    }
    return id;
}

void* InvokeGetGlobalTextureRaw(
    void* function, int nameId, void* methodInfo) noexcept {
    using Fn2 = void* (*)(int, void*);
    using Fn1 = void* (*)(int);
    void* tex = nullptr;
    if (function == nullptr || nameId == 0) {
        return nullptr;
    }
    if (methodInfo != nullptr) {
        __try {
            tex = reinterpret_cast<Fn2>(function)(nameId, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            tex = nullptr;
        }
    }
    if (tex == nullptr) {
        __try {
            tex = reinterpret_cast<Fn1>(function)(nameId);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            tex = nullptr;
        }
    }
    return tex;
}

int ShaderPropertyToID(const char* name) noexcept {
    if (name == nullptr) {
        return 0;
    }
    auto* klass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Shader");
    auto* method = FindNamed(klass, "PropertyToID");
    if (method == nullptr || method->function == nullptr) {
        return 0;
    }
    void* managed = UnityResolve::Invoke<void*, const char*>(
        "il2cpp_string_new", name);
    return InvokePropertyToIDRaw(method->function, managed, method->address);
}

void* ShaderGetGlobalTexture(int nameId) noexcept {
    auto* klass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Shader");
    auto* method = FindNamed(klass, "GetGlobalTexture");
    if (method == nullptr) {
        return nullptr;
    }
    return InvokeGetGlobalTextureRaw(
        method->function, nameId, method->address);
}

void AppendTextureSize(
    std::ostringstream& stream, const char* name, int nameId) noexcept {
    stream << " " << name << "=";
    if (nameId == 0) {
        stream << "id0";
        return;
    }
    void* tex = ShaderGetGlobalTexture(nameId);
    if (tex == nullptr) {
        stream << "null";
        return;
    }
    auto* texClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Texture");
    stream << InvokeInt(texClass, tex, "get_width") << "x"
           << InvokeInt(texClass, tex, "get_height");
}

void AppendCameraMatrixCompact(
    std::ostringstream& stream,
    void* camera,
    const char* getter,
    const char* tag) noexcept {
    auto* cameraClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
    auto* method = FindNamed(cameraClass, getter);
    float matrix[16]{};
    if (method == nullptr ||
        !GetCameraMatrixInjectedRaw(
            method->function, camera, method->address, matrix)) {
        stream << " " << tag << "=0";
        return;
    }
    stream << " " << tag << "p00=" << matrix[0] << " p02=" << matrix[8]
           << " p11=" << matrix[5] << " p12=" << matrix[9] << " p22="
           << matrix[10];
}

} // namespace

void ObserveSmaaT2xRenderPass(
    void* renderPass,
    void* renderContext,
    void* renderingData,
    int renderPassEvent,
    UnityStereoRenderer& renderer) noexcept {
    const bool diagnostics = GakumasLocal::Config::vrDiagnosticsStartupEnabled;
    const int eyeAaMode = GakumasLocal::Config::vrEyeAaMode;
    const bool functional = GakumasLocal::Config::vrRuntimeStartupEnabled &&
        (eyeAaMode == 4 || eyeAaMode == 5);
    if ((!diagnostics && !functional) || renderPass == nullptr ||
        renderingData == nullptr) {
        return;
    }
    ResolveSmaaT2xProbeApiOnce();
    ResolveSkyRuntimeDataLayout();
    const auto& api = g_smaaT2xProbeApi;
    if (!api.ready || !g_fieldsReady || g_cameraDataOffset < 16) {
        return;
    }

    const CameraDataFields fields = Unboxed(g_fields);
    void* cameraData = AddBytes(renderingData, g_cameraDataOffset - 16);
    void* camera = ReadPointer(cameraData, fields.camera);
    if (camera == nullptr || !renderer.IsEyeCamera(camera)) {
        return;
    }
    const char* role = renderer.ClassifyCamera(camera);
    const std::size_t eye = std::string_view(role) == "right" ? 1U : 0U;
    void* instanceClass = ReadPointer(renderPass, 0);
    if (!IsClassOrSubclass(instanceClass, api.postProcessPass->address)) {
        return;
    }
    void* rtHandle = ReadPointer(renderPass, api.motionVectors->offset);
    void* renderTexture = RuntimeInvokeZeroArg(api.getRt, rtHandle, false);
    void* nativePointer = RuntimeInvokeZeroArg(
        api.getNativeTexturePtr, renderTexture, true);
    if (functional) {
        (void)renderer.QueueSmaaT2xMotionVectorCopy(
            camera, renderContext, renderTexture, renderPassEvent);
    }
    if (!diagnostics || g_smaaT2xProbeSamples[eye] >= 12U) {
        return;
    }
    ++g_smaaT2xProbeSamples[eye];
    const std::uint64_t serial = ++g_smaaT2xPassSerial;
    const SmaaT2xNativeDescription native =
        DescribeSmaaT2xNativeTexture(nativePointer);
    const std::size_t other = eye == 0U ? 1U : 0U;

    std::ostringstream line;
    line << "[VR][smaa-t2x] SMAA_T2X_MV_CAPTURE_PROBE serial=" << serial
         << " eye=" << role
         << " sample=" << g_smaaT2xProbeSamples[eye]
         << " event=" << renderPassEvent
         << " passType=" << LiveClassName(instanceClass)
         << " pass=" << renderPass
         << " camera=" << camera
         << " currentCameraMatch="
         << (camera == renderer.CurrentCamera() ? 1 : 0)
         << " rtHandle=" << rtHandle
         << " renderTexture=" << renderTexture
         << " native=" << nativePointer
         << " d3d=" << native.identity
         << " d3dReady=" << (native.ready ? 1 : 0)
         << " size=" << native.width << 'x' << native.height
         << " format=" << native.format
         << " samples=" << native.samples
         << " sameHandleOther="
         << (rtHandle != nullptr && rtHandle == g_smaaT2xLastRtHandles[other]
                 ? 1
                 : 0)
         << " sameRenderTextureOther="
         << (renderTexture != nullptr &&
                     renderTexture == g_smaaT2xLastRenderTextures[other]
                 ? 1
                 : 0)
         << " sameNativeOther="
         << (nativePointer != nullptr &&
                     nativePointer == g_smaaT2xLastNativePointers[other]
                 ? 1
                 : 0)
         << " sameD3dOther="
         << (native.identity != nullptr &&
                     native.identity == g_smaaT2xLastD3dResources[other]
                 ? 1
                 : 0)
         << " stableHandleThisEye="
         << (rtHandle != nullptr && rtHandle == g_smaaT2xLastRtHandles[eye]
                 ? 1
                 : 0)
         << " stableD3dThisEye="
         << (native.identity != nullptr &&
                     native.identity == g_smaaT2xLastD3dResources[eye]
                 ? 1
                 : 0);
    Log(line.str());

    g_smaaT2xLastRtHandles[eye] = rtHandle;
    g_smaaT2xLastRenderTextures[eye] = renderTexture;
    g_smaaT2xLastNativePointers[eye] = nativePointer;
    g_smaaT2xLastD3dResources[eye] = native.identity;
}

void NoteSkyPassExecute(
    void* pass, void* renderingData, UnityStereoRenderer& renderer) noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    DumpSkyPassApiOnce();
    void* camera = renderer.CurrentCamera();
    if (camera == nullptr) {
        auto* cameraClass =
            FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
        camera = InvokePtr(cameraClass, nullptr, "get_current");
    }
    const char* role = renderer.ClassifyCamera(camera);
    const int index = RoleIndex(role);
    if (g_samples[static_cast<std::size_t>(index)] >= kSamplesPerRole) {
        g_pendingPass = nullptr;
        return;
    }

    auto* cameraClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
    auto* rtClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture");
    const float fov = InvokeFloat(cameraClass, camera, "get_fieldOfView");
    const float aspect = InvokeFloat(cameraClass, camera, "get_aspect");
    const int pixelW = InvokeInt(cameraClass, camera, "get_pixelWidth");
    const int pixelH = InvokeInt(cameraClass, camera, "get_pixelHeight");
    void* target = InvokePtr(cameraClass, camera, "get_targetTexture");
    const int targetW = InvokeInt(rtClass, target, "get_width");
    const int targetH = InvokeInt(rtClass, target, "get_height");
    const bool warmup = pixelW < 512 || pixelH < 512;
    const bool otherRole = std::string_view(role) == "other";
    auto& samples = g_samples[static_cast<std::size_t>(index)];
    if (warmup || otherRole) {
        if (samples >= 2U) {
            if (otherRole) {
                samples = kSamplesPerRole;
            }
            g_pendingPass = nullptr;
            return;
        }
    } else {
        auto& interesting = g_interesting[static_cast<std::size_t>(index)];
        ++interesting;
        if (samples >= kBurstSamples &&
            (interesting % kLaterStride) != 0U) {
            g_pendingPass = nullptr;
            return;
        }
    }
    ++samples;
    g_pendingPass = pass;

    std::ostringstream stream;
    stream << "[VR][sky] SKY_PASS_EXECUTE role=" << role
           << " name=" << CameraName(camera) << " fov=" << fov
           << " aspect=" << aspect << " pixel=" << pixelW << "x" << pixelH
           << " target=" << targetW << "x" << targetH;
    DescribeScaledSize(FindSkyPass(), stream);
    Log(stream.str());
    LogEmbeddedCameraData(
        renderingData, camera, target, role, pixelW, pixelH, fov, aspect);
}

void NoteSkyPassAfterExecute(void* pass) noexcept {
    if (pass == nullptr || pass != g_pendingPass) {
        return;
    }
    g_pendingPass = nullptr;
    LogViewDirMatrix(pass);
    DumpSkyMaterial(pass);
}

bool ShouldSkipOwnedEyeSky() noexcept {
    return CurrentEyeStash() != nullptr;
}

void LogSkipPass(const char* tag) noexcept {
    if (g_renderer == nullptr || tag == nullptr) {
        return;
    }
    const char* role = g_renderer->ClassifyCamera(g_renderer->CurrentCamera());
    const int slot = EyeSlot(role);
    if (slot < 0 ||
        !ShouldLogWrite(g_skipLogs[static_cast<std::size_t>(slot)])) {
        return;
    }
    auto* cameraClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
    void* camera = g_renderer->CurrentCamera();
    const int clearFlags = InvokeInt(cameraClass, camera, "get_clearFlags");
    EyeViewStash* stash = CurrentEyeStash();
    std::ostringstream stream;
    stream << "[VR][sky] " << tag << " role=" << (role != nullptr ? role : "?")
           << " clear=" << clearFlags
           << " pix=" << (stash != nullptr ? stash->pixW : 0) << "x"
           << (stash != nullptr ? stash->pixH : 0);
    Log(stream.str());
}

void CameraWorldFromView(const float view[16], float pos[3]) noexcept {
    if (view == nullptr || pos == nullptr) {
        return;
    }
    const float tx = view[12];
    const float ty = view[13];
    const float tz = view[14];
    pos[0] = -(view[0] * tx + view[1] * ty + view[2] * tz);
    pos[1] = -(view[4] * tx + view[5] * ty + view[6] * tz);
    pos[2] = -(view[8] * tx + view[9] * ty + view[10] * tz);
}

bool ShaderLooksLikeSky(void* material) noexcept {
    if (material == nullptr) {
        return false;
    }
    auto* matClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Material");
    void* shader = InvokePtr(matClass, material, "get_shader");
    const std::string name = ObjectName(shader);
    return name.find("VL/Sky") != std::string::npos ||
        name.find("Skybox") != std::string::npos;
}

void LogDrawMesh(
    void* mesh, void* material, float oldX, float oldY, float oldZ,
    const float cam[3], bool ok) noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    const int slot = EyeSlot(
        g_renderer != nullptr
            ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
            : nullptr);
    if (slot < 0 ||
        !ShouldLogWrite(g_drawLogs[static_cast<std::size_t>(slot)])) {
        return;
    }
    auto* matClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Material");
    void* shader = InvokePtr(matClass, material, "get_shader");
    std::ostringstream stream;
    stream << "[VR][sky] DRAW_MESH role="
           << (g_renderer != nullptr
                   ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
                   : "?")
           << " shader=" << ObjectName(shader)
           << " mesh=" << ObjectName(mesh)
           << " t=" << oldX << "," << oldY << "," << oldZ
           << " cam=" << cam[0] << "," << cam[1] << "," << cam[2]
           << " recenter=" << (ok ? "1" : "0");
    Log(stream.str());
}

void MaybeRecenterSkyMatrix(void* matrix, void* mesh, void* material) noexcept {
    if (!kRecenterSkyDraw || !g_inEyeSkyExecute || matrix == nullptr) {
        return;
    }
    EyeViewStash* stash = CurrentEyeStash();
    if (stash == nullptr || !ShaderLooksLikeSky(material)) {
        return;
    }
    float local[16]{};
    if (!CopyMatrixBytes(matrix, local)) {
        return;
    }
    float cam[3]{};
    CameraWorldFromView(stash->view, cam);
    const float oldX = local[12];
    const float oldY = local[13];
    const float oldZ = local[14];
    local[12] = cam[0];
    local[13] = cam[1];
    local[14] = cam[2];
    const bool ok = WriteMatrixBytes(local, matrix);
    LogDrawMesh(mesh, material, oldX, oldY, oldZ, cam, ok);
}

void DrawMeshDetour(
    void* cmd, void* mesh, void* matrix, void* material, int submesh, int pass,
    void* mpb, void* method) {
    __try {
        MaybeRecenterSkyMatrix(matrix, mesh, material);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (g_drawMeshOrig != nullptr) {
        g_drawMeshOrig(cmd, mesh, matrix, material, submesh, pass, mpb, method);
    }
}

void InstallDrawMeshHook() noexcept {
    if (g_drawMeshHookInstalled) {
        return;
    }
    g_drawMeshHookInstalled = true;
    auto* cmdClass = FindClass(
        "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "CommandBuffer");
    UnityResolve::Method* injected = nullptr;
    if (cmdClass != nullptr) {
        for (auto* method : cmdClass->methods) {
            if (method != nullptr && method->function != nullptr &&
                method->name == "DrawMesh_Injected" &&
                method->args.size() >= 6U) {
                injected = method;
                if (method->args.size() == 7U) {
                    break;
                }
            }
        }
    }
    const bool ready = injected != nullptr && injected->function != nullptr &&
        injected->static_function && injected->args.size() == 7U;
    std::ostringstream stream;
    stream << "[VR][sky] DRAW_MESH_HOOK ready=" << (ready ? "1" : "0");
    if (injected != nullptr) {
        stream << " static=" << (injected->static_function ? "1" : "0")
               << " args=" << injected->args.size();
        for (std::size_t index = 0; index < injected->args.size(); ++index) {
            stream << (index == 0U ? " types=" : ",")
                   << (injected->args[index] != nullptr
                           ? TypeName(injected->args[index]->pType)
                           : "?");
        }
    }
    Log(stream.str());
    if (!ready) {
        return;
    }
    const bool installed = GakumasVR::Hooks::CreateAndEnable(
        injected->function, reinterpret_cast<void*>(&DrawMeshDetour),
        reinterpret_cast<void**>(&g_drawMeshOrig),
        "CommandBuffer.DrawMesh_Injected");
    Log(std::string("[VR][sky] DRAW_MESH_HOOK installed=") +
        (installed ? "1" : "0"));
}

bool CopyStashMatrixSeh(Il2CppMat4* ret, const float matrix[16]) noexcept {
    if (ret == nullptr || matrix == nullptr) {
        return false;
    }
    bool ok = false;
    __try {
        std::memcpy(ret->m, matrix, sizeof(ret->m));
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

bool TryReplaceComputeRet(Il2CppMat4* ret, const char* via) noexcept {
    if (!kReplaceComputeViewDir || ret == nullptr || !g_inEyeSkyExecute) {
        return false;
    }
    EyeViewStash* stash = CurrentEyeStash();
    if (stash == nullptr) {
        return false;
    }
    const bool ok = CopyStashMatrixSeh(ret, stash->matrix);
    const char* role = g_renderer != nullptr
        ? g_renderer->ClassifyCamera(g_renderer->CurrentCamera())
        : "?";
    const int slot = EyeSlot(role);
    if (slot >= 0 &&
        ShouldLogWrite(g_computeLogs[static_cast<std::size_t>(slot)])) {
        std::ostringstream stream;
        stream << "[VR][sky] COMPUTE_VIEW_DIR role="
               << (role != nullptr ? role : "?")
               << " via=" << (via != nullptr ? via : "?")
               << " replaced=" << (ok ? "1" : "0")
               << " p02=" << stash->p02 << " p12=" << stash->p12
               << " fov=" << stash->fov
               << " pix=" << stash->pixW << "x" << stash->pixH;
        Log(stream.str());
    }
    return ok;
}

Il2CppMat4* __fastcall ComputeCamDetour(
    Il2CppMat4* ret,
    void* camera,
    const Il2CppVec4* resolution,
    Il2CppMat4* view,
    Il2CppMat4* gpuProj,
    void* methodInfo) {
    Il2CppMat4* result = nullptr;
    if (g_computeCamOrig != nullptr) {
        result = g_computeCamOrig(
            ret, camera, resolution, view, gpuProj, methodInfo);
    }
    TryReplaceComputeRet(ret, "cam");
    return result != nullptr ? result : ret;
}

Il2CppMat4* __fastcall ComputeFovDetour(
    Il2CppMat4* ret,
    float verticalFoV,
    Il2CppVec2 lensShift,
    const Il2CppVec4* screenSize,
    const Il2CppMat4* worldToView,
    bool renderToCubemap,
    float aspectRatio,
    bool isOrthographic,
    void* methodInfo) {
    Il2CppVec2 usedShift = lensShift;
    if (kReplaceComputeViewDir && g_inEyeSkyExecute) {
        EyeViewStash* stash = CurrentEyeStash();
        if (stash != nullptr) {
            usedShift.x = 0.5F * stash->p02;
            usedShift.y = 0.5F * stash->p12;
        }
    }
    Il2CppMat4* result = nullptr;
    if (g_computeFovOrig != nullptr) {
        result = g_computeFovOrig(
            ret, verticalFoV, usedShift, screenSize, worldToView,
            renderToCubemap, aspectRatio, isOrthographic, methodInfo);
    }
    TryReplaceComputeRet(ret, "fov");
    return result != nullptr ? result : ret;
}

UnityResolve::Method* FindComputeCamera(UnityResolve::Class* klass) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method == nullptr || !method->static_function ||
            method->function == nullptr ||
            method->name !=
                "ComputePixelCoordToWorldSpaceViewDirectionMatrix" ||
            method->args.size() != 4U || method->args[0] == nullptr ||
            method->args[0]->pType == nullptr) {
            continue;
        }
        if (TypeName(method->args[0]->pType).find("Camera") !=
            std::string::npos) {
            return method;
        }
    }
    return nullptr;
}

UnityResolve::Method* FindComputeFov(UnityResolve::Class* klass) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method == nullptr || !method->static_function ||
            method->function == nullptr ||
            method->name !=
                "ComputePixelCoordToWorldSpaceViewDirectionMatrix" ||
            method->args.size() != 7U || method->args[0] == nullptr ||
            method->args[0]->pType == nullptr) {
            continue;
        }
        if (TypeName(method->args[0]->pType).find("Single") !=
            std::string::npos) {
            return method;
        }
    }
    return nullptr;
}

void InstallComputeViewDirHooks(UnityResolve::Class* klass) noexcept {
    if (g_computeHooksInstalled || klass == nullptr) {
        return;
    }
    g_computeHooksInstalled = true;
    auto* cam = FindComputeCamera(klass);
    auto* fov = FindComputeFov(klass);
    const bool camReady = cam != nullptr && cam->function != nullptr;
    const bool fovReady = fov != nullptr && fov->function != nullptr;
    Log(std::string("[VR][sky] COMPUTE_VIEW_DIR_HOOK camReady=") +
        (camReady ? "1" : "0") + " fovReady=" + (fovReady ? "1" : "0") +
        " replace=" + (kReplaceComputeViewDir ? "1" : "0"));
    if (camReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            cam->function, reinterpret_cast<void*>(&ComputeCamDetour),
            reinterpret_cast<void**>(&g_computeCamOrig),
            "VLSkyPass.ComputeViewDir.Camera");
        Log(std::string("[VR][sky] COMPUTE_VIEW_DIR_HOOK camInstalled=") +
            (installed ? "1" : "0"));
    }
    if (fovReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            fov->function, reinterpret_cast<void*>(&ComputeFovDetour),
            reinterpret_cast<void**>(&g_computeFovOrig),
            "VLSkyPass.ComputeViewDir.Fov");
        Log(std::string("[VR][sky] COMPUTE_VIEW_DIR_HOOK fovInstalled=") +
            (installed ? "1" : "0"));
    }
}

void ExecuteDetour(void* self, void* context, void* renderingData, void* method) {
    if (g_renderer != nullptr) {
        __try {
            StashEyeViewDir(self, renderingData);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        __try {
            NoteSkyPassExecute(self, renderingData, *g_renderer);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    bool skip = false;
    __try {
        skip = kSkipEyeSkyPass && ShouldSkipOwnedEyeSky();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        skip = false;
    }
    if (skip) {
        __try {
            LogSkipPass("SKIP_SKY_PASS");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        __try {
            NoteSkyPassAfterExecute(self);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return;
    }
    __try {
        WriteStashedViewDir(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_inEyeSkyExecute = ShouldSkipOwnedEyeSky();
    if (g_executeOrig != nullptr) {
        g_executeOrig(self, context, renderingData, method);
    }
    g_inEyeSkyExecute = false;
    __try {
        NoteSkyPassAfterExecute(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void DrawSkyboxExecuteDetour(
    void* self, void* context, void* renderingData, void* method) {
    (void)self;
    if (g_renderer != nullptr && renderingData != nullptr) {
        __try {
            StashEyeViewDir(nullptr, renderingData);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    bool skip = false;
    __try {
        skip = ShouldSkipOwnedEyeSky();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        skip = false;
    }
    if (skip) {
        __try {
            LogSkipPass("SKIP_URP_SKYBOX");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return;
    }
    if (g_skyboxOrig != nullptr) {
        g_skyboxOrig(self, context, renderingData, method);
    }
}

void NoteDrawClouds(void* cloud) noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled || g_renderer == nullptr) {
        return;
    }
    const char* role = g_renderer->ClassifyCamera(g_renderer->CurrentCamera());
    const int index = RoleIndex(role);
    if (g_samples[static_cast<std::size_t>(index)] >= kSamplesPerRole) {
        return;
    }
    Log(std::string("[VR][sky] SKY_PASS_DRAW role=") + role +
        " cloud=" + (cloud != nullptr ? "1" : "0"));
    if (cloud == nullptr) {
        return;
    }
    auto* vlCloud = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering",
        "VLCloud");
    if (vlCloud == nullptr) {
        vlCloud = FindClass("vl-unity.Runtime.dll", "VL.Rendering", "VLCloud");
    }
    if (vlCloud == nullptr) {
        return;
    }
    static bool parameterLayoutDumped = false;
    const auto* dataField = vlCloud->Get<UnityResolve::Field>("cloudData");
    const auto* speedField = vlCloud->Get<UnityResolve::Field>("multiplySpeed");
    const auto* offsetField = vlCloud->Get<UnityResolve::Field>("cloudOffsetY");
    const auto* colorField = vlCloud->Get<UnityResolve::Field>("cloudEmissionColor");
    void* dataParam = dataField != nullptr
        ? ReadPointer(cloud, dataField->offset)
        : nullptr;
    void* speedParam = speedField != nullptr
        ? ReadPointer(cloud, speedField->offset)
        : nullptr;
    void* offsetParam = offsetField != nullptr
        ? ReadPointer(cloud, offsetField->offset)
        : nullptr;
    void* colorParam = colorField != nullptr
        ? ReadPointer(cloud, colorField->offset)
        : nullptr;
    if (!parameterLayoutDumped) {
        parameterLayoutDumped = true;
        LogInheritedFields(ReadPointer(dataParam, 0), "VPARAM_CLOUDDATA");
        LogInheritedFields(ReadPointer(speedParam, 0), "VPARAM_FLOAT");
        LogInheritedFields(ReadPointer(colorParam, 0), "VPARAM_COLOR");
    }
    bool speedOk = false;
    bool offsetOk = false;
    const float multiplySpeed = VolumeValueFloat(speedParam, &speedOk);
    const float cloudOffsetY = VolumeValueFloat(offsetParam, &offsetOk);
    const std::int32_t colorValueOff = ResolveVolumeField(
        colorParam, {"m_Value", "value"}, {});
    std::ostringstream volume;
    volume << "[VR][sky] VLCLOUD role=" << role
           << " dataParam=" << (dataParam != nullptr ? "1" : "0")
           << " dataOv=" << VolumeOverride(dataParam)
           << " speed=" << multiplySpeed << " speedOk=" << (speedOk ? "1" : "0")
           << " speedOv=" << VolumeOverride(speedParam)
           << " offsetY=" << cloudOffsetY << " offsetOk=" << (offsetOk ? "1" : "0");
    if (colorValueOff >= 0) {
        volume << " emit=" << ReadFloat(colorParam, colorValueOff) << ","
               << ReadFloat(colorParam, colorValueOff + 4) << ","
               << ReadFloat(colorParam, colorValueOff + 8) << ","
               << ReadFloat(colorParam, colorValueOff + 12);
    }
    Log(volume.str());

    void* data = VolumeValueObject(dataParam);
    if (data == nullptr) {
        Log("[VR][sky] CLOUDDATA missing value");
        return;
    }
    auto* cloudData = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering",
        "CloudData");
    if (cloudData == nullptr) {
        cloudData = FindClass("vl-unity.Runtime.dll", "VL.Rendering", "CloudData");
    }
    LogReadableFields(cloudData, data, "CLOUDDATA");
    const auto* layersField = cloudData != nullptr
        ? cloudData->Get<UnityResolve::Field>("cloudLayers")
        : nullptr;
    void* layers = layersField != nullptr
        ? ReadPointer(data, layersField->offset)
        : ReadPointer(data, 24);
    if (layers == nullptr) {
        Log("[VR][sky] CLOUDLAYER missing array");
        return;
    }
    const int count = ReadInt(layers, 24);
    auto* layer = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering",
        "CloudLayer");
    if (layer == nullptr) {
        layer = FindClass("vl-unity.Runtime.dll", "VL.Rendering", "CloudLayer");
    }
    const bool valueType = layer != nullptr && layer->address != nullptr &&
        UnityResolve::Invoke<bool>("il2cpp_class_is_valuetype", layer->address);
    int instanceSize = 0;
    if (layer != nullptr && layer->address != nullptr) {
        instanceSize = UnityResolve::Invoke<int>(
            "il2cpp_class_instance_size", layer->address);
    }
    Log("[VR][sky] CLOUDLAYER role=" + std::string(role) +
        " count=" + std::to_string(count) +
        " valuetype=" + (valueType ? "1" : "0") +
        " instanceSize=" + std::to_string(instanceSize));
    if (count <= 0 || count > 16) {
        return;
    }
    static bool layerLayoutDumped = false;
    const int stride = valueType
        ? (instanceSize > 16 ? instanceSize - 16 : (instanceSize > 0 ? instanceSize : 16))
        : 8;
    for (int indexLayer = 0; indexLayer < count; ++indexLayer) {
        if (!valueType) {
            void* layerPtr = ReadPointer(layers, 32 + indexLayer * 8);
            if (layerPtr == nullptr) {
                Log("[VR][sky] CLOUDLAYER i=" + std::to_string(indexLayer) +
                    " ptr=0");
                continue;
            }
            if (!layerLayoutDumped) {
                layerLayoutDumped = true;
                LogInheritedFields(ReadPointer(layerPtr, 0), "CLOUDLAYER_LIVE");
                if (layer != nullptr) {
                    LogReadableFields(layer, layerPtr, "CLOUDLAYER");
                }
            }
            const float rotate = ReadFloat(layerPtr, 24);
            void* mesh = ReadPointer(layerPtr, 16);
            auto* meshClass =
                FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh");
            Log("[VR][sky] CLOUDLAYER i=" + std::to_string(indexLayer) +
                " rotateSpeed=" + std::to_string(rotate) +
                " mesh=" + (mesh != nullptr ? "1" : "0") +
                " meshName=" + ObjectName(mesh) + " verts=" +
                std::to_string(InvokeInt(meshClass, mesh, "get_vertexCount")));
            continue;
        }
        const int base = 32 + indexLayer * stride;
        const float rotate = ReadFloat(layers, base + 8);
        void* mesh = ReadPointer(layers, base);
        auto* meshClass =
            FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Mesh");
        Log("[VR][sky] CLOUDLAYER i=" + std::to_string(indexLayer) +
            " layout=inline rotateSpeed=" + std::to_string(rotate) +
            " mesh=" + (mesh != nullptr ? "1" : "0") +
            " meshName=" + ObjectName(mesh) + " verts=" +
            std::to_string(InvokeInt(meshClass, mesh, "get_vertexCount")) +
            " stride=" + std::to_string(stride));
    }
}

void DrawCloudsDetour(void* self, void* first, void* second, void* method) {
    __try {
        WriteStashedViewDir(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        NoteDrawClouds(second);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (g_drawOrig != nullptr) {
        g_drawOrig(self, first, second, method);
    }
}

void EnsureSkyRenderHooks(UnityStereoRenderer& renderer) noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) {
        return;
    }
    g_renderer = &renderer;
    // The accepted sky ComputeViewDir replacement and JumpFlood temporal-off
    // both consume live RenderingData/CameraData offsets. Resolve those fields
    // on the ordinary runtime path; diagnostic dumps must never own functional
    // state initialization.
    ResolveSkyRuntimeDataLayout();
    // Functional VL post-process detours are ordinary VR runtime behavior.
    // Do not route their installation through a diagnostic dump: .153-.155
    // did so indirectly via DumpTaaTypesOnce(), leaving menu state live while
    // DrawFlare/VLMotionBlur had no installed consumer when diagnostics=0.
    InstallVlPostProcessHooks();
    // Read-only rooftop/sky-boundary probe. Resolve the exact live
    // FogUtility signatures and field table only under the frozen diagnostic
    // startup gate; never let this probe own functional VR initialization.
    InstallFogDiagnosticHooks();
    DumpSkyPassApiOnce();
    if (g_hooksInstalled) {
        return;
    }
    auto* klass = FindSkyPass();
    if (klass == nullptr) {
        return;
    }
    g_hooksInstalled = true;
    auto* execute = FindExecute(klass);
    const bool executeReady = execute != nullptr && execute->function != nullptr;
    Log(std::string("[VR][sky] SKY_PASS_EXECUTE_HOOK ready=") +
        (executeReady ? "1" : "0"));
    if (executeReady) {
        const bool installed = GakumasVR::Hooks::CreateAndEnable(
            execute->function, reinterpret_cast<void*>(&ExecuteDetour),
            reinterpret_cast<void**>(&g_executeOrig), "VLSkyPass.Execute");
        Log(std::string("[VR][sky] SKY_PASS_EXECUTE_HOOK installed=") +
            (installed ? "1" : "0"));
    }
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        auto* draw = FindNamed(klass, "DrawClouds");
        const bool drawReady = draw != nullptr && !draw->static_function &&
            draw->function != nullptr && draw->args.size() == 2U;
        Log(std::string("[VR][sky] SKY_PASS_DRAW_HOOK ready=") +
            (drawReady ? "1" : "0"));
        if (drawReady) {
            const bool installed = GakumasVR::Hooks::CreateAndEnable(
                draw->function, reinterpret_cast<void*>(&DrawCloudsDetour),
                reinterpret_cast<void**>(&g_drawOrig), "VLSkyPass.DrawClouds");
            Log(std::string("[VR][sky] SKY_PASS_DRAW_HOOK installed=") +
                (installed ? "1" : "0"));
        }
    }
    if (!g_skyboxHookInstalled) {
        g_skyboxHookInstalled = true;
        auto* skybox = FindClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "UnityEngine.Rendering.Universal", "DrawSkyboxPass");
        auto* skyboxExecute = FindExecute(skybox);
        const bool skyboxReady =
            skyboxExecute != nullptr && skyboxExecute->function != nullptr;
        Log(std::string("[VR][sky] SKIP_URP_SKYBOX hookReady=") +
            (skyboxReady ? "1" : "0"));
        if (skyboxReady) {
            const bool installed = GakumasVR::Hooks::CreateAndEnable(
                skyboxExecute->function,
                reinterpret_cast<void*>(&DrawSkyboxExecuteDetour),
                reinterpret_cast<void**>(&g_skyboxOrig),
                "DrawSkyboxPass.Execute");
            Log(std::string("[VR][sky] SKIP_URP_SKYBOX installed=") +
                (installed ? "1" : "0"));
        }
    }
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        InstallDrawMeshHook();
        InstallSetMatrixHook();
    }
    InstallComputeViewDirHooks(klass);
}

void NoteBeginEyeSky(void* camera) noexcept {
    if (g_renderer == nullptr || camera == nullptr) {
        return;
    }
    const int slot = EyeSlot(g_renderer->ClassifyCamera(camera));
    if (slot < 0) {
        return;
    }
    auto* cameraClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
    if (kForceEyeSolidClear) {
        static void* setFlagsFn = nullptr;
        static void* setBgFn = nullptr;
        static bool resolved = false;
        if (!resolved) {
            resolved = true;
            auto* setFlags = FindNamed(cameraClass, "set_clearFlags");
            if (setFlags != nullptr) {
                setFlagsFn = setFlags->function;
            }
            if (cameraClass != nullptr) {
                for (auto* method : cameraClass->methods) {
                    if (method != nullptr && method->function != nullptr &&
                        (method->name == "set_backgroundColor_Injected" ||
                         method->name == "SetBackgroundColor_Injected")) {
                        setBgFn = method->function;
                        break;
                    }
                }
            }
        }
        InvokeSetIntRaw(setFlagsFn, camera, 2);
        float black[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        InvokeSetColorRaw(setBgFn, camera, black);
    }
    const int after = InvokeInt(cameraClass, camera, "get_clearFlags");
    if (!ShouldLogWrite(g_clearLogs[static_cast<std::size_t>(slot)])) {
        return;
    }
    Log("[VR][sky] CLEAR_EYE_SKYBOX role=" +
        std::string(g_renderer->ClassifyCamera(camera)) +
        " after=" + std::to_string(after) +
        " write=" + (kForceEyeSolidClear ? "1" : "0"));
}

void ResetSkyTaaSamples() noexcept {
    g_samples.fill(0);
    g_interesting.fill(0);
    g_taaInputBurst = 8;
    g_taaInputStride = 0;
    g_taaPassBurst = 12U;
    g_taaPassStride = 0U;
    g_taaPassNulls = 0U;
    g_mvUpdateBurst = 8U;
    g_jumpFloodBurst = 12U;
    g_jumpFloodStride = 0U;
    g_vlMotionBlurBurst = 12U;
    g_vlMotionBlurStride = 0U;
    g_objectMvBurst = 12U;
    g_objectMvStride = 0U;
    g_stencilLightsBurst = 12U;
    g_stencilLightsStride = 0U;
}

void LogTaaForensics(
    void* camera,
    void* additionalData,
    const char* role,
    bool suppressed) noexcept {
    DumpTaaTypesOnce();
    if (g_taaInputBurst > 0U) {
        --g_taaInputBurst;
    } else if ((++g_taaInputStride % 90U) != 0U) {
        return;
    }
    if (camera == nullptr) {
        return;
    }
    auto* addClass = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal",
        "UniversalAdditionalCameraData");
    auto* taaPersistClass = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal",
        "TaaPersistentData");
    auto* motionPersistClass = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal",
        "MotionVectorsPersistentData");
    void* taaPersist = nullptr;
    void* motionPersist = nullptr;
    int resetHistory = -1;
    int antialiasing = -1;
    int depthMode = -1;
    int taaQuality = 0;
    float taaJitterScale = 0.0F;
    int taaResetFrames = 0;
    int taaJitterOff = 0;
    if (additionalData != nullptr && addClass != nullptr) {
        auto* resetMethod = FindNamed(addClass, "get_resetHistory");
        auto* aaMethod = FindNamed(addClass, "get_antialiasing");
        auto* taaMethod = FindNamed(addClass, "get_taaPersistentData");
        auto* motionMethod = FindNamed(addClass, "get_motionVectorsPersistentData");
        if (resetMethod != nullptr) {
            resetHistory = resetMethod->Invoke<bool>(additionalData) ? 1 : 0;
        }
        if (aaMethod != nullptr) {
            antialiasing = aaMethod->Invoke<int>(additionalData);
        }
        if (taaMethod != nullptr) {
            taaPersist = taaMethod->Invoke<void*>(additionalData);
        }
        if (motionMethod != nullptr) {
            motionPersist = motionMethod->Invoke<void*>(additionalData);
        }
        const auto* settingsField = addClass->Get<UnityResolve::Field>("m_TaaSettings");
        if (settingsField != nullptr && settingsField->offset >= 0) {
            taaQuality = ReadInt(additionalData, settingsField->offset);
            taaJitterScale = ReadFloat(additionalData, settingsField->offset + 8);
            taaResetFrames = ReadInt(additionalData, settingsField->offset + 24);
            taaJitterOff = ReadInt(additionalData, settingsField->offset + 28);
        }
    }
    auto* cameraClass =
        FindClass("UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
    depthMode = InvokeInt(cameraClass, camera, "get_depthTextureMode");
    if (!g_taaPersistLogged) {
        g_taaPersistLogged = true;
        if (taaPersist != nullptr) {
            LogReadableFields(taaPersistClass, taaPersist, "TAA_PERSIST");
        }
        if (motionPersist != nullptr) {
            LogReadableFields(motionPersistClass, motionPersist, "TAA_MOTION_PERSIST");
        }
    }
    const int prevVpId = ShaderPropertyToID("_PrevViewProjMatrix");
    const int nonJitteredVpId = ShaderPropertyToID("_NonJitteredViewProjMatrix");
    const int viewProjId = ShaderPropertyToID("_ViewProjMatrix");
    const int taaMvId = ShaderPropertyToID("_TaaMotionVectorTex");
    const int camMvId = ShaderPropertyToID("_CameraMotionVectorsTexture");
    const int depthId = ShaderPropertyToID("_CameraDepthTexture");
    const int accumId = ShaderPropertyToID("_TaaAccumulationTex");
    float prevVp[16]{};
    float nonJitteredVp[16]{};
    float viewProj[16]{};
    const bool havePrevVp = TryGetGlobalMatrix(prevVpId, prevVp);
    const bool haveNonJitteredVp = TryGetGlobalMatrix(nonJitteredVpId, nonJitteredVp);
    const bool haveViewProj = TryGetGlobalMatrix(viewProjId, viewProj);
    std::ostringstream stream;
    stream << "[VR][sky] TAA_INPUT role=" << (role != nullptr ? role : "?")
           << " suppressed=" << (suppressed ? "1" : "0")
           << " aa=" << antialiasing
           << " resetHistory=" << resetHistory
           << " depthMode=" << depthMode
           << " taaQ=" << taaQuality
           << " taaJS=" << taaJitterScale
           << " taaResetFrames=" << taaResetFrames
           << " taaJitterOff=" << taaJitterOff
           << " lastAccum=" << (taaPersist != nullptr
                                    ? ReadInt(taaPersist, 88)
                                    : -1)
           << " taaPersist=0x" << std::hex
           << reinterpret_cast<std::uintptr_t>(taaPersist)
           << " motionPersist=0x"
           << reinterpret_cast<std::uintptr_t>(motionPersist)
           << std::dec;
    // MotionVectorsPersistentData is the real prev-VP source for TAA and
    // motion vectors (.100). Sample it per line so a stale / identity /
    // cross-camera value shows up directly in the trail diagnostics.
    if (motionPersist != nullptr && motionPersistClass != nullptr) {
        const auto* lastFrameField =
            motionPersistClass->Get<UnityResolve::Field>("m_LastFrameIndex");
        if (lastFrameField != nullptr && lastFrameField->offset >= 0) {
            stream << " mvpdFrame="
                   << ReadInt(motionPersist, lastFrameField->offset);
        }
        const auto appendMatrix0 =
            [&](const char* tag, const char* fieldName) {
            const auto* field =
                motionPersistClass->Get<UnityResolve::Field>(fieldName);
            if (field == nullptr || field->offset < 0) {
                return;
            }
            void* array = ReadPointer(motionPersist, field->offset);
            if (array == nullptr) {
                stream << ' ' << tag << "=null";
                return;
            }
            // Il2CppArray header is 0x20 on 64-bit; Matrix4x4 memory order
            // is m00,m10,m20,m30,m01,... so [12..14] is the translation.
            const auto* m = reinterpret_cast<const float*>(
                reinterpret_cast<const std::uint8_t*>(array) + 0x20);
            stream << ' ' << tag << '=' << m[0] << ',' << m[5] << ','
                   << m[12] << ',' << m[13] << ',' << m[14];
        };
        appendMatrix0("mvpdVP", "m_ViewProjection");
        appendMatrix0("mvpdPrevVP", "m_PreviousViewProjection");
    }
    AppendCameraMatrixCompact(
        stream, camera, "get_projectionMatrix_Injected", "proj");
    AppendCameraMatrixCompact(
        stream, camera, "get_nonJitteredProjectionMatrix_Injected", "nonJit");
    AppendCameraMatrixCompact(
        stream, camera, "get_previousViewProjectionMatrix_Injected", "prevVP");
    if (havePrevVp) {
        stream << " shaderPrevP00=" << prevVp[0] << " p02=" << prevVp[8]
               << " p11=" << prevVp[5];
    } else {
        stream << " shaderPrev=0";
    }
    if (haveNonJitteredVp) {
        stream << " shaderNJ_P00=" << nonJitteredVp[0]
               << " p02=" << nonJitteredVp[8];
    } else {
        stream << " shaderNJ=0";
    }
    if (haveViewProj) {
        stream << " shaderVP_P00=" << viewProj[0] << " p02=" << viewProj[8];
    } else {
        stream << " shaderVP=0";
    }
    AppendTextureSize(stream, "taaMv", taaMvId);
    AppendTextureSize(stream, "camMv", camMvId);
    AppendTextureSize(stream, "depth", depthId);
    AppendTextureSize(stream, "accum", accumId);
    Log(stream.str());
}

} // namespace gakumas::vr
