#include "VrHandGlowSticks.hpp"

#include "SceneReadyGate.hpp"
#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "pose/HandPoseMailbox.hpp"
#include "pose/PoseMath.hpp"
#include "../GakumasLocalify/Il2cppUtils.hpp"
#include "../deps/UnityResolve/UnityResolve.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gakumas::vr {
namespace {

using UnityVector3 = UnityResolve::UnityType::Vector3;
using UnityVector4 = UnityResolve::UnityType::Vector4;
using UnityQuaternion = UnityResolve::UnityType::Quaternion;
using UnityColor = UnityResolve::UnityType::Color;
using UnityBounds = UnityResolve::UnityType::Bounds;
using UnityMatrix4x4 = UnityResolve::UnityType::Matrix4x4;
using Il2CppGCHandle = void*;

constexpr std::uint32_t kDiscoverInterval = 120;
constexpr std::uint32_t kMaxListLines = 24;
constexpr float kHandScaleMultiplier = 1.15F;
constexpr const char* kHandBaseShader = "Campus/Actor/Default";
constexpr const char* kHandEmissionShader =
    "Universal Render Pipeline/Particles/Unlit";

struct MethodRef {
    void* function = nullptr;
    void* methodInfo = nullptr;

    [[nodiscard]] bool Ready() const noexcept {
        return function != nullptr && methodInfo != nullptr;
    }

    [[nodiscard]] bool HasInfo() const noexcept {
        return methodInfo != nullptr;
    }
};

struct StrictMethodShape {
    bool staticFunction;
    std::string_view returnType;
    std::initializer_list<std::string_view> argumentTypes;
};

struct HandObject {
    void* gameObject = nullptr;
    void* transform = nullptr;
    void* renderer = nullptr;
    void* baseMaterial = nullptr;
    void* material = nullptr;
    void* materialInfo = nullptr;
    Il2CppGCHandle handle = nullptr;
    Il2CppGCHandle baseMaterialHandle = nullptr;
    Il2CppGCHandle materialHandle = nullptr;
    Il2CppGCHandle materialInfoHandle = nullptr;
    int colorIndex = 0;
    int emissionMaterialIndex = 0;
    bool active = false;
};

struct MobSource {
    void* renderer = nullptr;
    void* instancedMaterial = nullptr;
    int colorIndex = 0;
};

struct UniqueColor {
    UnityColor color{0.0F, 0.0F, 0.0F, 0.0F};
    int firstSlot = 0;
    int count = 0;
    std::string slots{};
};

// Campus.Crowd.PersonInfo (dump.cs TypeDefIndex 29073). CrowdCompute reads this
// as a structured buffer with stride 32 and packs intensity into ten bits.
struct CrowdPersonInfo {
    std::uint32_t flags = 0;
    float positionX = 0.0F;
    float positionY = 0.0F;
    float positionZ = 0.0F;
    float rotationX = 0.0F;
    float rotationY = 0.0F;
    float scale = 0.0F;
    float intensity = 0.0F;
};
static_assert(sizeof(CrowdPersonInfo) == 32U);

struct GraphicsBufferDescriptor {
    int count = -1;
    int stride = -1;
    int target = -1;
    bool readable = false;
};

struct CrowdAuditApi {
    bool resolved = false;
    MethodRef graphicsBufferGetCount;
    MethodRef graphicsBufferGetStride;
    MethodRef graphicsBufferGetTarget;
};

struct CrowdDrawApi {
    bool resolved = false;
    bool failed = false;
    UnityResolve::Class* graphicsBufferClass = nullptr;
    UnityResolve::Class* propertyBlockClass = nullptr;
    UnityResolve::Class* uint32Class = nullptr;
    UnityResolve::Class* matrixClass = nullptr;
    UnityResolve::Class* vector4Class = nullptr;
    MethodRef graphicsBufferCtor;
    MethodRef graphicsBufferSetData;
    MethodRef graphicsBufferIsValid;
    MethodRef graphicsBufferRelease;
    MethodRef propertyBlockCtor;
    MethodRef propertyBlockSetBuffer;
    MethodRef propertyBlockSetVector;
    MethodRef propertyBlockSetMatrixArray;
    MethodRef propertyBlockSetVectorArray;
    MethodRef meshGetIndexCount;
    MethodRef meshGetIndexStart;
    MethodRef meshGetBaseVertex;
    MethodRef commandDrawMeshInstancedIndirect;
    int personInfoBytesBufferId = 0;
    int boundingMinId = 0;
    int handMatricesId = 0;
    int penlightInfoBytesBufferId = 0;
    int penlightColorsId = 0;
    void* useColorTableKeyword = nullptr;
};

struct CrowdPrivateMaterial {
    void* source = nullptr;
    void* clone = nullptr;
    Il2CppGCHandle cloneHandle = nullptr;
};

struct CrowdDrawResources {
    void* personBuffer = nullptr;
    void* instanceBuffer = nullptr;
    void* argsBuffer = nullptr;
    void* propertyBlock = nullptr;
    void* personWords = nullptr;
    void* instanceWords = nullptr;
    void* argsWords = nullptr;
    void* handMatrices = nullptr;
    void* penlightColors = nullptr;
    Il2CppGCHandle personBufferHandle = nullptr;
    Il2CppGCHandle instanceBufferHandle = nullptr;
    Il2CppGCHandle argsBufferHandle = nullptr;
    Il2CppGCHandle propertyBlockHandle = nullptr;
    Il2CppGCHandle personWordsHandle = nullptr;
    Il2CppGCHandle instanceWordsHandle = nullptr;
    Il2CppGCHandle argsWordsHandle = nullptr;
    Il2CppGCHandle handMatricesHandle = nullptr;
    Il2CppGCHandle penlightColorsHandle = nullptr;
    std::vector<CrowdPrivateMaterial> materials{};
    bool ready = false;
};

struct GlowApi {
    bool resolved = false;
    bool failed = false;
    bool transformUsesNativeSelf = false;
    bool scaleUsesNativeSelf = false;
    bool getScaleUsesNativeSelf = false;
    bool meshBoundsUsesNativeSelf = false;
    UnityResolve::Class* gameObjectClass = nullptr;
    UnityResolve::Class* materialClass = nullptr;
    UnityResolve::Class* meshFilterClass = nullptr;
    UnityResolve::Class* meshRendererClass = nullptr;
    UnityResolve::Class* colorClass = nullptr;
    UnityResolve::Class* rendererClass = nullptr;
    MethodRef internalCreateGameObject;
    MethodRef addComponent;
    MethodRef setActive;
    MethodRef getTransform;
    MethodRef getComponent;
    MethodRef getGameObject;
    MethodRef setLayer;
    MethodRef getLayer;
    MethodRef dontDestroyOnLoad;
    MethodRef instantiate;
    MethodRef findShader;
    MethodRef materialCtor;
    MethodRef setRenderQueue;
    MethodRef setOverrideTag;
    MethodRef getShader;
    MethodRef getSharedMesh;
    MethodRef setSharedMesh;
    MethodRef getSharedMaterial;
    MethodRef setSharedMaterial;
    MethodRef setSharedMaterials;
    MethodRef setPositionAndRotation;
    MethodRef setLocalScale;
    MethodRef findObjectsOfTypeAll;
    MethodRef propertyToId;
    MethodRef setColorInjected;
    MethodRef getColorInjected;
    MethodRef getLocalScale;
    MethodRef getLossyScale;
    MethodRef getMeshBounds;
    MethodRef getMeshVertices;
    MethodRef getMeshColors;
    MethodRef setMeshColors;
    MethodRef propertyBlockCtor;
    MethodRef getPropertyBlock;
    MethodRef setPropertyBlock;
    MethodRef getPropertyBlockIndexed;
    MethodRef setPropertyBlockIndexed;
    MethodRef mpbSetColorInjected;
    MethodRef mpbGetColorInjected;
    MethodRef enableKeyword;
    MethodRef disableKeyword;
    MethodRef hasProperty;
    MethodRef getPropertyCount;
    MethodRef getPropertyName;
    MethodRef setVectorInjected;
    MethodRef getVectorInjected;
    MethodRef setFloatImpl;
    MethodRef getFloatImpl;
    MethodRef mpbSetVectorInjected;
    MethodRef materialInfoCtor;
    MethodRef materialInfoInit;
    MethodRef materialInfoSetColor;
    MethodRef materialInfoSetVector;
    MethodRef materialInfoSetFloat;
    MethodRef materialInfoApply;
    MethodRef materialInfoGetColor;
    MethodRef materialInfoGetFloat;
    MethodRef materialInfoGetVector;
    UnityResolve::Class* propertyBlockClass = nullptr;
    UnityResolve::Class* materialInfoClass = nullptr;
    const char* matSetName = "-";
    const char* matGetName = "-";
    const char* mpbSetName = "-";
    const char* mpbGetName = "-";
    const char* infoSetName = "-";
    bool getColorUsesOut = false;
    bool mpbGetColorUsesOut = false;
};

struct GlowState {
    GlowApi api{};
    std::array<HandObject, 2> hands{};
    std::vector<Il2CppGCHandle> retiredHandles{};
    void* officialMesh = nullptr;
    Il2CppGCHandle officialMeshHandle = nullptr;
    void* officialMaterial = nullptr;
    Il2CppGCHandle officialMaterialHandle = nullptr;
    void* officialBaseMaterial = nullptr;
    Il2CppGCHandle officialBaseMaterialHandle = nullptr;
    void* sourceRenderer = nullptr;
    int sourceLayer = 0;
    bool sourceFromMob = false;
    bool listed = false;
    UnityVector3 sourceLocalScale{1.0F, 1.0F, 1.0F};
    UnityVector3 sourceLossyScale{1.0F, 1.0F, 1.0F};
    UnityVector3 meshSize{0.0F, 0.0F, 0.0F};
    float audienceIntensity = 0.0F;
    void* propertyBlock = nullptr;
    Il2CppGCHandle propertyBlockHandle = nullptr;
    int penlightColorId = 0;
    int penlightProjectionUvId = 0;
    int emissionScaleId = 0;
    int baseColorId = 0;
    int legacyColorId = 0;
    std::vector<UnityColor> colorTable{};
    std::vector<UniqueColor> uniqueColors{};
    UnityResolve::Class* colorTableClass = nullptr;
    bool colorSourceManagedOnly = false;
    ULONGLONG lastColorReadMs = 0;
    void* colorTableInstance = nullptr;
    UnityColor stickyColor{0.0F, 0.0F, 0.0F, 0.0F};
    int stickySlot = -1;
    std::vector<MobSource> mobSources{};
    bool useColorProjection = false;
    void* penlightController = nullptr;
    void* registeredSetting = nullptr;
    Il2CppGCHandle registeredSettingHandle = nullptr;
    bool registered = false;
    bool paletteReady = false;
    bool hidden = true;
    std::uint32_t ticks = 0;
    std::string lastHideReason{};
    std::uint32_t lastPoseLogTick = 0;
    std::uint32_t lastColorLogTick = 0;
    std::uint32_t lastCameraLogTick = 0;
    CrowdAuditApi crowdAuditApi{};
    std::vector<std::pair<void*, void*>> crowdAuditPairs{};
    CrowdDrawApi crowdDrawApi{};
    CrowdDrawResources crowdDrawResources{};
    std::array<pose::Pose, 2> crowdHandPoses{};
    std::array<bool, 2> crowdHandPoseValid{};
    pose::Pose crowdGameHeadsetPose{};
    pose::Pose crowdOpenXrHeadCenter{};
    float crowdWorldScale = 1.0F;
    std::uint64_t crowdPoseBridgeRevision = 0;
    std::uint64_t crowdAppliedBridgeRevision = 0;
    std::uint64_t crowdAppliedHandRevision = 0;
    bool crowdPoseBridgeValid = false;
    std::vector<std::pair<void*, float>> crowdIntensitySources{};
    std::array<std::vector<std::pair<void*, void*>>, 2> crowdDrawPairs{};
    bool crowdDrawActive = false;
    std::uint32_t lastCrowdDrawTick = 0;
    std::uint32_t crowdDrawFailureTick = 0;
    ULONGLONG lastCrowdColorProbeMs = 0;
};

GlowState g_state;

void LogHandGlow(std::string_view message) noexcept {
    static_cast<void>(WriteVrLog(message));
}

std::ostringstream ClassicLine() noexcept {
    std::ostringstream line;
    line.imbue(std::locale::classic());
    return line;
}

void ClearAudienceColors(const char* reason) noexcept;
void* WriteObjectArray(
    void* elementClass, const std::vector<void*>& items) noexcept;
bool AssignHandMaterials(
    void* renderer, void* baseMaterial, void* emissionMaterial) noexcept;

void* ReadUnityNativePointer(void* managedObject) noexcept {
    if (managedObject == nullptr) {
        return nullptr;
    }
    __try {
        return reinterpret_cast<UnityResolve::UnityType::UnityObject*>(
                   managedObject)
            ->m_CachedPtr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool IsUnityManagedObjectAlive(void* managedObject) noexcept {
    if (managedObject == nullptr) {
        return false;
    }
    __try {
        return reinterpret_cast<UnityResolve::UnityType::UnityObject*>(
                   managedObject)
                   ->m_CachedPtr != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* NewIl2CppObject(void* klass) noexcept {
    return klass == nullptr
        ? nullptr
        : UnityResolve::Invoke<void*, void*>("il2cpp_object_new", klass);
}

Il2CppGCHandle CreateGcHandle(void* object) noexcept {
    return object == nullptr
        ? nullptr
        : UnityResolve::Invoke<Il2CppGCHandle>(
              "il2cpp_gchandle_new", object, false);
}

bool HasExactSignature(
    const UnityResolve::Method* method,
    bool expectedStatic,
    std::string_view expectedReturnType,
    std::initializer_list<std::string_view> expectedArgumentTypes) noexcept {
    if (method == nullptr || method->static_function != expectedStatic ||
        method->return_type == nullptr ||
        method->return_type->name != expectedReturnType ||
        method->args.size() != expectedArgumentTypes.size()) {
        return false;
    }
    std::size_t index = 0;
    for (const std::string_view expectedType : expectedArgumentTypes) {
        const auto* argument = method->args[index++];
        if (argument == nullptr || argument->pType == nullptr ||
            argument->pType->name != expectedType) {
            return false;
        }
    }
    return true;
}

UnityResolve::Method* ResolveStrictMethodAny(
    std::string_view assemblyName,
    std::string_view namespaceName,
    std::string_view className,
    std::string_view methodName,
    std::initializer_list<StrictMethodShape> expectedShapes) {
    auto* klass = Il2cppUtils::GetClass(
        std::string(assemblyName),
        std::string(namespaceName),
        std::string(className));
    if (klass == nullptr) {
        return nullptr;
    }
    std::vector<UnityResolve::Method*> exactMatches;
    for (auto* candidate : klass->methods) {
        if (candidate == nullptr || candidate->name != methodName) {
            continue;
        }
        const bool exact = std::any_of(
            expectedShapes.begin(),
            expectedShapes.end(),
            [candidate](const StrictMethodShape& shape) {
                return HasExactSignature(
                    candidate,
                    shape.staticFunction,
                    shape.returnType,
                    shape.argumentTypes);
            });
        if (exact) {
            exactMatches.push_back(candidate);
        }
    }
    if (exactMatches.size() == 1U && exactMatches.front()->address != nullptr &&
        exactMatches.front()->function != nullptr) {
        return exactMatches.front();
    }
    return nullptr;
}

UnityResolve::Method* ResolveStrictMethod(
    std::string_view assemblyName,
    std::string_view namespaceName,
    std::string_view className,
    std::string_view methodName,
    bool expectedStatic,
    std::string_view expectedReturnType,
    std::initializer_list<std::string_view> expectedArgumentTypes) {
    return ResolveStrictMethodAny(
        assemblyName,
        namespaceName,
        className,
        methodName,
        {{expectedStatic, expectedReturnType, expectedArgumentTypes}});
}

MethodRef MethodReference(UnityResolve::Method* method) noexcept {
    if (method == nullptr) {
        return {};
    }
    return {method->function, method->address};
}

UnityResolve::Method* FindInstanceIntIdMethod(
    UnityResolve::Class* klass,
    std::initializer_list<const char*> names,
    std::size_t expectedArgs) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    for (const char* name : names) {
        if (name == nullptr) {
            continue;
        }
        for (auto* method : klass->methods) {
            if (method == nullptr || method->address == nullptr ||
                method->function == nullptr || method->static_function ||
                method->name != name || method->args.size() != expectedArgs ||
                method->args[0] == nullptr ||
                method->args[0]->pType == nullptr) {
                continue;
            }
            const std::string& typeName = method->args[0]->pType->name;
            if (typeName == "System.Int32" || typeName == "Int32" ||
                typeName == "int") {
                return method;
            }
        }
    }
    return nullptr;
}

UnityResolve::Method* FindNamedInstance(
    UnityResolve::Class* klass,
    std::string_view name,
    std::size_t expectedArgs) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method != nullptr && method->function != nullptr &&
            method->address != nullptr && !method->static_function &&
            method->name == name && method->args.size() == expectedArgs) {
            return method;
        }
    }
    return nullptr;
}

bool RuntimeInvokeRaw(
    void* methodInfo,
    void* instance,
    void** arguments,
    void** result,
    void** exception) noexcept {
    using Invoke = void* (*)(void*, void*, void**, void**);
    static const auto invoke = reinterpret_cast<Invoke>(
        GetProcAddress(
            GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_runtime_invoke"));
    if (invoke == nullptr || methodInfo == nullptr || result == nullptr ||
        exception == nullptr) {
        return false;
    }
    __try {
        *result = invoke(methodInfo, instance, arguments, exception);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RuntimeInvoke(
    const MethodRef& method,
    void* instance,
    void** arguments,
    void** result) noexcept {
    if (result != nullptr) {
        *result = nullptr;
    }
    if (!method.HasInfo()) {
        return false;
    }
    void* exception = nullptr;
    void* invocationResult = nullptr;
    if (!RuntimeInvokeRaw(
            method.methodInfo, instance, arguments, &invocationResult,
            &exception) ||
        exception != nullptr) {
        return false;
    }
    if (result != nullptr) {
        *result = invocationResult;
    }
    return true;
}

void* UnboxObject(void* boxedValue) noexcept {
    if (boxedValue == nullptr) {
        return nullptr;
    }
    using Unbox = void* (*)(void*);
    static const auto unbox = reinterpret_cast<Unbox>(
        GetProcAddress(
            GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_object_unbox"));
    if (unbox == nullptr) {
        return nullptr;
    }
    __try {
        return unbox(boxedValue);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool UnboxColor(void* boxedValue, UnityColor* color) noexcept {
    void* raw = UnboxObject(boxedValue);
    if (raw == nullptr || color == nullptr) {
        return false;
    }
    __try {
        *color = *reinterpret_cast<UnityColor*>(raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool UnboxVector4(void* boxedValue, UnityVector4* value) noexcept {
    void* raw = UnboxObject(boxedValue);
    if (raw == nullptr || value == nullptr) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<UnityVector4*>(raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool UnboxFloat(void* boxedValue, float* value) noexcept {
    void* raw = UnboxObject(boxedValue);
    if (raw == nullptr || value == nullptr) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<float*>(raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int ManagedCallExceptionFilter(DWORD) noexcept {
    return EXCEPTION_CONTINUE_SEARCH;
}

template <typename Function, typename... Arguments>
bool InvokeManagedVoidSeh(
    const MethodRef& method, Arguments... arguments) {
    if (!method.Ready()) {
        return false;
    }
    __try {
        reinterpret_cast<Function>(method.function)(
            arguments..., method.methodInfo);
        return true;
    } __except (ManagedCallExceptionFilter(GetExceptionCode())) {
        return false;
    }
}

template <typename Return, typename Function, typename... Arguments>
bool InvokeManagedResultSeh(
    const MethodRef& method, Return* result, Arguments... arguments) {
    if (!method.Ready() || result == nullptr) {
        return false;
    }
    __try {
        *result = reinterpret_cast<Function>(method.function)(
            arguments..., method.methodInfo);
        return true;
    } __except (ManagedCallExceptionFilter(GetExceptionCode())) {
        return false;
    }
}

template <typename Function, typename... Arguments>
bool InvokeManagedVoid(
    const MethodRef& method, Arguments... arguments) noexcept {
    try {
        return InvokeManagedVoidSeh<Function>(method, arguments...);
    } catch (...) {
        return false;
    }
}

template <typename Return, typename Function, typename... Arguments>
bool InvokeManagedResult(
    const MethodRef& method, Return* result, Arguments... arguments) noexcept {
    try {
        return InvokeManagedResultSeh<Return, Function>(
            method, result, arguments...);
    } catch (...) {
        return false;
    }
}

void RetireHandle(Il2CppGCHandle handle) noexcept {
    if (handle != nullptr) {
        g_state.retiredHandles.push_back(handle);
    }
}

void* ReadRefAtOffsetSeh(void* object, std::int32_t offset) noexcept {
    if (object == nullptr || offset < 0) {
        return nullptr;
    }
    __try {
        return *reinterpret_cast<void**>(
            static_cast<char*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* ReadNamedRef(void* object, const char* fieldName) noexcept {
    if (object == nullptr || fieldName == nullptr) {
        return nullptr;
    }
    try {
        void* klass = UnityResolve::Invoke<void*>(
            "il2cpp_object_get_class", object);
        if (klass == nullptr) {
            return nullptr;
        }
        void* field = UnityResolve::Invoke<void*>(
            "il2cpp_class_get_field_from_name", klass, fieldName);
        if (field == nullptr) {
            return nullptr;
        }
        const auto offset = UnityResolve::Invoke<std::int32_t>(
            "il2cpp_field_get_offset", field);
        return ReadRefAtOffsetSeh(object, offset);
    } catch (...) {
        return nullptr;
    }
}

std::string ManagedObjectName(void* object) noexcept {
    if (object == nullptr || !IsUnityManagedObjectAlive(object)) {
        return "-";
    }
    try {
        const std::string name =
            reinterpret_cast<UnityResolve::UnityType::UnityObject*>(object)
                ->GetName();
        return name.empty() ? "-" : name;
    } catch (...) {
        return "-";
    }
}

UnityResolve::Class* FindCampusClass(
    const char* namespaze, const char* name) noexcept {
    struct CachedClass {
        const char* namespaze = nullptr;
        const char* name = nullptr;
        UnityResolve::Class* klass = nullptr;
        bool resolved = false;
    };
    static CachedClass cache[32]{};
    for (auto& entry : cache) {
        if (entry.resolved && entry.namespaze == namespaze &&
            entry.name == name) {
            return entry.klass;
        }
        if (!entry.resolved) {
            entry.namespaze = namespaze;
            entry.name = name;
            entry.klass = Il2cppUtils::GetClass(
                "Assembly-CSharp.dll", namespaze, name);
            if (entry.klass == nullptr) {
                entry.klass = Il2cppUtils::GetClass(
                    "campus-submodule.Runtime.dll", namespaze, name);
            }
            entry.resolved = true;
            return entry.klass;
        }
    }
    return nullptr;
}

std::vector<void*> FindAllOfClass(UnityResolve::Class* klass) noexcept {
    if (klass == nullptr) {
        return {};
    }
    void* type = klass->GetType();
    if (g_state.api.findObjectsOfTypeAll.Ready() && type != nullptr) {
        using FindAll = void* (*)(void*, void*);
        void* array = nullptr;
        if (InvokeManagedResult<void*, FindAll>(
                g_state.api.findObjectsOfTypeAll, &array, type) &&
            array != nullptr) {
            return reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(
                       array)
                ->ToVector();
        }
    }
    return klass->FindObjectsByType<void*>();
}

bool EnsureApi() noexcept {
    if (g_state.api.resolved) {
        return true;
    }
    if (g_state.api.failed) {
        return false;
    }

    GlowApi api{};
    api.gameObjectClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
    api.materialClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Material");
    api.meshFilterClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MeshFilter");
    api.meshRendererClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MeshRenderer");
    api.colorClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Color");
    api.rendererClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer");
    api.propertyBlockClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock");

    auto* internalCreate = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "Internal_CreateGameObject", true, "System.Void",
        {"UnityEngine.GameObject", "System.String"});
    auto* addComponent = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "AddComponent", false, "UnityEngine.Component", {"System.Type"});
    auto* setActive = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "SetActive", false, "System.Void", {"System.Boolean"});
    auto* getTransform = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "get_transform", false, "UnityEngine.Transform", {});
    auto* getComponent = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "GetComponent", false, "UnityEngine.Component", {"System.Type"});
    auto* getGameObject = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Component",
        "get_gameObject", false, "UnityEngine.GameObject", {});
    auto* setLayer = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "set_layer", false, "System.Void", {"System.Int32"});
    auto* getLayer = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "get_layer", false, "System.Int32", {});
    auto* dontDestroyOnLoad = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Object",
        "DontDestroyOnLoad", true, "System.Void", {"UnityEngine.Object"});
    auto* instantiate = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Object",
        "Instantiate", true, "UnityEngine.Object", {"UnityEngine.Object"});
    auto* findShader = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Shader",
        "Find", true, "UnityEngine.Shader", {"System.String"});
    auto* materialCtor = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Material",
        ".ctor", false, "System.Void", {"UnityEngine.Shader"});
    auto* setRenderQueue = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Material",
        "set_renderQueue", false, "System.Void", {"System.Int32"});
    auto* setOverrideTag = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Material",
        "SetOverrideTag", false, "System.Void",
        {"System.String", "System.String"});
    auto* getShader = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Material",
        "get_shader", false, "UnityEngine.Shader", {});
    auto* getSharedMesh = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MeshFilter",
        "get_sharedMesh", false, "UnityEngine.Mesh", {});
    auto* setSharedMesh = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MeshFilter",
        "set_sharedMesh", false, "System.Void", {"UnityEngine.Mesh"});
    auto* getSharedMaterial = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
        "get_sharedMaterial", false, "UnityEngine.Material", {});
    auto* setSharedMaterial = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
        "set_sharedMaterial", false, "System.Void", {"UnityEngine.Material"});
    auto* setSharedMaterials = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
        "set_sharedMaterials", false, "System.Void",
        {"UnityEngine.Material[]"});
    auto* setPosition = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Transform",
        "SetPositionAndRotation_Injected",
        {
            {false, "System.Void",
             {"UnityEngine.Vector3&", "UnityEngine.Quaternion&"}},
            {true, "System.Void",
             {"System.IntPtr", "UnityEngine.Vector3&",
              "UnityEngine.Quaternion&"}},
        });
    auto* setScale = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Transform",
        "set_localScale_Injected",
        {
            {false, "System.Void", {"UnityEngine.Vector3&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Vector3&"}},
        });
    auto* findAll = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Resources",
        "FindObjectsOfTypeAll", true, "UnityEngine.Object[]",
        {"System.Type"});
    auto* propertyToId = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Shader",
        "PropertyToID", true, "System.Int32", {"System.String"});
    auto* setColor = FindInstanceIntIdMethod(
        api.materialClass, {"SetColor", "SetColorImpl_Injected"}, 2U);
    auto* getColor = FindInstanceIntIdMethod(
        api.materialClass,
        {"GetColorImpl_Injected", "GetVectorImpl_Injected"}, 2U);
    api.getColorUsesOut = getColor != nullptr;
    if (getColor == nullptr) {
        getColor = FindInstanceIntIdMethod(
            api.materialClass, {"GetColor", "GetVector"}, 1U);
    }
    auto* getLocalScale = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Transform",
        "get_localScale_Injected",
        {
            {false, "System.Void", {"UnityEngine.Vector3&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Vector3&"}},
        });
    auto* getLossyScale = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Transform",
        "get_lossyScale_Injected",
        {
            {false, "System.Void", {"UnityEngine.Vector3&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Vector3&"}},
        });
    auto* getMeshBounds = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
        "get_bounds_Injected",
        {
            {false, "System.Void", {"UnityEngine.Bounds&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Bounds&"}},
        });
    auto* getMeshVertices = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
        "get_vertices", false, "UnityEngine.Vector3[]", {});
    auto* getMeshColors = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
        "get_colors", false, "UnityEngine.Color[]", {});
    auto* setMeshColors = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh",
        "set_colors", false, "System.Void", {"UnityEngine.Color[]"});
    auto* mpbCtor = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock",
        ".ctor", false, "System.Void", {});
    auto* getMpb = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
        "GetPropertyBlock", false, "System.Void",
        {"UnityEngine.MaterialPropertyBlock"});
    auto* setMpb = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
        "SetPropertyBlock", false, "System.Void",
        {"UnityEngine.MaterialPropertyBlock"});
    auto* getMpbIndexed = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
        "GetPropertyBlock", false, "System.Void",
        {"UnityEngine.MaterialPropertyBlock", "System.Int32"});
    auto* setMpbIndexed = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer",
        "SetPropertyBlock", false, "System.Void",
        {"UnityEngine.MaterialPropertyBlock", "System.Int32"});
    auto* mpbSetColor = FindInstanceIntIdMethod(
        api.propertyBlockClass, {"SetColor", "SetColorImpl_Injected"}, 2U);
    auto* mpbGetColor = FindInstanceIntIdMethod(
        api.propertyBlockClass,
        {"GetColorImpl_Injected", "GetVectorImpl_Injected"},
        2U);
    api.mpbGetColorUsesOut = mpbGetColor != nullptr;
    if (mpbGetColor == nullptr) {
        mpbGetColor = FindInstanceIntIdMethod(
            api.propertyBlockClass, {"GetColor", "GetVector"}, 1U);
    }
    auto* enableKeyword = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Material",
        "EnableKeyword", false, "System.Void", {"System.String"});
    auto* disableKeyword = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Material",
        "DisableKeyword", false, "System.Void", {"System.String"});
    auto* hasProperty = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Material",
        "HasProperty", false, "System.Boolean", {"System.Int32"});
    auto* getPropertyCount = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Shader",
        "GetPropertyCount", false, "System.Int32", {});
    auto* getPropertyName = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Shader",
        "GetPropertyName", false, "System.String", {"System.Int32"});
    auto* setVector = FindInstanceIntIdMethod(
        api.materialClass, {"SetVector", "SetVectorImpl_Injected"}, 2U);
    auto* getVector = FindInstanceIntIdMethod(
        api.materialClass, {"GetVectorImpl_Injected"}, 2U);
    auto* setFloat = FindInstanceIntIdMethod(
        api.materialClass, {"SetFloat", "SetFloatImpl"}, 2U);
    auto* getFloat = FindInstanceIntIdMethod(
        api.materialClass, {"GetFloat", "GetFloatImpl"}, 1U);
    auto* mpbSetVector = FindInstanceIntIdMethod(
        api.propertyBlockClass, {"SetVector", "SetVectorImpl_Injected"}, 2U);
    api.materialInfoClass = Il2cppUtils::GetClass(
        "vl-unity.Runtime.dll", "VL.Core", "MaterialInfo");
    UnityResolve::Method* infoCtor = nullptr;
    if (api.materialInfoClass != nullptr) {
        static constexpr const char* kCtorArgs[] = {
            "UnityEngine.Material", "UnityEngine.Renderer",
            "System.Int32", "UnityEngine.Material",
        };
        for (auto* candidate : api.materialInfoClass->methods) {
            if (candidate == nullptr || candidate->name != ".ctor" ||
                candidate->static_function || candidate->function == nullptr ||
                candidate->address == nullptr ||
                candidate->args.size() != std::size(kCtorArgs)) {
                continue;
            }
            bool exact = true;
            for (std::size_t index = 0; index < std::size(kCtorArgs); ++index) {
                exact = exact && candidate->args[index] != nullptr &&
                    candidate->args[index]->pType != nullptr &&
                    candidate->args[index]->pType->name == kCtorArgs[index];
            }
            if (exact) {
                infoCtor = candidate;
                break;
            }
        }
    }
    auto* infoSetColor = FindInstanceIntIdMethod(
        api.materialInfoClass, {"SetColor"}, 3U);
    auto* infoSetVector = FindInstanceIntIdMethod(
        api.materialInfoClass, {"SetVector"}, 2U);
    auto* infoSetFloat = FindInstanceIntIdMethod(
        api.materialInfoClass, {"SetFloat"}, 2U);
    auto* infoApply = FindNamedInstance(
        api.materialInfoClass, "ApplyProperty", 0U);
    auto* infoInit = FindNamedInstance(
        api.materialInfoClass, "InitializeProperty", 0U);
    auto* infoGetColor = FindNamedInstance(
        api.materialInfoClass, "GetColor", 1U);
    auto* infoGetFloat = FindNamedInstance(
        api.materialInfoClass, "GetFloat", 1U);
    auto* infoGetVector = FindNamedInstance(
        api.materialInfoClass, "GetVector", 1U);

    api.internalCreateGameObject = MethodReference(internalCreate);
    api.addComponent = MethodReference(addComponent);
    api.setActive = MethodReference(setActive);
    api.getTransform = MethodReference(getTransform);
    api.getComponent = MethodReference(getComponent);
    api.getGameObject = MethodReference(getGameObject);
    api.setLayer = MethodReference(setLayer);
    api.getLayer = MethodReference(getLayer);
    api.dontDestroyOnLoad = MethodReference(dontDestroyOnLoad);
    api.instantiate = MethodReference(instantiate);
    api.findShader = MethodReference(findShader);
    api.materialCtor = MethodReference(materialCtor);
    api.setRenderQueue = MethodReference(setRenderQueue);
    api.setOverrideTag = MethodReference(setOverrideTag);
    api.getShader = MethodReference(getShader);
    api.getSharedMesh = MethodReference(getSharedMesh);
    api.setSharedMesh = MethodReference(setSharedMesh);
    api.getSharedMaterial = MethodReference(getSharedMaterial);
    api.setSharedMaterial = MethodReference(setSharedMaterial);
    api.setSharedMaterials = MethodReference(setSharedMaterials);
    api.setPositionAndRotation = MethodReference(setPosition);
    api.setLocalScale = MethodReference(setScale);
    api.findObjectsOfTypeAll = MethodReference(findAll);
    api.propertyToId = MethodReference(propertyToId);
    api.setColorInjected = MethodReference(setColor);
    api.getColorInjected = MethodReference(getColor);
    api.getLocalScale = MethodReference(getLocalScale);
    api.getLossyScale = MethodReference(getLossyScale);
    api.getMeshBounds = MethodReference(getMeshBounds);
    api.getMeshVertices = MethodReference(getMeshVertices);
    api.getMeshColors = MethodReference(getMeshColors);
    api.setMeshColors = MethodReference(setMeshColors);
    api.propertyBlockCtor = MethodReference(mpbCtor);
    api.getPropertyBlock = MethodReference(getMpb);
    api.setPropertyBlock = MethodReference(setMpb);
    api.getPropertyBlockIndexed = MethodReference(getMpbIndexed);
    api.setPropertyBlockIndexed = MethodReference(setMpbIndexed);
    api.mpbSetColorInjected = MethodReference(mpbSetColor);
    api.mpbGetColorInjected = MethodReference(mpbGetColor);
    api.enableKeyword = MethodReference(enableKeyword);
    api.disableKeyword = MethodReference(disableKeyword);
    api.hasProperty = MethodReference(hasProperty);
    api.getPropertyCount = MethodReference(getPropertyCount);
    api.getPropertyName = MethodReference(getPropertyName);
    api.setVectorInjected = MethodReference(setVector);
    api.getVectorInjected = MethodReference(getVector);
    api.setFloatImpl = MethodReference(setFloat);
    api.getFloatImpl = MethodReference(getFloat);
    api.mpbSetVectorInjected = MethodReference(mpbSetVector);
    api.materialInfoCtor = MethodReference(infoCtor);
    api.materialInfoInit = MethodReference(infoInit);
    api.materialInfoSetColor = MethodReference(infoSetColor);
    api.materialInfoSetVector = MethodReference(infoSetVector);
    api.materialInfoSetFloat = MethodReference(infoSetFloat);
    api.materialInfoApply = MethodReference(infoApply);
    api.materialInfoGetColor = MethodReference(infoGetColor);
    api.materialInfoGetFloat = MethodReference(infoGetFloat);
    api.materialInfoGetVector = MethodReference(infoGetVector);
    api.matSetName = setColor != nullptr ? setColor->name.c_str() : "-";
    api.matGetName = getColor != nullptr ? getColor->name.c_str() : "-";
    api.mpbSetName = mpbSetColor != nullptr ? mpbSetColor->name.c_str() : "-";
    api.mpbGetName = mpbGetColor != nullptr ? mpbGetColor->name.c_str() : "-";
    api.infoSetName =
        infoSetColor != nullptr ? infoSetColor->name.c_str() : "-";
    api.transformUsesNativeSelf =
        setPosition != nullptr && setPosition->static_function;
    api.scaleUsesNativeSelf = setScale != nullptr && setScale->static_function;
    api.getScaleUsesNativeSelf =
        getLossyScale != nullptr && getLossyScale->static_function;
    api.meshBoundsUsesNativeSelf =
        getMeshBounds != nullptr && getMeshBounds->static_function;

    if (!api.internalCreateGameObject.Ready() || !api.addComponent.Ready() ||
        !api.setActive.Ready() || !api.getTransform.Ready() ||
        !api.dontDestroyOnLoad.Ready() || !api.setPositionAndRotation.Ready() ||
        !api.instantiate.Ready() || !api.getSharedMesh.Ready() ||
        !api.findShader.Ready() || !api.materialCtor.HasInfo() ||
        !api.setRenderQueue.HasInfo() || !api.setOverrideTag.HasInfo() ||
        !api.enableKeyword.Ready() || !api.disableKeyword.Ready() ||
        !api.setSharedMesh.Ready() || !api.getSharedMaterial.Ready() ||
        !api.setSharedMaterial.Ready() || !api.setSharedMaterials.Ready() ||
        !api.getPropertyBlockIndexed.Ready() ||
        !api.setPropertyBlockIndexed.Ready() || !api.getGameObject.Ready() ||
        !api.getMeshVertices.HasInfo() || !api.getMeshColors.HasInfo() ||
        !api.setMeshColors.HasInfo() ||
        api.gameObjectClass == nullptr || api.meshFilterClass == nullptr ||
        api.meshRendererClass == nullptr || api.materialClass == nullptr ||
        api.colorClass == nullptr) {
        g_state.api.failed = true;
        LogHandGlow("[VR][stereo] HAND_GLOW skip reason=api");
        return false;
    }
    api.resolved = true;
    g_state.api = api;
    auto line = ClassicLine();
    line << "[VR][stereo] HAND_GLOW api"
         << " matSet=" << api.matSetName
         << " matGet=" << api.matGetName
         << " mpbSet=" << api.mpbSetName
         << " mpbGet=" << api.mpbGetName
         << " infoSet=" << api.infoSetName
         << " infoApply=" << (api.materialInfoApply.HasInfo() ? 1 : 0)
         << " infoCtor=" << (api.materialInfoCtor.HasInfo() ? 1 : 0)
         << " infoGet=" << (api.materialInfoGetColor.HasInfo() ? 1 : 0)
         << " shaderCtor="
         << (api.findShader.Ready() && api.materialCtor.HasInfo() ? 1 : 0)
         << " dualMaterial=" << (api.setSharedMaterials.Ready() ? 1 : 0)
         << " indexedMpb="
         << (api.getPropertyBlockIndexed.Ready() &&
                     api.setPropertyBlockIndexed.Ready()
                 ? 1
                 : 0)
         << " meshColor="
         << (api.getMeshColors.HasInfo() && api.setMeshColors.HasInfo() ? 1 : 0);
    LogHandGlow(line.str());
    return true;
}

void* TransformSelf(void* transform) noexcept {
    if (transform == nullptr) {
        return nullptr;
    }
    return g_state.api.transformUsesNativeSelf
        ? ReadUnityNativePointer(transform)
        : transform;
}

void* ScaleSelf(void* transform) noexcept {
    if (transform == nullptr) {
        return nullptr;
    }
    return g_state.api.scaleUsesNativeSelf
        ? ReadUnityNativePointer(transform)
        : transform;
}

void* ScaleReadSelf(void* transform) noexcept {
    if (transform == nullptr) {
        return nullptr;
    }
    return g_state.api.getScaleUsesNativeSelf
        ? ReadUnityNativePointer(transform)
        : transform;
}

bool SetActive(void* gameObject, bool active) noexcept {
    using Fn = void (*)(void*, bool, void*);
    return InvokeManagedVoid<Fn>(g_state.api.setActive, gameObject, active);
}

void* GetComponent(void* gameObject, UnityResolve::Class* klass) noexcept {
    if (gameObject == nullptr || klass == nullptr ||
        !g_state.api.getComponent.Ready()) {
        return nullptr;
    }
    void* type = klass->GetType();
    if (type == nullptr) {
        return nullptr;
    }
    using Fn = void* (*)(void*, void*, void*);
    void* component = nullptr;
    if (!InvokeManagedResult<void*, Fn>(
            g_state.api.getComponent, &component, gameObject, type)) {
        return nullptr;
    }
    return component;
}

bool SetWorldPose(void* transform, const pose::Pose& pose) noexcept {
    void* self = TransformSelf(transform);
    if (self == nullptr) {
        return false;
    }
    const pose::Vector3 position = pose.position;
    const UnityVector3 unityPosition(position.x, position.y, position.z);
    const UnityQuaternion unityRotation(
        pose.orientation.x,
        pose.orientation.y,
        pose.orientation.z,
        pose.orientation.w);
    if (g_state.api.transformUsesNativeSelf) {
        using Fn = void (*)(
            void*, const UnityVector3*, const UnityQuaternion*, void*);
        return InvokeManagedVoid<Fn>(
            g_state.api.setPositionAndRotation,
            self,
            &unityPosition,
            &unityRotation);
    }
    using Fn = void (*)(
        void*, const UnityVector3*, const UnityQuaternion*, void*);
    return InvokeManagedVoid<Fn>(
        g_state.api.setPositionAndRotation,
        self,
        &unityPosition,
        &unityRotation);
}

bool SetLocalScale(void* transform, float x, float y, float z) noexcept {
    if (!g_state.api.setLocalScale.Ready()) {
        return false;
    }
    void* self = ScaleSelf(transform);
    if (self == nullptr) {
        return false;
    }
    const UnityVector3 scale(x, y, z);
    if (g_state.api.scaleUsesNativeSelf) {
        using Fn = void (*)(void*, const UnityVector3*, void*);
        return InvokeManagedVoid<Fn>(g_state.api.setLocalScale, self, &scale);
    }
    using Fn = void (*)(void*, const UnityVector3*, void*);
    return InvokeManagedVoid<Fn>(g_state.api.setLocalScale, self, &scale);
}

void* CloneUnityObject(void* source) noexcept {
    if (source == nullptr || !IsUnityManagedObjectAlive(source) ||
        !g_state.api.instantiate.Ready()) {
        return nullptr;
    }
    using Fn = void* (*)(void*, void*);
    void* clone = nullptr;
    if (!InvokeManagedResult<void*, Fn>(
            g_state.api.instantiate, &clone, source) ||
        clone == nullptr || !IsUnityManagedObjectAlive(clone)) {
        return nullptr;
    }
    return clone;
}

std::string ManagedClassName(void* object) noexcept {
    if (object == nullptr) {
        return "-";
    }
    try {
        void* klass = UnityResolve::Invoke<void*>(
            "il2cpp_object_get_class", object);
        if (klass == nullptr) {
            return "-";
        }
        const char* namespaze = UnityResolve::Invoke<const char*>(
            "il2cpp_class_get_namespace", klass);
        const char* name = UnityResolve::Invoke<const char*>(
            "il2cpp_class_get_name", klass);
        if (name == nullptr || *name == '\0') {
            return "-";
        }
        if (namespaze == nullptr || *namespaze == '\0') {
            return name;
        }
        return std::string(namespaze) + "." + name;
    } catch (...) {
        return "-";
    }
}

std::size_t ManagedArrayLength(void* array) noexcept {
    if (array == nullptr) {
        return 0U;
    }
    __try {
        return reinterpret_cast<
            UnityResolve::UnityType::Array<std::uint8_t>*>(array)
            ->max_length;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0U;
    }
}

void* CloneMaterial(void* source) noexcept {
    return CloneUnityObject(source);
}

void* GetGameObject(void* component) noexcept {
    if (component == nullptr || !g_state.api.getGameObject.Ready()) {
        return nullptr;
    }
    using Fn = void* (*)(void*, void*);
    void* gameObject = nullptr;
    if (!InvokeManagedResult<void*, Fn>(
            g_state.api.getGameObject, &gameObject, component)) {
        return nullptr;
    }
    return gameObject;
}

void* AddComponent(void* gameObject, UnityResolve::Class* klass) noexcept {
    if (gameObject == nullptr || klass == nullptr ||
        !g_state.api.addComponent.Ready()) {
        return nullptr;
    }
    void* type = klass->GetType();
    if (type == nullptr) {
        return nullptr;
    }
    using Fn = void* (*)(void*, void*, void*);
    void* component = nullptr;
    if (!InvokeManagedResult<void*, Fn>(
            g_state.api.addComponent, &component, gameObject, type)) {
        return nullptr;
    }
    return component;
}

int GetLayer(void* gameObject) noexcept {
    if (gameObject == nullptr || !g_state.api.getLayer.Ready()) {
        return 0;
    }
    using Fn = std::int32_t (*)(void*, void*);
    std::int32_t layer = 0;
    if (!InvokeManagedResult<std::int32_t, Fn>(
            g_state.api.getLayer, &layer, gameObject)) {
        return 0;
    }
    return layer;
}

bool SetLayer(void* gameObject, int layer) noexcept {
    if (gameObject == nullptr || !g_state.api.setLayer.Ready()) {
        return false;
    }
    using Fn = void (*)(void*, std::int32_t, void*);
    return InvokeManagedVoid<Fn>(g_state.api.setLayer, gameObject, layer);
}

void* GetSharedMesh(void* filter) noexcept {
    if (filter == nullptr || !g_state.api.getSharedMesh.Ready()) {
        return nullptr;
    }
    using Fn = void* (*)(void*, void*);
    void* mesh = nullptr;
    if (!InvokeManagedResult<void*, Fn>(
            g_state.api.getSharedMesh, &mesh, filter)) {
        return nullptr;
    }
    return mesh;
}

void* GetSharedMaterial(void* renderer) noexcept {
    if (renderer == nullptr || !g_state.api.getSharedMaterial.Ready()) {
        return nullptr;
    }
    using Fn = void* (*)(void*, void*);
    void* material = nullptr;
    if (!InvokeManagedResult<void*, Fn>(
            g_state.api.getSharedMaterial, &material, renderer)) {
        return nullptr;
    }
    return material;
}

void* GetShader(void* material) noexcept {
    if (material == nullptr || !g_state.api.getShader.Ready()) {
        return nullptr;
    }
    using Fn = void* (*)(void*, void*);
    void* shader = nullptr;
    if (!InvokeManagedResult<void*, Fn>(
            g_state.api.getShader, &shader, material)) {
        return nullptr;
    }
    return shader;
}

int ShaderPropertyToID(const char* name) noexcept {
    if (name == nullptr || !g_state.api.propertyToId.Ready()) {
        return 0;
    }
    auto* managed = UnityResolve::UnityType::String::New(name);
    if (managed == nullptr) {
        return 0;
    }
    using Fn = std::int32_t (*)(void*, void*);
    std::int32_t id = 0;
    if (!InvokeManagedResult<std::int32_t, Fn>(
            g_state.api.propertyToId, &id, managed)) {
        return 0;
    }
    return id;
}

int ReadStaticIntField(UnityResolve::Class* klass, const char* name) noexcept {
    if (klass == nullptr || name == nullptr) {
        return 0;
    }
    try {
        for (auto* field : klass->fields) {
            if (field == nullptr || field->address == nullptr ||
                field->name != name) {
                continue;
            }
            std::int32_t value = 0;
            // UnityResolve currently infers static_field from offset <= 0.
            // IL2CPP reports an offset within the class static-data block, so
            // every Crowd binding except the first one is falsely rejected.
            // These exact fields are proven static in dump.cs; read their
            // FieldInfo directly through the IL2CPP API.
            UnityResolve::Invoke<void, void*, std::int32_t*>(
                "il2cpp_field_static_get_value", field->address, &value);
            return value;
        }
    } catch (...) {
        return 0;
    }
    return 0;
}

void* ReadStaticRefField(UnityResolve::Class* klass, const char* name) noexcept {
    if (klass == nullptr || name == nullptr) {
        return nullptr;
    }
    try {
        for (auto* field : klass->fields) {
            if (field == nullptr || field->address == nullptr ||
                field->name != name) {
                continue;
            }
            void* value = nullptr;
            UnityResolve::Invoke<void, void*, void**>(
                "il2cpp_field_static_get_value", field->address, &value);
            return value;
        }
    } catch (...) {
        return nullptr;
    }
    return nullptr;
}

std::int32_t ReadIntAtOffsetSeh(void* object, std::int32_t offset) noexcept {
    if (object == nullptr || offset < 0) {
        return 0;
    }
    __try {
        return *reinterpret_cast<std::int32_t*>(
            static_cast<char*>(object) + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

using Il2CppWriteBarrierSetField = void (*)(void*, void**, void*);

Il2CppWriteBarrierSetField ResolveWriteBarrier() noexcept {
    static const auto writeBarrier = reinterpret_cast<Il2CppWriteBarrierSetField>(
        GetProcAddress(
            GetModuleHandleW(L"GameAssembly.dll"),
            "il2cpp_gc_wbarrier_set_field"));
    return writeBarrier;
}

bool StoreManagedReference(
    void* owner, void** slot, void* value) noexcept {
    const auto writeBarrier = ResolveWriteBarrier();
    if (owner == nullptr || slot == nullptr || writeBarrier == nullptr) {
        return false;
    }
    __try {
        writeBarrier(owner, slot, value);
        return true;
    } __except (EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool WriteRefAtOffsetSeh(
    void* object, std::int32_t offset, void* value) noexcept {
    if (object == nullptr || offset < 0) {
        return false;
    }
    auto** slot = reinterpret_cast<void**>(
        static_cast<char*>(object) + offset);
    return StoreManagedReference(object, slot, value);
}

void WriteIntAtOffsetSeh(
    void* object, std::int32_t offset, int value) noexcept {
    if (object == nullptr || offset < 0) {
        return;
    }
    __try {
        *reinterpret_cast<int*>(static_cast<char*>(object) + offset) = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteNamedRef(void* object, const char* fieldName, void* value) noexcept {
    if (object == nullptr || fieldName == nullptr) {
        return;
    }
    try {
        void* klass = UnityResolve::Invoke<void*>(
            "il2cpp_object_get_class", object);
        if (klass == nullptr) {
            return;
        }
        void* field = UnityResolve::Invoke<void*>(
            "il2cpp_class_get_field_from_name", klass, fieldName);
        if (field == nullptr) {
            return;
        }
        const auto offset = UnityResolve::Invoke<std::int32_t>(
            "il2cpp_field_get_offset", field);
        WriteRefAtOffsetSeh(object, offset, value);
    } catch (...) {
    }
}

void WriteNamedInt(void* object, const char* fieldName, int value) noexcept {
    if (object == nullptr || fieldName == nullptr) {
        return;
    }
    try {
        void* klass = UnityResolve::Invoke<void*>(
            "il2cpp_object_get_class", object);
        if (klass == nullptr) {
            return;
        }
        void* field = UnityResolve::Invoke<void*>(
            "il2cpp_class_get_field_from_name", klass, fieldName);
        if (field == nullptr) {
            return;
        }
        const auto offset = UnityResolve::Invoke<std::int32_t>(
            "il2cpp_field_get_offset", field);
        WriteIntAtOffsetSeh(object, offset, value);
    } catch (...) {
    }
}

int ReadNamedInt(void* object, const char* fieldName) noexcept {
    if (object == nullptr || fieldName == nullptr) {
        return 0;
    }
    try {
        void* klass = UnityResolve::Invoke<void*>(
            "il2cpp_object_get_class", object);
        if (klass == nullptr) {
            return 0;
        }
        void* field = UnityResolve::Invoke<void*>(
            "il2cpp_class_get_field_from_name", klass, fieldName);
        if (field == nullptr) {
            return 0;
        }
        const auto offset = UnityResolve::Invoke<std::int32_t>(
            "il2cpp_field_get_offset", field);
        return ReadIntAtOffsetSeh(object, offset);
    } catch (...) {
        return 0;
    }
}

void* InvokeInstanceObject(void* instance, UnityResolve::Class* klass,
    const char* methodName) noexcept {
    if (instance == nullptr || klass == nullptr || methodName == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method == nullptr || method->static_function ||
            method->name != methodName || !method->args.empty() ||
            method->function == nullptr) {
            continue;
        }
        const MethodRef ref = MethodReference(method);
        using Fn = void* (*)(void*, void*);
        void* result = nullptr;
        if (InvokeManagedResult<void*, Fn>(ref, &result, instance)) {
            return result;
        }
    }
    return nullptr;
}

std::string ManagedIl2CppString(void* object) noexcept {
    if (object == nullptr) {
        return {};
    }
    try {
        return reinterpret_cast<UnityResolve::UnityType::String*>(object)
            ->ToString();
    } catch (...) {
        return {};
    }
}

bool InvokeInstanceBool(void* instance, UnityResolve::Class* klass,
    const char* methodName, bool* value) noexcept {
    if (instance == nullptr || klass == nullptr || methodName == nullptr ||
        value == nullptr) {
        return false;
    }
    for (auto* method : klass->methods) {
        if (method == nullptr || method->static_function ||
            method->name != methodName || !method->args.empty() ||
            method->function == nullptr || method->return_type == nullptr ||
            method->return_type->name != "System.Boolean") {
            continue;
        }
        const MethodRef ref = MethodReference(method);
        using Fn = bool (*)(void*, void*);
        bool result = false;
        if (InvokeManagedResult<bool, Fn>(ref, &result, instance)) {
            *value = result;
            return true;
        }
    }
    return false;
}

int PenlightColorId() noexcept {
    if (g_state.penlightColorId != 0) {
        return g_state.penlightColorId;
    }
    auto* controllerClass = FindCampusClass(
        "Campus.MobAudience", "MobAudiencePenlightController");
    const int officialId =
        ReadStaticIntField(controllerClass, "PenlightColorID");
    if (officialId != 0) {
        g_state.penlightColorId = officialId;
        return officialId;
    }
    const char* names[] = {"_PenlightColor", "PenlightColor"};
    for (const char* name : names) {
        const int id = ShaderPropertyToID(name);
        if (id != 0) {
            return id;
        }
    }
    return 0;
}

int PenlightProjectionUvId() noexcept {
    if (g_state.penlightProjectionUvId != 0) {
        return g_state.penlightProjectionUvId;
    }
    auto* controllerClass = FindCampusClass(
        "Campus.MobAudience", "MobAudiencePenlightController");
    const int officialId =
        ReadStaticIntField(controllerClass, "PenlightProjectionUvID");
    if (officialId != 0) {
        g_state.penlightProjectionUvId = officialId;
        return officialId;
    }
    return ShaderPropertyToID("_PenlightProjectionUV");
}

int EmissionScaleId() noexcept {
    if (g_state.emissionScaleId == 0) {
        g_state.emissionScaleId = ShaderPropertyToID("_EmissionAlbedoScale");
    }
    return g_state.emissionScaleId;
}

int BaseColorId() noexcept {
    if (g_state.baseColorId == 0) {
        g_state.baseColorId = ShaderPropertyToID("_BaseColor");
    }
    return g_state.baseColorId;
}

bool EnsureCrowdAuditApi() noexcept {
    auto& api = g_state.crowdAuditApi;
    if (api.resolved) {
        return api.graphicsBufferGetCount.Ready() &&
            api.graphicsBufferGetStride.Ready() &&
            api.graphicsBufferGetTarget.Ready();
    }
    api.resolved = true;
    api.graphicsBufferGetCount = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
        "get_count", false, "System.Int32", {}));
    api.graphicsBufferGetStride = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
        "get_stride", false, "System.Int32", {}));
    api.graphicsBufferGetTarget = MethodReference(ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
        "get_target",
        {
            {false, "UnityEngine.GraphicsBuffer.Target", {}},
            {false, "GraphicsBuffer.Target", {}},
        }));
    const bool ready = api.graphicsBufferGetCount.Ready() &&
        api.graphicsBufferGetStride.Ready() &&
        api.graphicsBufferGetTarget.Ready();
    LogHandGlow(std::string("[VR][stereo] HAND_GLOW crowd buffer-api=") +
        (ready ? "1" : "0") +
        " methods=get_count,get_stride,get_target");
    return ready;
}

bool RefreshCrowdShaderBindings(CrowdDrawApi* api) noexcept {
    if (api == nullptr) {
        return false;
    }
    auto* crowdSystemData =
        FindCampusClass("Campus.Crowd", "CrowdSystemData");
    auto* crowdMeshSystem =
        FindCampusClass("Campus.Crowd", "CrowdMeshRenderSystem");
    auto* penlightSystem =
        FindCampusClass("Campus.Crowd", "PenlightMeshRenderSystem");
    api->personInfoBytesBufferId =
        ReadStaticIntField(crowdSystemData, "PersonInfoBytesBufferID");
    api->boundingMinId =
        ReadStaticIntField(crowdSystemData, "BoundingMinID");
    api->handMatricesId =
        ReadStaticIntField(crowdMeshSystem, "HandMatricesID");
    api->penlightInfoBytesBufferId =
        ReadStaticIntField(penlightSystem, "PenlightInfoBytesBufferID");
    api->penlightColorsId =
        ReadStaticIntField(penlightSystem, "PenlightColorsID");
    api->useColorTableKeyword =
        ReadStaticRefField(penlightSystem, "UseColorTableKeyword");
    return api->personInfoBytesBufferId != 0 && api->boundingMinId != 0 &&
        api->handMatricesId != 0 && api->penlightInfoBytesBufferId != 0 &&
        api->penlightColorsId != 0 && api->useColorTableKeyword != nullptr;
}

bool EnsureCrowdDrawApi() noexcept {
    auto& api = g_state.crowdDrawApi;
    if (api.resolved) {
        return RefreshCrowdShaderBindings(&api);
    }
    if (api.failed) {
        return false;
    }

    api.graphicsBufferClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer");
    api.propertyBlockClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock");
    api.uint32Class =
        Il2cppUtils::GetClass("mscorlib.dll", "System", "UInt32");
    api.matrixClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Matrix4x4");
    api.vector4Class = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Vector4");

    api.graphicsBufferCtor = MethodReference(ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer", ".ctor",
        {
            {false, "System.Void",
             {"UnityEngine.GraphicsBuffer.Target", "System.Int32",
              "System.Int32"}},
            {false, "System.Void",
             {"GraphicsBuffer.Target", "System.Int32", "System.Int32"}},
        }));
    api.graphicsBufferSetData = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
        "SetData", false, "System.Void", {"System.Array"}));
    api.graphicsBufferIsValid = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
        "IsValid", false, "System.Boolean", {}));
    api.graphicsBufferRelease = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GraphicsBuffer",
        "Release", false, "System.Void", {}));
    api.propertyBlockCtor = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock",
        ".ctor", false, "System.Void", {}));
    api.propertyBlockSetBuffer = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock",
        "SetBuffer", false, "System.Void",
        {"System.Int32", "UnityEngine.GraphicsBuffer"}));
    api.propertyBlockSetVector = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock",
        "SetVector", false, "System.Void",
        {"System.Int32", "UnityEngine.Vector4"}));
    api.propertyBlockSetMatrixArray = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock",
        "SetMatrixArray", false, "System.Void",
        {"System.Int32", "UnityEngine.Matrix4x4[]"}));
    api.propertyBlockSetVectorArray = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "MaterialPropertyBlock",
        "SetVectorArray", false, "System.Void",
        {"System.Int32", "UnityEngine.Vector4[]"}));
    api.meshGetIndexCount = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "GetIndexCount",
        false, "System.UInt32", {"System.Int32"}));
    api.meshGetIndexStart = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "GetIndexStart",
        false, "System.UInt32", {"System.Int32"}));
    api.meshGetBaseVertex = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Mesh", "GetBaseVertex",
        false, "System.UInt32", {"System.Int32"}));
    api.commandDrawMeshInstancedIndirect = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "CommandBuffer",
        "DrawMeshInstancedIndirect", false, "System.Void",
        {"UnityEngine.Mesh", "System.Int32", "UnityEngine.Material",
         "System.Int32", "UnityEngine.GraphicsBuffer", "System.Int32",
         "UnityEngine.MaterialPropertyBlock"}));
    const bool ready = api.graphicsBufferClass != nullptr &&
        api.propertyBlockClass != nullptr && api.uint32Class != nullptr &&
        api.matrixClass != nullptr && api.vector4Class != nullptr &&
        api.graphicsBufferCtor.Ready() && api.graphicsBufferSetData.Ready() &&
        api.graphicsBufferIsValid.Ready() &&
        api.graphicsBufferRelease.Ready() && api.propertyBlockCtor.Ready() &&
        api.propertyBlockSetBuffer.Ready() &&
        api.propertyBlockSetVector.HasInfo() &&
        api.propertyBlockSetMatrixArray.Ready() &&
        api.propertyBlockSetVectorArray.Ready() &&
        api.meshGetIndexCount.Ready() && api.meshGetIndexStart.Ready() &&
        api.meshGetBaseVertex.Ready() &&
        api.commandDrawMeshInstancedIndirect.Ready();
    api.failed = !ready;
    api.resolved = ready;
    const bool bindings = ready && RefreshCrowdShaderBindings(&api);
    auto line = ClassicLine();
    line << "[VR][stereo] HAND_GLOW crowd draw-api=" << (ready ? 1 : 0)
         << " bindings=" << (bindings ? 1 : 0)
         << " ids=" << api.personInfoBytesBufferId << ","
         << api.boundingMinId << "," << api.handMatricesId << ","
         << api.penlightInfoBytesBufferId << "," << api.penlightColorsId
         << " keyword=" << ManagedIl2CppString(api.useColorTableKeyword);
    LogHandGlow(line.str());
    return bindings;
}

GraphicsBufferDescriptor DescribeGraphicsBuffer(void* buffer) noexcept {
    GraphicsBufferDescriptor result{};
    if (buffer == nullptr || !EnsureCrowdAuditApi()) {
        return result;
    }
    using GetInt = int (*)(void*, void*);
    result.readable =
        InvokeManagedResult<int, GetInt>(
            g_state.crowdAuditApi.graphicsBufferGetCount,
            &result.count, buffer) &&
        InvokeManagedResult<int, GetInt>(
            g_state.crowdAuditApi.graphicsBufferGetStride,
            &result.stride, buffer) &&
        InvokeManagedResult<int, GetInt>(
            g_state.crowdAuditApi.graphicsBufferGetTarget,
            &result.target, buffer);
    return result;
}

void AppendBufferDescriptor(
    std::ostringstream& line,
    std::string_view label,
    void* buffer) noexcept {
    const auto descriptor = DescribeGraphicsBuffer(buffer);
    line << " " << label << "=";
    if (!descriptor.readable) {
        line << "-";
        return;
    }
    line << descriptor.count << "x" << descriptor.stride
         << ":target=0x" << std::hex << descriptor.target << std::dec;
}

int LegacyColorId() noexcept {
    if (g_state.legacyColorId == 0) {
        g_state.legacyColorId = ShaderPropertyToID("_Color");
    }
    return g_state.legacyColorId;
}

void EnableMaterialKeyword(void* material, const char* keyword) noexcept {
    if (material == nullptr || keyword == nullptr ||
        !g_state.api.enableKeyword.Ready()) {
        return;
    }
    auto* managed = UnityResolve::UnityType::String::New(keyword);
    if (managed == nullptr) {
        return;
    }
    using Fn = void (*)(void*, void*, void*);
    static_cast<void>(InvokeManagedVoid<Fn>(
        g_state.api.enableKeyword, material, managed));
}

bool EnableMaterialKeywordObject(void* material, void* keyword) noexcept {
    if (material == nullptr || keyword == nullptr ||
        !g_state.api.enableKeyword.Ready()) {
        return false;
    }
    using Fn = void (*)(void*, void*, void*);
    return InvokeManagedVoid<Fn>(
        g_state.api.enableKeyword, material, keyword);
}

void DisableMaterialKeyword(void* material, const char* keyword) noexcept {
    if (material == nullptr || keyword == nullptr ||
        !g_state.api.disableKeyword.Ready()) {
        return;
    }
    auto* managed = UnityResolve::UnityType::String::New(keyword);
    if (managed == nullptr) {
        return;
    }
    using Fn = void (*)(void*, void*, void*);
    static_cast<void>(InvokeManagedVoid<Fn>(
        g_state.api.disableKeyword, material, managed));
}

bool GetTransformScale(
    const MethodRef& method, void* transform, UnityVector3* scale) noexcept {
    if (!method.Ready() || transform == nullptr || scale == nullptr) {
        return false;
    }
    void* self = ScaleReadSelf(transform);
    if (self == nullptr) {
        return false;
    }
    using Fn = void (*)(void*, UnityVector3*, void*);
    return InvokeManagedVoid<Fn>(method, self, scale);
}

bool GetMeshSize(void* mesh, UnityVector3* size) noexcept {
    if (mesh == nullptr || size == nullptr ||
        !g_state.api.getMeshBounds.Ready()) {
        return false;
    }
    UnityBounds bounds{};
    void* self = g_state.api.meshBoundsUsesNativeSelf
        ? ReadUnityNativePointer(mesh)
        : mesh;
    if (self == nullptr) {
        return false;
    }
    using Fn = void (*)(void*, UnityBounds*, void*);
    if (!InvokeManagedVoid<Fn>(g_state.api.getMeshBounds, self, &bounds)) {
        return false;
    }
    size->x = bounds.m_vExtents.x * 2.0F;
    size->y = bounds.m_vExtents.y * 2.0F;
    size->z = bounds.m_vExtents.z * 2.0F;
    return true;
}

bool EnsurePropertyBlock() noexcept {
    if (g_state.propertyBlock != nullptr &&
        IsUnityManagedObjectAlive(g_state.propertyBlock)) {
        return true;
    }
    if (g_state.api.propertyBlockClass == nullptr ||
        !g_state.api.propertyBlockCtor.Ready()) {
        return false;
    }
    void* block = NewIl2CppObject(g_state.api.propertyBlockClass->address);
    if (block == nullptr) {
        return false;
    }
    using Ctor = void (*)(void*, void*);
    if (!InvokeManagedVoid<Ctor>(g_state.api.propertyBlockCtor, block)) {
        return false;
    }
    RetireHandle(g_state.propertyBlockHandle);
    g_state.propertyBlockHandle = CreateGcHandle(block);
    g_state.propertyBlock = block;
    return g_state.propertyBlockHandle != nullptr;
}

bool GetMpbColor(void* block, int propertyId, UnityColor* color) noexcept {
    if (block == nullptr || color == nullptr || propertyId == 0 ||
        !g_state.api.mpbGetColorInjected.HasInfo()) {
        return false;
    }
    int id = propertyId;
    if (g_state.api.mpbGetColorUsesOut) {
        void* arguments[] = {&id, color};
        void* ignored = nullptr;
        return RuntimeInvoke(
            g_state.api.mpbGetColorInjected, block, arguments, &ignored);
    }
    void* arguments[] = {&id};
    void* boxed = nullptr;
    return RuntimeInvoke(
               g_state.api.mpbGetColorInjected, block, arguments, &boxed) &&
        UnboxColor(boxed, color);
}

bool SetMpbColor(void* block, int propertyId, const UnityColor& color) noexcept {
    if (block == nullptr || propertyId == 0 ||
        !g_state.api.mpbSetColorInjected.HasInfo()) {
        return false;
    }
    int id = propertyId;
    UnityColor local = color;
    void* arguments[] = {&id, &local};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.mpbSetColorInjected, block, arguments, &ignored);
}

bool SameColor(const UnityColor& left, const UnityColor& right) noexcept {
    return std::fabs(left.r - right.r) + std::fabs(left.g - right.g) +
        std::fabs(left.b - right.b) <
        0.02F;
}

float ColorLuma(const UnityColor& color) noexcept {
    return color.r + color.g + color.b;
}

UnityColor ScaleByAudienceIntensity(const UnityColor& color) noexcept {
    const float intensity = std::clamp(g_state.audienceIntensity, 0.0F, 4.0F);
    return UnityColor(
        color.r * intensity,
        color.g * intensity,
        color.b * intensity,
        color.a);
}

bool HasMaterialProperty(void* material, int propertyId) noexcept {
    if (material == nullptr || propertyId == 0 ||
        !g_state.api.hasProperty.Ready()) {
        return false;
    }
    using Fn = bool (*)(void*, std::int32_t, void*);
    bool has = false;
    return InvokeManagedResult<bool, Fn>(
               g_state.api.hasProperty, &has, material, propertyId) &&
        has;
}

bool GetMaterialColor(
    void* material, int propertyId, UnityColor* color) noexcept {
    if (material == nullptr || color == nullptr || propertyId == 0 ||
        !g_state.api.getColorInjected.HasInfo()) {
        return false;
    }
    int id = propertyId;
    if (g_state.api.getColorUsesOut) {
        void* arguments[] = {&id, color};
        void* ignored = nullptr;
        return RuntimeInvoke(
            g_state.api.getColorInjected, material, arguments, &ignored);
    }
    void* arguments[] = {&id};
    void* boxed = nullptr;
    return RuntimeInvoke(
               g_state.api.getColorInjected, material, arguments, &boxed) &&
        UnboxColor(boxed, color);
}

bool SetMaterialColor(
    void* material, int propertyId, const UnityColor& color) noexcept {
    if (material == nullptr || propertyId == 0 ||
        !g_state.api.setColorInjected.HasInfo()) {
        return false;
    }
    int id = propertyId;
    UnityColor local = color;
    void* arguments[] = {&id, &local};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.setColorInjected, material, arguments, &ignored);
}

bool SetMaterialVector(
    void* material, int propertyId, const UnityVector4& value) noexcept {
    if (material == nullptr || propertyId == 0 ||
        !g_state.api.setVectorInjected.HasInfo()) {
        return false;
    }
    int id = propertyId;
    UnityVector4 local = value;
    void* arguments[] = {&id, &local};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.setVectorInjected, material, arguments, &ignored);
}

bool GetMaterialVector(
    void* material, int propertyId, UnityVector4* value) noexcept {
    if (material == nullptr || value == nullptr || propertyId == 0 ||
        !g_state.api.getVectorInjected.HasInfo()) {
        return false;
    }
    int id = propertyId;
    void* arguments[] = {&id, value};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.getVectorInjected, material, arguments, &ignored);
}

bool SetMaterialFloat(void* material, int propertyId, float value) noexcept {
    if (material == nullptr || propertyId == 0 ||
        !g_state.api.setFloatImpl.HasInfo()) {
        return false;
    }
    int id = propertyId;
    float local = value;
    void* arguments[] = {&id, &local};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.setFloatImpl, material, arguments, &ignored);
}

float GetMaterialFloat(void* material, int propertyId) noexcept {
    if (material == nullptr || propertyId == 0 ||
        !g_state.api.getFloatImpl.HasInfo()) {
        return 0.0F;
    }
    int id = propertyId;
    void* arguments[] = {&id};
    void* boxed = nullptr;
    float value = 0.0F;
    if (!RuntimeInvoke(
            g_state.api.getFloatImpl, material, arguments, &boxed) ||
        !UnboxFloat(boxed, &value)) {
        return 0.0F;
    }
    return value;
}

bool SetMaterialRenderQueue(void* material, int queue) noexcept {
    if (material == nullptr || !g_state.api.setRenderQueue.HasInfo()) {
        return false;
    }
    void* arguments[] = {&queue};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.setRenderQueue, material, arguments, &ignored);
}

bool SetMaterialOverrideTag(
    void* material, const char* tag, const char* value) noexcept {
    if (material == nullptr || tag == nullptr || value == nullptr ||
        !g_state.api.setOverrideTag.HasInfo()) {
        return false;
    }
    auto* managedTag = UnityResolve::UnityType::String::New(tag);
    auto* managedValue = UnityResolve::UnityType::String::New(value);
    if (managedTag == nullptr || managedValue == nullptr) {
        return false;
    }
    void* arguments[] = {managedTag, managedValue};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.setOverrideTag, material, arguments, &ignored);
}

bool ConfigureHandEmissionMaterial(void* material) noexcept {
    if (material == nullptr ||
        ManagedObjectName(GetShader(material)) != kHandEmissionShader) {
        return false;
    }
    // Crowd/DefaultPenlight writes colored emission separately from its white
    // lit GBuffer substrate. This material reproduces only that emission term:
    // Particle/Unlit preserves the Crowd mesh COLOR.r ramp and additive blend
    // places it over the opaque lit substrate configured below.
    const struct {
        const char* name;
        float value;
    } values[] = {
        {"_Surface", 1.0F},
        {"_Blend", 2.0F},
        {"_BlendOp", 0.0F},
        {"_SrcBlend", 5.0F},
        {"_DstBlend", 1.0F},
        {"_SrcBlendAlpha", 1.0F},
        {"_DstBlendAlpha", 1.0F},
        {"_ZWrite", 0.0F},
        {"_Cull", 2.0F},
        {"_LightingEnabled", 0.0F},
        {"_AlphaClip", 0.0F},
        {"_AlphaToMask", 0.0F},
        {"_ColorMask", 15.0F},
        {"_Mode", 2.0F},
        {"_ColorMode", 2.0F},
        {"_ZTest", 4.0F},
    };
    bool configured = true;
    for (const auto& item : values) {
        const int id = ShaderPropertyToID(item.name);
        configured = SetMaterialFloat(material, id, item.value) && configured;
    }
    EnableMaterialKeyword(material, "_SURFACE_TYPE_TRANSPARENT");
    DisableMaterialKeyword(material, "_ALPHATEST_ON");
    DisableMaterialKeyword(material, "_ALPHAPREMULTIPLY_ON");
    DisableMaterialKeyword(material, "_ALPHAMODULATE_ON");
    configured = SetMaterialRenderQueue(material, 3000) && configured;
    configured = SetMaterialOverrideTag(
                     material, "RenderType", "Transparent") &&
        configured;
    return configured;
}

bool ConfigureHandBaseMaterial(void* material) noexcept {
    if (material == nullptr ||
        ManagedObjectName(GetShader(material)) != kHandBaseShader) {
        return false;
    }
    const struct {
        const char* name;
        float value;
    } values[] = {
        {"_SrcBlend", 1.0F},
        {"_DstBlend", 0.0F},
        {"_ZWrite", 1.0F},
        {"_Cull", 2.0F},
        {"_ColorMask", 15.0F},
        {"_ColorMask1", 15.0F},
        {"_RenderMode", 0.0F},
        {"_ShaderType", 0.0F},
        {"_VertexColor", 0.0F},
        {"_EnableEmission", 0.0F},
    };
    bool configured = true;
    for (const auto& item : values) {
        configured =
            SetMaterialFloat(material, ShaderPropertyToID(item.name), item.value) &&
            configured;
    }
    const UnityColor white(1.0F, 1.0F, 1.0F, 1.0F);
    configured = SetMaterialColor(material, BaseColorId(), white) && configured;
    configured =
        SetMaterialColor(material, ShaderPropertyToID("_MultiplyColor"), white) &&
        configured;
    EnableMaterialKeyword(material, "_RENDERMODE_NORMAL");
    DisableMaterialKeyword(material, "_VERTEXCOLOR_ON");
    DisableMaterialKeyword(material, "_ALPHATEST_ON");
    DisableMaterialKeyword(material, "_ALPHAPREMULTIPLY_ON");
    DisableMaterialKeyword(material, "_EMISSION");
    DisableMaterialKeyword(material, "_CULL_OFF");
    configured = SetMaterialRenderQueue(material, 2000) && configured;
    configured = SetMaterialOverrideTag(material, "RenderType", "Opaque") &&
        configured;
    return configured;
}

void* CreateMaterialForShader(const char* shaderName) noexcept {
    if (g_state.api.materialClass == nullptr ||
        g_state.api.materialClass->address == nullptr ||
        !g_state.api.findShader.HasInfo() ||
        !g_state.api.materialCtor.HasInfo() || shaderName == nullptr) {
        return nullptr;
    }
    auto* managedShaderName = UnityResolve::UnityType::String::New(shaderName);
    if (managedShaderName == nullptr) {
        return nullptr;
    }
    void* findArguments[] = {managedShaderName};
    void* shader = nullptr;
    if (!RuntimeInvoke(
            g_state.api.findShader, nullptr, findArguments, &shader) ||
        shader == nullptr || !IsUnityManagedObjectAlive(shader)) {
        LogHandGlow(
            std::string("[VR][stereo] HAND_GLOW material skip=shader-find name=") +
            shaderName);
        return nullptr;
    }
    void* material = NewIl2CppObject(g_state.api.materialClass->address);
    if (material == nullptr) {
        return nullptr;
    }
    void* ctorArguments[] = {shader};
    void* ignored = nullptr;
    if (!RuntimeInvoke(
            g_state.api.materialCtor, material, ctorArguments, &ignored) ||
        !IsUnityManagedObjectAlive(material)) {
        LogHandGlow(
            std::string("[VR][stereo] HAND_GLOW material skip=ctor name=") +
            shaderName);
        return nullptr;
    }
    return material;
}

void* CreateHandEmissionMaterial() noexcept {
    void* material = CreateMaterialForShader(kHandEmissionShader);
    if (material == nullptr || !ConfigureHandEmissionMaterial(material)) {
        LogHandGlow("[VR][stereo] HAND_GLOW material skip=configure-emission");
        return nullptr;
    }
    return material;
}

void* CreateHandBaseMaterial() noexcept {
    void* material = CreateMaterialForShader(kHandBaseShader);
    if (material == nullptr || !ConfigureHandBaseMaterial(material)) {
        LogHandGlow("[VR][stereo] HAND_GLOW material skip=configure-base");
        return nullptr;
    }
    return material;
}

bool SetMpbVector(
    void* block, int propertyId, const UnityVector4& value) noexcept {
    if (block == nullptr || propertyId == 0 ||
        !g_state.api.mpbSetVectorInjected.HasInfo()) {
        return false;
    }
    int id = propertyId;
    UnityVector4 local = value;
    void* arguments[] = {&id, &local};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.mpbSetVectorInjected, block, arguments, &ignored);
}

void LogShaderProperties(void* material) noexcept {
    if (material == nullptr || !g_state.api.getPropertyCount.Ready() ||
        !g_state.api.getPropertyName.Ready()) {
        return;
    }
    void* shader = GetShader(material);
    if (shader == nullptr) {
        return;
    }
    using CountFn = std::int32_t (*)(void*, void*);
    std::int32_t count = 0;
    if (!InvokeManagedResult<std::int32_t, CountFn>(
            g_state.api.getPropertyCount, &count, shader) ||
        count <= 0) {
        return;
    }
    const int colorId = PenlightColorId();
    const int logged = (std::min)(count, 32);
    auto line = ClassicLine();
    line << "[VR][stereo] HAND_GLOW list kind=shader"
         << " n=" << count << " id=" << colorId
         << " has=" << (HasMaterialProperty(material, colorId) ? 1 : 0);
    UnityColor official(0.0F, 0.0F, 0.0F, 0.0F);
    if (GetMaterialColor(material, colorId, &official)) {
        line << " official=" << official.r << "," << official.g << ","
             << official.b << "," << official.a;
    }
    for (int i = 0; i < logged; ++i) {
        using NameFn = void* (*)(void*, std::int32_t, void*);
        void* nameObject = nullptr;
        if (!InvokeManagedResult<void*, NameFn>(
                g_state.api.getPropertyName, &nameObject, shader, i) ||
            nameObject == nullptr) {
            continue;
        }
        const std::string name = ManagedIl2CppString(nameObject);
        if (name.empty()) {
            continue;
        }
        const int id = ShaderPropertyToID(name.c_str());
        UnityColor color(0.0F, 0.0F, 0.0F, 0.0F);
        const bool got = GetMaterialColor(material, id, &color);
        line << " p" << i << "=" << name << "@" << id;
        if (got && ColorLuma(color) > 0.01F) {
            line << ":" << color.r << "," << color.g << "," << color.b;
        }
    }
    LogHandGlow(line.str());
}

bool ReadRendererMpbColor(void* renderer, UnityColor* color) noexcept {
    if (renderer == nullptr || color == nullptr || !EnsurePropertyBlock() ||
        !g_state.api.getPropertyBlock.Ready()) {
        return false;
    }
    using Fn = void (*)(void*, void*, void*);
    if (!InvokeManagedVoid<Fn>(
            g_state.api.getPropertyBlock, renderer, g_state.propertyBlock)) {
        return false;
    }
    return GetMpbColor(g_state.propertyBlock, PenlightColorId(), color);
}

bool CopyMpbFromMob(int colorIndex, void* destinationRenderer) noexcept {
    if (destinationRenderer == nullptr || !EnsurePropertyBlock() ||
        !g_state.api.getPropertyBlock.Ready() ||
        !g_state.api.setPropertyBlock.Ready()) {
        return false;
    }
    void* source = nullptr;
    for (const auto& mob : g_state.mobSources) {
        if (mob.renderer == nullptr ||
            !IsUnityManagedObjectAlive(mob.renderer)) {
            continue;
        }
        if (mob.colorIndex == colorIndex || source == nullptr) {
            source = mob.renderer;
            if (mob.colorIndex == colorIndex) {
                break;
            }
        }
    }
    if (source == nullptr) {
        return false;
    }
    using Fn = void (*)(void*, void*, void*);
    if (!InvokeManagedVoid<Fn>(
            g_state.api.getPropertyBlock, source, g_state.propertyBlock)) {
        return false;
    }
    return InvokeManagedVoid<Fn>(
        g_state.api.setPropertyBlock, destinationRenderer, g_state.propertyBlock);
}

bool OverlayOfficialColor(
    void* renderer, int materialIndex, const UnityColor& color) noexcept {
    const int colorId = PenlightColorId();
    if (renderer == nullptr || colorId == 0 || !EnsurePropertyBlock() ||
        !g_state.api.getPropertyBlockIndexed.Ready() ||
        !g_state.api.setPropertyBlockIndexed.Ready()) {
        return false;
    }
    using Fn = void (*)(void*, void*, int, void*);
    if (!InvokeManagedVoid<Fn>(
            g_state.api.getPropertyBlockIndexed,
            renderer,
            g_state.propertyBlock,
            materialIndex)) {
        return false;
    }
    const UnityColor scaled = ScaleByAudienceIntensity(color);
    const bool wroteColor = SetMpbColor(g_state.propertyBlock, colorId, color);
    const bool wroteBase =
        SetMpbColor(g_state.propertyBlock, BaseColorId(), scaled);
    const bool wroteLegacy =
        SetMpbColor(g_state.propertyBlock, LegacyColorId(), scaled);
    const UnityVector4 officialVector(color.r, color.g, color.b, color.a);
    const UnityVector4 scaledVector(scaled.r, scaled.g, scaled.b, scaled.a);
    static_cast<void>(
        SetMpbVector(g_state.propertyBlock, colorId, officialVector));
    static_cast<void>(
        SetMpbVector(g_state.propertyBlock, BaseColorId(), scaledVector));
    static_cast<void>(
        SetMpbVector(g_state.propertyBlock, LegacyColorId(), scaledVector));
    const bool applied = InvokeManagedVoid<Fn>(
        g_state.api.setPropertyBlockIndexed,
        renderer,
        g_state.propertyBlock,
        materialIndex);
    return wroteColor && wroteBase && wroteLegacy && applied;
}

bool EnsureHandMaterialInfo(HandObject& hand) noexcept {
    if (hand.materialInfo != nullptr) {
        return true;
    }
    if (hand.renderer == nullptr || hand.material == nullptr ||
        !IsUnityManagedObjectAlive(hand.renderer) ||
        !IsUnityManagedObjectAlive(hand.material) ||
        g_state.api.materialInfoClass == nullptr ||
        !g_state.api.materialInfoCtor.HasInfo()) {
        return false;
    }
    void* info = NewIl2CppObject(g_state.api.materialInfoClass->address);
    if (info == nullptr) {
        return false;
    }
    // The opaque substrate is optional so a missing base shader can never hide
    // the glow stick. The official controller always owns the emission slot.
    int materialIndex = hand.emissionMaterialIndex;
    void* arguments[] = {
        hand.material, hand.renderer, &materialIndex, hand.material};
    void* ignored = nullptr;
    if (!RuntimeInvoke(
            g_state.api.materialInfoCtor, info, arguments, &ignored)) {
        return false;
    }
    Il2CppGCHandle handle = CreateGcHandle(info);
    if (handle == nullptr) {
        return false;
    }
    if (g_state.api.materialInfoInit.HasInfo()) {
        static_cast<void>(RuntimeInvoke(
            g_state.api.materialInfoInit, info, nullptr, &ignored));
    }
    hand.materialInfo = info;
    hand.materialInfoHandle = handle;
    void* instanced = ReadNamedRef(info, "_instancedMaterial");
    if (instanced != nullptr && IsUnityManagedObjectAlive(instanced) &&
        instanced != hand.material) {
        RetireHandle(hand.materialHandle);
        hand.materialHandle = CreateGcHandle(instanced);
        hand.material = instanced;
        static_cast<void>(AssignHandMaterials(
            hand.renderer, hand.baseMaterial, instanced));
    }
    return true;
}

bool ReadMaterialInfoColor(void* info, int propertyId, UnityColor* color) noexcept {
    if (info == nullptr || color == nullptr || propertyId == 0 ||
        !g_state.api.materialInfoGetColor.HasInfo()) {
        return false;
    }
    int id = propertyId;
    void* arguments[] = {&id};
    void* boxed = nullptr;
    return RuntimeInvoke(
               g_state.api.materialInfoGetColor, info, arguments, &boxed) &&
        UnboxColor(boxed, color);
}

bool ReadMaterialInfoFloat(void* info, int propertyId, float* value) noexcept {
    if (info == nullptr || value == nullptr || propertyId == 0 ||
        !g_state.api.materialInfoGetFloat.HasInfo()) {
        return false;
    }
    int id = propertyId;
    void* arguments[] = {&id};
    void* boxed = nullptr;
    return RuntimeInvoke(
               g_state.api.materialInfoGetFloat, info, arguments, &boxed) &&
        UnboxFloat(boxed, value);
}

bool WriteMaterialInfoFloat(void* info, int propertyId, float value) noexcept {
    if (info == nullptr || propertyId == 0 ||
        !g_state.api.materialInfoSetFloat.HasInfo()) {
        return false;
    }
    int id = propertyId;
    float local = value;
    void* arguments[] = {&id, &local};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.materialInfoSetFloat, info, arguments, &ignored);
}

bool ReadMaterialInfoVector(
    void* info, int propertyId, UnityVector4* value) noexcept {
    if (info == nullptr || value == nullptr || propertyId == 0 ||
        !g_state.api.materialInfoGetVector.HasInfo()) {
        return false;
    }
    int id = propertyId;
    void* arguments[] = {&id};
    void* boxed = nullptr;
    return RuntimeInvoke(
               g_state.api.materialInfoGetVector, info, arguments, &boxed) &&
        UnboxVector4(boxed, value);
}

bool WriteMaterialInfoVector(
    void* info, int propertyId, const UnityVector4& value) noexcept {
    if (info == nullptr || propertyId == 0 ||
        !g_state.api.materialInfoSetVector.HasInfo()) {
        return false;
    }
    int id = propertyId;
    UnityVector4 local = value;
    void* arguments[] = {&id, &local};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.materialInfoSetVector, info, arguments, &ignored);
}

void ApplyHandMaterialInfo(void* info) noexcept {
    if (info == nullptr || !g_state.api.materialInfoApply.HasInfo()) {
        return;
    }
    void* ignored = nullptr;
    static_cast<void>(RuntimeInvoke(
        g_state.api.materialInfoApply, info, nullptr, &ignored));
}

bool SetMaterialInfoColor(
    void* info, int propertyId, const UnityColor& color) noexcept {
    if (info == nullptr || propertyId == 0 ||
        !g_state.api.materialInfoSetColor.HasInfo()) {
        return false;
    }
    int colorId = propertyId;
    UnityColor local = color;
    bool convertLinear = false;
    void* arguments[] = {&colorId, &local, &convertLinear};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.api.materialInfoSetColor, info, arguments, &ignored);
}

bool WriteHandMaterialInfoColors(
    void* info, const UnityColor& color) noexcept {
    const UnityColor scaled = ScaleByAudienceIntensity(color);
    const bool wroteOfficial =
        SetMaterialInfoColor(info, PenlightColorId(), color);
    const bool wroteBase = SetMaterialInfoColor(info, BaseColorId(), scaled);
    const bool wroteLegacy = SetMaterialInfoColor(info, LegacyColorId(), scaled);
    ApplyHandMaterialInfo(info);
    return wroteOfficial && wroteBase && wroteLegacy;
}

void* RuntimeSettingClass() noexcept {
    auto* controllerClass = FindCampusClass(
        "Campus.MobAudience", "MobAudiencePenlightController");
    if (controllerClass == nullptr || controllerClass->address == nullptr) {
        return nullptr;
    }
    return Il2cppUtils::find_nested_class_from_name(
        controllerClass->address, "RuntimeMobPenlightSetting");
}

void* NewManagedArray(void* elementClass, std::uintptr_t count) noexcept {
    if (elementClass == nullptr) {
        return nullptr;
    }
    return UnityResolve::Invoke<void*, void*, std::uintptr_t>(
        "il2cpp_array_new", elementClass, count);
}

void* CloneCrowdMeshWithVertexGradient(
    void* sourceMesh, const UnityVector3& meshSize) noexcept {
    if (sourceMesh == nullptr || !IsUnityManagedObjectAlive(sourceMesh) ||
        g_state.api.colorClass == nullptr ||
        g_state.api.colorClass->address == nullptr ||
        !g_state.api.getMeshVertices.HasInfo() ||
        !g_state.api.getMeshColors.HasInfo() ||
        !g_state.api.setMeshColors.HasInfo()) {
        return nullptr;
    }

    void* verticesObject = nullptr;
    if (!RuntimeInvoke(
            g_state.api.getMeshVertices, sourceMesh, nullptr, &verticesObject) ||
        verticesObject == nullptr) {
        LogHandGlow(
            "[VR][stereo] HAND_GLOW gradient skip=get-vertices");
        return nullptr;
    }
    void* colorsObject = nullptr;
    if (!RuntimeInvoke(
            g_state.api.getMeshColors, sourceMesh, nullptr, &colorsObject) ||
        colorsObject == nullptr) {
        LogHandGlow("[VR][stereo] HAND_GLOW gradient skip=get-colors");
        return nullptr;
    }
    const auto vertices =
        reinterpret_cast<UnityResolve::UnityType::Array<UnityVector3>*>(
            verticesObject)
            ->ToVector();
    const auto sourceColors =
        reinterpret_cast<UnityResolve::UnityType::Array<UnityColor>*>(
            colorsObject)
            ->ToVector();
    if (vertices.empty() || sourceColors.size() != vertices.size()) {
        LogHandGlow(
            "[VR][stereo] HAND_GLOW gradient skip=color-count");
        return nullptr;
    }

    int axis = 0;
    if (meshSize.y > meshSize.x && meshSize.y >= meshSize.z) {
        axis = 1;
    } else if (meshSize.z > meshSize.x && meshSize.z > meshSize.y) {
        axis = 2;
    }
    const auto coordinate = [axis](const UnityVector3& vertex) noexcept {
        if (axis == 1) {
            return vertex.y;
        }
        if (axis == 2) {
            return vertex.z;
        }
        return vertex.x;
    };
    float minimum = coordinate(vertices.front());
    float maximum = minimum;
    for (const auto& vertex : vertices) {
        minimum = (std::min)(minimum, coordinate(vertex));
        maximum = (std::max)(maximum, coordinate(vertex));
    }
    const float span = maximum - minimum;
    if (!(span > 1.0e-5F) || !std::isfinite(span)) {
        LogHandGlow(
            "[VR][stereo] HAND_GLOW gradient skip=invalid-axis");
        return nullptr;
    }

    float minimumColor = std::clamp(sourceColors.front().r, 0.0F, 1.0F);
    float maximumColor = minimumColor;
    float lowerSum = 0.0F;
    float upperSum = 0.0F;
    int lowerCount = 0;
    int upperCount = 0;
    void* colorObject = NewManagedArray(
        g_state.api.colorClass->address, sourceColors.size());
    if (colorObject == nullptr) {
        LogHandGlow("[VR][stereo] HAND_GLOW gradient skip=color-array");
        return nullptr;
    }
    auto* colorArray = reinterpret_cast<
        UnityResolve::UnityType::Array<UnityColor>*>(colorObject);
    for (std::size_t index = 0; index < sourceColors.size(); ++index) {
        const float scalar = std::clamp(sourceColors[index].r, 0.0F, 1.0F);
        minimumColor = (std::min)(minimumColor, scalar);
        maximumColor = (std::max)(maximumColor, scalar);
        const float longitudinal =
            (coordinate(vertices[index]) - minimum) / span;
        if (longitudinal <= 0.05F) {
            lowerSum += scalar;
            ++lowerCount;
        }
        if (longitudinal >= 0.95F) {
            upperSum += scalar;
            ++upperCount;
        }
        // Crowd DefaultPenlight consumes only COLOR.r as its longitudinal
        // emission scalar. Particle/Unlit consumes rgba, so replicate that
        // exact authored scalar to rgb and keep alpha fully contributing.
        colorArray->At(static_cast<unsigned int>(index)) =
            UnityColor(scalar, scalar, scalar, 1.0F);
    }

    void* clone = CloneUnityObject(sourceMesh);
    if (clone == nullptr) {
        LogHandGlow("[VR][stereo] HAND_GLOW gradient skip=clone-mesh");
        return nullptr;
    }
    void* arguments[] = {colorObject};
    void* ignored = nullptr;
    if (!RuntimeInvoke(g_state.api.setMeshColors, clone, arguments, &ignored)) {
        LogHandGlow("[VR][stereo] HAND_GLOW gradient skip=set-colors");
        return nullptr;
    }

    auto line = ClassicLine();
    line << "[VR][stereo] HAND_GLOW gradient mesh=private-crowd"
         << " axis=" << (axis == 0 ? "x" : (axis == 1 ? "y" : "z"))
         << " vertices=" << vertices.size()
         << " range=" << minimum << "," << maximum
         << " colorR=" << minimumColor << ".." << maximumColor
         << " lower="
         << (lowerCount > 0 ? lowerSum / static_cast<float>(lowerCount) : -1.0F)
         << " upper="
         << (upperCount > 0 ? upperSum / static_cast<float>(upperCount) : -1.0F)
         << " source=audience-vertex";
    LogHandGlow(line.str());
    return clone;
}

void InvokeDefaultCtor(void* klass, void* object) noexcept {
    if (klass == nullptr || object == nullptr) {
        return;
    }
    auto* method = Il2cppUtils::il2cpp_class_get_method_from_name(
        klass, ".ctor", 0);
    if (method == nullptr) {
        return;
    }
    void* exception = nullptr;
    void* result = nullptr;
    static_cast<void>(RuntimeInvokeRaw(
        method, object, nullptr, &result, &exception));
}

void* WriteObjectArray(void* elementClass, const std::vector<void*>& items) noexcept {
    void* array = NewManagedArray(elementClass, items.size());
    if (array == nullptr) {
        return nullptr;
    }
    auto* typed =
        reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(array);
    for (std::size_t i = 0; i < items.size(); ++i) {
        void** slot = &typed->At(static_cast<unsigned int>(i));
        if (!StoreManagedReference(array, slot, items[i])) {
            return nullptr;
        }
    }
    return array;
}

bool AssignHandMaterials(
    void* renderer, void* baseMaterial, void* emissionMaterial) noexcept {
    if (renderer == nullptr || emissionMaterial == nullptr ||
        g_state.api.materialClass == nullptr ||
        g_state.api.materialClass->address == nullptr ||
        !g_state.api.setSharedMaterial.Ready()) {
        return false;
    }
    if (baseMaterial == nullptr) {
        using Fn = void (*)(void*, void*, void*);
        return InvokeManagedVoid<Fn>(
            g_state.api.setSharedMaterial, renderer, emissionMaterial);
    }
    if (!g_state.api.setSharedMaterials.Ready()) {
        return false;
    }
    void* materials = WriteObjectArray(
        g_state.api.materialClass->address, {baseMaterial, emissionMaterial});
    if (materials == nullptr) {
        return false;
    }
    using Fn = void (*)(void*, void*, void*);
    return InvokeManagedVoid<Fn>(
        g_state.api.setSharedMaterials, renderer, materials);
}

bool RegisteredInfosMatch(
    void* setting, const std::vector<void*>& infos) noexcept {
    if (setting == nullptr) {
        return false;
    }
    void* existing = ReadNamedRef(setting, "MaterialInfos");
    if (existing == nullptr) {
        return false;
    }
    auto items =
        reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(existing)
            ->ToVector();
    if (items.size() != infos.size()) {
        return false;
    }
    for (std::size_t i = 0; i < infos.size(); ++i) {
        if (items[i] != infos[i]) {
            return false;
        }
    }
    return true;
}

void* LivePenlightController() noexcept {
    if (g_state.penlightController != nullptr &&
        IsUnityManagedObjectAlive(g_state.penlightController)) {
        return g_state.penlightController;
    }
    g_state.penlightController = nullptr;
    g_state.registered = false;
    g_state.registeredSetting = nullptr;
    auto* controllerClass = FindCampusClass(
        "Campus.MobAudience", "MobAudiencePenlightController");
    if (controllerClass == nullptr) {
        return nullptr;
    }
    const auto controllers = FindAllOfClass(controllerClass);
    for (void* controller : controllers) {
        if (controller != nullptr && IsUnityManagedObjectAlive(controller) &&
            ReadNamedRef(controller, "_runtimeMobPenlightSettings") !=
                nullptr) {
            g_state.penlightController = controller;
            return controller;
        }
    }
    return nullptr;
}

bool SettingStillRegistered(void* controller, void* setting) noexcept {
    if (controller == nullptr || setting == nullptr) {
        return false;
    }
    void* runtime = ReadNamedRef(controller, "_runtimeMobPenlightSettings");
    if (runtime == nullptr) {
        return false;
    }
    auto items =
        reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(runtime)
            ->ToVector();
    for (void* item : items) {
        if (item == setting) {
            return true;
        }
    }
    return false;
}

void UnregisterHandsFromOfficialController() noexcept {
    void* controller = g_state.penlightController;
    void* setting = g_state.registeredSetting;
    if (controller != nullptr && IsUnityManagedObjectAlive(controller) &&
        setting != nullptr) {
        void* runtime = ReadNamedRef(controller, "_runtimeMobPenlightSettings");
        void* settingClass = RuntimeSettingClass();
        if (runtime != nullptr && settingClass != nullptr) {
            auto oldItems =
                reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(
                    runtime)
                    ->ToVector();
            std::vector<void*> kept;
            kept.reserve(oldItems.size());
            for (void* item : oldItems) {
                if (item != setting) {
                    kept.push_back(item);
                }
            }
            if (kept.size() != oldItems.size()) {
                void* shrunk = WriteObjectArray(settingClass, kept);
                if (shrunk != nullptr) {
                    WriteNamedRef(
                        controller, "_runtimeMobPenlightSettings", shrunk);
                }
            }
        }
    }
    RetireHandle(g_state.registeredSettingHandle);
    g_state.registeredSettingHandle = nullptr;
    g_state.registeredSetting = nullptr;
    g_state.registered = false;
}

void LogRegisterSkip(const char* reason) noexcept {
    static std::string lastReason;
    if (reason == nullptr || lastReason == reason) {
        return;
    }
    lastReason = reason;
    LogHandGlow(
        std::string("[VR][stereo] HAND_GLOW register skip=") + reason);
}

bool RegisterHandsWithOfficialController() noexcept {
    void* controller = LivePenlightController();
    if (controller == nullptr || g_state.api.materialInfoClass == nullptr ||
        g_state.api.materialInfoClass->address == nullptr) {
        LogRegisterSkip(controller == nullptr ? "controller" : "info-class");
        return false;
    }
    std::vector<void*> infos;
    for (auto& hand : g_state.hands) {
        if (EnsureHandMaterialInfo(hand) && hand.materialInfo != nullptr) {
            infos.push_back(hand.materialInfo);
        }
    }
    if (infos.empty()) {
        UnregisterHandsFromOfficialController();
        LogRegisterSkip("no-info");
        return false;
    }
    const int slot = g_state.stickySlot >= 0 ? g_state.stickySlot : 0;
    if (g_state.registered &&
        SettingStillRegistered(controller, g_state.registeredSetting)) {
        if (ReadNamedInt(g_state.registeredSetting, "ColorTableIndex") !=
            slot) {
            WriteNamedInt(g_state.registeredSetting, "ColorTableIndex", slot);
        }
        if (!RegisteredInfosMatch(g_state.registeredSetting, infos)) {
            void* infoArray = WriteObjectArray(
                g_state.api.materialInfoClass->address, infos);
            if (infoArray != nullptr) {
                WriteNamedRef(
                    g_state.registeredSetting, "MaterialInfos", infoArray);
            }
        }
        return true;
    }
    void* settingClass = RuntimeSettingClass();
    if (settingClass == nullptr) {
        LogRegisterSkip("nested");
        return false;
    }
    void* setting = NewIl2CppObject(settingClass);
    if (setting == nullptr) {
        LogRegisterSkip("setting");
        return false;
    }
    InvokeDefaultCtor(settingClass, setting);
    void* infoArray =
        WriteObjectArray(g_state.api.materialInfoClass->address, infos);
    if (infoArray == nullptr) {
        LogRegisterSkip("info-array");
        return false;
    }
    WriteNamedInt(setting, "MobActorIndex", -1);
    WriteNamedInt(setting, "ColorTableIndex", slot);
    WriteNamedRef(setting, "MaterialInfos", infoArray);
    void* runtime = ReadNamedRef(controller, "_runtimeMobPenlightSettings");
    if (runtime == nullptr) {
        LogRegisterSkip("runtime");
        return false;
    }
    auto oldItems =
        reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(runtime)
            ->ToVector();
    std::vector<void*> grownItems = oldItems;
    grownItems.push_back(setting);
    void* grown = WriteObjectArray(settingClass, grownItems);
    if (grown == nullptr) {
        LogRegisterSkip("grow");
        return false;
    }
    WriteNamedRef(controller, "_runtimeMobPenlightSettings", grown);
    RetireHandle(g_state.registeredSettingHandle);
    g_state.registeredSettingHandle = CreateGcHandle(setting);
    g_state.registeredSetting = setting;
    g_state.registered = true;
    auto line = ClassicLine();
    line << "[VR][stereo] HAND_GLOW register infos=" << infos.size()
         << " idx=" << slot << " prev=" << oldItems.size();
    LogHandGlow(line.str());
    return true;
}

UnityColor FirstOfficialInfoColor(void* controller, void* skipSetting) noexcept {
    UnityColor color(0.0F, 0.0F, 0.0F, 0.0F);
    if (controller == nullptr) {
        return color;
    }
    void* runtime = ReadNamedRef(controller, "_runtimeMobPenlightSettings");
    if (runtime == nullptr) {
        return color;
    }
    auto settings =
        reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(runtime)
            ->ToVector();
    for (void* setting : settings) {
        if (setting == nullptr || setting == skipSetting) {
            continue;
        }
        void* infos = ReadNamedRef(setting, "MaterialInfos");
        if (infos == nullptr) {
            continue;
        }
        auto infoItems =
            reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(infos)
                ->ToVector();
        for (void* info : infoItems) {
            UnityColor candidate(0.0F, 0.0F, 0.0F, 0.0F);
            if (info != nullptr &&
                ReadMaterialInfoColor(info, PenlightColorId(), &candidate) &&
                ColorLuma(candidate) > 0.01F) {
                return candidate;
            }
        }
    }
    return color;
}

bool CopyOfficialInfoOntoHands(void* controller) noexcept {
    if (controller == nullptr) {
        return false;
    }
    void* runtime = ReadNamedRef(controller, "_runtimeMobPenlightSettings");
    if (runtime == nullptr) {
        return false;
    }
    auto settings =
        reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(runtime)
            ->ToVector();
    void* source = nullptr;
    UnityColor color(0.0F, 0.0F, 0.0F, 0.0F);
    float emit = 0.0F;
    UnityVector4 uv(0.0F, 0.0F, 0.0F, 0.0F);
    bool haveUv = false;
    for (void* setting : settings) {
        if (setting == nullptr || setting == g_state.registeredSetting) {
            continue;
        }
        void* infos = ReadNamedRef(setting, "MaterialInfos");
        if (infos == nullptr) {
            continue;
        }
        auto infoItems =
            reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(infos)
                ->ToVector();
        for (void* info : infoItems) {
            UnityColor candidate(0.0F, 0.0F, 0.0F, 0.0F);
            if (info == nullptr ||
                !ReadMaterialInfoColor(
                    info, PenlightColorId(), &candidate) ||
                ColorLuma(candidate) <= 0.01F) {
                continue;
            }
            source = info;
            color = candidate;
            static_cast<void>(ReadMaterialInfoFloat(
                info, EmissionScaleId(), &emit));
            haveUv = ReadMaterialInfoVector(
                info, PenlightProjectionUvId(), &uv);
            break;
        }
        if (source != nullptr) {
            break;
        }
    }
    if (source == nullptr) {
        return false;
    }
    bool wrote = false;
    for (auto& hand : g_state.hands) {
        if (!EnsureHandMaterialInfo(hand) || hand.materialInfo == nullptr) {
            continue;
        }
        wrote = WriteHandMaterialInfoColors(hand.materialInfo, color) || wrote;
        if (emit > 0.01F) {
            static_cast<void>(WriteMaterialInfoFloat(
                hand.materialInfo, EmissionScaleId(), emit));
        }
        if (haveUv) {
            static_cast<void>(WriteMaterialInfoVector(
                hand.materialInfo, PenlightProjectionUvId(), uv));
        }
        ApplyHandMaterialInfo(hand.materialInfo);
    }
    return wrote;
}

bool ReadColorTable(
    UnityResolve::Class* klass, void* instance, std::vector<UnityColor>* colors,
    std::ostringstream* line) noexcept {
    auto* getter = FindNamedInstance(klass, "get_ColorTable", 0U);
    if (!HasExactSignature(getter, false, "UnityEngine.Vector4[]", {})) {
        return false;
    }
    void* table = nullptr;
    if (!RuntimeInvoke(MethodReference(getter), instance, nullptr, &table)) {
        return false;
    }
    if (table == nullptr) {
        return false;
    }
    auto items =
        reinterpret_cast<UnityResolve::UnityType::Array<UnityVector4>*>(table)
            ->ToVector();
    if (items.empty()) {
        return false;
    }
    if (colors != nullptr) {
        colors->clear();
        colors->reserve(items.size());
    }
    if (line != nullptr) {
        *line << " n=" << items.size();
    }
    const std::size_t logged =
        (std::min)(items.size(), static_cast<std::size_t>(8));
    for (std::size_t i = 0; i < items.size(); ++i) {
        UnityColor color(items[i].x, items[i].y, items[i].z, items[i].w);
        if (colors != nullptr) {
            colors->push_back(color);
        }
        if (line != nullptr && i < logged) {
            *line << " c" << i << "=" << items[i].x << "," << items[i].y << ","
                  << items[i].z << "," << items[i].w;
        }
    }
    return true;
}

void ComputePalette() noexcept {
    g_state.uniqueColors.clear();
    g_state.paletteReady = false;
    for (std::size_t i = 0; i < g_state.colorTable.size(); ++i) {
        const UnityColor& color = g_state.colorTable[i];
        UniqueColor* match = nullptr;
        for (auto& unique : g_state.uniqueColors) {
            if (SameColor(unique.color, color)) {
                match = &unique;
                break;
            }
        }
        if (match == nullptr) {
            UniqueColor created{};
            created.color = color;
            created.firstSlot = static_cast<int>(i);
            created.count = 1;
            created.slots = std::to_string(i);
            g_state.uniqueColors.push_back(created);
            continue;
        }
        match->count += 1;
        match->slots += ",";
        match->slots += std::to_string(i);
    }
}

int FindUniqueIndex(const UnityColor& color) noexcept {
    for (std::size_t i = 0; i < g_state.uniqueColors.size(); ++i) {
        if (SameColor(g_state.uniqueColors[i].color, color)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void LogPalettePick(const char* rule, int uniqueIndex) noexcept {
    auto line = ClassicLine();
    const char* token = "rule=?";
    if (rule != nullptr && std::strcmp(rule, "keep") == 0) {
        token = "rule=keep";
    } else if (rule != nullptr && std::strcmp(rule, "random") == 0) {
        token = "rule=random";
    } else if (rule != nullptr && std::strcmp(rule, "cycle") == 0) {
        token = "rule=cycle";
    } else if (rule != nullptr && std::strcmp(rule, "only") == 0) {
        token = "rule=only";
    } else if (rule != nullptr && std::strcmp(rule, "none") == 0) {
        token = "rule=none";
    }
    line << "[VR][stereo] HAND_GLOW pick n=" << g_state.colorTable.size()
         << " unique=" << g_state.uniqueColors.size()
         << " " << token
         << " both=" << g_state.stickySlot;
    if (uniqueIndex >= 0 &&
        uniqueIndex < static_cast<int>(g_state.uniqueColors.size())) {
        const auto& unique =
            g_state.uniqueColors[static_cast<std::size_t>(uniqueIndex)];
        line << " rgba=" << unique.color.r << "," << unique.color.g << ","
             << unique.color.b << " slots=" << unique.slots;
    }
    const std::size_t logged =
        (std::min)(g_state.uniqueColors.size(), static_cast<std::size_t>(4));
    for (std::size_t i = 0; i < logged; ++i) {
        const auto& unique = g_state.uniqueColors[i];
        line << " u" << i << "=" << unique.color.r << "," << unique.color.g
             << "," << unique.color.b << " slots=" << unique.slots
             << " n=" << unique.count;
    }
    LogHandGlow(line.str());
}

void ChooseSticky(bool log, const char* forcedRule) noexcept {
    if (g_state.uniqueColors.empty()) {
        g_state.stickySlot = -1;
        g_state.paletteReady = false;
        if (log) {
            LogHandGlow("[VR][stereo] HAND_GLOW pick unique=0 rule=none");
        }
        return;
    }
    const int previousSlot = g_state.stickySlot;
    int uniqueIndex = g_state.stickySlot >= 0
        ? FindUniqueIndex(g_state.stickyColor)
        : -1;
    const char* rule = "keep";
    if (uniqueIndex < 0) {
        if (g_state.uniqueColors.size() == 1U) {
            uniqueIndex = 0;
            rule = "only";
        } else {
            uniqueIndex = static_cast<int>(
                GetTickCount64() % g_state.uniqueColors.size());
            rule = "random";
        }
    }
    if (forcedRule != nullptr) {
        rule = forcedRule;
    }
    const auto& unique =
        g_state.uniqueColors[static_cast<std::size_t>(uniqueIndex)];
    g_state.stickySlot = unique.firstSlot;
    g_state.stickyColor = unique.color;
    g_state.paletteReady = true;
    if (log || g_state.stickySlot != previousSlot) {
        LogPalettePick(rule, uniqueIndex);
    }
}

int SlotForHand(std::size_t) noexcept {
    return g_state.stickySlot >= 0 ? g_state.stickySlot : 0;
}

void RequestCycleHandGlowColor() noexcept {
    if (!EnsureApi()) {
        return;
    }
    // Consume only the most recent official draw's palette; never rediscover
    // a global or cached object from the menu.
    if (!SceneReadyAllowsStereoRender()) {
        ClearAudienceColors("cycle-scene-not-ready");
        return;
    }
    if (g_state.uniqueColors.size() < 2U) {
        ChooseSticky(true, g_state.uniqueColors.empty() ? "none" : "only");
        return;
    }
    int current = FindUniqueIndex(g_state.stickyColor);
    if (current < 0) {
        current = 0;
    }
    const int next =
        (current + 1) % static_cast<int>(g_state.uniqueColors.size());
    const auto& unique = g_state.uniqueColors[static_cast<std::size_t>(next)];
    g_state.stickySlot = unique.firstSlot;
    g_state.stickyColor = unique.color;
    g_state.paletteReady = true;
    LogPalettePick("cycle", next);
}

void* MaterialForColorIndex(int colorIndex) noexcept {
    static_cast<void>(colorIndex);
    return g_state.officialMaterial;
}

// Cache metadata, never scene objects. Field offsets come from the actual
// runtime class table; steady draws need only class/offset reads.
void* ReadCrowdColorRef(void* object, const char* name) noexcept {
    if (object == nullptr) {
        return nullptr;
    }
    struct FieldSlot { void* klass; const char* name; std::int32_t offset; };
    static std::array<FieldSlot, 12> slots{};
    void* klass = UnityResolve::Invoke<void*>("il2cpp_object_get_class", object);
    if (klass == nullptr) {
        return nullptr;
    }
    for (auto& slot : slots) {
        if (slot.klass == klass && std::strcmp(slot.name, name) == 0) {
            return ReadRefAtOffsetSeh(object, slot.offset);
        }
    }
    void* field = UnityResolve::Invoke<void*>(
        "il2cpp_class_get_field_from_name", klass, name);
    const auto offset = field != nullptr
        ? UnityResolve::Invoke<std::int32_t>("il2cpp_field_get_offset", field) : -1;
    for (auto& slot : slots) {
        if (slot.klass == nullptr) {
            slot = {klass, name, offset};
            break;
        }
    }
    return ReadRefAtOffsetSeh(object, offset);
}

void ClearAudienceColors(const char* reason) noexcept {
    if (g_state.colorTableInstance != nullptr || !g_state.colorTable.empty()) {
        LogHandGlow(std::string("[VR][stereo] HAND_GLOW color-source clear reason=") + reason);
    }
    g_state.colorTable.clear();
    g_state.uniqueColors.clear();
    g_state.colorTableInstance = nullptr;
    g_state.colorTableClass = nullptr;
    g_state.colorSourceManagedOnly = false;
    g_state.lastColorReadMs = 0;
    g_state.stickySlot = -1;
    g_state.stickyColor = UnityColor(0.0F, 0.0F, 0.0F, 0.0F);
    g_state.paletteReady = false;
}

// Only resolve the current official draw's object. The default implementation
// is managed-only; reading Unity's m_CachedPtr on it would be invalid.
UnityResolve::Class* CurrentAudienceClass(void* audience) noexcept {
    if (audience == nullptr) {
        return nullptr;
    }
    void* actualClass = UnityResolve::Invoke<void*>(
        "il2cpp_object_get_class", audience);
    if (g_state.colorTableClass != nullptr &&
        g_state.colorTableClass->address == actualClass) {
        return g_state.colorSourceManagedOnly || IsUnityManagedObjectAlive(audience)
            ? g_state.colorTableClass : nullptr;
    }
    const char* names[] = {
        "AudiencePenlightTimeline", "AudiencePenlightComponent",
        "DefaultAudiencePenlight"};
    for (const char* name : names) {
        auto* klass = FindCampusClass("Campus.AudiencePenlight", name);
        if (klass != nullptr && klass->address == actualClass) {
            if (std::strcmp(name, "DefaultAudiencePenlight") != 0 &&
                !IsUnityManagedObjectAlive(audience)) {
                return nullptr;
            }
            return klass;
        }
    }
    return nullptr;
}

bool RefreshCurrentCrowdColors(void* crowdSystem) noexcept {
    void* volume = ReadCrowdColorRef(crowdSystem, "_audiencePenlightVolume");
    void* audience = volume != nullptr && IsUnityManagedObjectAlive(volume)
        ? ReadCrowdColorRef(volume, "_audiencePenlight") : nullptr;
    if (audience == nullptr) {
        audience = ReadCrowdColorRef(crowdSystem, "_defaultAudiencePenlight");
    }
    auto* klass = CurrentAudienceClass(audience);
    const bool changed = audience != g_state.colorTableInstance ||
        klass != g_state.colorTableClass;
    const ULONGLONG now = GetTickCount64();
    // The official source identity/liveness is checked every draw. Only the
    // managed getter and palette work are throttled, independently of Tick.
    if (klass != nullptr && !changed && !g_state.colorTable.empty() &&
        now - g_state.lastColorReadMs < 16U) {
        return true;
    }
    std::vector<UnityColor> colors;
    if (klass == nullptr || !ReadColorTable(klass, audience, &colors, nullptr)) {
        ClearAudienceColors("current-crowd-unreadable");
        return false;
    }
    g_state.lastColorReadMs = now;
    const bool identical = colors.size() == g_state.colorTable.size() &&
        std::equal(colors.begin(), colors.end(), g_state.colorTable.begin(),
            [](const UnityColor& a, const UnityColor& b) {
                return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
            });
    if (!changed && identical) {
        return true;
    }
    g_state.colorTable = std::move(colors);
    g_state.colorTableInstance = audience;
    g_state.colorTableClass = klass;
    g_state.colorSourceManagedOnly =
        klass == FindCampusClass("Campus.AudiencePenlight", "DefaultAudiencePenlight");
    ComputePalette();
    ChooseSticky(changed, nullptr);
    if (changed) {
        auto line = ClassicLine();
        line << "[VR][stereo] HAND_GLOW color-source bind current-crowd="
             << crowdSystem << " audience=" << audience
             << " type=" << ManagedClassName(audience);
        auto* getter = FindNamedInstance(klass, "get_ColorTable", 0U);
        line << " getter=" << getter->function
             << " methodInfo=" << getter->address;
        LogHandGlow(line.str());
    }
    return true;
}

UnityColor ColorForIndex(int colorIndex) noexcept {
    if (g_state.colorTable.empty()) {
        return UnityColor(0.0F, 0.0F, 0.0F, 0.0F);
    }
    const int count = static_cast<int>(g_state.colorTable.size());
    int index = colorIndex % count;
    if (index < 0) {
        index += count;
    }
    return g_state.colorTable[static_cast<std::size_t>(index)];
}

void ApplyOfficialScale(void* transform) noexcept {
    if (transform == nullptr) {
        return;
    }
    static_cast<void>(SetLocalScale(
        transform,
        g_state.sourceLossyScale.x * kHandScaleMultiplier,
        g_state.sourceLossyScale.y * kHandScaleMultiplier,
        g_state.sourceLossyScale.z * kHandScaleMultiplier));
}

void ApplyOfficialColor(HandObject* hand, int colorIndex, bool log) noexcept {
    if (hand == nullptr) {
        return;
    }
    void* renderer = hand->renderer;
    void* material = hand->material;
    UnityColor color = ColorForIndex(colorIndex);
    if (ColorLuma(color) <= 0.01F) {
        static_cast<void>(ReadRendererMpbColor(renderer, &color));
    }
    const int colorId = PenlightColorId();
    const UnityColor scaled = ScaleByAudienceIntensity(color);
    const bool haveInfo = EnsureHandMaterialInfo(*hand);
    bool infoWrote = false;
    bool matWrote = false;
    bool mpbWrote = false;
    if (ColorLuma(color) > 0.01F) {
        if (haveInfo) {
            infoWrote = WriteHandMaterialInfoColors(hand->materialInfo, color);
        }
        const bool matOfficial = SetMaterialColor(material, colorId, color);
        const bool matBase =
            SetMaterialColor(material, BaseColorId(), scaled);
        const bool matLegacy =
            SetMaterialColor(material, LegacyColorId(), scaled);
        matWrote = matOfficial && matBase && matLegacy;
        const UnityVector4 officialVector(color.r, color.g, color.b, color.a);
        const UnityVector4 scaledVector(scaled.r, scaled.g, scaled.b, scaled.a);
        static_cast<void>(SetMaterialVector(material, colorId, officialVector));
        static_cast<void>(
            SetMaterialVector(material, BaseColorId(), scaledVector));
        static_cast<void>(
            SetMaterialVector(material, LegacyColorId(), scaledVector));
        mpbWrote =
            OverlayOfficialColor(renderer, hand->emissionMaterialIndex, color);
    }
    if (log) {
        UnityColor infoColor(0.0F, 0.0F, 0.0F, 0.0F);
        UnityColor matBase(0.0F, 0.0F, 0.0F, 0.0F);
        UnityColor mpbBase(0.0F, 0.0F, 0.0F, 0.0F);
        static_cast<void>(ReadMaterialInfoColor(
            hand->materialInfo, BaseColorId(), &infoColor));
        const bool gotMaterial =
            GetMaterialColor(material, BaseColorId(), &matBase);
        bool gotMpb = false;
        if (renderer != nullptr && EnsurePropertyBlock() &&
            g_state.api.getPropertyBlockIndexed.Ready()) {
            using Fn = void (*)(void*, void*, int, void*);
            gotMpb = InvokeManagedVoid<Fn>(
                         g_state.api.getPropertyBlockIndexed,
                         renderer,
                         g_state.propertyBlock,
                         hand->emissionMaterialIndex) &&
                GetMpbColor(g_state.propertyBlock, BaseColorId(), &mpbBase);
        }
        auto line = ClassicLine();
        line << "[VR][stereo] HAND_GLOW apply color src="
             << (g_state.registered
                     ? "official"
                     : (g_state.colorTable.empty() ? "mpb" : "table"))
             << " idx=" << colorIndex
             << " both=" << g_state.stickySlot
             << " id=" << colorId
             << " registered=" << (g_state.registered ? 1 : 0)
             << " shader=" << ManagedObjectName(GetShader(material))
             << " set=" << (infoWrote ? 1 : 0) << (matWrote ? 1 : 0)
             << (mpbWrote ? 1 : 0)
             << " got=" << (gotMaterial ? 1 : 0) << (gotMpb ? 1 : 0)
             << " rgba=" << color.r << "," << color.g << "," << color.b << ","
             << color.a
             << " audienceIntensity=" << g_state.audienceIntensity
             << " scaled=" << scaled.r << "," << scaled.g << "," << scaled.b
             << " infoBase=" << infoColor.r << "," << infoColor.g << ","
             << infoColor.b
             << " matBase=" << matBase.r << "," << matBase.g << ","
             << matBase.b
             << " mpbBase=" << mpbBase.r << "," << mpbBase.g << ","
             << mpbBase.b
             << " mesh=crowd";
        LogHandGlow(line.str());
    }
}

void ClearOfficialAssets() noexcept {
    UnregisterHandsFromOfficialController();
    RetireHandle(g_state.officialMeshHandle);
    g_state.officialMeshHandle = nullptr;
    RetireHandle(g_state.officialMaterialHandle);
    g_state.officialMaterialHandle = nullptr;
    g_state.officialMaterial = nullptr;
    RetireHandle(g_state.officialBaseMaterialHandle);
    g_state.officialBaseMaterialHandle = nullptr;
    g_state.officialBaseMaterial = nullptr;
    g_state.officialMesh = nullptr;
    g_state.sourceRenderer = nullptr;
    g_state.sourceLayer = 0;
    g_state.sourceFromMob = false;
    g_state.listed = false;
    g_state.colorTable.clear();
    g_state.uniqueColors.clear();
    g_state.colorTableClass = nullptr;
    g_state.colorTableInstance = nullptr;
    g_state.stickyColor = UnityColor(0.0F, 0.0F, 0.0F, 0.0F);
    g_state.stickySlot = -1;
    g_state.paletteReady = false;
    g_state.penlightController = nullptr;
    g_state.registeredSetting = nullptr;
    g_state.registered = false;
    g_state.mobSources.clear();
    g_state.useColorProjection = false;
    g_state.sourceLocalScale = {1.0F, 1.0F, 1.0F};
    g_state.sourceLossyScale = {1.0F, 1.0F, 1.0F};
    g_state.meshSize = {0.0F, 0.0F, 0.0F};
    g_state.audienceIntensity = 0.0F;
}

bool HasDrawableAssets() noexcept {
    return g_state.sourceFromMob && g_state.officialMesh != nullptr &&
        IsUnityManagedObjectAlive(g_state.officialMesh) &&
        g_state.officialMaterial != nullptr &&
        IsUnityManagedObjectAlive(g_state.officialMaterial);
}

void* ApplyDrawableAssets(
    void* gameObject, void* baseMaterial, void* emissionMaterial) noexcept {
    if (gameObject == nullptr || !HasDrawableAssets()) {
        return nullptr;
    }
    void* filter = GetComponent(gameObject, g_state.api.meshFilterClass);
    void* renderer = GetComponent(gameObject, g_state.api.meshRendererClass);
    if (renderer == nullptr) {
        renderer = GetComponent(gameObject, g_state.api.rendererClass);
    }
    if (filter != nullptr && g_state.api.setSharedMesh.Ready()) {
        using Fn = void (*)(void*, void*, void*);
        static_cast<void>(InvokeManagedVoid<Fn>(
            g_state.api.setSharedMesh, filter, g_state.officialMesh));
    }
    void* assignedBase = baseMaterial != nullptr
        ? baseMaterial
        : g_state.officialBaseMaterial;
    void* assignedEmission = emissionMaterial != nullptr
        ? emissionMaterial
        : g_state.officialMaterial;
    if (!AssignHandMaterials(renderer, assignedBase, assignedEmission)) {
        return nullptr;
    }
    static_cast<void>(SetLayer(gameObject, g_state.sourceLayer));
    return renderer;
}

bool CrowdAuditAlreadySeen(void* crowdSystem, void* penlightSystem) noexcept {
    const auto pair = std::make_pair(crowdSystem, penlightSystem);
    if (std::find(
            g_state.crowdAuditPairs.begin(),
            g_state.crowdAuditPairs.end(), pair) !=
        g_state.crowdAuditPairs.end()) {
        return true;
    }
    if (g_state.crowdAuditPairs.size() >= 32U) {
        g_state.crowdAuditPairs.clear();
    }
    g_state.crowdAuditPairs.push_back(pair);
    return false;
}

void AuditCrowdRenderSystem(void* crowdSystem, int eventType) noexcept {
    // CrowdSystem.EventType.GBuffer == 1. Audit one complete draw path per
    // live system, after the game has populated counters and parameters.
    if (crowdSystem == nullptr || eventType != 1) {
        return;
    }
    void* penlightSystem =
        ReadNamedRef(crowdSystem, "_penlightMeshRenderSystem");
    if (CrowdAuditAlreadySeen(crowdSystem, penlightSystem)) {
        return;
    }

    static_cast<void>(EnsureApi());
    void* crowdVolume = ReadNamedRef(crowdSystem, "_crowdVolume");
    void* crowdData = ReadNamedRef(crowdVolume, "crowdData");
    void* people = ReadNamedRef(crowdData, "personInfos");
    void* crowdSystemData =
        ReadNamedRef(crowdSystem, "_crowdSystemData");
    void* personBytes =
        ReadNamedRef(crowdSystemData, "_personInfoBytesBuffer");
    void* crowdMeshSystem =
        ReadNamedRef(crowdSystem, "_crowdMeshRenderSystem");
    void* handMatrices = ReadNamedRef(crowdMeshSystem, "_handMatrices");

    void* meshData = ReadNamedRef(penlightSystem, "_meshData");
    void* sourceMesh = ReadNamedRef(meshData, "mesh");
    void* renderMesh = ReadNamedRef(penlightSystem, "_mesh");
    void* subMeshes = ReadNamedRef(meshData, "subMeshes");
    const int subMeshCount =
        ReadNamedInt(penlightSystem, "_subMeshCount");
    void* indirectArgs =
        ReadNamedRef(penlightSystem, "_indirectArgsBuffer");
    void* instanceBufferArray =
        ReadNamedRef(penlightSystem, "_penlightInfoBytesBuffers");

    void* audienceVolume =
        ReadNamedRef(crowdSystem, "_audiencePenlightVolume");
    void* audience = ReadNamedRef(audienceVolume, "_audiencePenlight");
    if (audience == nullptr) {
        audience = ReadNamedRef(crowdSystem, "_defaultAudiencePenlight");
    }
    void* colorTable =
        ReadNamedRef(audience, "<ColorTable>k__BackingField");

    UnityVector3 sourceSize{};
    UnityVector3 renderSize{};
    if (sourceMesh != nullptr && IsUnityManagedObjectAlive(sourceMesh)) {
        static_cast<void>(GetMeshSize(sourceMesh, &sourceSize));
    }
    if (renderMesh != nullptr && IsUnityManagedObjectAlive(renderMesh)) {
        static_cast<void>(GetMeshSize(renderMesh, &renderSize));
    }

    auto header = ClassicLine();
    header << "[VR][stereo] HAND_GLOW crowd audit"
           << " ownership=read-only"
           << " event=GBuffer"
           << " system=" << crowdSystem
           << " penlightSystem=" << penlightSystem
           << " volume=" << ManagedObjectName(crowdVolume)
           << " people=" << ManagedArrayLength(people)
           << " sourceMesh=" << ManagedObjectName(sourceMesh)
           << " sourceSize=" << sourceSize.x << "," << sourceSize.y << ","
           << sourceSize.z
           << " renderMesh=" << ManagedObjectName(renderMesh)
           << " renderSize=" << renderSize.x << "," << renderSize.y << ","
           << renderSize.z
           << " subMeshes=" << subMeshCount
           << " handMatrices=" << ManagedArrayLength(handMatrices)
           << " audience=" << ManagedClassName(audience)
           << " colors=" << ManagedArrayLength(colorTable)
           << " passes=predepth:2,gbuffer:1";
    AppendBufferDescriptor(header, "personBytes", personBytes);
    AppendBufferDescriptor(header, "indirectArgs", indirectArgs);
    LogHandGlow(header.str());

    std::vector<void*> subMeshItems{};
    if (subMeshes != nullptr) {
        subMeshItems = reinterpret_cast<
            UnityResolve::UnityType::Array<void*>*>(subMeshes)->ToVector();
    }
    std::vector<void*> instanceBuffers{};
    if (instanceBufferArray != nullptr) {
        instanceBuffers = reinterpret_cast<
            UnityResolve::UnityType::Array<void*>*>(instanceBufferArray)
                              ->ToVector();
    }
    const std::size_t count =
        std::max(subMeshItems.size(), instanceBuffers.size());
    for (std::size_t index = 0; index < count; ++index) {
        void* subMesh = index < subMeshItems.size()
            ? subMeshItems[index]
            : nullptr;
        void* material = ReadNamedRef(subMesh, "material");
        void* shader = material != nullptr &&
                IsUnityManagedObjectAlive(material)
            ? GetShader(material)
            : nullptr;
        auto line = ClassicLine();
        line << "[VR][stereo] HAND_GLOW crowd submesh=" << index
             << " penlightType=" << (index + 1U)
             << " indexCount=" << ReadNamedInt(subMesh, "indexCount")
             << " indexStart=" << ReadNamedInt(subMesh, "indexStart")
             << " vertexStart=" << ReadNamedInt(subMesh, "vertexStart")
             << " vertexCount=" << ReadNamedInt(subMesh, "vertexCount")
             << " material=" << ManagedObjectName(material)
             << " shader=" << ManagedObjectName(shader)
             << " argsOffset=" << (index * 20U);
        AppendBufferDescriptor(
            line, "instances",
            index < instanceBuffers.size() ? instanceBuffers[index] : nullptr);
        LogHandGlow(line.str());
    }
}

bool CrowdGraphicsBufferValid(void* buffer) noexcept {
    if (buffer == nullptr ||
        !g_state.crowdDrawApi.graphicsBufferIsValid.Ready()) {
        return false;
    }
    using Fn = bool (*)(void*, void*);
    bool valid = false;
    return InvokeManagedResult<bool, Fn>(
               g_state.crowdDrawApi.graphicsBufferIsValid, &valid, buffer) &&
        valid;
}

void ReleaseCrowdGraphicsBuffer(void* buffer) noexcept {
    if (!CrowdGraphicsBufferValid(buffer) ||
        !g_state.crowdDrawApi.graphicsBufferRelease.Ready()) {
        return;
    }
    using Fn = void (*)(void*, void*);
    static_cast<void>(InvokeManagedVoid<Fn>(
        g_state.crowdDrawApi.graphicsBufferRelease, buffer));
}

void ResetCrowdDrawResources() noexcept {
    auto& resources = g_state.crowdDrawResources;
    ReleaseCrowdGraphicsBuffer(resources.personBuffer);
    ReleaseCrowdGraphicsBuffer(resources.instanceBuffer);
    ReleaseCrowdGraphicsBuffer(resources.argsBuffer);
    RetireHandle(resources.personBufferHandle);
    RetireHandle(resources.instanceBufferHandle);
    RetireHandle(resources.argsBufferHandle);
    RetireHandle(resources.propertyBlockHandle);
    RetireHandle(resources.personWordsHandle);
    RetireHandle(resources.instanceWordsHandle);
    RetireHandle(resources.argsWordsHandle);
    RetireHandle(resources.handMatricesHandle);
    RetireHandle(resources.penlightColorsHandle);
    for (auto& material : resources.materials) {
        RetireHandle(material.cloneHandle);
    }
    resources = {};
}

void* CreateCrowdGraphicsBuffer(int target, int count, int stride) noexcept {
    auto& api = g_state.crowdDrawApi;
    if (api.graphicsBufferClass == nullptr ||
        api.graphicsBufferClass->address == nullptr ||
        !api.graphicsBufferCtor.HasInfo()) {
        return nullptr;
    }
    void* buffer = NewIl2CppObject(api.graphicsBufferClass->address);
    if (buffer == nullptr) {
        return nullptr;
    }
    void* arguments[] = {&target, &count, &stride};
    void* ignored = nullptr;
    if (!RuntimeInvoke(
            api.graphicsBufferCtor, buffer, arguments, &ignored) ||
        !CrowdGraphicsBufferValid(buffer)) {
        return nullptr;
    }
    return buffer;
}

bool EnsureCrowdDrawResources() noexcept {
    auto& resources = g_state.crowdDrawResources;
    if (resources.ready && CrowdGraphicsBufferValid(resources.personBuffer) &&
        CrowdGraphicsBufferValid(resources.instanceBuffer) &&
        CrowdGraphicsBufferValid(resources.argsBuffer) &&
        resources.propertyBlock != nullptr &&
        ReadNamedRef(resources.propertyBlock, "m_Ptr") != nullptr) {
        return true;
    }
    if (resources.ready || resources.personBuffer != nullptr ||
        resources.instanceBuffer != nullptr || resources.argsBuffer != nullptr) {
        LogHandGlow(
            "[VR][stereo] HAND_GLOW crowd recreate reason=dead-private-resource");
        ResetCrowdDrawResources();
    }
    if (!EnsureCrowdDrawApi()) {
        return false;
    }

    auto& api = g_state.crowdDrawApi;
    resources.personBuffer = CreateCrowdGraphicsBuffer(0x20, 8, 4);
    resources.instanceBuffer = CreateCrowdGraphicsBuffer(0x48, 2, 4);
    // The current Live mesh has two submeshes. Keep room for eight so a later
    // Live can still use its discovered mesh without sharing the game's args.
    resources.argsBuffer = CreateCrowdGraphicsBuffer(0x100, 40, 4);
    resources.propertyBlock =
        NewIl2CppObject(api.propertyBlockClass->address);
    if (resources.propertyBlock != nullptr) {
        void* ignored = nullptr;
        if (!RuntimeInvoke(
                api.propertyBlockCtor,
                resources.propertyBlock,
                nullptr,
                &ignored)) {
            resources.propertyBlock = nullptr;
        }
    }
    resources.personWords = NewManagedArray(api.uint32Class->address, 8U);
    resources.instanceWords = NewManagedArray(api.uint32Class->address, 2U);
    resources.argsWords = NewManagedArray(api.uint32Class->address, 40U);
    resources.handMatrices = NewManagedArray(api.matrixClass->address, 16U);
    resources.penlightColors = NewManagedArray(api.vector4Class->address, 8U);

    resources.personBufferHandle = CreateGcHandle(resources.personBuffer);
    resources.instanceBufferHandle = CreateGcHandle(resources.instanceBuffer);
    resources.argsBufferHandle = CreateGcHandle(resources.argsBuffer);
    resources.propertyBlockHandle = CreateGcHandle(resources.propertyBlock);
    resources.personWordsHandle = CreateGcHandle(resources.personWords);
    resources.instanceWordsHandle = CreateGcHandle(resources.instanceWords);
    resources.argsWordsHandle = CreateGcHandle(resources.argsWords);
    resources.handMatricesHandle = CreateGcHandle(resources.handMatrices);
    resources.penlightColorsHandle = CreateGcHandle(resources.penlightColors);

    resources.ready = resources.personBufferHandle != nullptr &&
        resources.instanceBufferHandle != nullptr &&
        resources.argsBufferHandle != nullptr &&
        resources.propertyBlockHandle != nullptr &&
        resources.personWordsHandle != nullptr &&
        resources.instanceWordsHandle != nullptr &&
        resources.argsWordsHandle != nullptr &&
        resources.handMatricesHandle != nullptr &&
        resources.penlightColorsHandle != nullptr;
    if (!resources.ready) {
        LogHandGlow(
            "[VR][stereo] HAND_GLOW crowd skip reason=private-resource-create");
        ResetCrowdDrawResources();
        return false;
    }
    auto line = ClassicLine();
    line << "[VR][stereo] HAND_GLOW crowd private ownership=mod";
    AppendBufferDescriptor(line, "personBytes", resources.personBuffer);
    AppendBufferDescriptor(line, "instances", resources.instanceBuffer);
    AppendBufferDescriptor(line, "indirectArgs", resources.argsBuffer);
    line << " people=2 sticks=2 matrixCount=16 colorCount=8";
    LogHandGlow(line.str());
    return true;
}

UnityMatrix4x4 IdentityCrowdMatrix() noexcept {
    UnityMatrix4x4 matrix{};
    auto* values = reinterpret_cast<float*>(&matrix);
    values[0] = 1.0F;
    values[5] = 1.0F;
    values[10] = 1.0F;
    values[15] = 1.0F;
    return matrix;
}

UnityMatrix4x4 CrowdPoseMatrix(const pose::Pose& pose) noexcept {
    const auto& orientation = pose.orientation;
    const float lengthSquared = orientation.x * orientation.x +
        orientation.y * orientation.y + orientation.z * orientation.z +
        orientation.w * orientation.w;
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-8F) {
        UnityMatrix4x4 matrix = IdentityCrowdMatrix();
        auto* values = reinterpret_cast<float*>(&matrix);
        values[12] = pose.position.x;
        values[13] = pose.position.y;
        values[14] = pose.position.z;
        return matrix;
    }
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    const float x = orientation.x * inverseLength;
    const float y = orientation.y * inverseLength;
    const float z = orientation.z * inverseLength;
    const float w = orientation.w * inverseLength;
    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;

    UnityMatrix4x4 matrix{};
    auto* values = reinterpret_cast<float*>(&matrix);
    // Unity Matrix4x4 memory order is m00,m10,m20,m30,m01,... . The Crowd
    // shader receives this through the public SetMatrixArray API.
    values[0] = 1.0F - 2.0F * (yy + zz);
    values[1] = 2.0F * (xy + wz);
    values[2] = 2.0F * (xz - wy);
    values[4] = 2.0F * (xy - wz);
    values[5] = 1.0F - 2.0F * (xx + zz);
    values[6] = 2.0F * (yz + wx);
    values[8] = 2.0F * (xz + wy);
    values[9] = 2.0F * (yz - wx);
    values[10] = 1.0F - 2.0F * (xx + yy);
    // The official shader applies this complete hand matrix before the packed
    // person root. Keep the person at zero and carry controller translation in
    // float here, avoiding Crowd's centimetre uint16 position quantization.
    values[12] = pose.position.x;
    values[13] = pose.position.y;
    values[14] = pose.position.z;
    values[15] = 1.0F;
    return matrix;
}

bool RefreshCrowdHandPosesFromMailbox() noexcept {
    if (!g_state.crowdPoseBridgeValid) {
        g_state.crowdHandPoseValid.fill(false);
        return false;
    }
    pose::HandPoseSample hands{};
    if (!pose::HandTrackingMailbox().Read(hands) || !hands.valid) {
        g_state.crowdHandPoseValid.fill(false);
        return false;
    }
    if (hands.revision == g_state.crowdAppliedHandRevision &&
        g_state.crowdAppliedBridgeRevision ==
            g_state.crowdPoseBridgeRevision) {
        return g_state.crowdHandPoseValid[0] ||
            g_state.crowdHandPoseValid[1];
    }

    g_state.crowdHandPoseValid.fill(false);
    for (std::size_t hand = 0; hand < g_state.crowdHandPoses.size(); ++hand) {
        if (!hands.hands[hand].valid ||
            !pose::IsFinite(hands.hands[hand].pose.position) ||
            !pose::IsFinite(hands.hands[hand].pose.orientation)) {
            continue;
        }
        pose::Pose worldPose{};
        if (!pose::TryComposeGamePose(
                g_state.crowdGameHeadsetPose,
                g_state.crowdOpenXrHeadCenter,
                hands.hands[hand].pose,
                g_state.crowdWorldScale,
                worldPose)) {
            continue;
        }
        g_state.crowdHandPoses[hand] = worldPose;
        g_state.crowdHandPoseValid[hand] = true;
    }
    g_state.crowdAppliedHandRevision = hands.revision;
    g_state.crowdAppliedBridgeRevision = g_state.crowdPoseBridgeRevision;
    return g_state.crowdHandPoseValid[0] || g_state.crowdHandPoseValid[1];
}

bool UploadCrowdBuffer(void* buffer, void* array) noexcept {
    if (!CrowdGraphicsBufferValid(buffer) || array == nullptr) {
        return false;
    }
    using Fn = void (*)(void*, void*, void*);
    return InvokeManagedVoid<Fn>(
        g_state.crowdDrawApi.graphicsBufferSetData, buffer, array);
}

bool SetCrowdPropertyBuffer(int id, void* buffer) noexcept {
    auto& resources = g_state.crowdDrawResources;
    if (resources.propertyBlock == nullptr || id == 0 || buffer == nullptr) {
        return false;
    }
    using Fn = void (*)(void*, int, void*, void*);
    return InvokeManagedVoid<Fn>(
        g_state.crowdDrawApi.propertyBlockSetBuffer,
        resources.propertyBlock,
        id,
        buffer);
}

bool SetCrowdPropertyVector(int id, const UnityVector4& vector) noexcept {
    auto& resources = g_state.crowdDrawResources;
    if (resources.propertyBlock == nullptr || id == 0 ||
        !g_state.crowdDrawApi.propertyBlockSetVector.HasInfo()) {
        return false;
    }
    UnityVector4 copy = vector;
    void* arguments[] = {&id, &copy};
    void* ignored = nullptr;
    return RuntimeInvoke(
        g_state.crowdDrawApi.propertyBlockSetVector,
        resources.propertyBlock,
        arguments,
        &ignored);
}

bool SetCrowdPropertyArray(
    const MethodRef& method, int id, void* array) noexcept {
    auto& resources = g_state.crowdDrawResources;
    if (resources.propertyBlock == nullptr || id == 0 || array == nullptr) {
        return false;
    }
    using Fn = void (*)(void*, int, void*, void*);
    return InvokeManagedVoid<Fn>(
        method, resources.propertyBlock, id, array);
}

bool ReadCrowdMeshDrawArgs(
    void* mesh, int submesh, std::uint32_t* indexCount,
    std::uint32_t* indexStart, std::uint32_t* baseVertex) noexcept {
    if (mesh == nullptr || indexCount == nullptr || indexStart == nullptr ||
        baseVertex == nullptr) {
        return false;
    }
    using Fn = std::uint32_t (*)(void*, int, void*);
    return InvokeManagedResult<std::uint32_t, Fn>(
               g_state.crowdDrawApi.meshGetIndexCount,
               indexCount,
               mesh,
               submesh) &&
        InvokeManagedResult<std::uint32_t, Fn>(
               g_state.crowdDrawApi.meshGetIndexStart,
               indexStart,
               mesh,
               submesh) &&
        InvokeManagedResult<std::uint32_t, Fn>(
               g_state.crowdDrawApi.meshGetBaseVertex,
               baseVertex,
               mesh,
               submesh);
}

void* GetPrivateCrowdMaterial(void* source) noexcept {
    if (source == nullptr || !IsUnityManagedObjectAlive(source)) {
        return nullptr;
    }
    auto& resources = g_state.crowdDrawResources;
    for (auto it = resources.materials.begin();
         it != resources.materials.end();) {
        if (it->source == nullptr || it->clone == nullptr ||
            !IsUnityManagedObjectAlive(it->source) ||
            !IsUnityManagedObjectAlive(it->clone)) {
            RetireHandle(it->cloneHandle);
            it = resources.materials.erase(it);
            continue;
        }
        if (it->source == source) {
            return it->clone;
        }
        ++it;
    }

    void* clone = CloneMaterial(source);
    auto& api = g_state.crowdDrawApi;
    if (clone == nullptr || !IsUnityManagedObjectAlive(clone) ||
        !EnableMaterialKeywordObject(clone, api.useColorTableKeyword)) {
        return nullptr;
    }
    Il2CppGCHandle cloneHandle = CreateGcHandle(clone);
    if (cloneHandle == nullptr) {
        return nullptr;
    }
    if (resources.materials.size() >= 32U) {
        for (auto& material : resources.materials) {
            RetireHandle(material.cloneHandle);
        }
        resources.materials.clear();
    }
    resources.materials.push_back({source, clone, cloneHandle});
    auto line = ClassicLine();
    line << "[VR][stereo] HAND_GLOW crowd material ownership=mod"
         << " source=" << source
         << " clone=" << clone
         << " keyword=" << ManagedIl2CppString(api.useColorTableKeyword)
         << " isolation=material-local";
    LogHandGlow(line.str());
    return clone;
}

bool CrowdDrawAlreadySeen(
    void* crowdSystem, void* renderMesh, int eventType) noexcept {
    auto& seenPairs = g_state.crowdDrawPairs[
        eventType == 0 ? 0U : 1U];
    const auto pair = std::make_pair(crowdSystem, renderMesh);
    if (std::find(
            seenPairs.begin(),
            seenPairs.end(),
            pair) != seenPairs.end()) {
        return true;
    }
    if (seenPairs.size() >= 32U) {
        seenPairs.clear();
    }
    seenPairs.push_back(pair);
    return false;
}

void LogCrowdDrawFailure(std::string_view reason) noexcept {
    if (g_state.crowdDrawFailureTick != 0U &&
        (g_state.ticks - g_state.crowdDrawFailureTick) < 120U) {
        return;
    }
    g_state.crowdDrawFailureTick = g_state.ticks;
    LogHandGlow(
        std::string("[VR][stereo] HAND_GLOW crowd skip reason=") +
        std::string(reason));
}

bool ReadCrowdPersonIntensityArray(
    void* personInfos, float* intensity, bool log) noexcept;

bool ReadCrowdSystemIntensity(
    void* crowdSystem, float* intensity) noexcept {
    if (crowdSystem == nullptr || intensity == nullptr) {
        return false;
    }
    void* systemData = ReadNamedRef(crowdSystem, "_crowdSystemData");
    void* crowdData = ReadNamedRef(systemData, "_crowdData");
    if (crowdData == nullptr) {
        return false;
    }
    const auto found = std::find_if(
        g_state.crowdIntensitySources.begin(),
        g_state.crowdIntensitySources.end(),
        [crowdData](const auto& entry) { return entry.first == crowdData; });
    if (found != g_state.crowdIntensitySources.end()) {
        *intensity = found->second;
        return true;
    }
    void* personInfos = ReadNamedRef(crowdData, "personInfos");
    float measured = 0.0F;
    if (!ReadCrowdPersonIntensityArray(personInfos, &measured, true)) {
        return false;
    }
    if (g_state.crowdIntensitySources.size() >= 32U) {
        g_state.crowdIntensitySources.clear();
    }
    g_state.crowdIntensitySources.emplace_back(crowdData, measured);
    g_state.audienceIntensity = measured;
    *intensity = measured;
    return true;
}

void DrawControllerSticksThroughCrowd(
    void* crowdSystem, void* commandBuffer, int eventType) noexcept {
    if (crowdSystem == nullptr || commandBuffer == nullptr ||
        (eventType != 0 && eventType != 1) ||
        !GakumasLocal::Config::vrHandGlowSticks ||
        !SceneReadyAllowsStereoRender()) {
        return;
    }
    if (!RefreshCrowdHandPosesFromMailbox()) {
        return;
    }
    const int validCount = static_cast<int>(
        g_state.crowdHandPoseValid[0]) +
        static_cast<int>(g_state.crowdHandPoseValid[1]);
    if (validCount == 0) {
        return;
    }
    if (!EnsureApi() || !EnsureCrowdDrawApi() ||
        !EnsureCrowdDrawResources()) {
        LogCrowdDrawFailure("api-or-private-resource");
        return;
    }
    if (!RefreshCurrentCrowdColors(crowdSystem)) {
        LogCrowdDrawFailure("color-table");
        return;
    }
    float intensity = 0.0F;
    if (!ReadCrowdSystemIntensity(crowdSystem, &intensity) ||
        intensity <= 0.0F) {
        LogCrowdDrawFailure("live-intensity");
        return;
    }

    void* penlightSystem =
        ReadNamedRef(crowdSystem, "_penlightMeshRenderSystem");
    void* meshData = ReadNamedRef(penlightSystem, "_meshData");
    void* renderMesh = ReadNamedRef(penlightSystem, "_mesh");
    void* subMeshes = ReadNamedRef(meshData, "subMeshes");
    if (penlightSystem == nullptr || renderMesh == nullptr ||
        !IsUnityManagedObjectAlive(renderMesh) || subMeshes == nullptr) {
        LogCrowdDrawFailure("live-mesh");
        return;
    }
    const auto subMeshItems = reinterpret_cast<
        UnityResolve::UnityType::Array<void*>*>(subMeshes)->ToVector();
    constexpr std::size_t kMaximumPrivateSubmeshes = 8U;
    if (subMeshItems.empty() ||
        subMeshItems.size() > kMaximumPrivateSubmeshes) {
        LogCrowdDrawFailure("live-submesh");
        return;
    }

    auto& resources = g_state.crowdDrawResources;
    auto* personWords = reinterpret_cast<
        UnityResolve::UnityType::Array<std::uint32_t>*>(resources.personWords);
    auto* instanceWords = reinterpret_cast<
        UnityResolve::UnityType::Array<std::uint32_t>*>(resources.instanceWords);
    auto* argsWords = reinterpret_cast<
        UnityResolve::UnityType::Array<std::uint32_t>*>(resources.argsWords);
    auto* matrices = reinterpret_cast<
        UnityResolve::UnityType::Array<UnityMatrix4x4>*>(
        resources.handMatrices);
    auto* colors = reinterpret_cast<
        UnityResolve::UnityType::Array<UnityVector4>*>(resources.penlightColors);

    for (unsigned int index = 0; index < 8U; ++index) {
        personWords->At(index) = 0U;
    }
    for (unsigned int index = 0; index < 2U; ++index) {
        instanceWords->At(index) = 0U;
    }
    for (unsigned int index = 0; index < 40U; ++index) {
        argsWords->At(index) = 0U;
    }
    const UnityMatrix4x4 identity = IdentityCrowdMatrix();
    for (unsigned int index = 0; index < 16U; ++index) {
        matrices->At(index) = identity;
    }
    for (unsigned int index = 0; index < 8U; ++index) {
        const UnityColor& color = g_state.colorTable[
            index % g_state.colorTable.size()];
        colors->At(index) = UnityVector4(color.r, color.g, color.b, color.a);
    }

    intensity = std::clamp(intensity, 0.0F, 4.0F);
    const std::uint32_t intensityBits = static_cast<std::uint32_t>(
        std::clamp(intensity * 0.25F, 0.0F, 1.0F) * 1023.0F);
    const std::uint32_t colorSlot =
        static_cast<std::uint32_t>((std::max)(0, g_state.stickySlot)) & 7U;
    int compactIndex = 0;
    for (std::size_t hand = 0; hand < g_state.crowdHandPoses.size(); ++hand) {
        if (!g_state.crowdHandPoseValid[hand]) {
            continue;
        }
        const auto& pose = g_state.crowdHandPoses[hand];
        const unsigned int personOffset =
            static_cast<unsigned int>(compactIndex * 4);
        personWords->At(personOffset + 0U) = intensityBits << 5U;
        personWords->At(personOffset + 1U) = 0U;
        personWords->At(personOffset + 2U) = 1000U << 16U;
        // High/low uint16 decode to the identity XZ root basis
        // (0.99997, 0.0): worldX=localX, worldZ=localZ.
        personWords->At(personOffset + 3U) = 0xFFFF8000U;
        matrices->At(static_cast<unsigned int>(compactIndex)) =
            CrowdPoseMatrix(pose);
        // person[0..14], hand matrix[15..18], fan selector 0[19..21],
        // exact ColorTable slot[22..24], size class 1[25..27].
        instanceWords->At(static_cast<unsigned int>(compactIndex)) =
            static_cast<std::uint32_t>(compactIndex) |
            (static_cast<std::uint32_t>(compactIndex) << 15U) |
            (colorSlot << 22U) | (1U << 25U);
        ++compactIndex;
    }

    std::vector<void*> drawMaterials{};
    drawMaterials.reserve(subMeshItems.size());
    for (std::size_t submesh = 0; submesh < subMeshItems.size(); ++submesh) {
        void* sourceMaterial =
            ReadNamedRef(subMeshItems[submesh], "material");
        void* material = GetPrivateCrowdMaterial(sourceMaterial);
        if (material == nullptr) {
            LogCrowdDrawFailure("live-material");
            return;
        }
        std::uint32_t indexCount = 0U;
        std::uint32_t indexStart = 0U;
        std::uint32_t baseVertex = 0U;
        if (!ReadCrowdMeshDrawArgs(
                renderMesh,
                static_cast<int>(submesh),
                &indexCount,
                &indexStart,
                &baseVertex) ||
            indexCount == 0U) {
            LogCrowdDrawFailure("mesh-args");
            return;
        }
        const unsigned int argsOffset =
            static_cast<unsigned int>(submesh * 5U);
        argsWords->At(argsOffset + 0U) = indexCount;
        argsWords->At(argsOffset + 1U) =
            static_cast<std::uint32_t>(compactIndex);
        argsWords->At(argsOffset + 2U) = indexStart;
        argsWords->At(argsOffset + 3U) = baseVertex;
        argsWords->At(argsOffset + 4U) = 0U;
        drawMaterials.push_back(material);
    }

    auto& api = g_state.crowdDrawApi;
    const UnityVector4 boundingMin(0.0F, 0.0F, 0.0F, 0.0F);
    const bool uploaded =
        UploadCrowdBuffer(resources.personBuffer, resources.personWords) &&
        UploadCrowdBuffer(resources.instanceBuffer, resources.instanceWords) &&
        UploadCrowdBuffer(resources.argsBuffer, resources.argsWords);
    const bool bound =
        SetCrowdPropertyBuffer(
            api.personInfoBytesBufferId, resources.personBuffer) &&
        SetCrowdPropertyBuffer(
            api.penlightInfoBytesBufferId, resources.instanceBuffer) &&
        SetCrowdPropertyVector(api.boundingMinId, boundingMin) &&
        SetCrowdPropertyArray(
            api.propertyBlockSetMatrixArray,
            api.handMatricesId,
            resources.handMatrices) &&
        SetCrowdPropertyArray(
            api.propertyBlockSetVectorArray,
            api.penlightColorsId,
            resources.penlightColors);
    if (!uploaded || !bound) {
        LogCrowdDrawFailure(uploaded ? "property-bind" : "buffer-upload");
        return;
    }

    using DrawFn = void (*)(
        void*, void*, int, void*, int, void*, int, void*, void*);
    const int shaderPass = eventType == 0 ? 2 : 1;
    bool drew = true;
    for (std::size_t submesh = 0; submesh < drawMaterials.size(); ++submesh) {
        drew = InvokeManagedVoid<DrawFn>(
                   api.commandDrawMeshInstancedIndirect,
                   commandBuffer,
                   renderMesh,
                   static_cast<int>(submesh),
                   drawMaterials[submesh],
                   shaderPass,
                   resources.argsBuffer,
                   static_cast<int>(submesh * 20U),
                   resources.propertyBlock) &&
            drew;
    }
    if (!drew) {
        LogCrowdDrawFailure("draw-call");
        return;
    }
    // Only a successful GBuffer command proves that the visible-color route is
    // alive.  A PreDepth-only success must not hide the legacy fallback.
    if (eventType == 1) {
        g_state.crowdDrawActive = true;
        g_state.lastCrowdDrawTick = g_state.ticks;
    }
    const bool firstDraw =
        !CrowdDrawAlreadySeen(crowdSystem, renderMesh, eventType);
    const ULONGLONG probeNow = GetTickCount64();
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled &&
        eventType == 1 &&
        (firstDraw || probeNow - g_state.lastCrowdColorProbeMs >= 2000U)) {
        g_state.lastCrowdColorProbeMs = probeNow;
        void* volume = ReadNamedRef(crowdSystem, "_audiencePenlightVolume");
        void* audience = ReadNamedRef(volume, "_audiencePenlight");
        if (audience == nullptr) {
            audience = ReadNamedRef(crowdSystem, "_defaultAudiencePenlight");
        }
        auto probe = ClassicLine();
        probe << "[VR][stereo] HAND_GLOW color-probe system=" << crowdSystem
              << " audience=" << audience
              << " audienceType=" << ManagedClassName(audience)
              << " selected=" << g_state.colorTableInstance
              << " selectedType=" << ManagedClassName(g_state.colorTableInstance)
              << " sameSource=" << (audience == g_state.colorTableInstance)
              << " slot=" << colorSlot
              << " unique=" << g_state.uniqueColors.size()
              << " intensity=" << intensity;
        // Persist the resolved live entry alongside the values. This is a
        // diagnostic observation of the existing getter, not a new callable.
        auto* getter = FindNamedInstance(
            g_state.colorTableClass, "get_ColorTable", 0U);
        if (getter != nullptr) {
            probe << " getter=" << getter->name
                  << " function=" << getter->function
                  << " methodInfo=" << getter->address;
        }
        for (unsigned int i = 0; i < 8U; ++i) {
            const auto& value = colors->At(i);
            probe << " uploaded" << i << "=" << value.x << ","
                  << value.y << "," << value.z << "," << value.w;
        }
        LogHandGlow(probe.str());
    }
    if (firstDraw) {
        auto line = ClassicLine();
        line << "[VR][stereo] HAND_GLOW crowd draw ownership=mod"
             << " event=" << (eventType == 0 ? "PreDepth" : "GBuffer")
             << " system=" << crowdSystem
             << " mesh=" << ManagedObjectName(renderMesh)
             << " material=" << ManagedObjectName(drawMaterials.front())
             << " shader=" << ManagedObjectName(GetShader(drawMaterials.front()))
             << " submeshes=" << drawMaterials.size()
             << " pass=" << shaderPass
             << " instances=" << compactIndex
             << " colorSlot=" << colorSlot
             << " intensity=" << intensity
             << " poseRevision=" << g_state.crowdAppliedHandRevision
             << " position=hand-matrix-f32"
             << " sizeClass=1";
        LogHandGlow(line.str());
    }
}

void LogCrowdFamily() noexcept {
    auto* volumeClass = FindCampusClass("Campus.Crowd", "CrowdVolume");
    if (volumeClass == nullptr) {
        return;
    }
    const auto volumes = FindAllOfClass(volumeClass);
    for (void* volume : volumes) {
        if (volume == nullptr || !IsUnityManagedObjectAlive(volume)) {
            continue;
        }
        void* modelData = ReadNamedRef(volume, "modelData");
        if (modelData == nullptr) {
            continue;
        }
        void* meshData = ReadNamedRef(modelData, "penlightMeshData");
        if (meshData == nullptr) {
            continue;
        }
        void* mesh = ReadNamedRef(meshData, "mesh");
        void* subMeshes = ReadNamedRef(meshData, "subMeshes");
        const bool meshOk = mesh != nullptr && IsUnityManagedObjectAlive(mesh);
        UnityVector3 size{};
        if (meshOk) {
            static_cast<void>(GetMeshSize(mesh, &size));
        }
        std::string shader = "-";
        if (subMeshes != nullptr) {
            auto items =
                reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(
                    subMeshes)
                    ->ToVector();
            for (void* sub : items) {
                if (sub == nullptr) {
                    continue;
                }
                void* material = ReadNamedRef(sub, "material");
                if (material != nullptr && IsUnityManagedObjectAlive(material)) {
                    shader = ManagedObjectName(GetShader(material));
                    break;
                }
            }
        }
        if (meshOk || shader != "-") {
            std::ostringstream line;
            line << "[VR][stereo] HAND_GLOW list kind=crowd"
                 << " mesh=" << ManagedObjectName(mesh)
                 << " size=" << size.x << "," << size.y << "," << size.z
                 << " shader=" << shader
                 << " material=gpu-instance";
            LogHandGlow(line.str());
            return;
        }
    }
}

float QuantizeCrowdIntensity(float intensity) noexcept {
    // CrowdCompute.UpdatePerson: saturate(intensity * 0.25), truncate to ten
    // bits; Crowd/DefaultPenlight decodes those bits with 4 / 1023.
    const float normalized = std::clamp(intensity * 0.25F, 0.0F, 1.0F);
    const auto packed = static_cast<std::uint32_t>(normalized * 1023.0F);
    return static_cast<float>(packed) * (4.0F / 1023.0F);
}

bool ReadCrowdPersonIntensityArray(
    void* personInfos, float* intensity, bool log) noexcept {
    if (personInfos == nullptr || intensity == nullptr) {
        return false;
    }
    const auto people = reinterpret_cast<
        UnityResolve::UnityType::Array<CrowdPersonInfo>*>(personInfos)
                            ->ToVector();
    std::vector<float> visibleIntensities;
    visibleIntensities.reserve(people.size());
    for (const auto& person : people) {
        if (!std::isfinite(person.intensity)) {
            continue;
        }
        const float decoded = QuantizeCrowdIntensity(person.intensity);
        if (decoded > 0.0F) {
            visibleIntensities.push_back(decoded);
        }
    }
    if (visibleIntensities.empty()) {
        return false;
    }
    std::sort(visibleIntensities.begin(), visibleIntensities.end());
    *intensity = visibleIntensities[visibleIntensities.size() / 2U];
    if (log) {
        auto line = ClassicLine();
        line << "[VR][stereo] HAND_GLOW audience intensity"
             << " people=" << people.size()
             << " visible=" << visibleIntensities.size()
             << " min=" << visibleIntensities.front()
             << " median=" << *intensity
             << " max=" << visibleIntensities.back()
             << " source=current-crowd-system"
             << " formula=sat(i/4)*1023-u10";
        LogHandGlow(line.str());
    }
    return true;
}

bool ReadCrowdAudienceIntensity(
    void* volume, float* intensity, bool log) noexcept {
    if (volume == nullptr || intensity == nullptr) {
        return false;
    }
    void* crowdData = ReadNamedRef(volume, "crowdData");
    if (crowdData == nullptr || !IsUnityManagedObjectAlive(crowdData)) {
        return false;
    }
    void* personInfos = ReadNamedRef(crowdData, "personInfos");
    if (personInfos == nullptr) {
        return false;
    }
    return ReadCrowdPersonIntensityArray(personInfos, intensity, log);
}

void* FindCrowdPenlightMesh(
    UnityVector3* size, float* intensity, bool log) noexcept {
    auto* volumeClass = FindCampusClass("Campus.Crowd", "CrowdVolume");
    if (volumeClass == nullptr) {
        return nullptr;
    }
    const auto volumes = FindAllOfClass(volumeClass);
    for (void* volume : volumes) {
        if (volume == nullptr || !IsUnityManagedObjectAlive(volume)) {
            continue;
        }
        void* modelData = ReadNamedRef(volume, "modelData");
        if (modelData == nullptr || !IsUnityManagedObjectAlive(modelData)) {
            continue;
        }
        void* meshData = ReadNamedRef(modelData, "penlightMeshData");
        if (meshData == nullptr || !IsUnityManagedObjectAlive(meshData)) {
            continue;
        }
        void* mesh = ReadNamedRef(meshData, "mesh");
        if (mesh != nullptr && IsUnityManagedObjectAlive(mesh)) {
            float measuredIntensity = 0.0F;
            if (!ReadCrowdAudienceIntensity(
                    volume, &measuredIntensity, log)) {
                if (log) {
                    LogHandGlow(
                        "[VR][stereo] HAND_GLOW discover skip=crowd-intensity");
                }
                continue;
            }
            if (size != nullptr) {
                static_cast<void>(GetMeshSize(mesh, size));
            }
            if (intensity != nullptr) {
                *intensity = measuredIntensity;
            }
            return mesh;
        }
    }
    return nullptr;
}

bool TryDiscoverMobPenlight(bool logList) noexcept {
    auto* controllerClass = FindCampusClass(
        "Campus.MobAudience", "MobAudiencePenlightController");
    if (controllerClass == nullptr) {
        return false;
    }
    UnityVector3 crowdSize{};
    float crowdIntensity = 0.0F;
    void* crowdMesh =
        FindCrowdPenlightMesh(&crowdSize, &crowdIntensity, logList);
    if (crowdMesh == nullptr || !IsUnityManagedObjectAlive(crowdMesh)) {
        if (logList) {
            LogHandGlow(
                "[VR][stereo] HAND_GLOW discover skip=crowd-mesh");
        }
        return false;
    }
    struct Candidate {
        void* renderer = nullptr;
        void* mesh = nullptr;
        void* material = nullptr;
        void* gameObject = nullptr;
        void* transform = nullptr;
        UnityVector3 localScale{1.0F, 1.0F, 1.0F};
        UnityVector3 lossyScale{1.0F, 1.0F, 1.0F};
        UnityVector3 meshSize{};
        int colorIndex = 0;
        int layer = 0;
        float score = -1.0F;
        std::string shader{};
    };
    Candidate best{};
    std::uint32_t listed = 0;
    g_state.mobSources.clear();

    struct RuntimeMat {
        void* renderer = nullptr;
        void* material = nullptr;
        int colorIndex = 0;
    };
    std::vector<RuntimeMat> runtimeMats;

    const auto controllers = FindAllOfClass(controllerClass);
    int liveControllers = 0;
    int runtimeArrays = 0;
    int runtimeSettings = 0;
    for (void* controller : controllers) {
        if (controller == nullptr || !IsUnityManagedObjectAlive(controller)) {
            continue;
        }
        ++liveControllers;
        void* runtime = ReadNamedRef(controller, "_runtimeMobPenlightSettings");
        if (runtime == nullptr) {
            continue;
        }
        ++runtimeArrays;
        auto runtimeItems =
            reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(runtime)
                ->ToVector();
        for (void* setting : runtimeItems) {
            if (setting == nullptr) {
                continue;
            }
            ++runtimeSettings;
            const int colorIndex = ReadNamedInt(setting, "ColorTableIndex");
            void* infos = ReadNamedRef(setting, "MaterialInfos");
            if (infos == nullptr) {
                continue;
            }
            auto infoItems =
                reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(infos)
                    ->ToVector();
            for (void* info : infoItems) {
                if (info == nullptr) {
                    continue;
                }
                void* renderer = ReadNamedRef(info, "_renderer");
                void* instanced = ReadNamedRef(info, "_instancedMaterial");
                const bool instancedOk = instanced != nullptr &&
                    IsUnityManagedObjectAlive(instanced);
                if (instancedOk) {
                    runtimeMats.push_back({renderer, instanced, colorIndex});
                }
                if (logList && listed < kMaxListLines) {
                    UnityColor infoColor(0.0F, 0.0F, 0.0F, 0.0F);
                    UnityColor matColor(0.0F, 0.0F, 0.0F, 0.0F);
                    float emit = 0.0F;
                    static_cast<void>(ReadMaterialInfoColor(
                        info, PenlightColorId(), &infoColor));
                    static_cast<void>(ReadMaterialInfoFloat(
                        info, EmissionScaleId(), &emit));
                    if (instancedOk) {
                        static_cast<void>(GetMaterialColor(
                            instanced, PenlightColorId(), &matColor));
                    }
                    auto line = ClassicLine();
                    line << "[VR][stereo] HAND_GLOW list kind=mobinfo"
                         << " colorIdx=" << colorIndex
                         << " inst=" << (instancedOk ? 1 : 0)
                         << " id=" << PenlightColorId()
                         << " info=" << infoColor.r << "," << infoColor.g
                         << "," << infoColor.b << "," << infoColor.a
                         << " emit=" << emit
                         << " mat=" << matColor.r << "," << matColor.g << ","
                         << matColor.b << "," << matColor.a;
                    LogHandGlow(line.str());
                    ++listed;
                }
            }
        }
    }
    if (logList) {
        auto line = ClassicLine();
        line << "[VR][stereo] HAND_GLOW list kind=runtime"
             << " controllers=" << liveControllers
             << " runtimeArr=" << runtimeArrays
             << " settings=" << runtimeSettings
             << " inst=" << runtimeMats.size();
        LogHandGlow(line.str());
    }

    for (void* controller : controllers) {
        if (controller == nullptr || !IsUnityManagedObjectAlive(controller)) {
            continue;
        }
        void* settings = ReadNamedRef(controller, "penlightSettings");
        if (settings == nullptr) {
            continue;
        }
        auto settingItems =
            reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(settings)
                ->ToVector();
        for (void* setting : settingItems) {
            if (setting == nullptr) {
                continue;
            }
            const int colorIndex = ReadNamedInt(setting, "ColorTableIndex");
            void* renderers = ReadNamedRef(setting, "PenlightRenderers");
            if (renderers == nullptr) {
                continue;
            }
            auto rendererItems =
                reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(
                    renderers)
                    ->ToVector();
            for (void* renderer : rendererItems) {
                if (renderer == nullptr ||
                    !IsUnityManagedObjectAlive(renderer)) {
                    continue;
                }
                void* shared = GetSharedMaterial(renderer);
                if (shared == nullptr || !IsUnityManagedObjectAlive(shared)) {
                    continue;
                }
                void* sourceObject = GetGameObject(renderer);
                void* filter = GetComponent(
                    sourceObject, g_state.api.meshFilterClass);
                void* mesh = GetSharedMesh(filter);
                if (mesh == nullptr || !IsUnityManagedObjectAlive(mesh)) {
                    continue;
                }
                using GetTransform = void* (*)(void*, void*);
                void* transform = nullptr;
                if (sourceObject != nullptr && g_state.api.getTransform.Ready()) {
                    static_cast<void>(InvokeManagedResult<void*, GetTransform>(
                        g_state.api.getTransform, &transform, sourceObject));
                }
                void* instanced = nullptr;
                for (const auto& runtimeMat : runtimeMats) {
                    if (runtimeMat.renderer == renderer &&
                        runtimeMat.material != nullptr) {
                        instanced = runtimeMat.material;
                        break;
                    }
                }
                if (instanced == nullptr) {
                    for (const auto& runtimeMat : runtimeMats) {
                        if (runtimeMat.colorIndex == colorIndex &&
                            runtimeMat.material != nullptr) {
                            instanced = runtimeMat.material;
                            break;
                        }
                    }
                }
                void* material = instanced != nullptr ? instanced : shared;
                Candidate current{};
                current.renderer = renderer;
                current.mesh = mesh;
                current.material = material;
                current.gameObject = sourceObject;
                current.transform = transform;
                current.colorIndex = colorIndex;
                current.layer = GetLayer(sourceObject);
                current.shader = ManagedObjectName(GetShader(material));
                static_cast<void>(GetMeshSize(mesh, &current.meshSize));
                static_cast<void>(GetTransformScale(
                    g_state.api.getLocalScale, transform, &current.localScale));
                static_cast<void>(GetTransformScale(
                    g_state.api.getLossyScale, transform, &current.lossyScale));
                const float extent = (std::max)(
                    current.meshSize.x,
                    (std::max)(current.meshSize.y, current.meshSize.z));
                current.score = extent;
                if (current.shader.find("MobPenlight") != std::string::npos) {
                    current.score += 10.0F;
                }
                if (instanced != nullptr) {
                    current.score += 20.0F;
                }
                UnityColor mpbColor(0.0F, 0.0F, 0.0F, 0.0F);
                const bool hasMpb = ReadRendererMpbColor(renderer, &mpbColor);
                if (logList && listed < kMaxListLines) {
                    UnityColor matColor(0.0F, 0.0F, 0.0F, 0.0F);
                    static_cast<void>(GetMaterialColor(
                        material, PenlightColorId(), &matColor));
                    auto line = ClassicLine();
                    line << "[VR][stereo] HAND_GLOW list kind=mob"
                         << " shader=" << current.shader
                         << " mesh=" << ManagedObjectName(mesh)
                         << " size=" << current.meshSize.x << ","
                         << current.meshSize.y << "," << current.meshSize.z
                         << " local=" << current.localScale.x << ","
                         << current.localScale.y << "," << current.localScale.z
                         << " lossy=" << current.lossyScale.x << ","
                         << current.lossyScale.y << "," << current.lossyScale.z
                         << " colorIdx=" << colorIndex
                         << " layer=" << current.layer
                         << " inst=" << (instanced != nullptr ? 1 : 0);
                    if (hasMpb) {
                        line << " mpb=" << mpbColor.r << "," << mpbColor.g
                             << "," << mpbColor.b << "," << mpbColor.a;
                    }
                    line << " mat=" << matColor.r << "," << matColor.g << ","
                         << matColor.b << "," << matColor.a;
                    LogHandGlow(line.str());
                    ++listed;
                }
                g_state.mobSources.push_back({renderer, instanced, colorIndex});
                if (current.score > best.score) {
                    best = current;
                }
            }
        }
    }

    if (best.renderer == nullptr || best.material == nullptr) {
        return HasDrawableAssets();
    }
    const bool hadAssets = HasDrawableAssets();
    if (!hadAssets) {
        void* meshClone = CloneCrowdMeshWithVertexGradient(
            crowdMesh, crowdSize);
        if (meshClone == nullptr) {
            LogHandGlow(
                "[VR][stereo] HAND_GLOW discover skip=gradient-mesh");
            return false;
        }
        void* handMaterial = CreateHandEmissionMaterial();
        if (handMaterial == nullptr) {
            LogHandGlow(
                "[VR][stereo] HAND_GLOW discover skip=emission-material");
            return false;
        }
        void* handBaseMaterial = CreateHandBaseMaterial();
        if (handBaseMaterial == nullptr) {
            LogHandGlow(
                "[VR][stereo] HAND_GLOW discover degrade=emission-only reason=base-material");
        }
        RetireHandle(g_state.officialMeshHandle);
        g_state.officialMeshHandle = CreateGcHandle(meshClone);
        g_state.officialMesh = meshClone;
        RetireHandle(g_state.officialMaterialHandle);
        g_state.officialMaterialHandle = CreateGcHandle(handMaterial);
        g_state.officialMaterial = handMaterial;
        RetireHandle(g_state.officialBaseMaterialHandle);
        g_state.officialBaseMaterialHandle = CreateGcHandle(handBaseMaterial);
        g_state.officialBaseMaterial = handBaseMaterial;
        if (g_state.officialMeshHandle == nullptr ||
            g_state.officialMaterialHandle == nullptr ||
            (handBaseMaterial != nullptr &&
             g_state.officialBaseMaterialHandle == nullptr)) {
            LogHandGlow(
                "[VR][stereo] HAND_GLOW discover skip=asset-handle");
            ClearOfficialAssets();
            return false;
        }
    }
    // Match the two outputs proven in Crowd/DefaultPenlight's GBuffer fragment:
    // an opaque white lit substrate plus a colored emission term multiplied by
    // the authored COLOR.r ramp. Keep the emission overlay forward-only so the
    // hand mesh does not re-enter the broken penlight reflection path.
    g_state.sourceRenderer = best.renderer;
    g_state.sourceLayer = best.layer;
    g_state.sourceFromMob = true;
    g_state.sourceLocalScale = best.localScale;
    g_state.sourceLossyScale = best.lossyScale;
    g_state.audienceIntensity = crowdIntensity;
    if (std::fabs(g_state.sourceLossyScale.x) +
            std::fabs(g_state.sourceLossyScale.y) +
            std::fabs(g_state.sourceLossyScale.z) <
        0.01F) {
        g_state.sourceLossyScale = {1.0F, 1.0F, 1.0F};
    }
    g_state.meshSize = crowdSize;
    if (logList || !hadAssets) {
        LogShaderProperties(g_state.officialBaseMaterial);
        LogShaderProperties(g_state.officialMaterial);
        auto line = ClassicLine();
        line << "[VR][stereo] HAND_GLOW discover source=mob"
             << " pick=" << best.shader
             << " mesh=crowd"
             << " size=" << g_state.meshSize.x << "," << g_state.meshSize.y
             << "," << g_state.meshSize.z
             << " scale=" << best.lossyScale.x << "," << best.lossyScale.y
             << "," << best.lossyScale.z
             << " colorIdx=" << best.colorIndex
             << " layer=" << best.layer
             << " proj=" << (g_state.useColorProjection ? 1 : 0)
             << " id=" << PenlightColorId()
             << " runtime=" << runtimeMats.size()
             << " gradient=audience-intensity*color-r"
             << " intensity=" << g_state.audienceIntensity
             << " baseShader="
             << ManagedObjectName(GetShader(g_state.officialBaseMaterial))
             << " emissionShader=" << kHandEmissionShader
             << " layers="
             << (g_state.officialBaseMaterial != nullptr
                     ? "opaque-actor-lit+additive-color-r"
                     : "emission-only-fail-open");
        LogHandGlow(line.str());
    }
    return true;
}

void DiscoverAssets() noexcept {
    if (g_state.officialBaseMaterial != nullptr &&
        !IsUnityManagedObjectAlive(g_state.officialBaseMaterial)) {
        ClearOfficialAssets();
    }
    if (g_state.officialMaterial != nullptr &&
        !IsUnityManagedObjectAlive(g_state.officialMaterial)) {
        ClearOfficialAssets();
    }
    if (g_state.officialMesh != nullptr &&
        !IsUnityManagedObjectAlive(g_state.officialMesh)) {
        ClearOfficialAssets();
    }
    const bool logList = !g_state.listed;
    if (logList) {
        LogCrowdFamily();
    }
    if (TryDiscoverMobPenlight(logList)) {
        g_state.listed = true;
        return;
    }
    if (logList) {
        g_state.listed = true;
    }
}

void DropHand(HandObject& hand, const char* reason) noexcept {
    if (hand.gameObject != nullptr) {
        LogHandGlow(
            std::string("[VR][stereo] HAND_GLOW recreate reason=") + reason);
    }
    RetireHandle(hand.handle);
    RetireHandle(hand.baseMaterialHandle);
    RetireHandle(hand.materialHandle);
    RetireHandle(hand.materialInfoHandle);
    hand = {};
}

void DropAllHands(const char* reason) noexcept {
    UnregisterHandsFromOfficialController();
    for (auto& hand : g_state.hands) {
        if (hand.gameObject != nullptr) {
            DropHand(hand, reason);
        }
    }
}

bool EnsureHand(std::size_t index) noexcept {
    auto& hand = g_state.hands[index];
    if (hand.gameObject != nullptr &&
        IsUnityManagedObjectAlive(hand.gameObject) &&
        hand.transform != nullptr) {
        return true;
    }
    if (hand.gameObject != nullptr) {
        DropHand(hand, "dead");
    }
    if (!HasDrawableAssets() || !EnsureApi()) {
        return false;
    }

    void* gameObject = NewIl2CppObject(g_state.api.gameObjectClass->address);
    auto* managedName = UnityResolve::UnityType::String::New(
        index == 0U ? "__GakumasVrHandGlow0" : "__GakumasVrHandGlow1");
    if (gameObject == nullptr || managedName == nullptr) {
        LogHandGlow("[VR][stereo] HAND_GLOW skip reason=create");
        return false;
    }
    using CreateGameObject = void (*)(void*, void*, void*);
    if (!InvokeManagedVoid<CreateGameObject>(
            g_state.api.internalCreateGameObject, gameObject, managedName)) {
        LogHandGlow("[VR][stereo] HAND_GLOW skip reason=create");
        return false;
    }
    static_cast<void>(SetActive(gameObject, false));
    using DontDestroy = void (*)(void*, void*);
    if (!InvokeManagedVoid<DontDestroy>(
            g_state.api.dontDestroyOnLoad, gameObject)) {
        LogHandGlow("[VR][stereo] HAND_GLOW skip reason=ddol");
        return false;
    }
    if (AddComponent(gameObject, g_state.api.meshFilterClass) == nullptr ||
        AddComponent(gameObject, g_state.api.meshRendererClass) == nullptr) {
        LogHandGlow("[VR][stereo] HAND_GLOW skip reason=add-component");
        return false;
    }
    hand.colorIndex = SlotForHand(index);
    void* sourceMaterial = MaterialForColorIndex(hand.colorIndex);
    void* clonedMaterial = CloneMaterial(sourceMaterial);
    if (clonedMaterial == nullptr) {
        LogHandGlow("[VR][stereo] HAND_GLOW skip reason=clone-material");
        return false;
    }
    if (!ConfigureHandEmissionMaterial(clonedMaterial)) {
        LogHandGlow(
            "[VR][stereo] HAND_GLOW skip reason=emission-material");
        return false;
    }
    void* clonedBaseMaterial = nullptr;
    if (g_state.officialBaseMaterial != nullptr &&
        IsUnityManagedObjectAlive(g_state.officialBaseMaterial)) {
        clonedBaseMaterial = CloneMaterial(g_state.officialBaseMaterial);
        if (clonedBaseMaterial == nullptr ||
            !ConfigureHandBaseMaterial(clonedBaseMaterial)) {
            LogHandGlow(
                "[VR][stereo] HAND_GLOW create degrade=emission-only reason=base-material");
            clonedBaseMaterial = nullptr;
        }
    }
    Il2CppGCHandle materialHandle = CreateGcHandle(clonedMaterial);
    Il2CppGCHandle baseMaterialHandle = clonedBaseMaterial != nullptr
        ? CreateGcHandle(clonedBaseMaterial)
        : nullptr;
    if (materialHandle == nullptr) {
        LogHandGlow("[VR][stereo] HAND_GLOW skip reason=material-gchandle");
        return false;
    }
    if (clonedBaseMaterial != nullptr && baseMaterialHandle == nullptr) {
        LogHandGlow(
            "[VR][stereo] HAND_GLOW create degrade=emission-only reason=base-gchandle");
        clonedBaseMaterial = nullptr;
    }
    void* renderer = ApplyDrawableAssets(
        gameObject, clonedBaseMaterial, clonedMaterial);
    if (renderer == nullptr) {
        RetireHandle(materialHandle);
        RetireHandle(baseMaterialHandle);
        LogHandGlow("[VR][stereo] HAND_GLOW skip reason=dual-material");
        return false;
    }
    using GetTransform = void* (*)(void*, void*);
    void* transform = nullptr;
    if (!InvokeManagedResult<void*, GetTransform>(
            g_state.api.getTransform, &transform, gameObject) ||
        transform == nullptr) {
        RetireHandle(materialHandle);
        RetireHandle(baseMaterialHandle);
        LogHandGlow("[VR][stereo] HAND_GLOW skip reason=transform");
        return false;
    }
    ApplyOfficialScale(transform);
    Il2CppGCHandle handle = CreateGcHandle(gameObject);
    if (handle == nullptr) {
        RetireHandle(materialHandle);
        RetireHandle(baseMaterialHandle);
        LogHandGlow("[VR][stereo] HAND_GLOW skip reason=gchandle");
        return false;
    }
    hand.gameObject = gameObject;
    hand.transform = transform;
    hand.renderer = renderer;
    hand.baseMaterial = clonedBaseMaterial;
    hand.material = clonedMaterial;
    hand.handle = handle;
    hand.baseMaterialHandle = baseMaterialHandle;
    hand.materialHandle = materialHandle;
    hand.emissionMaterialIndex = clonedBaseMaterial != nullptr ? 1 : 0;
    hand.active = false;
    ApplyOfficialColor(&hand, hand.colorIndex, true);
    auto line = ClassicLine();
    line << "[VR][stereo] HAND_GLOW create hand=" << index
         << " officialMesh=1 officialMat=1 layer=" << g_state.sourceLayer
          << " shader="
          << ManagedObjectName(GetShader(clonedMaterial))
          << " baseShader=" << ManagedObjectName(GetShader(clonedBaseMaterial))
          << " emissionSlot=" << hand.emissionMaterialIndex
         << " mesh=crowd"
         << " size=" << g_state.meshSize.x << "," << g_state.meshSize.y << ","
         << g_state.meshSize.z
         << " scale=" << g_state.sourceLossyScale.x * kHandScaleMultiplier
         << "," << g_state.sourceLossyScale.y * kHandScaleMultiplier << ","
         << g_state.sourceLossyScale.z * kHandScaleMultiplier
         << " idx=" << hand.colorIndex
         << " unique=" << g_state.uniqueColors.size();
    LogHandGlow(line.str());
    return true;
}

void HideHands(const char* reason) noexcept {
    bool changed = false;
    for (auto& hand : g_state.hands) {
        if (hand.gameObject == nullptr) {
            continue;
        }
        if (!IsUnityManagedObjectAlive(hand.gameObject)) {
            DropHand(hand, "dead");
            continue;
        }
        if (hand.active) {
            static_cast<void>(SetActive(hand.gameObject, false));
            hand.active = false;
            changed = true;
        }
    }
    g_state.hidden = true;
    if (changed || g_state.lastHideReason != reason) {
        g_state.lastHideReason = reason != nullptr ? reason : "?";
        LogHandGlow(
            std::string("[VR][stereo] HAND_GLOW hide reason=") +
            g_state.lastHideReason);
    }
}

void RefreshOfficialAssets() noexcept {
    DiscoverAssets();
    if (!HasDrawableAssets()) {
        return;
    }
    for (std::size_t index = 0; index < g_state.hands.size(); ++index) {
        auto& hand = g_state.hands[index];
        if (hand.gameObject == nullptr ||
            !IsUnityManagedObjectAlive(hand.gameObject)) {
            continue;
        }
        hand.colorIndex = SlotForHand(index);
        if (g_state.officialBaseMaterial != nullptr &&
            IsUnityManagedObjectAlive(g_state.officialBaseMaterial) &&
            (hand.baseMaterial == nullptr ||
             !IsUnityManagedObjectAlive(hand.baseMaterial))) {
            void* clonedBase = CloneMaterial(g_state.officialBaseMaterial);
            if (clonedBase == nullptr ||
                !ConfigureHandBaseMaterial(clonedBase)) {
                LogHandGlow(
                    "[VR][stereo] HAND_GLOW refresh degrade=emission-only reason=base-material");
                clonedBase = nullptr;
            }
            RetireHandle(hand.baseMaterialHandle);
            hand.baseMaterialHandle = clonedBase != nullptr
                ? CreateGcHandle(clonedBase)
                : nullptr;
            if (clonedBase != nullptr && hand.baseMaterialHandle == nullptr) {
                LogHandGlow(
                    "[VR][stereo] HAND_GLOW refresh degrade=emission-only reason=base-gchandle");
                clonedBase = nullptr;
            }
            hand.baseMaterial = clonedBase;
        }
        if (hand.material == nullptr ||
            !IsUnityManagedObjectAlive(hand.material)) {
            void* cloned = CloneMaterial(MaterialForColorIndex(hand.colorIndex));
            if (cloned != nullptr) {
                if (!ConfigureHandEmissionMaterial(cloned)) {
                    LogHandGlow(
                        "[VR][stereo] HAND_GLOW recreate reason=emission-material");
                    continue;
                }
                RetireHandle(hand.materialHandle);
                RetireHandle(hand.materialInfoHandle);
                hand.materialHandle = CreateGcHandle(cloned);
                hand.material = cloned;
                hand.materialInfo = nullptr;
                hand.materialInfoHandle = nullptr;
            }
        }
        const int previousEmissionSlot = hand.emissionMaterialIndex;
        hand.emissionMaterialIndex = hand.baseMaterial != nullptr ? 1 : 0;
        if (previousEmissionSlot != hand.emissionMaterialIndex) {
            RetireHandle(hand.materialInfoHandle);
            hand.materialInfo = nullptr;
            hand.materialInfoHandle = nullptr;
        }
        hand.renderer = ApplyDrawableAssets(
            hand.gameObject, hand.baseMaterial, hand.material);
        ApplyOfficialScale(hand.transform);
        ApplyOfficialColor(&hand, hand.colorIndex, true);
    }
}

} // namespace

void TickHandGlowSticks(
    const pose::Pose& headsetPose,
    bool headsetValid,
    const pose::StereoPoseSample& trackingSample) noexcept {
    ++g_state.ticks;
    g_state.crowdHandPoseValid.fill(false);
    if (!GakumasLocal::Config::vrHandGlowSticks) {
        ClearAudienceColors("config-off");
        HideHands("config-off");
        return;
    }
    if (!SceneReadyAllowsStereoRender()) {
        ClearAudienceColors("scene-not-ready");
        g_state.listed = false;
        g_state.crowdPoseBridgeValid = false;
        g_state.crowdIntensitySources.clear();
        g_state.audienceIntensity = 0.0F;
        HideHands("scene-not-ready");
        return;
    }
    if (!headsetValid || !pose::IsFinite(headsetPose.position) ||
        !pose::IsFinite(headsetPose.orientation)) {
        HideHands("headset-invalid");
        return;
    }
    if (!trackingSample.valid || trackingSample.viewCount != 2U) {
        HideHands("tracking-invalid");
        return;
    }

    pose::Pose hmdOpenXrCenter{};
    std::array<pose::Pose, 2> eyes{
        trackingSample.eyes[0].pose, trackingSample.eyes[1].pose};
    if (!pose::TryCenterStereoPose(eyes, hmdOpenXrCenter)) {
        HideHands("hmd-center");
        return;
    }

    if (!EnsureApi()) {
        return;
    }

    g_state.crowdGameHeadsetPose = headsetPose;
    g_state.crowdOpenXrHeadCenter = hmdOpenXrCenter;
    g_state.crowdWorldScale = GakumasLocal::Config::vrWorldScale;
    g_state.crowdPoseBridgeValid = true;
    ++g_state.crowdPoseBridgeRevision;
    if (!RefreshCrowdHandPosesFromMailbox()) {
        HideHands("pose-invalid");
        return;
    }

    // The Crowd hook is the only render path. Fail closed while its draw is
    // pending so a binding/resource error cannot be mistaken for success from
    // the legacy transparent GameObject implementation.
    const bool crowdHeartbeat = g_state.crowdDrawActive &&
        (g_state.ticks - g_state.lastCrowdDrawTick) <= 4U;
    HideHands(crowdHeartbeat
        ? "crowd-indirect-active"
        : "crowd-indirect-pending");
}

void CycleHandGlowStickColor() noexcept {
    RequestCycleHandGlowColor();
    const int slot = SlotForHand(0);
    const bool registered = RegisterHandsWithOfficialController();
    const UnityColor color = ColorForIndex(slot);
    for (auto& hand : g_state.hands) {
        if (hand.renderer == nullptr || hand.material == nullptr ||
            !IsUnityManagedObjectAlive(hand.renderer) ||
            !IsUnityManagedObjectAlive(hand.material)) {
            continue;
        }
        hand.colorIndex = slot;
        if (registered && EnsureHandMaterialInfo(hand)) {
            // Match .301's immediate two-hand switch instead of waiting for a
            // later camera update to notice the new ColorTableIndex.
            static_cast<void>(
                WriteHandMaterialInfoColors(hand.materialInfo, color));
        }
        ApplyOfficialColor(&hand, slot, true);
    }
}

void AfterOfficialPenlightCamera(void* controller) noexcept {
    if (controller == nullptr || !GakumasLocal::Config::vrHandGlowSticks) {
        return;
    }
    if (!g_state.api.resolved && !EnsureApi()) {
        return;
    }
    const bool ours = g_state.registered &&
        SettingStillRegistered(controller, g_state.registeredSetting);
    if (ours) {
        for (auto& hand : g_state.hands) {
            ApplyHandMaterialInfo(hand.materialInfo);
        }
    } else {
        static_cast<void>(CopyOfficialInfoOntoHands(controller));
    }
    if ((g_state.ticks - g_state.lastCameraLogTick) < 120U &&
        g_state.lastCameraLogTick != 0U) {
        return;
    }
    g_state.lastCameraLogTick = g_state.ticks;
    const auto official = FirstOfficialInfoColor(
        controller, g_state.registeredSetting);
    UnityResolve::UnityType::Color hand(0.0F, 0.0F, 0.0F, 0.0F);
    for (const auto& object : g_state.hands) {
        if (object.materialInfo != nullptr &&
            ReadMaterialInfoColor(
                object.materialInfo, PenlightColorId(), &hand) &&
            ColorLuma(hand) > 0.01F) {
            break;
        }
    }
    auto line = ClassicLine();
    line << "[VR][stereo] HAND_GLOW camera registered=" << (ours ? 1 : 0)
         << " official=" << official.r << "," << official.g << ","
         << official.b << "," << official.a
         << " hand=" << hand.r << "," << hand.g << "," << hand.b << ","
         << hand.a;
    LogHandGlow(line.str());
}

void AfterOfficialCrowdRender(
    void* crowdSystem, void* commandBuffer, int eventType) noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) {
        return;
    }
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        AuditCrowdRenderSystem(crowdSystem, eventType);
    }
    DrawControllerSticksThroughCrowd(
        crowdSystem, commandBuffer, eventType);
}

} // namespace gakumas::vr
