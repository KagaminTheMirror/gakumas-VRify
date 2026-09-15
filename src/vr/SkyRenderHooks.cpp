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

constexpr std::uint32_t kBurstSamples = 8U;
constexpr std::uint32_t kLaterStride = 30U;

bool g_hooksInstalled = false;
bool g_fieldsReady = false;
bool g_runtimeLayoutMissLogged = false;
std::int32_t g_cameraDataOffset = -1;

struct CameraDataFields {
    std::int32_t view = -1;
    std::int32_t proj = -1;
    std::int32_t camera = -1;
    std::int32_t pixelW = -1;
    std::int32_t pixelH = -1;
    std::int32_t antialiasing = -1;
};

CameraDataFields g_fields{};

struct EyeViewStash {
    bool ready = false;
    float matrix[16]{};
    float fov = 0.0F;
    float p02 = 0.0F;
    float p12 = 0.0F;
    int pixW = 0;
    int pixH = 0;
};

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
std::array<std::uint32_t, 2> g_skipLogs{};
using ComputeCamFn = Il2CppMat4* (__fastcall*)(
    Il2CppMat4*, void*, const Il2CppVec4*, Il2CppMat4*, Il2CppMat4*, void*);
using ComputeFovFn = Il2CppMat4* (__fastcall*)(
    Il2CppMat4*, float, Il2CppVec2, const Il2CppVec4*, const Il2CppMat4*, bool,
    float, bool, void*);
ComputeCamFn g_computeCamOrig = nullptr;
ComputeFovFn g_computeFovOrig = nullptr;
bool g_skyboxHookInstalled = false;
bool g_computeHooksInstalled = false;
bool g_inEyeSkyExecute = false;
std::array<std::uint32_t, 2> g_computeLogs{};

float ReadFloat(void* instance, std::int32_t offset) noexcept;
int ReadInt(void* instance, std::int32_t offset) noexcept;
void* ReadPointer(void* instance, std::int32_t offset) noexcept;
bool ReadMatrix(void* instance, std::int32_t offset, float out[16]) noexcept;
UnityStereoRenderer* g_renderer = nullptr;

struct MotionVectorApi {
    bool attempted = false;
    bool ready = false;
    UnityResolve::Class* postProcessPass = nullptr;
    UnityResolve::Class* rtHandle = nullptr;
    UnityResolve::Field* motionVectors = nullptr;
    UnityResolve::Method* getRt = nullptr;
};

MotionVectorApi g_motionVectorApi{};

using PassFn = void (*)(void*, void*, void*, void*);
PassFn g_executeOrig = nullptr;
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
    resolved.camera = FindInstanceFieldOffset(cameraData, "camera");
    resolved.pixelW = FindInstanceFieldOffset(cameraData, "pixelWidth");
    resolved.pixelH = FindInstanceFieldOffset(cameraData, "pixelHeight");
    resolved.antialiasing = FindInstanceFieldOffset(cameraData, "antialiasing");

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
        std::to_string(g_fields.antialiasing));
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





void ResolveMotionVectorApiOnce() noexcept {
    if (g_motionVectorApi.attempted ||
        !GakumasLocal::Config::vrRuntimeStartupEnabled) {
        return;
    }
    g_motionVectorApi.attempted = true;
    auto& api = g_motionVectorApi;
    api.postProcessPass = FindClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "PostProcessPass");
    api.rtHandle = FindClass(
        "Unity.RenderPipelines.Core.Runtime.dll",
        "UnityEngine.Rendering", "RTHandle");
    api.motionVectors = FindExactInstanceField(
        api.postProcessPass, "m_MotionVectors",
        "UnityEngine.Rendering.RTHandle");
    api.getRt = FindExactInstanceZeroArg(
        api.rtHandle, "get_rt", "UnityEngine.RenderTexture");
    api.ready = api.postProcessPass != nullptr && api.rtHandle != nullptr &&
        api.motionVectors != nullptr && api.getRt != nullptr;

    Log(std::string("[VR][smaa-t2x] SMAA_T2X_API ready=") + (api.ready ? "1" : "0"));
}


void InstallVlPostProcessHooks() noexcept;

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
    out.camera = shift(src.camera);
    out.pixelW = shift(src.pixelW);
    out.pixelH = shift(src.pixelH);
    out.antialiasing = shift(src.antialiasing);
    return out;
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









// .39 proved dump offsets include a 16-byte boxed header. .40 reads only
// the unboxed layout and logs XR offset / view translation over time.


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



bool ShouldLogWrite(std::uint32_t& counter) noexcept {
    ++counter;
    return counter <= kBurstSamples || (counter % kLaterStride) == 0U;
}



void StashEyeViewDir(void* pass, void* renderingData) noexcept {
    if (g_renderer == nullptr || renderingData == nullptr) {
        return;
    }
    ResolveSkyRuntimeDataLayout();
    (void)pass;
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
    stash.ready = true;
    stash.fov = fov;
    stash.p02 = proj[8];
    stash.p12 = proj[9];
    stash.pixW = pixW;
    stash.pixH = pixH;
}

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
MvUpdateFn g_mvUpdateOrig = nullptr;
JumpFloodFn g_jumpFloodOrig = nullptr;
VlMotionBlurFn g_vlMotionBlurOrig = nullptr;
VlTextureBlurFn g_vlTextureBlurOrig = nullptr;
VlDrawFlareFn g_vlDrawFlareOrig = nullptr;
ObjectMvFn g_objectMvOrig = nullptr;
StencilLightsFn g_stencilLightsOrig = nullptr;
bool g_vlPostProcessHooksInstalled = false;
bool g_jumpFloodFaulted = false;
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
    if (!g_jumpFloodFaulted && cameraData != nullptr && g_fieldsReady) {
        __try {
            JumpFloodOutlineBody(cameraData);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_jumpFloodFaulted = true;
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













} // namespace

void ObserveSmaaT2xRenderPass(
    void* renderPass,
    void* renderContext,
    void* renderingData,
    int renderPassEvent,
    UnityStereoRenderer& renderer) noexcept {
    const int eyeAaMode = GakumasLocal::Config::vrEyeAaMode;
    const bool functional = GakumasLocal::Config::vrRuntimeStartupEnabled &&
        (eyeAaMode == 4 || eyeAaMode == 5);
    if (!functional || renderPass == nullptr ||
        renderingData == nullptr) {
        return;
    }
    ResolveMotionVectorApiOnce();
    ResolveSkyRuntimeDataLayout();
    const auto& api = g_motionVectorApi;
    if (!api.ready || !g_fieldsReady || g_cameraDataOffset < 16) {
        return;
    }

    const CameraDataFields fields = Unboxed(g_fields);
    void* cameraData = AddBytes(renderingData, g_cameraDataOffset - 16);
    void* camera = ReadPointer(cameraData, fields.camera);
    if (camera == nullptr || !renderer.IsEyeCamera(camera)) {
        return;
    }
    void* instanceClass = ReadPointer(renderPass, 0);
    if (!IsClassOrSubclass(instanceClass, api.postProcessPass->address)) {
        return;
    }
    void* rtHandle = ReadPointer(renderPass, api.motionVectors->offset);
    void* renderTexture = RuntimeInvokeZeroArg(api.getRt, rtHandle, false);
    if (functional) {
        (void)renderer.QueueSmaaT2xMotionVectorCopy(
            camera, renderContext, renderTexture, renderPassEvent);
    }
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
    }
    g_inEyeSkyExecute = ShouldSkipOwnedEyeSky();
    if (g_executeOrig != nullptr) {
        g_executeOrig(self, context, renderingData, method);
    }
    g_inEyeSkyExecute = false;
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

    InstallComputeViewDirHooks(klass);
}

} // namespace gakumas::vr
