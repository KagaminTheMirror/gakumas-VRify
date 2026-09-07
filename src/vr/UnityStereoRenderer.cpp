#include "UnityStereoRenderer.hpp"

#include "LivePause.hpp"
#include "LivenessCrashProbe.hpp"
#include "GripTransparencyTrace.hpp"
#include "SceneReadyGate.hpp"
#include "VrHandGlowSticks.hpp"
#include "SkyRenderHooks.hpp"
#include "VrFreeCamera.hpp"
#include "VrRuntime.hpp"
#include "camera/FovEquivalence.hpp"
#include "camera/ProjectionIntrinsics.hpp"
#include "d3d11/TextureFingerprint.hpp"
#include "../GakumasLocalify/Il2cppUtils.hpp"
#include "config/VrifyConfig.hpp"
#include "../deps/UnityResolve/UnityResolve.hpp"
#include "../hooks/HookManager.hpp"

#include <Windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace gakumas::vr {
namespace {

using UnityVector3 = UnityResolve::UnityType::Vector3;
using UnityVector2 = UnityResolve::UnityType::Vector2;
using UnityVector4 = UnityResolve::UnityType::Vector4;
using UnityQuaternion = UnityResolve::UnityType::Quaternion;
using UnityMatrix4x4 = UnityResolve::UnityType::Matrix4x4;

// The authored far-background mountain shell and VL cloud cards extend well
// beyond the source camera's 1000 m far plane. Grip's narrow frustum mostly
// conceals that intersection, while the OpenXR eye frustum exposes it as a
// head-following arc. Keep the source/Grip camera untouched and widen only the
// two eye cameras far enough to contain the recovered scene geometry.
constexpr float kMinimumEyeFarClip = 5000.0F;

// ADV (授業 / 初星コミュ) shots raise the source camera near plane to
// 0.2-0.8 m per authored shot (Campus.ADV.CameraSettingData ->
// Cinemachine LensSettings.NearClipPlane) to hide props near the authored
// camera. Inheriting that near plane makes the HMD carry a head-attached
// clip box that cross-sections desks, bedding and characters
// (evidence/head-clip-box/). Keep the source/Grip camera untouched and
// clamp only the eye near plane; reversed-Z keeps 0.02/5000 precise.
constexpr float kMaximumEyeNearClip = 0.02F;

struct UnityLayerMaskValue {
    std::int32_t mask = 0;
};

static_assert(sizeof(UnityLayerMaskValue) == sizeof(std::int32_t));

// Unity 2022/6000 RenderTextureDescriptor is a blittable sequence of thirteen
// 32-bit fields. We still verify the runtime value size before copying it so a
// future game/Unity update fails closed instead of corrupting managed memory.
struct UnityRenderTextureDescriptorValue {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t msaaSamples = 1;
    std::int32_t volumeDepth = 1;
    std::int32_t mipCount = 1;
    std::int32_t graphicsFormat = 0;
    std::int32_t stencilFormat = 0;
    std::int32_t depthStencilFormat = 0;
    std::int32_t dimension = 2;
    std::int32_t shadowSamplingMode = 0;
    std::int32_t vrUsage = 0;
    std::uint32_t flags = 0;
    std::int32_t memoryless = 0;
};

static_assert(sizeof(UnityRenderTextureDescriptorValue) == 52U);

// Stable serialized prefix of UniversalAdditionalCameraData.m_TaaSettings.
// The two trailing fields are per-camera reset/jitter counters and must remain
// independent between the source, left eye, and right eye.
struct UnityTaaSettingsPrefix {
    std::int32_t quality = 0;
    float frameInfluence = 0.0F;
    float jitterScale = 0.0F;
    float mipBias = 0.0F;
    float varianceClampScale = 0.0F;
    float contrastAdaptiveSharpening = 0.0F;
};

static_assert(sizeof(UnityTaaSettingsPrefix) == 24U);

bool TryPrepareSmaaT2xMotionHistoryProjection(
    float* liveProjection,
    const float* expectedJitteredProjection,
    const float* unjitteredProjection,
    float tolerance,
    float* incomingMaximumError,
    float* correctedMaximumError) noexcept {
    __try {
        return d3d11::PrepareSmaaT2xMotionHistoryProjection(
            liveProjection,
            expectedJitteredProjection,
            unjitteredProjection,
            tolerance,
            incomingMaximumError,
            correctedMaximumError);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename Value>
bool ReadManagedField(
    void* object,
    std::int32_t offset,
    Value* value) noexcept {
    if (object == nullptr || offset < 0 || value == nullptr) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<const Value*>(
            static_cast<const unsigned char*>(object) + offset);
        return true;
    } __except (EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

template <typename Value>
bool WriteManagedField(
    void* object,
    std::int32_t offset,
    const Value& value) noexcept {
    if (object == nullptr || offset < 0) {
        return false;
    }
    __try {
        *reinterpret_cast<Value*>(
            static_cast<unsigned char*>(object) + offset) = value;
        return true;
    } __except (EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool ReadTaaSettingsPrefix(
    void* object,
    std::int32_t offset,
    UnityTaaSettingsPrefix* value) noexcept {
    return ReadManagedField(object, offset, value);
}

bool WriteTaaSettingsPrefix(
    void* object,
    std::int32_t offset,
    const UnityTaaSettingsPrefix& value) noexcept {
    return WriteManagedField(object, offset, value);
}

bool FieldNameMatches(
    const char* name,
    std::initializer_list<const char*> names) noexcept {
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

bool FieldTypeMatches(
    const char* typeName,
    std::initializer_list<const char*> typeNames) noexcept {
    if (typeNames.size() == 0U) {
        return true;
    }
    if (typeName == nullptr) {
        return false;
    }
    for (const char* want : typeNames) {
        if (want != nullptr && std::strcmp(typeName, want) == 0) {
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
            if (FieldTypeMatches(typeName, typeNames)) {
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
            WriteVrLog(
                std::string("[VR][stereo] ") + tag +
                " class=" + (className != nullptr ? className : "?") +
                " name=" + (name != nullptr ? name : "?") +
                " type=" + (typeName != nullptr ? typeName : "?") +
                " offset=" + std::to_string(offset));
        }
        current = UnityResolve::Invoke<void*>(
            "il2cpp_class_get_parent", current);
    }
}

std::int32_t FindNamedFieldOffset(
    UnityResolve::Class* klass,
    std::initializer_list<const char*> names,
    std::initializer_list<const char*> typeNames) noexcept {
    if (klass == nullptr) {
        return -1;
    }
    for (const char* name : names) {
        const auto* field = klass->Get<UnityResolve::Field>(name);
        if (field == nullptr || field->static_field || field->offset < 0 ||
            field->type == nullptr) {
            continue;
        }
        if (typeNames.size() == 0U) {
            return field->offset;
        }
        for (const char* typeName : typeNames) {
            if (field->type->name == typeName) {
                return field->offset;
            }
        }
    }
    return FindFieldOffsetOnIl2CppClass(
        klass->address, names, typeNames);
}

std::int32_t FindInstanceBoolOffset(
    UnityResolve::Class* klass,
    std::initializer_list<const char*> names) noexcept {
    if (klass == nullptr) {
        return -1;
    }
    for (const char* name : names) {
        const auto* field = klass->Get<UnityResolve::Field>(name);
        if (field == nullptr || field->static_field || field->offset < 0 ||
            field->type == nullptr) {
            continue;
        }
        if (field->type->name == "System.Boolean" ||
            field->type->name == "Boolean") {
            return field->offset;
        }
    }
    return -1;
}

std::int32_t FindClassFieldOffset(
    UnityResolve::Class* klass,
    std::initializer_list<const char*> names) noexcept {
    if (klass == nullptr) {
        return -1;
    }
    for (const char* name : names) {
        const auto* field = klass->Get<UnityResolve::Field>(name);
        if (field != nullptr && !field->static_field && field->offset >= 0) {
            return field->offset;
        }
    }
    return -1;
}

float PhysicalVerticalFovDegrees(
    float sensorHeightMillimeters,
    float focalLengthMillimeters) noexcept {
    if (!std::isfinite(sensorHeightMillimeters) ||
        !std::isfinite(focalLengthMillimeters) ||
        sensorHeightMillimeters <= 0.0F || focalLengthMillimeters <= 0.0F) {
        return 0.0F;
    }
    constexpr float kPi = 3.14159265358979323846F;
    return 2.0F * std::atan(sensorHeightMillimeters * 0.5F /
                            focalLengthMillimeters) *
        (180.0F / kPi);
}

constexpr float kModeBodyVerticalFovDegrees = 29.9F;

float BloomScreenScale(float sourceFovDegrees, float eyeFovDegrees) noexcept {
    if (!std::isfinite(sourceFovDegrees) || !std::isfinite(eyeFovDegrees) ||
        sourceFovDegrees <= 1.0F || eyeFovDegrees <= 1.0F) {
        return 0.3F;
    }
    return std::min(1.0F, std::max(0.08F, sourceFovDegrees / eyeFovDegrees));
}

float ModeBodyScreenScale(float eyeFovDegrees) noexcept {
    return BloomScreenScale(kModeBodyVerticalFovDegrees, eyeFovDegrees);
}

// Campus/Actor inverted-hull width lives in _OutlineParam.xy
// (min/max object-space size). Actor materials do not serialize
// that property (HasProperty=0, GetVector=0); the pre-fix hull
// reads the Campus/VL global, which starts at this official
// vector. Menu 0 = hide, 1 = this vector (pre-fix fat), default
// ≈ mode-body 29.9/100.24. Leave zw (distance falloff) alone.
const UnityVector4 kOfficialOutlineParam{
    0.05F, 5.0F, 0.011111F, 4.987752F};

void ScaleOutlineParamWidth(UnityVector4* value, float screenScale) noexcept {
    if (value == nullptr || !std::isfinite(screenScale)) {
        return;
    }
    const float widthScale = std::clamp(screenScale, 0.0F, 1.0F);
    if (std::isfinite(value->x)) {
        value->x *= widthScale;
    }
    if (std::isfinite(value->y)) {
        value->y *= widthScale;
    }
}

bool ReadVector4Seh(void* vector4, UnityVector4* out) noexcept {
    if (vector4 == nullptr || out == nullptr) {
        return false;
    }
    __try {
        *out = *static_cast<const UnityVector4*>(vector4);
        return std::isfinite(out->x) && std::isfinite(out->y);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteVector4Seh(void* vector4, const UnityVector4& value) noexcept {
    if (vector4 == nullptr) {
        return false;
    }
    __try {
        *static_cast<UnityVector4*>(vector4) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

UnityResolve::Method* FindMethodByName(
    UnityResolve::Class* klass,
    std::string_view name,
    bool requireStatic,
    std::size_t minArgs) noexcept {
    if (klass == nullptr) {
        return nullptr;
    }
    for (auto* method : klass->methods) {
        if (method != nullptr && method->function != nullptr &&
            method->name == name &&
            method->static_function == requireStatic &&
            method->args.size() >= minArgs) {
            return method;
        }
    }
    return nullptr;
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
                method->static_function || method->name != name ||
                method->args.size() != expectedArgs ||
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

void* ReadVolumeParameter(void* owner, std::int32_t fieldOffset) noexcept {
    void* parameter = nullptr;
    if (owner == nullptr || fieldOffset < 0 ||
        !ReadManagedField(owner, fieldOffset, &parameter)) {
        return nullptr;
    }
    return parameter;
}

std::int32_t ResolveVolumeField(
    void* parameter,
    std::initializer_list<const char*> names,
    std::initializer_list<const char*> typeNames) noexcept {
    if (parameter == nullptr) {
        return -1;
    }
    void* klass = nullptr;
    if (!ReadManagedField(parameter, 0, &klass) || klass == nullptr) {
        return -1;
    }
    return FindFieldOffsetOnIl2CppClass(klass, names, typeNames);
}

void EnsureVolumeParameterOffsets(
    std::int32_t* floatValueOffset,
    std::int32_t* intValueOffset,
    std::int32_t* overrideOffset,
    void* parameter) noexcept {
    if (parameter == nullptr) {
        return;
    }
    if (floatValueOffset != nullptr && *floatValueOffset < 0) {
        *floatValueOffset = ResolveVolumeField(
            parameter, {"m_Value", "value"}, {"System.Single", "Single"});
    }
    if (intValueOffset != nullptr && *intValueOffset < 0) {
        *intValueOffset = ResolveVolumeField(
            parameter, {"m_Value", "value"}, {"System.Int32", "Int32"});
    }
    if (overrideOffset != nullptr && *overrideOffset < 0) {
        *overrideOffset = ResolveVolumeField(
            parameter, {"m_OverrideState", "overrideState"},
            {"System.Boolean", "Boolean"});
    }
}

bool ClearVolumeOverride(
    void* parameter,
    std::int32_t overrideOffset) noexcept {
    if (parameter == nullptr || overrideOffset < 0) {
        return false;
    }
    return WriteManagedField(parameter, overrideOffset, false);
}

bool ReadVolumeFloat(
    void* parameter,
    std::int32_t floatValueOffset,
    float* value) noexcept {
    if (parameter == nullptr || floatValueOffset < 0 || value == nullptr) {
        return false;
    }
    return ReadManagedField(parameter, floatValueOffset, value);
}

bool WriteVolumeFloat(
    void* parameter,
    std::int32_t floatValueOffset,
    std::int32_t overrideOffset,
    float value) noexcept {
    if (parameter == nullptr || floatValueOffset < 0) {
        return false;
    }
    if (overrideOffset >= 0) {
        WriteManagedField(parameter, overrideOffset, true);
    }
    if (!WriteManagedField(parameter, floatValueOffset, value)) {
        return false;
    }
    float after = -1.0F;
    return ReadManagedField(parameter, floatValueOffset, &after) &&
        std::abs(after - value) <= 0.0001F;
}

bool ReadVolumeInt(
    void* parameter,
    std::int32_t intValueOffset,
    std::int32_t* value) noexcept {
    if (parameter == nullptr || intValueOffset < 0 || value == nullptr) {
        return false;
    }
    return ReadManagedField(parameter, intValueOffset, value);
}

bool WriteVolumeInt(
    void* parameter,
    std::int32_t intValueOffset,
    std::int32_t overrideOffset,
    std::int32_t value) noexcept {
    if (parameter == nullptr || intValueOffset < 0) {
        return false;
    }
    if (overrideOffset >= 0) {
        WriteManagedField(parameter, overrideOffset, true);
    }
    return WriteManagedField(parameter, intValueOffset, value);
}

bool IsValidTaaSettings(const UnityTaaSettingsPrefix& value) noexcept {
    const auto finiteAndBounded = [](float number) noexcept {
        return std::isfinite(number) && std::abs(number) < 100.0F;
    };
    return value.quality >= 0 && value.quality <= 4 &&
        finiteAndBounded(value.frameInfluence) &&
        finiteAndBounded(value.jitterScale) &&
        finiteAndBounded(value.mipBias) &&
        finiteAndBounded(value.varianceClampScale) &&
        finiteAndBounded(value.contrastAdaptiveSharpening);
}

// URP UniversalAdditionalCameraData.AntialiasingMode values.
constexpr int kAntialiasingNone = 0;
constexpr int kAntialiasingSmaa = 2;
constexpr int kAntialiasingTaa = 3;

struct EyeAaResolve {
    int mode = 0;
    int antialiasing = 0;
    int antialiasingQuality = 0;
    UnityTaaSettingsPrefix taa{};
    bool writeTaa = false;
    bool overrideApplied = false;
};

const char* EyeAaModeName(int mode) noexcept {
    switch (mode) {
    case 1:
        return "taa";
    case 2:
        return "smaa";
    case 3:
        return "none";
    case 4:
        return "smaa-t2x";
    case 5:
        return "tscmaa";
    default:
        return "inherit";
    }
}

EyeAaResolve ResolveEyeAa(
    int sourceAntialiasing,
    int sourceAntialiasingQuality,
    const UnityTaaSettingsPrefix& sourceTaa,
    bool sourceTaaValid,
    bool nativeTemporalAaActive) noexcept {
    GakumasLocal::Config::ClampVrEyeAaSettings();
    EyeAaResolve resolved;
    resolved.mode = GakumasLocal::Config::vrEyeAaMode;
    resolved.antialiasing = sourceAntialiasing;
    resolved.antialiasingQuality = sourceAntialiasingQuality;
    if (sourceTaaValid) {
        resolved.taa = sourceTaa;
        resolved.writeTaa = sourceAntialiasing == kAntialiasingTaa;
    }
    if (resolved.mode == 1) {
        resolved.antialiasing = kAntialiasingTaa;
        resolved.taa.quality = GakumasLocal::Config::vrEyeTaaQuality;
        resolved.taa.frameInfluence =
            GakumasLocal::Config::vrEyeTaaFrameInfluence;
        resolved.taa.jitterScale = GakumasLocal::Config::vrEyeTaaJitterScale;
        resolved.taa.mipBias = GakumasLocal::Config::vrEyeTaaMipBias;
        resolved.taa.varianceClampScale =
            GakumasLocal::Config::vrEyeTaaVarianceClamp;
        resolved.taa.contrastAdaptiveSharpening =
            GakumasLocal::Config::vrEyeTaaSharpen;
        resolved.writeTaa = IsValidTaaSettings(resolved.taa);
        resolved.overrideApplied = true;
    } else if (resolved.mode == 2) {
        resolved.antialiasing = kAntialiasingSmaa;
        resolved.antialiasingQuality = GakumasLocal::Config::vrEyeSmaaQuality;
        resolved.writeTaa = false;
        resolved.overrideApplied = true;
    } else if (resolved.mode == 3) {
        resolved.antialiasing = kAntialiasingNone;
        resolved.writeTaa = false;
        resolved.overrideApplied = true;
    } else if (resolved.mode == 4) {
        // The first pair is an unjittered URP-SMAA warmup. Once native
        // resources and both per-eye MV snapshots are ready, the official
        // spatial SMAA pass runs after URP and URP AA must be disabled.
        resolved.antialiasing = nativeTemporalAaActive
            ? kAntialiasingNone
            : kAntialiasingSmaa;
        resolved.antialiasingQuality = GakumasLocal::Config::vrEyeSmaaQuality;
        resolved.writeTaa = false;
        resolved.overrideApplied = true;
    } else if (resolved.mode == 5) {
        // TSCMAA owns both the spatial CMAA2 pass and temporal resolve once
        // its per-eye motion snapshots are ready. Until then, publish an
        // ordinary URP-SMAA pair so a cold or rebuilt history is never exposed.
        resolved.antialiasing = nativeTemporalAaActive
            ? kAntialiasingNone
            : kAntialiasingSmaa;
        resolved.antialiasingQuality = GakumasLocal::Config::vrEyeSmaaQuality;
        resolved.writeTaa = false;
        resolved.overrideApplied = true;
    }
    return resolved;
}

bool IsFinitePose(const pose::Pose& value) noexcept {
    const float normSquared =
        value.orientation.x * value.orientation.x +
        value.orientation.y * value.orientation.y +
        value.orientation.z * value.orientation.z +
        value.orientation.w * value.orientation.w;
    return std::isfinite(value.position.x) && std::isfinite(value.position.y) &&
        std::isfinite(value.position.z) && std::isfinite(value.orientation.x) &&
        std::isfinite(value.orientation.y) && std::isfinite(value.orientation.z) &&
        std::isfinite(value.orientation.w) && std::isfinite(normSquared) &&
        normSquared > 0.25F && normSquared < 2.25F;
}

bool IsFiniteFov(const pose::EyeFov& fov) noexcept {
    return std::isfinite(fov.angleLeft) && std::isfinite(fov.angleRight) &&
        std::isfinite(fov.angleUp) && std::isfinite(fov.angleDown) &&
        fov.angleLeft < 0.0F && fov.angleRight > 0.0F &&
        fov.angleDown < 0.0F && fov.angleUp > 0.0F &&
        fov.angleRight - fov.angleLeft < 3.13F &&
        fov.angleUp - fov.angleDown < 3.13F;
}

void* ReadUnityNativePointer(void* managedObject) noexcept {
    if (managedObject == nullptr) {
        return nullptr;
    }
    __try {
        return reinterpret_cast<UnityResolve::UnityType::UnityObject*>(
            managedObject)->m_CachedPtr;
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

bool QueryD3D11Texture(void* nativePointer, ID3D11Texture2D** texture) noexcept {
    if (nativePointer == nullptr || texture == nullptr) {
        return false;
    }
    *texture = nullptr;
    return SUCCEEDED(reinterpret_cast<IUnknown*>(nativePointer)->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture)));
}

bool ReadInt32Field(void* object, std::int32_t offset, std::int32_t* value) noexcept {
    return ReadManagedField(object, offset, value);
}

UnityStereoRenderer::MethodRef MethodReference(UnityResolve::Method* method) noexcept {
    if (method == nullptr) {
        return {};
    }
    return {method->function, method->address};
}

std::string MethodSignature(const UnityResolve::Method* method) {
    if (method == nullptr) {
        return "<null>";
    }
    std::ostringstream stream;
    stream << (method->static_function ? "static " : "instance ");
    stream << (method->return_type != nullptr
        ? method->return_type->name
        : "<missing-return-type>");
    stream << ' ';
    if (method->klass != nullptr) {
        if (!method->klass->namespaze.empty()) {
            stream << method->klass->namespaze << '.';
        }
        stream << method->klass->name << '.';
    }
    stream << method->name << '(';
    for (std::size_t index = 0; index < method->args.size(); ++index) {
        if (index != 0U) {
            stream << ", ";
        }
        const auto* argument = method->args[index];
        stream << (argument != nullptr && argument->pType != nullptr
            ? argument->pType->name
            : "<missing-argument-type>");
    }
    stream << ')';
    return stream.str();
}

bool MethodArgTypeContains(
    const UnityResolve::Method* method,
    std::size_t index,
    std::string_view needle) noexcept {
    if (method == nullptr || index >= method->args.size() ||
        method->args[index] == nullptr ||
        method->args[index]->pType == nullptr) {
        return false;
    }
    return method->args[index]->pType->name.find(needle) != std::string::npos;
}

void DumpSetupCameraCandidates(
    UnityResolve::Class* klass, const char* typeName) noexcept {
    if (klass == nullptr || typeName == nullptr) {
        return;
    }
    std::uint32_t logged = 0;
    for (auto* method : klass->methods) {
        if (method == nullptr ||
            method->name.find("SetupCamera") == std::string::npos) {
            continue;
        }
        if (logged >= 16U) {
            break;
        }
        ++logged;
        WriteVrLog(
            std::string("[VR][shadow] MATCAP_SETUP_CANDIDATE type=") +
            typeName + ' ' + MethodSignature(method) +
            " fn=" + (method->function != nullptr ? "1" : "0") +
            " info=" + (method->address != nullptr ? "1" : "0"));
    }
    if (logged == 0U) {
        WriteVrLog(
            std::string("[VR][shadow] MATCAP_SETUP_CANDIDATE type=") +
            typeName + " none");
    }
}

UnityResolve::Method* PickSetupCameraProperties(
    UnityResolve::Class* klass,
    bool* isStaticOut,
    bool* hasStereoOut,
    bool* hasEyeOut) noexcept {
    if (isStaticOut != nullptr) {
        *isStaticOut = false;
    }
    if (hasStereoOut != nullptr) {
        *hasStereoOut = false;
    }
    if (hasEyeOut != nullptr) {
        *hasEyeOut = false;
    }
    if (klass == nullptr) {
        return nullptr;
    }
    UnityResolve::Method* best = nullptr;
    int bestScore = -1;
    bool bestStatic = false;
    bool bestStereo = false;
    bool bestEye = false;
    for (auto* method : klass->methods) {
        if (method == nullptr || method->address == nullptr ||
            (method->name != "SetupCameraProperties" &&
             method->name != "SetupCameraProperties_Internal")) {
            continue;
        }
        std::size_t index = 0;
        const bool isStatic = method->static_function;
        if (isStatic && MethodArgTypeContains(method, 0, "IntPtr")) {
            index = 1;
        }
        if (!MethodArgTypeContains(method, index, "Camera")) {
            continue;
        }
        ++index;
        bool hasStereo = false;
        bool hasEye = false;
        if (index < method->args.size() &&
            MethodArgTypeContains(method, index, "Boolean")) {
            hasStereo = true;
            ++index;
        }
        if (index < method->args.size() &&
            MethodArgTypeContains(method, index, "Int32")) {
            hasEye = true;
            ++index;
        }
        if (index != method->args.size()) {
            continue;
        }
        int score = 0;
        if (method->function != nullptr) {
            score += 8;
        }
        if (method->name == "SetupCameraProperties") {
            score += 4;
        }
        if (hasStereo) {
            score += 2;
        }
        if (hasEye) {
            score += 1;
        }
        if (score > bestScore) {
            bestScore = score;
            best = method;
            bestStatic = isStatic;
            bestStereo = hasStereo;
            bestEye = hasEye;
        }
    }
    if (best != nullptr) {
        if (isStaticOut != nullptr) {
            *isStaticOut = bestStatic;
        }
        if (hasStereoOut != nullptr) {
            *hasStereoOut = bestStereo;
        }
        if (hasEyeOut != nullptr) {
            *hasEyeOut = bestEye;
        }
    }
    return best;
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

struct StrictMethodShape {
    bool staticFunction;
    std::string_view returnType;
    std::initializer_list<std::string_view> argumentTypes;
};

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
        WriteVrLog(
            "[VR][stereo] QUEUE_LADDER_API_REJECTED target=class-unavailable");
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
    std::ostringstream stream;
    stream << "[VR][stereo] QUEUE_LADDER_API_REJECTED target="
           << namespaceName << '.' << className << '.' << methodName
           << " exactMatches=" << exactMatches.size();
    WriteVrLog(stream.str());
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
    auto* klass = Il2cppUtils::GetClass(
        std::string(assemblyName),
        std::string(namespaceName),
        std::string(className));
    if (klass == nullptr) {
        WriteVrLog(
            "[VR][stereo] NORMAL_CAMERA_DIAGNOSTIC_API_REJECTED target=\"" +
            std::string(namespaceName) + '.' + std::string(className) + '.' +
            std::string(methodName) + "\" reason=class-unavailable");
        return nullptr;
    }

    std::vector<UnityResolve::Method*> candidates;
    std::vector<UnityResolve::Method*> exactMatches;
    for (auto* candidate : klass->methods) {
        if (candidate == nullptr || candidate->name != methodName) {
            continue;
        }
        candidates.push_back(candidate);
        if (HasExactSignature(
                candidate,
                expectedStatic,
                expectedReturnType,
                expectedArgumentTypes)) {
            exactMatches.push_back(candidate);
        }
    }
    if (exactMatches.size() == 1U && exactMatches.front()->address != nullptr &&
        exactMatches.front()->function != nullptr) {
        return exactMatches.front();
    }

    std::ostringstream failure;
    failure << "[VR][stereo] NORMAL_CAMERA_DIAGNOSTIC_API_REJECTED target=\""
            << namespaceName << '.' << className << '.' << methodName
            << "\" exactMatches=" << exactMatches.size()
            << " candidates=" << candidates.size();
    WriteVrLog(failure.str());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        std::ostringstream diagnostic;
        diagnostic << "[VR][stereo] NORMAL_CAMERA_DIAGNOSTIC_API_CANDIDATE target=\""
                   << namespaceName << '.' << className << '.' << methodName
                   << "\" index=" << index << " signature=\""
                   << MethodSignature(candidates[index]) << "\"";
        WriteVrLog(diagnostic.str());
    }
    return nullptr;
}

// UnityResolve appends interface methods to the requested class method list.
// Descriptor access is diagnostic-only: use it only when exactly one callable
// ABI match exists, and skip it entirely rather than guessing on duplicates.
UnityResolve::Method* ResolveDiagnosticMethod(
    std::string_view assemblyName,
    std::string_view namespaceName,
    std::string_view className,
    std::string_view methodName,
    bool expectedStatic,
    std::string_view expectedReturnType,
    std::initializer_list<std::string_view> expectedArgumentTypes) {
    auto* klass = Il2cppUtils::GetClass(
        std::string(assemblyName),
        std::string(namespaceName),
        std::string(className));
    if (klass == nullptr) {
        return nullptr;
    }
    std::vector<UnityResolve::Method*> exactMatches;
    for (auto* candidate : klass->methods) {
        if (candidate == nullptr || candidate->name != methodName ||
            !HasExactSignature(candidate, expectedStatic, expectedReturnType,
                               expectedArgumentTypes) ||
            candidate->address == nullptr || candidate->function == nullptr) {
            continue;
        }
        exactMatches.push_back(candidate);
    }
    UnityResolve::Method* selected = exactMatches.size() == 1U
        ? exactMatches.front()
        : nullptr;
    std::ostringstream state;
    state << "[VR][stereo] DIAGNOSTIC_API_"
          << (selected != nullptr ? "SELECTED" : "UNAVAILABLE")
          << " target=\"" << namespaceName << '.' << className << '.'
          << methodName << "\" exactMatches=" << exactMatches.size();
    if (exactMatches.size() > 1U) {
        state << " duplicateSkipped=1";
    }
    WriteVrLog(state.str());
    return selected;
}

constexpr DWORD kMicrosoftCppExceptionCode = 0xE06D7363UL;

int ManagedCallExceptionFilter(DWORD code) noexcept {
    (void)code;
    // Diagnostic builds intentionally preserve the original first-fault stack.
    // VrLog is flushed before every risky call, so Windows/Unity crash handling
    // can capture the exact native context instead of resuming corrupted state.
    return EXCEPTION_CONTINUE_SEARCH;
}

template <typename Function, typename... Arguments>
bool InvokeManagedVoidSeh(
    const UnityStereoRenderer::MethodRef& method,
    Arguments... arguments) {
    if (!method.Ready()) {
        return false;
    }
    __try {
        reinterpret_cast<Function>(method.function)(arguments..., method.methodInfo);
        return true;
    } __except (ManagedCallExceptionFilter(GetExceptionCode())) {
        return false;
    }
}

template <typename Return, typename Function, typename... Arguments>
bool InvokeManagedResultSeh(
    const UnityStereoRenderer::MethodRef& method,
    Return* result,
    Arguments... arguments) {
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
    const UnityStereoRenderer::MethodRef& method,
    Arguments... arguments) noexcept {
    try {
        return InvokeManagedVoidSeh<Function>(method, arguments...);
    } catch (...) {
        return false;
    }
}

template <typename Return, typename Function, typename... Arguments>
bool InvokeManagedResult(
    const UnityStereoRenderer::MethodRef& method,
    Return* result,
    Arguments... arguments) noexcept {
    try {
        return InvokeManagedResultSeh<Return, Function>(
            method, result, arguments...);
    } catch (...) {
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

bool TryFormatManagedExceptionRaw(
    void* exception,
    char* buffer,
    int bufferSize) noexcept {
    if (exception == nullptr || buffer == nullptr || bufferSize <= 0) {
        return false;
    }
    using FormatException = void (*)(void*, char*, int);
    static const auto formatException = reinterpret_cast<FormatException>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_format_exception"));
    if (formatException == nullptr) {
        return false;
    }
    __try {
        formatException(exception, buffer, bufferSize);
        return true;
    } __except (ManagedCallExceptionFilter(GetExceptionCode())) {
        return false;
    }
}

bool RuntimeInvokeRaw(
    void* methodInfo,
    void* instance,
    void** arguments,
    void** result,
    void** exception) noexcept {
    using Invoke = void* (*)(void*, void*, void**, void**);
    static const auto invoke = reinterpret_cast<Invoke>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_runtime_invoke"));
    if (invoke == nullptr || methodInfo == nullptr || result == nullptr ||
        exception == nullptr) {
        return false;
    }
    __try {
        *result = invoke(methodInfo, instance, arguments, exception);
        return true;
    } __except (ManagedCallExceptionFilter(GetExceptionCode())) {
        return false;
    }
}

bool RuntimeInvoke(
    const UnityStereoRenderer::MethodRef& method,
    void* instance,
    void** arguments,
    void** result,
    std::string* exceptionText) noexcept {
    if (result != nullptr) {
        *result = nullptr;
    }
    if (exceptionText != nullptr) {
        exceptionText->clear();
    }
    if (method.methodInfo == nullptr) {
        return false;
    }
    void* exception = nullptr;
    void* invocationResult = nullptr;
    if (!RuntimeInvokeRaw(
            method.methodInfo,
            instance,
            arguments,
            &invocationResult,
            &exception)) {
        if (exceptionText != nullptr) {
            *exceptionText = "<native fault in il2cpp_runtime_invoke>";
        }
        return false;
    }
    if (exception != nullptr) {
        if (exceptionText != nullptr) {
            char buffer[1024]{};
            *exceptionText = TryFormatManagedExceptionRaw(
                exception, buffer, static_cast<int>(sizeof(buffer))) &&
                    buffer[0] != '\0'
                ? std::string(buffer)
                : "<managed exception>";
        }
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
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_object_unbox"));
    if (unbox == nullptr) {
        return nullptr;
    }
    __try {
        return unbox(boxedValue);
    } __except (ManagedCallExceptionFilter(GetExceptionCode())) {
        return nullptr;
    }
}

int ClassValueSize(void* klass) noexcept {
    if (klass == nullptr) {
        return -1;
    }
    using GetValueSize = int (*)(void*, std::uint32_t*);
    static const auto getValueSize = reinterpret_cast<GetValueSize>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"),
                       "il2cpp_class_value_size"));
    if (getValueSize == nullptr) {
        return -1;
    }
    std::uint32_t alignment = 0;
    __try {
        return getValueSize(klass, &alignment);
    } __except (ManagedCallExceptionFilter(GetExceptionCode())) {
        return -1;
    }
}

bool ReadRenderTextureDescriptor(
    const UnityStereoRenderer::MethodRef& getter,
    void* instance,
    int runtimeValueSize,
    UnityRenderTextureDescriptorValue* descriptor,
    std::string* exceptionText) noexcept {
    if (instance == nullptr || descriptor == nullptr ||
        runtimeValueSize != static_cast<int>(sizeof(*descriptor))) {
        return false;
    }
    void* boxedDescriptor = nullptr;
    if (!RuntimeInvoke(getter, instance, nullptr, &boxedDescriptor,
                       exceptionText)) {
        return false;
    }
    void* value = UnboxObject(boxedDescriptor);
    if (value == nullptr) {
        return false;
    }
    __try {
        std::memcpy(descriptor, value, sizeof(*descriptor));
        return true;
    } __except (ManagedCallExceptionFilter(GetExceptionCode())) {
        return false;
    }
}

std::string DescriptorState(
    const UnityRenderTextureDescriptorValue& descriptor) {
    std::ostringstream stream;
    stream << descriptor.width << 'x' << descriptor.height
           << " msaa=" << descriptor.msaaSamples
           << " volumeDepth=" << descriptor.volumeDepth
           << " mipCount=" << descriptor.mipCount
           << " graphicsFormat=" << descriptor.graphicsFormat
           << " stencilFormat=" << descriptor.stencilFormat
           << " depthStencilFormat=" << descriptor.depthStencilFormat
           << " dimension=" << descriptor.dimension
           << " shadowSampling=" << descriptor.shadowSamplingMode
           << " vrUsage=" << descriptor.vrUsage
           << " flags=0x" << std::hex << descriptor.flags << std::dec
           << " memoryless=" << descriptor.memoryless;
    return stream.str();
}

bool UnboxBoolean(void* boxedValue, bool* value) noexcept {
    if (boxedValue == nullptr || value == nullptr) {
        return false;
    }
    __try {
        void* unboxed = UnboxObject(boxedValue);
        if (unboxed == nullptr) {
            return false;
        }
        *value = *static_cast<const bool*>(unboxed);
        return true;
    } __except (ManagedCallExceptionFilter(GetExceptionCode())) {
        return false;
    }
}

} // namespace

bool UnityStereoRenderer::EnsureManagedApi() noexcept {
    if (apiInitialized_ || stage_ == LadderStage::Failed) {
        return apiInitialized_;
    }

    Log("[VR][stereo] CALL_BEGIN stage=api.resolve");
    auto* gameObjectClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject");
    auto* renderTextureClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture");
    auto* renderTextureDescriptorClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTextureDescriptor");
    auto* cameraClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
    auto* universalCameraDataClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData");
    auto* vlAdditionalCameraDataClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "VL.Rendering", "VLSRPAdditionalCameraData");
    auto* vlCameraControllerClass = Il2cppUtils::GetClass(
        "vl-unity.Runtime.dll", "VL.Rendering", "VLSRPCameraController");
    if (gameObjectClass == nullptr || renderTextureClass == nullptr ||
        renderTextureDescriptorClass == nullptr ||
        cameraClass == nullptr || universalCameraDataClass == nullptr ||
        vlAdditionalCameraDataClass == nullptr || vlCameraControllerClass == nullptr) {
        return FailStage("api.class-metadata");
    }

    auto* internalCreateGameObject = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "Internal_CreateGameObject",
        true,
        "System.Void",
        {"UnityEngine.GameObject", "System.String"});
    auto* gameObjectSetActive = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "SetActive",
        false,
        "System.Void",
        {"System.Boolean"});
    auto* gameObjectGetLayer = ResolveDiagnosticMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "get_layer", false, "System.Int32", {});
    auto* gameObjectGetActiveSelf = ResolveDiagnosticMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "get_activeSelf", false, "System.Boolean", {});
    auto* gameObjectSetTag = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "set_tag",
        false,
        "System.Void",
        {"System.String"});
    auto* gameObjectCompareTag = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "CompareTag",
        false,
        "System.Boolean",
        {"System.String"});
    auto* componentGetGameObject = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Component",
        "get_gameObject",
        false,
        "UnityEngine.GameObject",
        {});
    auto* addComponent = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "AddComponent",
        false,
        "UnityEngine.Component",
        {"System.Type"});
    auto* dontDestroyOnLoad = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Object",
        "DontDestroyOnLoad",
        true,
        "System.Void",
        {"UnityEngine.Object"});
    auto* cameraCopyFrom = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "CopyFrom",
        false,
        "System.Void",
        {"UnityEngine.Camera"});
    auto* cameraSetTargetTexture = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_targetTexture",
        false,
        "System.Void",
        {"UnityEngine.RenderTexture"});
    auto* cameraGetTargetTexture = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_targetTexture", false, "UnityEngine.RenderTexture", {});
    auto* cameraSetCullingMask = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_cullingMask",
        false,
        "System.Void",
        {"System.Int32"});
    auto* cameraGetCullingMask = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_cullingMask",
        false,
        "System.Int32",
        {});
    auto* cameraResetWorldToCameraMatrix = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "ResetWorldToCameraMatrix",
        false,
        "System.Void",
        {});
    auto* cameraSetNonJitteredProjectionMatrixInjected = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_nonJitteredProjectionMatrix_Injected",
        {
            {false, "System.Void", {"UnityEngine.Matrix4x4&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Matrix4x4&"}},
        });
    if (cameraSetNonJitteredProjectionMatrixInjected == nullptr) {
        cameraSetNonJitteredProjectionMatrixInjected = ResolveStrictMethodAny(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
            "set_nonJitteredProjectionMatrix",
            {
                {false, "System.Void", {"UnityEngine.Matrix4x4"}},
                {false, "System.Void", {"UnityEngine.Matrix4x4&"}},
                {true, "System.Void",
                 {"System.IntPtr", "UnityEngine.Matrix4x4&"}},
            });
    }
    UnityResolve::Method* cameraGetProjectionMatrixInjected = nullptr;
    UnityResolve::Method* cameraGetNonJitteredProjectionMatrixInjected = nullptr;
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        cameraGetProjectionMatrixInjected = ResolveStrictMethodAny(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
            "get_projectionMatrix_Injected",
            {
                {false, "System.Void", {"UnityEngine.Matrix4x4&"}},
                {true, "System.Void",
                 {"System.IntPtr", "UnityEngine.Matrix4x4&"}},
            });
        cameraGetNonJitteredProjectionMatrixInjected = ResolveStrictMethodAny(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
            "get_nonJitteredProjectionMatrix_Injected",
            {
                {false, "System.Void", {"UnityEngine.Matrix4x4&"}},
                {true, "System.Void",
                 {"System.IntPtr", "UnityEngine.Matrix4x4&"}},
            });
    }
    auto* cameraGetDepth = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_depth", false, "System.Single", {});
    auto* cameraSetDepth = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_depth", false, "System.Void", {"System.Single"});
    auto* cameraGetNearClipPlane = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_nearClipPlane", false, "System.Single", {});
    auto* cameraSetNearClipPlane = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_nearClipPlane", false, "System.Void", {"System.Single"});
    auto* cameraGetFarClipPlane = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_farClipPlane", false, "System.Single", {});
    auto* cameraSetFarClipPlane = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_farClipPlane", false, "System.Void", {"System.Single"});
    auto* cameraGetAllowHdr = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_allowHDR", false, "System.Boolean", {});
    auto* cameraGetAllowMsaa = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_allowMSAA", false, "System.Boolean", {});
    auto* cameraGetDepthTextureMode = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_depthTextureMode", false, "UnityEngine.DepthTextureMode", {});
    auto* cameraSetDepthTextureMode = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_depthTextureMode", false, "System.Void",
        {"UnityEngine.DepthTextureMode"});
    auto* cameraGetFieldOfView = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_fieldOfView", false, "System.Single", {});
    auto* cameraSetFieldOfView = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_fieldOfView", false, "System.Void", {"System.Single"});
    auto* cameraGetAspect = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_aspect", false, "System.Single", {});
    auto* cameraSetAspect = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_aspect", false, "System.Void", {"System.Single"});
    auto* cameraGetUsePhysicalProperties = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_usePhysicalProperties", false, "System.Boolean", {});
    auto* cameraGetApertureInjected = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_aperture_Injected", true, "System.Single", {"System.IntPtr"});
    auto* cameraGetFocusDistanceInjected = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_focusDistance_Injected", true, "System.Single", {"System.IntPtr"});
    auto* cameraGetFocalLengthInjected = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_focalLength_Injected", true, "System.Single", {"System.IntPtr"});
    auto* cameraSetFocalLengthInjected = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_focalLength_Injected", true, "System.Void",
        {"System.IntPtr", "System.Single"});
    auto* cameraGetSensorSizeInjected = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_sensorSize_Injected", true, "System.Void",
        {"System.IntPtr", "UnityEngine.Vector2&"});
    auto* cameraSetSensorSizeInjected = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_sensorSize_Injected", true, "System.Void",
        {"System.IntPtr", "UnityEngine.Vector2&"});
    auto* cameraGetGateFittedFieldOfViewInjected = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "GetGateFittedFieldOfView_Injected", true, "System.Single",
        {"System.IntPtr"});
    auto* cameraGetGateFittedLensShiftInjected = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "GetGateFittedLensShift_Injected", true, "System.Void",
        {"System.IntPtr", "UnityEngine.Vector2&"});
    auto* cameraGetLensShiftInjected = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_lensShift_Injected",
        {
            {false, "System.Void", {"UnityEngine.Vector2&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Vector2&"}},
        });
    auto* cameraSetLensShiftInjected = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_lensShift_Injected",
        {
            {false, "System.Void", {"UnityEngine.Vector2&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Vector2&"}},
        });
    auto* cameraSetProjectionMatrixInjected = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "set_projectionMatrix_Injected",
        {
            {false, "System.Void", {"UnityEngine.Matrix4x4&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Matrix4x4&"}},
        });
    auto* behaviourSetEnabled = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Behaviour",
        "set_enabled",
        false,
        "System.Void",
        {"System.Boolean"});
    auto* behaviourGetEnabled = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Behaviour",
        "get_enabled",
        false,
        "System.Boolean",
        {});
    auto* componentGetComponent = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Component",
        "GetComponent", false, "UnityEngine.Component", {"System.Type"});
    auto* componentGetTransform = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Component",
        "get_transform", false, "UnityEngine.Transform", {});
    auto* gameObjectGetTransform = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
        "get_transform", false, "UnityEngine.Transform", {});
    auto* transformGetParent = ResolveDiagnosticMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Transform",
        "get_parent", false, "UnityEngine.Transform", {});
    auto* transformSetPositionAndRotationInjected = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Transform",
        "SetPositionAndRotation_Injected",
        {
            {false, "System.Void", {"UnityEngine.Vector3&", "UnityEngine.Quaternion&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Vector3&", "UnityEngine.Quaternion&"}},
        });
    auto* transformGetPositionInjected = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Transform",
        "get_position_Injected",
        {
            {false, "System.Void", {"UnityEngine.Vector3&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Vector3&"}},
        });
    auto* transformGetRotationInjected = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Transform",
        "get_rotation_Injected",
        {
            {false, "System.Void", {"UnityEngine.Quaternion&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Quaternion&"}},
        });
    auto* matrixFrustumInjected = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Matrix4x4",
        "Frustum_Injected", true, "System.Void",
        {"System.Single", "System.Single", "System.Single", "System.Single",
         "System.Single", "System.Single", "UnityEngine.Matrix4x4&"});
    auto* renderTextureConstructor = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture",
        ".ctor",
        false,
        "System.Void",
        {"System.Int32", "System.Int32", "System.Int32",
         "UnityEngine.RenderTextureFormat"});
    auto* renderTextureDescriptorConstructor = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture",
        ".ctor", false, "System.Void",
        {"UnityEngine.RenderTextureDescriptor"});
    auto* renderTextureCreate = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture",
        "Create",
        false,
        "System.Boolean",
        {});
    auto* renderTextureRelease = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture",
        "Release",
        false,
        "System.Void",
        {});
    auto* renderTextureGetDescriptor = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture",
        "get_descriptor", false, "UnityEngine.RenderTextureDescriptor", {});
    auto* renderTextureGetSrgb = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture",
        "get_sRGB", false, "System.Boolean", {});
    auto* renderTextureGetGraphicsFormat = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture",
        "get_graphicsFormat", false,
        "UnityEngine.Experimental.Rendering.GraphicsFormat", {});
    auto* textureGetNativeTexturePtr = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Texture",
        "GetNativeTexturePtr", false, "System.IntPtr", {});
    auto* commandBufferKlass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
        "CommandBuffer");
    auto* commandBufferConstructor = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "CommandBuffer",
        ".ctor", false, "System.Void", {});
    auto* commandBufferClear = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "CommandBuffer",
        "Clear", false, "System.Void", {});
    // dump.cs: CommandBuffer exposes CopyTexture only through
    // RenderTargetIdentifier overloads; the (Texture,Texture) pair lives on
    // static Graphics, whose immediate queue would bypass the context order.
    auto* commandBufferCopyTexture = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine.Rendering", "CommandBuffer",
        "CopyTexture", false, "System.Void",
        {"UnityEngine.Rendering.RenderTargetIdentifier",
         "UnityEngine.Rendering.RenderTargetIdentifier"});
    auto* renderTargetIdentifierConstructor = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
        "RenderTargetIdentifier", ".ctor", false, "System.Void",
        {"UnityEngine.Texture"});
    auto* scriptableRenderContextExecuteCommandBuffer = ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
        "ScriptableRenderContext", "ExecuteCommandBuffer", false,
        "System.Void", {"UnityEngine.Rendering.CommandBuffer"});
    auto* universalSetRenderer = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "SetRenderer", false, "System.Void", {"System.Int32"});
    auto* universalGetRenderType = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_renderType", false,
        "UnityEngine.Rendering.Universal.CameraRenderType", {});
    auto* universalSetRenderType = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_renderType", false, "System.Void",
        {"UnityEngine.Rendering.Universal.CameraRenderType"});
    auto* universalGetRenderPostProcessing = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_renderPostProcessing", false, "System.Boolean", {});
    auto* universalSetRenderPostProcessing = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_renderPostProcessing", false, "System.Void", {"System.Boolean"});
    auto* universalGetAntialiasing = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_antialiasing", false,
        "UnityEngine.Rendering.Universal.AntialiasingMode", {});
    auto* universalSetAntialiasing = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_antialiasing", false, "System.Void",
        {"UnityEngine.Rendering.Universal.AntialiasingMode"});
    auto* universalGetAntialiasingQuality = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_antialiasingQuality", false,
        "UnityEngine.Rendering.Universal.AntialiasingQuality", {});
    auto* universalSetAntialiasingQuality = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_antialiasingQuality", false, "System.Void",
        {"UnityEngine.Rendering.Universal.AntialiasingQuality"});
    auto* universalGetRequiresDepthOption = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_requiresDepthOption", false,
        "UnityEngine.Rendering.Universal.CameraOverrideOption", {});
    auto* universalSetRequiresDepthOption = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_requiresDepthOption", false, "System.Void",
        {"UnityEngine.Rendering.Universal.CameraOverrideOption"});
    auto* universalGetRequiresColorOption = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_requiresColorOption", false,
        "UnityEngine.Rendering.Universal.CameraOverrideOption", {});
    auto* universalSetRequiresColorOption = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_requiresColorOption", false, "System.Void",
        {"UnityEngine.Rendering.Universal.CameraOverrideOption"});
    auto* universalGetRequiresDepthTexture = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_requiresDepthTexture", false, "System.Boolean", {});
    auto* universalGetRequiresColorTexture = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_requiresColorTexture", false, "System.Boolean", {});
    auto* universalGetVolumeLayerMask = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_volumeLayerMask", false, "UnityEngine.LayerMask", {});
    auto* universalSetVolumeLayerMask = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_volumeLayerMask", false, "System.Void", {"UnityEngine.LayerMask"});
    auto* universalGetVolumeTrigger = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_volumeTrigger", false, "UnityEngine.Transform", {});
    auto* universalSetVolumeTrigger = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_volumeTrigger", false, "System.Void", {"UnityEngine.Transform"});
    auto* universalGetVolumeFrameworkUpdateMode = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_volumeFrameworkUpdateMode", false,
        "UnityEngine.Rendering.Universal.VolumeFrameworkUpdateMode", {});
    auto* universalSetVolumeFrameworkUpdateMode = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_volumeFrameworkUpdateMode", false, "System.Void",
        {"UnityEngine.Rendering.Universal.VolumeFrameworkUpdateMode"});
    auto* universalGetRequiresVolumeFrameworkUpdate = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_requiresVolumeFrameworkUpdate", false, "System.Boolean", {});
    auto* universalGetVolumeStack = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_volumeStack", false, "UnityEngine.Rendering.VolumeStack", {});
    auto* universalGetOrCreateVolumeStack = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "GetOrCreateVolumeStack", false, "System.Void", {});
    auto* universalGetTaaPersistentData = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_taaPersistentData", false,
        "UnityEngine.Rendering.Universal.TaaPersistentData", {});
    auto* universalGetMotionVectorsPersistentData = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_motionVectorsPersistentData", false,
        "UnityEngine.Rendering.Universal.MotionVectorsPersistentData", {});
    auto* universalGetResetHistory = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_resetHistory", false, "System.Boolean", {});
    auto* cameraExtensionsUpdateVolumeStack = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "CameraExtensions",
        "UpdateVolumeStack", true, "System.Void", {"UnityEngine.Camera"});
    auto* universalGetStopNan = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_stopNaN", false, "System.Boolean", {});
    auto* universalSetStopNan = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_stopNaN", false, "System.Void", {"System.Boolean"});
    auto* universalGetDithering = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_dithering", false, "System.Boolean", {});
    auto* universalSetDithering = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_dithering", false, "System.Void", {"System.Boolean"});
    auto* universalGetAllowHdrOutput = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_allowHDROutput", false, "System.Boolean", {});
    auto* universalSetAllowHdrOutput = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_allowHDROutput", false, "System.Void", {"System.Boolean"});
    auto* universalSetResetHistory = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_resetHistory", false, "System.Void", {"System.Boolean"});
    auto* universalGetRenderShadows = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_renderShadows", false, "System.Boolean", {});
    auto* universalSetRenderShadows = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_renderShadows", false, "System.Void", {"System.Boolean"});
    auto* universalSetAllowXrRendering = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_allowXRRendering", false, "System.Void", {"System.Boolean"});
    auto* universalGetFastRendering = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_fastRendering", false, "System.Boolean", {});
    auto* universalSetFastRendering = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_fastRendering", false, "System.Void", {"System.Boolean"});
    auto* universalGetNeedsAlphaChannel = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "get_needsAlphaChannel", false, "System.Boolean", {});
    auto* universalSetNeedsAlphaChannel = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData",
        "set_needsAlphaChannel", false, "System.Void", {"System.Boolean"});
    auto* vlAdditionalSetTargetTexture = ResolveStrictMethod(
        "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering",
        "VLSRPAdditionalCameraData", "set_targetTexture", false,
        "System.Void", {"UnityEngine.RenderTexture"});
    auto* vlControllerGetTargetTextureDescriptor = ResolveDiagnosticMethod(
        "vl-unity.Runtime.dll", "VL.Rendering", "VLSRPCameraController",
        "get_targetTextureDescriptor", false,
        "UnityEngine.RenderTextureDescriptor", {});
    auto* vlControllerGetHdrTargetTextureDescriptor = ResolveDiagnosticMethod(
        "vl-unity.Runtime.dll", "VL.Rendering", "VLSRPCameraController",
        "get_hdrTargetTextureDescriptor", false,
        "UnityEngine.RenderTextureDescriptor", {});
    auto* volumeStackGetComponent = ResolveDiagnosticMethod(
        "Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
        "VolumeStack", "GetComponent", false,
        "UnityEngine.Rendering.VolumeComponent", {"System.Type"});
    auto* volumeComponentSetActive = ResolveDiagnosticMethod(
        "Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
        "VolumeComponent", "set_active", false, "System.Void",
        {"System.Boolean"});
    auto* vlDofClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering", "VLDOF");
    auto* urpDofClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "DepthOfField");
    auto* vlBloomClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering",
        "VLBloom");
    auto* urpBloomClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "Bloom");
    auto* chromaticAberrationClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "ChromaticAberration");
    auto* lensDistortionClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "LensDistortion");
    auto* motionBlurClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "MotionBlur");
    auto* vignetteClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "Vignette");
    auto* uiTextureOverlayClass = Il2cppUtils::GetClass(
        "campus-submodule.Runtime.dll", "Campus.Common",
        "UITextureOverlay");
    auto* liveCameraOverlayClass = Il2cppUtils::GetClass(
        "campus-submodule.Runtime.dll", "Campus.Common.Live",
        "CampusLiveCameraOverlay");
    auto* particleSystemClass = Il2cppUtils::GetClass(
        "UnityEngine.ParticleSystemModule.dll", "UnityEngine",
        "ParticleSystem");
    if (particleSystemClass == nullptr) {
        particleSystemClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "ParticleSystem");
    }
    auto* volumeComponentClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
        "VolumeComponent");
    auto* volumeParameterClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
        "VolumeParameter");
    auto* minFloatParameterClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
        "MinFloatParameter");
    auto* floatParameterClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
        "FloatParameter");
    auto* clampedIntParameterClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
        "ClampedIntParameter");
    auto* dofModeParameterClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "DepthOfFieldModeParameter");
    const std::initializer_list<UnityResolve::Method*> methods{
        internalCreateGameObject,
        gameObjectSetActive,
        addComponent,
        dontDestroyOnLoad,
        cameraCopyFrom,
        cameraSetTargetTexture,
        cameraGetTargetTexture,
        cameraSetCullingMask,
        cameraGetCullingMask,
        cameraGetDepth,
        cameraSetDepth,
        cameraGetNearClipPlane,
        cameraSetNearClipPlane,
        cameraGetFarClipPlane,
        cameraSetFarClipPlane,
        cameraGetAllowHdr,
        cameraGetAllowMsaa,
        cameraGetDepthTextureMode,
        cameraSetDepthTextureMode,
        cameraGetFieldOfView,
        cameraSetFieldOfView,
        cameraGetAspect,
        cameraSetAspect,
        cameraGetUsePhysicalProperties,
        cameraGetApertureInjected,
        cameraGetFocusDistanceInjected,
        cameraGetFocalLengthInjected,
        cameraSetFocalLengthInjected,
        cameraGetSensorSizeInjected,
        cameraSetSensorSizeInjected,
        cameraGetGateFittedFieldOfViewInjected,
        cameraGetGateFittedLensShiftInjected,
        cameraGetLensShiftInjected,
        cameraSetLensShiftInjected,
        cameraSetProjectionMatrixInjected,
        behaviourSetEnabled,
        behaviourGetEnabled,
        componentGetComponent,
        componentGetTransform,
        gameObjectGetTransform,
        transformSetPositionAndRotationInjected,
        matrixFrustumInjected,
        renderTextureConstructor,
        renderTextureDescriptorConstructor,
        renderTextureCreate,
        renderTextureRelease,
        renderTextureGetDescriptor,
        renderTextureGetSrgb,
        renderTextureGetGraphicsFormat,
        textureGetNativeTexturePtr,
        universalSetRenderer,
        universalGetRenderType,
        universalSetRenderType,
        universalGetRenderPostProcessing,
        universalSetRenderPostProcessing,
        universalGetAntialiasing,
        universalSetAntialiasing,
        universalGetAntialiasingQuality,
        universalSetAntialiasingQuality,
        universalGetRequiresDepthOption,
        universalSetRequiresDepthOption,
        universalGetRequiresColorOption,
        universalSetRequiresColorOption,
        universalGetRequiresDepthTexture,
        universalGetRequiresColorTexture,
        universalGetVolumeLayerMask,
        universalSetVolumeLayerMask,
        universalGetVolumeTrigger,
        universalSetVolumeTrigger,
        universalGetVolumeFrameworkUpdateMode,
        universalSetVolumeFrameworkUpdateMode,
        universalGetRequiresVolumeFrameworkUpdate,
        universalGetVolumeStack,
        universalGetOrCreateVolumeStack,
        universalGetTaaPersistentData,
        universalGetMotionVectorsPersistentData,
        universalGetResetHistory,
        cameraExtensionsUpdateVolumeStack,
        universalGetStopNan,
        universalSetStopNan,
        universalGetDithering,
        universalSetDithering,
        universalGetAllowHdrOutput,
        universalSetAllowHdrOutput,
        universalSetResetHistory,
        universalGetRenderShadows,
        universalSetRenderShadows,
        universalSetAllowXrRendering,
        universalGetFastRendering,
        universalSetFastRendering,
        universalGetNeedsAlphaChannel,
        universalSetNeedsAlphaChannel,
        vlAdditionalSetTargetTexture,
    };
    if (std::any_of(methods.begin(), methods.end(), [](const auto* method) {
            return method == nullptr || method->address == nullptr ||
                method->function == nullptr;
        })) {
        return FailStage("api.method-shape");
    }

    api_.internalCreateGameObject = MethodReference(internalCreateGameObject);
    api_.gameObjectSetActive = MethodReference(gameObjectSetActive);
    if (gameObjectGetActiveSelf != nullptr) {
        api_.gameObjectGetActiveSelf = MethodReference(gameObjectGetActiveSelf);
    }
    if (gameObjectGetLayer != nullptr) {
        api_.gameObjectGetLayer = MethodReference(gameObjectGetLayer);
    }
    if (gameObjectSetTag != nullptr) {
        api_.gameObjectSetTag = MethodReference(gameObjectSetTag);
    }
    if (gameObjectCompareTag != nullptr) {
        api_.gameObjectCompareTag = MethodReference(gameObjectCompareTag);
    }
    if (componentGetGameObject != nullptr) {
        api_.componentGetGameObject = MethodReference(componentGetGameObject);
    }
    Log(std::string("[VR][stereo] EYE_MAIN_CAMERA_TAG policy=never-write api=") +
        (api_.gameObjectSetTag.Ready() ? "1" : "0") +
        " compare=" + (api_.gameObjectCompareTag.Ready() ? "1" : "0") +
        " gameObject=" + (api_.componentGetGameObject.Ready() ? "1" : "0"));
    api_.addComponent = MethodReference(addComponent);
    api_.dontDestroyOnLoad = MethodReference(dontDestroyOnLoad);
    api_.cameraCopyFrom = MethodReference(cameraCopyFrom);
    api_.cameraSetTargetTexture = MethodReference(cameraSetTargetTexture);
    api_.cameraGetTargetTexture = MethodReference(cameraGetTargetTexture);
    api_.cameraSetCullingMask = MethodReference(cameraSetCullingMask);
    api_.cameraGetCullingMask = MethodReference(cameraGetCullingMask);
    if (cameraResetWorldToCameraMatrix != nullptr) {
        api_.cameraResetWorldToCameraMatrix =
            MethodReference(cameraResetWorldToCameraMatrix);
    }
    if (cameraSetNonJitteredProjectionMatrixInjected != nullptr) {
        api_.cameraSetNonJitteredProjectionMatrixInjected =
            MethodReference(cameraSetNonJitteredProjectionMatrixInjected);
        api_.nonJitteredProjectionUsesNativeSelf =
            cameraSetNonJitteredProjectionMatrixInjected->static_function;
    }
    api_.cameraGetDepth = MethodReference(cameraGetDepth);
    api_.cameraSetDepth = MethodReference(cameraSetDepth);
    api_.cameraGetNearClipPlane = MethodReference(cameraGetNearClipPlane);
    api_.cameraSetNearClipPlane = MethodReference(cameraSetNearClipPlane);
    api_.cameraGetFarClipPlane = MethodReference(cameraGetFarClipPlane);
    api_.cameraSetFarClipPlane = MethodReference(cameraSetFarClipPlane);
    api_.cameraGetAllowHdr = MethodReference(cameraGetAllowHdr);
    api_.cameraGetAllowMsaa = MethodReference(cameraGetAllowMsaa);
    api_.cameraGetDepthTextureMode = MethodReference(cameraGetDepthTextureMode);
    api_.cameraSetDepthTextureMode = MethodReference(cameraSetDepthTextureMode);
    api_.cameraGetFieldOfView = MethodReference(cameraGetFieldOfView);
    api_.cameraSetFieldOfView = MethodReference(cameraSetFieldOfView);
    api_.cameraGetAspect = MethodReference(cameraGetAspect);
    api_.cameraSetAspect = MethodReference(cameraSetAspect);
    api_.cameraGetUsePhysicalProperties =
        MethodReference(cameraGetUsePhysicalProperties);
    api_.cameraGetApertureInjected =
        MethodReference(cameraGetApertureInjected);
    api_.cameraGetFocusDistanceInjected =
        MethodReference(cameraGetFocusDistanceInjected);
    api_.cameraGetFocalLengthInjected =
        MethodReference(cameraGetFocalLengthInjected);
    api_.cameraSetFocalLengthInjected =
        MethodReference(cameraSetFocalLengthInjected);
    api_.cameraGetSensorSizeInjected =
        MethodReference(cameraGetSensorSizeInjected);
    api_.cameraSetSensorSizeInjected =
        MethodReference(cameraSetSensorSizeInjected);
    api_.cameraGetGateFittedFieldOfViewInjected =
        MethodReference(cameraGetGateFittedFieldOfViewInjected);
    api_.cameraGetGateFittedLensShiftInjected =
        MethodReference(cameraGetGateFittedLensShiftInjected);
    api_.cameraGetLensShiftInjected =
        MethodReference(cameraGetLensShiftInjected);
    api_.cameraSetLensShiftInjected =
        MethodReference(cameraSetLensShiftInjected);
    api_.lensShiftGetterUsesNativeSelf =
        cameraGetLensShiftInjected->static_function;
    api_.lensShiftSetterUsesNativeSelf =
        cameraSetLensShiftInjected->static_function;
    if (cameraGetProjectionMatrixInjected != nullptr) {
        api_.cameraGetProjectionMatrixInjected =
            MethodReference(cameraGetProjectionMatrixInjected);
        api_.projectionGetterUsesNativeSelf =
            cameraGetProjectionMatrixInjected->static_function;
    }
    if (cameraGetNonJitteredProjectionMatrixInjected != nullptr) {
        api_.cameraGetNonJitteredProjectionMatrixInjected =
            MethodReference(cameraGetNonJitteredProjectionMatrixInjected);
        api_.nonJitteredProjectionGetterUsesNativeSelf =
            cameraGetNonJitteredProjectionMatrixInjected->static_function;
    }
    api_.cameraSetProjectionMatrixInjected =
        MethodReference(cameraSetProjectionMatrixInjected);
    api_.projectionUsesNativeSelf =
        cameraSetProjectionMatrixInjected->static_function;
    api_.behaviourSetEnabled = MethodReference(behaviourSetEnabled);
    api_.behaviourGetEnabled = MethodReference(behaviourGetEnabled);
    api_.componentGetComponent = MethodReference(componentGetComponent);
    api_.componentGetTransform = MethodReference(componentGetTransform);
    api_.gameObjectGetTransform = MethodReference(gameObjectGetTransform);
    if (transformGetParent != nullptr) {
        api_.transformGetParent = MethodReference(transformGetParent);
    }
    api_.transformSetPositionAndRotationInjected =
        MethodReference(transformSetPositionAndRotationInjected);
    api_.transformUsesNativeSelf =
        transformSetPositionAndRotationInjected->static_function;
    // Optional: only the actor-shadow anchor needs to read a camera pose back.
    // A miss disables that feature instead of failing the whole ladder.
    if (transformGetPositionInjected != nullptr) {
        api_.transformGetPositionInjected =
            MethodReference(transformGetPositionInjected);
        api_.transformPositionGetterUsesNativeSelf =
            transformGetPositionInjected->static_function;
    }
    if (transformGetRotationInjected != nullptr) {
        api_.transformGetRotationInjected =
            MethodReference(transformGetRotationInjected);
        api_.transformRotationGetterUsesNativeSelf =
            transformGetRotationInjected->static_function;
    }
    api_.matrixFrustumInjected = MethodReference(matrixFrustumInjected);
    api_.renderTextureConstructor = MethodReference(renderTextureConstructor);
    api_.renderTextureDescriptorConstructor =
        MethodReference(renderTextureDescriptorConstructor);
    api_.renderTextureCreate = MethodReference(renderTextureCreate);
    api_.renderTextureRelease = MethodReference(renderTextureRelease);
    api_.renderTextureGetDescriptor = MethodReference(renderTextureGetDescriptor);
    api_.renderTextureGetSrgb = MethodReference(renderTextureGetSrgb);
    api_.renderTextureGetGraphicsFormat =
        MethodReference(renderTextureGetGraphicsFormat);
    api_.textureGetNativeTexturePtr = MethodReference(textureGetNativeTexturePtr);
    api_.commandBufferClass =
        commandBufferKlass != nullptr ? commandBufferKlass->address : nullptr;
    api_.commandBufferConstructor = MethodReference(commandBufferConstructor);
    api_.commandBufferClear = MethodReference(commandBufferClear);
    api_.commandBufferCopyTexture = MethodReference(commandBufferCopyTexture);
    api_.renderTargetIdentifierConstructor =
        MethodReference(renderTargetIdentifierConstructor);
    api_.scriptableRenderContextExecuteCommandBuffer =
        MethodReference(scriptableRenderContextExecuteCommandBuffer);
    Log(std::string("[VR][smaa-t2x] SMAA_T2X_API commandBufferClass=") +
        (api_.commandBufferClass != nullptr ? "1" : "0") +
        " ctor=" + (api_.commandBufferConstructor.Ready() ? "1" : "0") +
        " clear=" + (api_.commandBufferClear.Ready() ? "1" : "0") +
        " copyTexture2=" +
        (api_.commandBufferCopyTexture.Ready() ? "1" : "0") +
        " rtIdentifierCtor=" +
        (api_.renderTargetIdentifierConstructor.Ready() ? "1" : "0") +
        " contextExecute=" +
        (api_.scriptableRenderContextExecuteCommandBuffer.Ready() ? "1" : "0") +
        " copySignature=RenderTargetIdentifier,RenderTargetIdentifier");
    api_.universalSetRenderer = MethodReference(universalSetRenderer);
    api_.universalGetRenderType = MethodReference(universalGetRenderType);
    api_.universalSetRenderType = MethodReference(universalSetRenderType);
    api_.universalGetRenderPostProcessing =
        MethodReference(universalGetRenderPostProcessing);
    api_.universalSetRenderPostProcessing =
        MethodReference(universalSetRenderPostProcessing);
    api_.universalGetAntialiasing =
        MethodReference(universalGetAntialiasing);
    api_.universalSetAntialiasing =
        MethodReference(universalSetAntialiasing);
    api_.universalGetAntialiasingQuality =
        MethodReference(universalGetAntialiasingQuality);
    api_.universalSetAntialiasingQuality =
        MethodReference(universalSetAntialiasingQuality);
    api_.universalGetRequiresDepthOption =
        MethodReference(universalGetRequiresDepthOption);
    api_.universalSetRequiresDepthOption =
        MethodReference(universalSetRequiresDepthOption);
    api_.universalGetRequiresColorOption =
        MethodReference(universalGetRequiresColorOption);
    api_.universalSetRequiresColorOption =
        MethodReference(universalSetRequiresColorOption);
    api_.universalGetRequiresDepthTexture =
        MethodReference(universalGetRequiresDepthTexture);
    api_.universalGetRequiresColorTexture =
        MethodReference(universalGetRequiresColorTexture);
    api_.universalGetVolumeLayerMask =
        MethodReference(universalGetVolumeLayerMask);
    api_.universalSetVolumeLayerMask =
        MethodReference(universalSetVolumeLayerMask);
    api_.universalGetVolumeTrigger =
        MethodReference(universalGetVolumeTrigger);
    api_.universalSetVolumeTrigger =
        MethodReference(universalSetVolumeTrigger);
    api_.universalGetVolumeFrameworkUpdateMode =
        MethodReference(universalGetVolumeFrameworkUpdateMode);
    api_.universalSetVolumeFrameworkUpdateMode =
        MethodReference(universalSetVolumeFrameworkUpdateMode);
    api_.universalGetRequiresVolumeFrameworkUpdate =
        MethodReference(universalGetRequiresVolumeFrameworkUpdate);
    api_.universalGetVolumeStack = MethodReference(universalGetVolumeStack);
    api_.universalGetOrCreateVolumeStack =
        MethodReference(universalGetOrCreateVolumeStack);
    api_.universalGetTaaPersistentData =
        MethodReference(universalGetTaaPersistentData);
    api_.universalGetMotionVectorsPersistentData =
        MethodReference(universalGetMotionVectorsPersistentData);
    api_.universalGetResetHistory = MethodReference(universalGetResetHistory);
    api_.cameraExtensionsUpdateVolumeStack =
        MethodReference(cameraExtensionsUpdateVolumeStack);
    api_.universalGetStopNan = MethodReference(universalGetStopNan);
    api_.universalSetStopNan = MethodReference(universalSetStopNan);
    api_.universalGetDithering = MethodReference(universalGetDithering);
    api_.universalSetDithering = MethodReference(universalSetDithering);
    api_.universalGetAllowHdrOutput = MethodReference(universalGetAllowHdrOutput);
    api_.universalSetAllowHdrOutput = MethodReference(universalSetAllowHdrOutput);
    api_.universalSetResetHistory = MethodReference(universalSetResetHistory);
    api_.universalGetRenderShadows = MethodReference(universalGetRenderShadows);
    api_.universalSetRenderShadows = MethodReference(universalSetRenderShadows);
    api_.universalSetAllowXrRendering =
        MethodReference(universalSetAllowXrRendering);
    api_.universalGetFastRendering = MethodReference(universalGetFastRendering);
    api_.universalSetFastRendering = MethodReference(universalSetFastRendering);
    api_.universalGetNeedsAlphaChannel =
        MethodReference(universalGetNeedsAlphaChannel);
    api_.universalSetNeedsAlphaChannel =
        MethodReference(universalSetNeedsAlphaChannel);
    api_.vlAdditionalSetTargetTexture =
        MethodReference(vlAdditionalSetTargetTexture);
    api_.vlControllerGetTargetTextureDescriptor =
        MethodReference(vlControllerGetTargetTextureDescriptor);
    api_.vlControllerGetHdrTargetTextureDescriptor =
        MethodReference(vlControllerGetHdrTargetTextureDescriptor);
    api_.volumeStackGetComponent = MethodReference(volumeStackGetComponent);
    api_.volumeComponentSetActive = MethodReference(volumeComponentSetActive);
    api_.gameObjectClass = gameObjectClass->address;
    api_.renderTextureClass = renderTextureClass->address;
    api_.renderTextureDescriptorClass = renderTextureDescriptorClass->address;
    api_.cameraReflectionType = cameraClass->GetType();
    api_.universalCameraDataReflectionType = universalCameraDataClass->GetType();
    api_.vlAdditionalCameraDataReflectionType =
        vlAdditionalCameraDataClass->GetType();
    api_.vlCameraControllerReflectionType = vlCameraControllerClass->GetType();
    api_.vlDofReflectionType =
        vlDofClass != nullptr ? vlDofClass->GetType() : nullptr;
    api_.urpDofReflectionType =
        urpDofClass != nullptr ? urpDofClass->GetType() : nullptr;
    api_.vlBloomReflectionType =
        vlBloomClass != nullptr ? vlBloomClass->GetType() : nullptr;
    api_.urpBloomReflectionType =
        urpBloomClass != nullptr ? urpBloomClass->GetType() : nullptr;
    api_.chromaticAberrationReflectionType =
        chromaticAberrationClass != nullptr
            ? chromaticAberrationClass->GetType()
            : nullptr;
    api_.lensDistortionReflectionType =
        lensDistortionClass != nullptr ? lensDistortionClass->GetType()
                                       : nullptr;
    api_.motionBlurReflectionType =
        motionBlurClass != nullptr ? motionBlurClass->GetType() : nullptr;
    api_.vignetteReflectionType =
        vignetteClass != nullptr ? vignetteClass->GetType() : nullptr;
    api_.lensFlareClass = nullptr;
    api_.lensFlareScaleOffset = -1;
    api_.lensFlareIntensityOffset = -1;
    if (auto* coreAssembly =
            UnityResolve::Get("Unity.RenderPipelines.Core.Runtime.dll")) {
        auto* lensFlareClass =
            coreAssembly->Get("LensFlareComponentSRP", "UnityEngine.Rendering");
        api_.lensFlareClass = lensFlareClass;
        api_.lensFlareScaleOffset =
            FindClassFieldOffset(lensFlareClass, {"scale"});
        api_.lensFlareIntensityOffset =
            FindClassFieldOffset(lensFlareClass, {"intensity"});
    }
    api_.proFlareClass = nullptr;
    api_.proFlareGlobalScaleOffset = -1;
    api_.proFlareGlobalBrightnessOffset = -1;
    if (auto* proFlareAssembly = UnityResolve::Get("ProFlare.Runtime.dll")) {
        auto* proFlareClass = proFlareAssembly->Get("ProFlare");
        if (proFlareClass == nullptr) {
            proFlareClass = proFlareAssembly->Get("ProFlare", "");
        }
        api_.proFlareClass = proFlareClass;
        api_.proFlareGlobalScaleOffset =
            FindClassFieldOffset(proFlareClass, {"GlobalScale"});
        api_.proFlareGlobalBrightnessOffset =
            FindClassFieldOffset(proFlareClass, {"GlobalBrightness"});
        api_.proFlareDynamicEdgeBoostOffset =
            FindClassFieldOffset(proFlareClass, {"DynamicEdgeBoost"});
        api_.proFlareDynamicCenterBoostOffset =
            FindClassFieldOffset(proFlareClass, {"DynamicCenterBoost"});
        if (proFlareClass != nullptr) {
            std::uint32_t logged = 0;
            for (auto* field : proFlareClass->fields) {
                if (field == nullptr || logged >= 40U) {
                    break;
                }
                ++logged;
                Log(std::string("[VR][stereo] PRO_FLARE_FIELD name=") +
                    field->name + " type=" +
                    (field->type != nullptr ? field->type->name : "?") +
                    " offset=" + std::to_string(field->offset) +
                    " static=" + (field->static_field ? "1" : "0"));
            }
        }
    }
    api_.uiTextureOverlayClass = uiTextureOverlayClass;
    api_.uiTextureOverlayShaderOffset =
        FindClassFieldOffset(uiTextureOverlayClass, {"_shader"});
    api_.uiTextureOverlayTextureOffset =
        FindClassFieldOffset(uiTextureOverlayClass, {"_overlayTexture"});
    api_.uiTextureOverlaySpriteOffset =
        FindClassFieldOffset(uiTextureOverlayClass, {"_overlaySprite"});
    api_.uiTextureOverlayColorOffset =
        FindClassFieldOffset(uiTextureOverlayClass, {"_overlayColor"});
    api_.uiTextureOverlayClampUvOffset =
        FindClassFieldOffset(uiTextureOverlayClass, {"_clampOverlayUv"});
    api_.uiTextureOverlayAlphaMaskOffset =
        FindClassFieldOffset(uiTextureOverlayClass, {"_alphaMask"});
    api_.uiTextureOverlayModeOffset =
        FindClassFieldOffset(uiTextureOverlayClass, {"_mode"});
    api_.uiTextureOverlayMaterialOffset =
        FindClassFieldOffset(uiTextureOverlayClass, {"_material"});
    if (!uiTextureOverlayApiLogged_) {
        uiTextureOverlayApiLogged_ = true;
        Log(std::string("[VR][stereo] UI_TEXTURE_OVERLAY_API type=") +
            (uiTextureOverlayClass != nullptr ? "1" : "0") +
            " enabled=" + (api_.behaviourGetEnabled.Ready() ? "1" : "0") +
            " layer=" + (api_.gameObjectGetLayer.Ready() ? "1" : "0") +
            " parent=" + (api_.transformGetParent.Ready() ? "1" : "0") +
            " fields=" +
            std::to_string(api_.uiTextureOverlayShaderOffset) + "," +
            std::to_string(api_.uiTextureOverlayTextureOffset) + "," +
            std::to_string(api_.uiTextureOverlaySpriteOffset) + "," +
            std::to_string(api_.uiTextureOverlayColorOffset) + "," +
            std::to_string(api_.uiTextureOverlayClampUvOffset) + "," +
            std::to_string(api_.uiTextureOverlayAlphaMaskOffset) + "," +
            std::to_string(api_.uiTextureOverlayModeOffset) + "," +
            std::to_string(api_.uiTextureOverlayMaterialOffset));
        if (uiTextureOverlayClass != nullptr) {
            for (auto* field : uiTextureOverlayClass->fields) {
                if (field == nullptr) {
                    continue;
                }
                Log("[VR][stereo] UI_TEXTURE_OVERLAY_FIELD name=" +
                    field->name + " type=" +
                    (field->type != nullptr ? field->type->name : "?") +
                    " offset=" + std::to_string(field->offset) +
                    " static=" + (field->static_field ? "1" : "0"));
            }
            for (auto* method : uiTextureOverlayClass->methods) {
                if (method == nullptr) {
                    continue;
                }
                std::ostringstream entry;
                entry << "[VR][stereo] UI_TEXTURE_OVERLAY_METHOD signature=\""
                      << MethodSignature(method) << "\" address="
                      << method->address << " function=" << method->function;
                Log(entry.str());
            }
        }
    }
    api_.liveCameraOverlayClass = liveCameraOverlayClass;
    api_.liveCameraOverlayOffsetFieldOffset =
        FindClassFieldOffset(liveCameraOverlayClass, {"_offset"});
    api_.liveCameraOverlayScalerFieldOffset =
        FindClassFieldOffset(liveCameraOverlayClass, {"_scalerMagnification"});
    if (!liveCameraOverlayApiLogged_) {
        liveCameraOverlayApiLogged_ = true;
        Log(std::string("[VR][stereo] LIVE_CAMERA_OVERLAY_API type=") +
            (liveCameraOverlayClass != nullptr ? "1" : "0") +
            " activeSelf=" +
            (api_.gameObjectGetActiveSelf.Ready() ? "1" : "0") +
            " setActive=" + (api_.gameObjectSetActive.Ready() ? "1" : "0") +
            " layer=" + (api_.gameObjectGetLayer.Ready() ? "1" : "0") +
            " parent=" + (api_.transformGetParent.Ready() ? "1" : "0") +
            " offset=" +
            std::to_string(api_.liveCameraOverlayOffsetFieldOffset) +
            " scaler=" +
            std::to_string(api_.liveCameraOverlayScalerFieldOffset));
        if (liveCameraOverlayClass != nullptr) {
            for (auto* field : liveCameraOverlayClass->fields) {
                if (field == nullptr) {
                    continue;
                }
                Log("[VR][stereo] LIVE_CAMERA_OVERLAY_FIELD name=" +
                    field->name + " type=" +
                    (field->type != nullptr ? field->type->name : "?") +
                    " offset=" + std::to_string(field->offset) +
                    " static=" + (field->static_field ? "1" : "0"));
            }
            for (auto* method : liveCameraOverlayClass->methods) {
                if (method == nullptr) {
                    continue;
                }
                std::ostringstream entry;
                entry << "[VR][stereo] LIVE_CAMERA_OVERLAY_METHOD signature=\""
                      << MethodSignature(method) << "\" address="
                      << method->address << " function=" << method->function;
                Log(entry.str());
            }
        }
    }
    api_.particleSystemClass = particleSystemClass;
    if (!cmovParticleApiLogged_) {
        cmovParticleApiLogged_ = true;
        Log(std::string("[VR][stereo] LIVE_CMOV_PS_API type=") +
            (particleSystemClass != nullptr ? "1" : "0") +
            " activeSelf=" +
            (api_.gameObjectGetActiveSelf.Ready() ? "1" : "0") +
            " setActive=" + (api_.gameObjectSetActive.Ready() ? "1" : "0") +
            " parent=" + (api_.transformGetParent.Ready() ? "1" : "0"));
    }
    api_.volumeComponentActiveOffset = FindInstanceBoolOffset(
        volumeComponentClass, {"m_Active", "active", "<active>k__BackingField"});
    if (api_.volumeComponentActiveOffset < 0) {
        api_.volumeComponentActiveOffset = FindInstanceBoolOffset(
            vlDofClass, {"m_Active", "active", "<active>k__BackingField"});
    }
    if (api_.volumeComponentActiveOffset < 0) {
        api_.volumeComponentActiveOffset = FindInstanceBoolOffset(
            urpDofClass, {"m_Active", "active", "<active>k__BackingField"});
    }
    api_.volumeParameterOverrideOffset = FindNamedFieldOffset(
        volumeParameterClass, {"m_OverrideState", "overrideState"},
        {"System.Boolean", "Boolean"});
    api_.volumeParameterFloatValueOffset = FindNamedFieldOffset(
        minFloatParameterClass, {"m_Value", "value"},
        {"System.Single", "Single"});
    if (api_.volumeParameterFloatValueOffset < 0) {
        api_.volumeParameterFloatValueOffset = FindNamedFieldOffset(
            floatParameterClass, {"m_Value", "value"},
            {"System.Single", "Single"});
    }
    api_.volumeParameterIntValueOffset = FindNamedFieldOffset(
        dofModeParameterClass, {"m_Value", "value"},
        {"System.Int32", "Int32",
         "UnityEngine.Rendering.Universal.DepthOfFieldMode"});
    if (api_.volumeParameterIntValueOffset < 0) {
        api_.volumeParameterIntValueOffset = FindNamedFieldOffset(
            clampedIntParameterClass, {"m_Value", "value"},
            {"System.Int32", "Int32"});
    }
    if (vlDofClass != nullptr) {
        if (const auto* field =
                vlDofClass->Get<UnityResolve::Field>("maxBlurSpread")) {
            api_.vlDofMaxBlurSpreadFieldOffset = field->offset;
        }
        if (const auto* field =
                vlDofClass->Get<UnityResolve::Field>("foregroundBlurExtrude")) {
            api_.vlDofForegroundBlurFieldOffset = field->offset;
        }
        if (const auto* field =
                vlDofClass->Get<UnityResolve::Field>("quality")) {
            api_.vlDofQualityFieldOffset = field->offset;
        }
    }
    if (urpDofClass != nullptr) {
        if (const auto* field = urpDofClass->Get<UnityResolve::Field>("mode")) {
            api_.urpDofModeFieldOffset = field->offset;
        }
    }
    api_.vlBloomIntensityFieldOffset = FindClassFieldOffset(
        vlBloomClass, {"intensity", "bloomIntensity", "strength"});
    api_.vlBloomScatterFieldOffset = FindClassFieldOffset(
        vlBloomClass, {"scatter", "bloomScatter", "spread"});
    api_.vlBloomDiffusionFieldOffset = FindClassFieldOffset(
        vlBloomClass, {"diffusion"});
    api_.urpBloomIntensityFieldOffset = FindClassFieldOffset(
        urpBloomClass, {"intensity"});
    api_.urpBloomScatterFieldOffset = FindClassFieldOffset(
        urpBloomClass, {"scatter"});
    api_.urpBloomDirtIntensityFieldOffset = FindClassFieldOffset(
        urpBloomClass, {"dirtIntensity"});
    api_.chromaticAberrationIntensityFieldOffset = FindClassFieldOffset(
        chromaticAberrationClass, {"intensity"});
    api_.lensDistortionIntensityFieldOffset = FindClassFieldOffset(
        lensDistortionClass, {"intensity"});
    api_.motionBlurIntensityFieldOffset = FindClassFieldOffset(
        motionBlurClass, {"intensity"});
    api_.vignetteIntensityFieldOffset = FindClassFieldOffset(
        vignetteClass, {"intensity"});
    api_.vignetteSmoothnessFieldOffset = FindClassFieldOffset(
        vignetteClass, {"smoothness"});
    const auto* rendererIndexField = universalCameraDataClass->Get<UnityResolve::Field>(
        "m_RendererIndex");
    const auto* taaSettingsField = universalCameraDataClass->Get<UnityResolve::Field>(
        "m_TaaSettings");
    const auto* clearDepthField = universalCameraDataClass->Get<UnityResolve::Field>(
        "m_ClearDepth");
    const auto* useScreenCoordOverrideField =
        universalCameraDataClass->Get<UnityResolve::Field>(
            "m_UseScreenCoordOverride");
    const auto* screenSizeOverrideField =
        universalCameraDataClass->Get<UnityResolve::Field>(
            "m_ScreenSizeOverride");
    const auto* screenCoordScaleBiasField =
        universalCameraDataClass->Get<UnityResolve::Field>(
            "m_ScreenCoordScaleBias");
    const auto* renderScaleField = universalCameraDataClass->Get<UnityResolve::Field>(
        "m_RenderScale");
    if (rendererIndexField != nullptr && !rendererIndexField->static_field &&
        rendererIndexField->type != nullptr &&
        rendererIndexField->type->name == "System.Int32") {
        api_.universalRendererIndexOffset = rendererIndexField->offset;
    }
    if (taaSettingsField != nullptr && !taaSettingsField->static_field &&
        taaSettingsField->offset >= 0) {
        api_.universalTaaSettingsOffset = taaSettingsField->offset;
    }
    if (clearDepthField != nullptr && !clearDepthField->static_field &&
        clearDepthField->offset >= 0) {
        api_.universalClearDepthOffset = clearDepthField->offset;
    }
    if (useScreenCoordOverrideField != nullptr &&
        !useScreenCoordOverrideField->static_field &&
        useScreenCoordOverrideField->offset >= 0) {
        api_.universalUseScreenCoordOverrideOffset =
            useScreenCoordOverrideField->offset;
    }
    if (screenSizeOverrideField != nullptr &&
        !screenSizeOverrideField->static_field &&
        screenSizeOverrideField->offset >= 0) {
        api_.universalScreenSizeOverrideOffset =
            screenSizeOverrideField->offset;
    }
    if (screenCoordScaleBiasField != nullptr &&
        !screenCoordScaleBiasField->static_field &&
        screenCoordScaleBiasField->offset >= 0) {
        api_.universalScreenCoordScaleBiasOffset =
            screenCoordScaleBiasField->offset;
    }
    if (renderScaleField != nullptr && !renderScaleField->static_field &&
        renderScaleField->offset >= 0) {
        api_.universalRenderScaleOffset = renderScaleField->offset;
    }
    api_.renderTextureDescriptorValueSize =
        ClassValueSize(api_.renderTextureDescriptorClass);
    if (api_.gameObjectClass == nullptr || api_.renderTextureClass == nullptr ||
        api_.renderTextureDescriptorClass == nullptr ||
        api_.cameraReflectionType == nullptr ||
        api_.universalCameraDataReflectionType == nullptr ||
        api_.vlAdditionalCameraDataReflectionType == nullptr ||
        api_.vlCameraControllerReflectionType == nullptr ||
        api_.universalRendererIndexOffset < 0 ||
        api_.universalTaaSettingsOffset < 0 ||
        api_.universalClearDepthOffset < 0 ||
        api_.universalUseScreenCoordOverrideOffset < 0 ||
        api_.universalScreenSizeOverrideOffset < 0 ||
        api_.universalScreenCoordScaleBiasOffset < 0 ||
        api_.universalRenderScaleOffset < 0 ||
        api_.renderTextureDescriptorValueSize !=
            static_cast<int>(sizeof(UnityRenderTextureDescriptorValue))) {
        return FailStage("api.runtime-type");
    }

    apiInitialized_ = true;
    Log("[VR][stereo] CALL_OK stage=api.resolve");
    Log("[VR][stereo] VLSRP_ADAPTER_API_READY controllerInstances=0 descriptorBytes=" +
        std::to_string(api_.renderTextureDescriptorValueSize));
    Log("[VR][stereo] QUEUE_LADDER_API_READY backend=ordinary-camera-list stages=A/B/C manualRenderApi=0");
    Log(std::string("[VR][stereo] EYE_VLDOF_SUPPRESS_API ") +
        "getComponent=" +
        (api_.volumeStackGetComponent.Ready() ? "1" : "0") +
        " setActive=" +
        (api_.volumeComponentSetActive.Ready() ? "1" : "0") +
        " vlDofType=" + (api_.vlDofReflectionType != nullptr ? "1" : "0") +
        " urpDofType=" + (api_.urpDofReflectionType != nullptr ? "1" : "0") +
        " activeOffset=" +
        std::to_string(api_.volumeComponentActiveOffset) +
        " overrideOffset=" +
        std::to_string(api_.volumeParameterOverrideOffset) +
        " floatValueOffset=" +
        std::to_string(api_.volumeParameterFloatValueOffset) +
        " intValueOffset=" +
        std::to_string(api_.volumeParameterIntValueOffset));
    Log(std::string("[VR][stereo] EYE_BLOOM_SUPPRESS_API ") +
        "vlBloomType=" + (api_.vlBloomReflectionType != nullptr ? "1" : "0") +
        " urpBloomType=" +
        (api_.urpBloomReflectionType != nullptr ? "1" : "0") +
        " vlIntensityOffset=" +
        std::to_string(api_.vlBloomIntensityFieldOffset) +
        " vlDiffusionOffset=" +
        std::to_string(api_.vlBloomDiffusionFieldOffset) +
        " urpIntensityOffset=" +
        std::to_string(api_.urpBloomIntensityFieldOffset));
    Log(std::string("[VR][stereo] EYE_FOV_SCALE_API ") +
        "caType=" +
        (api_.chromaticAberrationReflectionType != nullptr ? "1" : "0") +
        " distortionType=" +
        (api_.lensDistortionReflectionType != nullptr ? "1" : "0") +
        " motionBlurType=" +
        (api_.motionBlurReflectionType != nullptr ? "1" : "0") +
        " vignetteType=" +
        (api_.vignetteReflectionType != nullptr ? "1" : "0") +
        " caIntensityOffset=" +
        std::to_string(api_.chromaticAberrationIntensityFieldOffset) +
        " distortionIntensityOffset=" +
        std::to_string(api_.lensDistortionIntensityFieldOffset) +
        " motionBlurIntensityOffset=" +
        std::to_string(api_.motionBlurIntensityFieldOffset) +
        " vignetteIntensityOffset=" +
        std::to_string(api_.vignetteIntensityFieldOffset));
    Log(std::string("[VR][stereo] LENS_FLARE_SCALE_API ") +
        "type=" + (api_.lensFlareClass != nullptr ? "1" : "0") +
        " scaleOffset=" + std::to_string(api_.lensFlareScaleOffset) +
        " intensityOffset=" +
        std::to_string(api_.lensFlareIntensityOffset));
    Log(std::string("[VR][stereo] PRO_FLARE_SCALE_API ") +
        "type=" + (api_.proFlareClass != nullptr ? "1" : "0") +
        " globalScaleOffset=" +
        std::to_string(api_.proFlareGlobalScaleOffset) +
        " globalBrightnessOffset=" +
        std::to_string(api_.proFlareGlobalBrightnessOffset) +
        " edgeBoostOffset=" +
        std::to_string(api_.proFlareDynamicEdgeBoostOffset) +
        " centerBoostOffset=" +
        std::to_string(api_.proFlareDynamicCenterBoostOffset));
    const std::string_view assemblies[] = {
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "vl-unity.Runtime.dll",
    };
    const std::string_view needles[] = {
        "Bloom", "DOF", "Dof", "Fog", "Sky", "Cloud", "Glare", "Flare",
        "Bokeh", "Glow", "Lens", "Anamorphic", "Exposure", "Tone",
        "Vignette", "Chromatic", "Motion", "Grain",
    };
    for (const auto assemblyName : assemblies) {
        auto* assembly = UnityResolve::Get(std::string(assemblyName));
        if (assembly == nullptr) {
            continue;
        }
        std::uint32_t loggedTypes = 0;
        for (auto* klass : assembly->classes) {
            if (klass == nullptr || loggedTypes >= 24U) {
                continue;
            }
            const bool interesting = std::any_of(
                std::begin(needles), std::end(needles),
                [klass](std::string_view needle) {
                    return klass->name.find(needle) != std::string::npos;
                });
            if (!interesting) {
                continue;
            }
            ++loggedTypes;
            Log("[VR][stereo] VL_EFFECT_TYPE assembly=" +
                std::string(assemblyName) + " ns=" + klass->namespaze +
                " name=" + klass->name);
        }
    }
    const std::pair<const char*, const char*> knownTypes[] = {
        {"VL.Rendering", "VLBloom"},
        {"VL.Rendering", "VLFog"},
        {"VL.Rendering", "VLSky"},
        {"VL.Rendering", "VLCloud"},
        {"VL.Rendering", "VLGlare"},
        {"UnityEngine.Rendering.Universal", "Bloom"},
        {"UnityEngine.Rendering.Universal", "DepthOfField"},
    };
    for (const auto& known : knownTypes) {
        auto* found = Il2cppUtils::GetClass(
            "Unity.RenderPipelines.Universal.Runtime.dll", known.first,
            known.second);
        if (found == nullptr) {
            found = Il2cppUtils::GetClass(
                "vl-unity.Runtime.dll", known.first, known.second);
        }
        Log(std::string("[VR][stereo] VL_EFFECT_LOOKUP ns=") + known.first +
            " name=" + known.second + " found=" +
            (found != nullptr ? "1" : "0"));
    }
    if (auto* coreAssembly =
            UnityResolve::Get("Unity.RenderPipelines.Core.Runtime.dll")) {
        for (const char* flareName :
             {"LensFlareComponentSRP", "LensFlareDataSRP"}) {
            auto* found =
                coreAssembly->Get(flareName, "UnityEngine.Rendering");
            Log(std::string("[VR][stereo] LENS_FLARE_LOOKUP name=") +
                flareName +
                " found=" + (found != nullptr ? "1" : "0"));
        }
    }
    if (vlDofClass != nullptr) {
        std::uint32_t loggedMethods = 0;
        for (auto* method : vlDofClass->methods) {
            if (method == nullptr || loggedMethods >= 24U) {
                continue;
            }
            ++loggedMethods;
            Log("[VR][stereo] VLDOF_METHOD name=" + method->name +
                " signature=\"" + MethodSignature(method) + "\"");
        }
    }
    const auto logFields = [this](UnityResolve::Class* klass,
                                  const char* tag) {
        if (klass == nullptr) {
            return;
        }
        std::uint32_t logged = 0;
        for (auto* field : klass->fields) {
            if (field == nullptr || logged >= 32U) {
                continue;
            }
            ++logged;
            Log(std::string("[VR][stereo] ") + tag + " name=" + field->name +
                " type=" +
                (field->type != nullptr ? field->type->name : "?") +
                " offset=" + std::to_string(field->offset) +
                " static=" + (field->static_field ? "1" : "0"));
        }
    };
    logFields(volumeComponentClass, "VOLUME_COMPONENT_FIELD");
    logFields(volumeParameterClass, "VOLUME_PARAMETER_FIELD");
    logFields(minFloatParameterClass, "MIN_FLOAT_PARAMETER_FIELD");
    logFields(floatParameterClass, "FLOAT_PARAMETER_FIELD");
    logFields(vlDofClass, "VLDOF_FIELD");
    logFields(urpDofClass, "URP_DOF_FIELD");
    logFields(vlBloomClass, "VLBLOOM_FIELD");
    logFields(urpBloomClass, "URP_BLOOM_FIELD");
    logFields(chromaticAberrationClass, "CHROMATIC_ABERRATION_FIELD");
    logFields(lensDistortionClass, "LENS_DISTORTION_FIELD");
    logFields(motionBlurClass, "MOTION_BLUR_FIELD");
    logFields(vignetteClass, "VIGNETTE_FIELD");
    if (minFloatParameterClass != nullptr) {
        LogInheritedFields(
            minFloatParameterClass->address, "VOLUME_PARAMETER_INHERITED_FIELD");
    } else if (floatParameterClass != nullptr) {
        LogInheritedFields(
            floatParameterClass->address, "VOLUME_PARAMETER_INHERITED_FIELD");
    }
    if (dofModeParameterClass != nullptr) {
        LogInheritedFields(
            dofModeParameterClass->address, "VOLUME_INT_PARAMETER_INHERITED_FIELD");
    }
    const std::tuple<const char*, const char*, const char*> fovVolumes[] = {
        {"Unity.RenderPipelines.Universal.Runtime.dll",
         "UnityEngine.Rendering.Universal", "Tonemapping"},
        {"Unity.RenderPipelines.Universal.Runtime.dll",
         "UnityEngine.Rendering.Universal", "ColorAdjustments"},
        {"Unity.RenderPipelines.Universal.Runtime.dll",
         "UnityEngine.Rendering.Universal", "WhiteBalance"},
        {"Unity.RenderPipelines.Universal.Runtime.dll",
         "UnityEngine.Rendering.Universal", "FilmGrain"},
        {"Unity.RenderPipelines.Universal.Runtime.dll",
         "UnityEngine.Rendering.Universal", "ChromaticAberration"},
        {"Unity.RenderPipelines.Universal.Runtime.dll",
         "UnityEngine.Rendering.Universal", "LensDistortion"},
        {"Unity.RenderPipelines.Universal.Runtime.dll",
         "UnityEngine.Rendering.Universal", "MotionBlur"},
        {"Unity.RenderPipelines.Universal.Runtime.dll",
         "UnityEngine.Rendering.Universal", "Vignette"},
        {"Unity.RenderPipelines.Universal.Runtime.dll",
         "UnityEngine.Rendering.Universal", "LiftGammaGain"},
        {"Unity.RenderPipelines.Universal.Runtime.dll",
         "UnityEngine.Rendering.Universal", "ShadowsMidtonesHighlights"},
        {"vl-unity.Runtime.dll", "VL.Rendering", "VLExposure"},
        {"Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering",
         "VLExposure"},
        {"Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
         "LensFlareComponentSRP"},
    };
    for (const auto& volume : fovVolumes) {
        auto* assembly = UnityResolve::Get(std::get<0>(volume));
        auto* found = assembly != nullptr
            ? assembly->Get(std::get<2>(volume), std::get<1>(volume))
            : nullptr;
        Log(std::string("[VR][stereo] FOV_VOLUME_LOOKUP ns=") +
            std::get<1>(volume) + " name=" + std::get<2>(volume) +
            " found=" + (found != nullptr ? "1" : "0"));
        if (found != nullptr) {
            logFields(found, "FOV_VOLUME_FIELD");
        }
    }
    EnsureSkyRenderHooks(*this);
    return true;
}

bool UnityStereoRenderer::EnsureCamera(std::size_t eye) noexcept {
    if (eye >= eyeCameras_.size()) {
        return FailStage("resources.camera-index", eye);
    }
    if (eyeCameras_[eye] != nullptr) {
        SyncEyeAsMainCameraTag();
        return true;
    }
    const char* eyeName = eye == 0U ? "left" : "right";
    Log("[VR][stereo] CALL_BEGIN stage=resources.game-object-allocate eye=" +
        std::string(eyeName));
    eyeGameObjects_[eye] = NewIl2CppObject(api_.gameObjectClass);
    if (eyeGameObjects_[eye] == nullptr) {
        return FailStage("resources.game-object-allocation", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.game-object-allocate eye=" +
        std::string(eyeName));
    Log("[VR][stereo] CALL_BEGIN stage=resources.game-object-root eye=" +
        std::string(eyeName));
    eyeGameObjectHandles_[eye] = CreateGcHandle(eyeGameObjects_[eye]);
    if (eyeGameObjectHandles_[eye] == nullptr) {
        return FailStage("resources.game-object-gchandle", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.game-object-root eye=" +
        std::string(eyeName));
    Log("[VR][stereo] CALL_BEGIN stage=resources.name-allocate eye=" +
        std::string(eyeName));
    auto* managedName = UnityResolve::UnityType::String::New(
        eye == 0U ? "__GakumasVrQueueLeft" : "__GakumasVrQueueRight");
    using CreateGameObject = void (*)(void*, void*, void*);
    using AddComponent = void* (*)(void*, void*, void*);
    using DontDestroyOnLoad = void (*)(void*, void*);
    if (managedName == nullptr) {
        return FailStage("resources.name-allocation", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.name-allocate eye=" +
        std::string(eyeName));
    Log("[VR][stereo] CALL_BEGIN stage=resources.game-object-create eye=" +
        std::string(eyeName));
    if (!InvokeManagedVoid<CreateGameObject>(
            api_.internalCreateGameObject, eyeGameObjects_[eye], managedName)) {
        return FailStage("resources.internal-create-game-object", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.game-object-create eye=" +
        std::string(eyeName));
    gameObjectActive_[eye] = true;
    if (!SetGameObjectActive(eye, false)) {
        return FailStage("resources.initial-deactivate", eye);
    }
    Log("[VR][stereo] CALL_BEGIN stage=resources.camera-add eye=" +
        std::string(eyeName));
    if (!InvokeManagedResult<void*, AddComponent>(
            api_.addComponent, &eyeCameras_[eye], eyeGameObjects_[eye],
            api_.cameraReflectionType) || eyeCameras_[eye] == nullptr) {
        return FailStage("resources.add-camera", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.camera-add eye=" +
        std::string(eyeName));
    Log("[VR][stereo] CALL_BEGIN stage=resources.camera-root eye=" +
        std::string(eyeName));
    eyeCameraHandles_[eye] = CreateGcHandle(eyeCameras_[eye]);
    if (eyeCameraHandles_[eye] == nullptr) {
        return FailStage("resources.camera-gchandle", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.camera-root eye=" +
        std::string(eyeName));
    cameraEnabled_[eye] = true;
    if (!SetCameraEnabled(eye, false)) {
        return FailStage("resources.camera-initialize", eye);
    }
    if (!EnsureCameraDataComponents(eye)) {
        return false;
    }
    Log("[VR][stereo] CALL_BEGIN stage=resources.dont-destroy eye=" +
        std::string(eyeName));
    if (!InvokeManagedVoid<DontDestroyOnLoad>(
            api_.dontDestroyOnLoad, eyeGameObjects_[eye])) {
        return FailStage("resources.dont-destroy", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.dont-destroy eye=" +
        std::string(eyeName));
    SyncEyeAsMainCameraTag();
    return true;
}

bool UnityStereoRenderer::EnsureCameraDataComponents(std::size_t eye) noexcept {
    if (eye >= eyeCameras_.size() || eyeGameObjects_[eye] == nullptr) {
        return FailStage("resources.camera-data-index", eye);
    }
    using AddComponent = void* (*)(void*, void*, void*);
    using SetBool = void (*)(void*, bool, void*);
    const std::string eyeName = eye == 0U ? "left" : "right";
    if (eyeUniversalCameraData_[eye] == nullptr) {
        Log("[VR][stereo] CALL_BEGIN stage=resources.universal-data-add eye=" +
            eyeName);
        if (!InvokeManagedResult<void*, AddComponent>(
                api_.addComponent, &eyeUniversalCameraData_[eye],
                eyeGameObjects_[eye], api_.universalCameraDataReflectionType) ||
            eyeUniversalCameraData_[eye] == nullptr) {
            return FailStage("resources.universal-data-add", eye);
        }
        Log("[VR][stereo] CALL_OK stage=resources.universal-data-add eye=" +
            eyeName);
        eyeUniversalCameraDataHandles_[eye] =
            CreateGcHandle(eyeUniversalCameraData_[eye]);
        if (eyeUniversalCameraDataHandles_[eye] == nullptr) {
            return FailStage("resources.universal-data-root", eye);
        }
    }
    if (eyeVlAdditionalCameraData_[eye] == nullptr) {
        Log("[VR][stereo] CALL_BEGIN stage=resources.vlsrp-data-add eye=" +
            eyeName);
        if (!InvokeManagedResult<void*, AddComponent>(
                api_.addComponent, &eyeVlAdditionalCameraData_[eye],
                eyeGameObjects_[eye], api_.vlAdditionalCameraDataReflectionType) ||
            eyeVlAdditionalCameraData_[eye] == nullptr) {
            return FailStage("resources.vlsrp-data-add", eye);
        }
        Log("[VR][stereo] CALL_OK stage=resources.vlsrp-data-add eye=" +
            eyeName);
        eyeVlAdditionalCameraDataHandles_[eye] =
            CreateGcHandle(eyeVlAdditionalCameraData_[eye]);
        if (eyeVlAdditionalCameraDataHandles_[eye] == nullptr) {
            return FailStage("resources.vlsrp-data-root", eye);
        }
    }
    Log("[VR][stereo] CALL_BEGIN stage=resources.universal-data-safety eye=" +
        eyeName);
    if (!InvokeManagedVoid<SetBool>(
            api_.universalSetAllowXrRendering,
            eyeUniversalCameraData_[eye], false) ||
        !InvokeManagedVoid<SetBool>(
            api_.universalSetRenderShadows,
            eyeUniversalCameraData_[eye], false)) {
        return FailStage("resources.universal-data-safety", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.universal-data-safety eye=" +
        eyeName + " allowXR=0 renderShadows=0");
    return true;
}

bool UnityStereoRenderer::CreateRenderTarget(
    std::size_t eye,
    std::uint32_t width,
    std::uint32_t height,
    const char* family,
    void* descriptorSource,
    void** target,
    Il2CppGCHandle* handle,
    bool fatal) noexcept {
    const auto fail = [this, fatal](const char* stage, std::size_t failedEye) {
        if (fatal) {
            return FailStage(stage, failedEye);
        }
        Log("[VR][smaa-t2x] SMAA_T2X_FAULT stage=" + std::string(stage) +
            " eye=" + (failedEye == 0U ? "left" : "right") +
            " action=fallback");
        return false;
    };
    if (eye >= 2U || width == 0U || height == 0U || family == nullptr ||
        target == nullptr || handle == nullptr || *target != nullptr) {
        return fail("resources.target-arguments", eye);
    }
    std::ostringstream begin;
    begin << "[VR][stereo] CALL_BEGIN stage=resources.target-create family="
          << family << " eye=" << (eye == 0U ? "left" : "right")
          << " size=" << width << 'x' << height;
    Log(begin.str());
    Log("[VR][stereo] CALL_BEGIN stage=resources.target-allocate family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right"));
    *target = NewIl2CppObject(api_.renderTextureClass);
    if (*target == nullptr) {
        return fail("resources.target-allocation", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.target-allocate family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right"));
    Log("[VR][stereo] CALL_BEGIN stage=resources.target-root family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right"));
    *handle = CreateGcHandle(*target);
    if (*handle == nullptr) {
        return fail("resources.target-gchandle", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.target-root family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right"));
    int managedWidth = static_cast<int>(width);
    int managedHeight = static_cast<int>(height);
    int depth = 24;
    int format = 0;
    UnityRenderTextureDescriptorValue descriptor{};
    const bool cloneDescriptor = descriptorSource != nullptr;
    void* legacyConstructorArguments[]{
        &managedWidth, &managedHeight, &depth, &format};
    void* descriptorConstructorArguments[]{&descriptor};
    void** constructorArguments = legacyConstructorArguments;
    MethodRef constructor = api_.renderTextureConstructor;
    std::string exception;
    if (cloneDescriptor) {
        Log("[VR][stereo] CALL_BEGIN stage=resources.target-descriptor-read family=" +
            std::string(family) + " eye=" +
            (eye == 0U ? "left" : "right"));
        if (!ReadRenderTextureDescriptor(
                api_.renderTextureGetDescriptor, descriptorSource,
                api_.renderTextureDescriptorValueSize, &descriptor,
                &exception)) {
            Log("[VR][stereo] MANAGED_EXCEPTION stage=resources.target-descriptor-read detail=\"" +
                exception + '"');
            return fail("resources.target-descriptor-read", eye);
        }
        Log("[VR][stereo] SOURCE_FINAL_DESCRIPTOR eye=" +
            std::string(eye == 0U ? "left" : "right") + " " +
            DescriptorState(descriptor));
        descriptor.width = managedWidth;
        descriptor.height = managedHeight;
        descriptor.msaaSamples = 1;
        descriptor.volumeDepth = 1;
        descriptor.dimension = 2; // TextureDimension.Tex2D
        descriptor.vrUsage = 0;   // VRTextureUsage.None
        constexpr std::uint32_t kEyeTexture = 1U << 3U;
        constexpr std::uint32_t kNoResolvedColorSurface = 1U << 8U;
        constexpr std::uint32_t kDynamicallyScalable = 1U << 10U;
        constexpr std::uint32_t kBindMs = 1U << 11U;
        constexpr std::uint32_t kDynamicallyScalableExplicit = 1U << 17U;
        descriptor.flags &= ~(kEyeTexture | kNoResolvedColorSurface |
                              kDynamicallyScalable | kBindMs |
                              kDynamicallyScalableExplicit);
        constructorArguments = descriptorConstructorArguments;
        constructor = api_.renderTextureDescriptorConstructor;
        Log("[VR][stereo] EYE_FINAL_DESCRIPTOR_REQUEST eye=" +
            std::string(eye == 0U ? "left" : "right") + " " +
            DescriptorState(descriptor));
    }
    Log("[VR][stereo] CALL_BEGIN stage=resources.target-ctor family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right"));
    if (!RuntimeInvoke(constructor, *target,
                       constructorArguments, nullptr, &exception)) {
        Log("[VR][stereo] MANAGED_EXCEPTION stage=resources.target-ctor detail=\"" +
            exception + '"');
        return fail("resources.target-ctor", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.target-ctor family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right"));
    void* boxedCreated = nullptr;
    Log("[VR][stereo] CALL_BEGIN stage=resources.target-create-call family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right"));
    if (!RuntimeInvoke(api_.renderTextureCreate, *target, nullptr,
                       &boxedCreated, &exception)) {
        Log("[VR][stereo] MANAGED_EXCEPTION stage=resources.target-create detail=\"" +
            exception + '"');
        return fail("resources.target-create", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.target-create-call family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right"));
    bool created = false;
    Log("[VR][stereo] CALL_BEGIN stage=resources.target-unbox family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right"));
    if (!UnboxBoolean(boxedCreated, &created) || !created) {
        return fail("resources.target-create-result", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.target-unbox family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right"));
    using GetBool = bool (*)(void*, void*);
    using GetInt = int (*)(void*, void*);
    bool srgb = false;
    int graphicsFormat = 0;
    if (!InvokeManagedResult<bool, GetBool>(
            api_.renderTextureGetSrgb, &srgb, *target) ||
        !InvokeManagedResult<int, GetInt>(
            api_.renderTextureGetGraphicsFormat, &graphicsFormat, *target)) {
        return fail("resources.target-color-metadata", eye);
    }
    Log("[VR][stereo] CALL_OK stage=resources.target-create family=" +
        std::string(family) + " eye=" + (eye == 0U ? "left" : "right") +
        " srgb=" + (srgb ? "1" : "0") +
        " graphicsFormat=" + std::to_string(graphicsFormat));
    if (cloneDescriptor) {
        UnityRenderTextureDescriptorValue effectiveDescriptor{};
        if (!ReadRenderTextureDescriptor(
                api_.renderTextureGetDescriptor, *target,
                api_.renderTextureDescriptorValueSize, &effectiveDescriptor,
                &exception)) {
            return fail("resources.target-descriptor-verify", eye);
        }
        Log("[VR][stereo] EYE_FINAL_DESCRIPTOR_READY eye=" +
            std::string(eye == 0U ? "left" : "right") + " " +
            DescriptorState(effectiveDescriptor));
    }
    RecordLifetimeWrite(
        LifetimeWriteCategory::RenderTarget, family, *target, *handle,
        static_cast<std::intptr_t>(eye));
    return true;
}

bool UnityStereoRenderer::EnsureAdmissionTarget(std::size_t eye) noexcept {
    return eye < admissionTargets_.size() &&
        (admissionTargets_[eye] != nullptr ||
         CreateRenderTarget(eye, kAdmissionTargetWidth, kAdmissionTargetHeight, "admission",
                             nullptr, &admissionTargets_[eye],
                             &admissionTargetHandles_[eye]));
}

bool UnityStereoRenderer::RetireFullTargets(
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t generation) noexcept {
    Log("[VR][stereo] FULL_TARGET_RETIRE_BEGIN old=" +
        std::to_string(fullWidth_) + "x" + std::to_string(fullHeight_) +
        " new=" + std::to_string(width) + "x" + std::to_string(height) +
        " generation=" + std::to_string(generation));
    // No stage is armed here and no publish is pending, so the eye cameras own
    // nothing the pipeline is still reading. The mailbox holds its own staging
    // copies, so the OpenXR worker never reads these textures again.
    InvalidateUnityStereoFrame();
    using SetTarget = void (*)(void*, void*, void*);
    for (std::size_t eye = 0; eye < fullTargets_.size(); ++eye) {
        if (fullTargets_[eye] == nullptr) {
            continue;
        }
        if (cameraEnabled_[eye] && !SetCameraEnabled(eye, false)) {
            return FailStage("resources.full-target-disable", eye);
        }
        if (eyeCameras_[eye] != nullptr) {
            if (!InvokeManagedVoid<SetTarget>(
                    api_.cameraSetTargetTexture, eyeCameras_[eye], nullptr)) {
                return FailStage("resources.full-target-unbind", eye);
            }
            RecordLifetimeWrite(
                LifetimeWriteCategory::TargetTexture, "eye-unbind",
                eyeCameras_[eye], nullptr, static_cast<std::intptr_t>(eye));
        }
        std::string exception;
        if (!RuntimeInvoke(api_.renderTextureRelease, fullTargets_[eye], nullptr,
                           nullptr, &exception)) {
            Log("[VR][stereo] MANAGED_EXCEPTION stage=resources.full-target-release detail=\"" +
                exception + '"');
            return FailStage("resources.full-target-release", eye);
        }
        RecordLifetimeWrite(
            LifetimeWriteCategory::RenderTarget, "release", fullTargets_[eye],
            fullTargetHandles_[eye], static_cast<std::intptr_t>(eye));
        if (fullTargetHandles_[eye] != nullptr) {
            retiredFullTargetHandles_.push_back(fullTargetHandles_[eye]);
        }
        fullTargets_[eye] = nullptr;
        fullTargetHandles_[eye] = nullptr;
    }
    fullWidth_ = 0;
    fullHeight_ = 0;
    fullGeneration_ = 0;
    continuousStereo_ = false;
    sourceFingerprintValid_.fill(false);
    RetireSmaaT2xMotionCopyTargets("full-target-retired");
    ResetTemporalHistories("full-target-retired");
    Log("[VR][stereo] FULL_TARGET_RETIRED retiredHandles=" +
        std::to_string(retiredFullTargetHandles_.size()));
    return true;
}

bool UnityStereoRenderer::EnsureFullTarget(
    std::size_t eye,
    std::uint32_t width,
    std::uint32_t height,
    std::uint64_t generation) noexcept {
    if (eye >= fullTargets_.size()) {
        return FailStage("resources.full-target-index", eye);
    }
    if (fullTargets_[eye] != nullptr &&
        (width != fullWidth_ || height != fullHeight_) &&
        !RetireFullTargets(width, height, generation)) {
        return false;
    }
    if (fullTargets_[eye] != nullptr) {
        if (generation != fullGeneration_) {
            if (eye == 0U) {
                Log("[VR][stereo] FULL_TARGET_GENERATION_REBOUND old=" +
                    std::to_string(fullGeneration_) + " new=" +
                    std::to_string(generation) + " size=" +
                    std::to_string(width) + "x" + std::to_string(height));
                fullGeneration_ = generation;
                InvalidateUnityStereoFrame();
                continuousStereo_ = false;
                sourceFingerprintValid_.fill(false);
            }
        }
        return true;
    }
    if (fullWidth_ == 0U) {
        fullWidth_ = width;
        fullHeight_ = height;
        fullGeneration_ = generation;
    }
    if (width != fullWidth_ || height != fullHeight_ ||
        generation != fullGeneration_) {
        return FailStage("resources.full-target-generation", eye);
    }
    return CreateRenderTarget(eye, width, height, "full", latestSourceTarget_,
                              &fullTargets_[eye], &fullTargetHandles_[eye]);
}

bool UnityStereoRenderer::SetGameObjectActive(
    std::size_t eye,
    bool active) noexcept {
    if (eye >= eyeGameObjects_.size() || eyeGameObjects_[eye] == nullptr) {
        return !active;
    }
    using SetActive = void (*)(void*, bool, void*);
    std::ostringstream begin;
    begin << "[VR][stereo] CALL_BEGIN stage=game-object-set-active eye="
          << (eye == 0U ? "left" : "right") << " value=" << active;
    Log(begin.str());
    if (!InvokeManagedVoid<SetActive>(api_.gameObjectSetActive,
                                      eyeGameObjects_[eye], active)) {
        return false;
    }
    gameObjectActive_[eye] = active;
    RecordLifetimeWrite(
        LifetimeWriteCategory::EyeGameObject, "set-active",
        eyeGameObjects_[eye], nullptr, active ? 1 : 0);
    Log("[VR][stereo] CALL_OK stage=game-object-set-active eye=" +
        std::string(eye == 0U ? "left" : "right") +
        " value=" + (active ? "1" : "0"));
    return true;
}

bool UnityStereoRenderer::SetCameraEnabled(
    std::size_t eye,
    bool enabled) noexcept {
    if (eye >= eyeCameras_.size() || eyeCameras_[eye] == nullptr) {
        return !enabled;
    }
    using SetEnabled = void (*)(void*, bool, void*);
    Log("[VR][stereo] CALL_BEGIN stage=camera-set-enabled eye=" +
        std::string(eye == 0U ? "left" : "right") +
        " value=" + (enabled ? "1" : "0"));
    if (!InvokeManagedVoid<SetEnabled>(api_.behaviourSetEnabled,
                                       eyeCameras_[eye], enabled)) {
        return false;
    }
    cameraEnabled_[eye] = enabled;
    RecordLifetimeWrite(
        LifetimeWriteCategory::EyeCamera, "set-enabled", eyeCameras_[eye],
        nullptr, enabled ? 1 : 0);
    Log("[VR][stereo] CALL_OK stage=camera-set-enabled eye=" +
        std::string(eye == 0U ? "left" : "right") +
        " value=" + (enabled ? "1" : "0"));
    return true;
}

bool UnityStereoRenderer::ReadCameraEnabled(void* camera, bool* enabled) noexcept {
    if (camera == nullptr || enabled == nullptr ||
        !api_.behaviourGetEnabled.Ready()) {
        return false;
    }
    using GetEnabled = bool (*)(void*, void*);
    return InvokeManagedResult<bool, GetEnabled>(
        api_.behaviourGetEnabled, enabled, camera);
}

bool UnityStereoRenderer::WriteSourceCameraEnabled(
    void* camera,
    bool enabled) noexcept {
    if (camera == nullptr || !api_.behaviourSetEnabled.Ready() ||
        !IsUnityManagedObjectAlive(camera)) {
        return false;
    }
    using SetEnabled = void (*)(void*, bool, void*);
    if (!InvokeManagedVoid<SetEnabled>(api_.behaviourSetEnabled, camera, enabled)) {
        return false;
    }
    RecordLifetimeWrite(
        LifetimeWriteCategory::SourceCamera, "set-enabled", camera, nullptr,
        enabled ? 1 : 0);
    return true;
}

void UnityStereoRenderer::ApplySourceTinyMode(void* sourceCamera) noexcept {
    if (sourceCamera == nullptr || !IsUnityManagedObjectAlive(sourceCamera) ||
        !api_.cameraGetTargetTexture.Ready() ||
        !api_.cameraSetTargetTexture.Ready()) {
        return;
    }
    // Scene unload between Lives destroys the native RenderTexture while
    // the GC handle keeps the managed shell rooted. Binding a destroyed RT
    // reads back as null, which looked like SOURCE_TINY_REBIND_FIGHT
    // gameTarget=0x0 forever — the .118 "dead after the first Live" bug.
    if (tinySourceTarget_ != nullptr &&
        !IsUnityManagedObjectAlive(tinySourceTarget_)) {
        if (tinySourceTargetHandle_ != nullptr) {
            // Same policy as retired eye textures (.69): keep the shell
            // rooted so a collected wrapper never dangles inside Unity.
            retiredFullTargetHandles_.push_back(tinySourceTargetHandle_);
        }
        tinySourceTarget_ = nullptr;
        tinySourceTargetHandle_ = nullptr;
        tinyModeActive_ = false;
        tinySourceCamera_ = nullptr;
        tinySavedTarget_ = nullptr;
        tinyAppliedLogged_ = false;
        tinyRebindLogged_ = false;
        Log("[VR][stereo] SOURCE_TINY_TARGET_RECREATE reason=destroyed");
    }
    if (tinySourceTarget_ == nullptr) {
        if (tinyTargetCreateFailed_) {
            return;
        }
        if (!CreateRenderTarget(0U, 128U, 128U, "tiny-source", nullptr,
                                &tinySourceTarget_, &tinySourceTargetHandle_)) {
            // CreateRenderTarget already logged; do not retry every Tick.
            tinyTargetCreateFailed_ = true;
            return;
        }
    }
    if (tinyModeActive_ && tinySourceCamera_ != sourceCamera) {
        LiftSourceTinyMode("source-boundary");
    }
    using GetTarget = void* (*)(void*, void*);
    using SetTarget = void (*)(void*, void*, void*);
    void* current = nullptr;
    if (!InvokeManagedResult<void*, GetTarget>(
            api_.cameraGetTargetTexture, &current, sourceCamera)) {
        return;
    }
    if (tinyModeActive_ && current == tinySourceTarget_) {
        return;
    }
    // First apply, or the game rebound the target mid-Live: remember the
    // game's target so lift and the source-boundary check see it, then
    // point the source at the dummy RT.
    tinySavedTarget_ = current;
    if (!InvokeManagedVoid<SetTarget>(api_.cameraSetTargetTexture,
                                     sourceCamera, tinySourceTarget_)) {
        Log("[VR][stereo] SOURCE_TINY failed=write");
        return;
    }
    RecordLifetimeWrite(
        LifetimeWriteCategory::TargetTexture, "source-tiny-bind",
        sourceCamera, tinySourceTarget_);
    if (!tinyModeActive_) {
        tinyModeActive_ = true;
        tinySourceCamera_ = sourceCamera;
        if (!tinyAppliedLogged_) {
            tinyAppliedLogged_ = true;
            Log("[VR][stereo] SOURCE_TINY_APPLIED size=128x128 savedTarget=0x" +
                std::to_string(reinterpret_cast<std::uintptr_t>(current)));
        }
        return;
    }
    if (!tinyRebindLogged_) {
        tinyRebindLogged_ = true;
        Log("[VR][stereo] SOURCE_TINY_REBIND_FIGHT gameTarget=0x" +
            std::to_string(reinterpret_cast<std::uintptr_t>(current)));
    }
}

void UnityStereoRenderer::LiftSourceTinyMode(const char* reason) noexcept {
    if (!tinyModeActive_) {
        return;
    }
    const char* tag = reason != nullptr ? reason : "unknown";
    if (tinySourceCamera_ != nullptr &&
        IsUnityManagedObjectAlive(tinySourceCamera_) &&
        api_.cameraGetTargetTexture.Ready() &&
        api_.cameraSetTargetTexture.Ready()) {
        using GetTarget = void* (*)(void*, void*);
        using SetTarget = void (*)(void*, void*, void*);
        void* current = nullptr;
        if (InvokeManagedResult<void*, GetTarget>(
                api_.cameraGetTargetTexture, &current, tinySourceCamera_) &&
            // current == nullptr covers a destroyed tiny RT (dead objects
            // read back as null); we were the last owner, restore the game's
            // target either way.
            (current == tinySourceTarget_ || current == nullptr) &&
            current != tinySavedTarget_) {
            if (!InvokeManagedVoid<SetTarget>(api_.cameraSetTargetTexture,
                                             tinySourceCamera_,
                                             tinySavedTarget_)) {
                Log("[VR][stereo] SOURCE_TINY_LIFTED failed=write");
            } else {
                RecordLifetimeWrite(
                    LifetimeWriteCategory::TargetTexture, "source-tiny-lift",
                    tinySourceCamera_, tinySavedTarget_);
            }
        }
    }
    tinyModeActive_ = false;
    tinySourceCamera_ = nullptr;
    tinySavedTarget_ = nullptr;
    tinyAppliedLogged_ = false;
    tinyRebindLogged_ = false;
    Log(std::string("[VR][stereo] SOURCE_TINY_LIFTED reason=") + tag);
    // Lifts also run outside SyncSourceCameraSuppression (stage-terminal,
    // stage-failed, release); republish so transparency never outlives tiny.
    PublishGripTransparency();
}

void UnityStereoRenderer::RestoreSourceCamera(const char* reason) noexcept {
    // Tiny mode lifts wherever a full restore would run (scene-ineligible,
    // source-boundary, stage-terminal, release, toggle-off), including when
    // the source was never enabled=false-suppressed.
    LiftSourceTinyMode(reason);
    if (!sourceCameraSuppressed_ || suppressedSourceCamera_ == nullptr) {
        return;
    }
    const char* tag = reason != nullptr ? reason : "unknown";
    // Untag the eye before the source re-enables: native FindMainCamera
    // must never see two enabled MainCamera-tagged cameras at once.
    sourceCameraSuppressed_ = false;
    SyncEyeAsMainCameraTag();
    if (IsUnityManagedObjectAlive(suppressedSourceCamera_)) {
        if (!WriteSourceCameraEnabled(suppressedSourceCamera_, true)) {
            Log(std::string("[VR][stereo] SOURCE_CAMERA_RESTORED failed=write reason=") +
                tag);
            sourceCameraSuppressed_ = true;
            SyncEyeAsMainCameraTag();
            return;
        }
    }
    Log(std::string("[VR][stereo] SOURCE_CAMERA_RESTORED reason=") + tag);
    suppressedSourceCamera_ = nullptr;
    sourceSuppressLogged_ = false;
    sourceReenableFightLogged_ = false;
    sourceHistoryResetLatched_ = false;
    sourceCutSignatureValid_ = false;
    copyFromSkippedLogged_ = false;
    // History on the source is a minute stale. Drop any reset backlog
    // queued while it was disabled (frames would drain through Grip as
    // fat outlines), then pulse a single fresh reset.
    ClearSourceTemporalReset(latestSourceUniversalData_);
    RequestOneFrameSourceTemporalReset(latestSourceUniversalData_);
    ResetTemporalHistories("source-camera-restored");
    // Restores also run outside SyncSourceCameraSuppression; see
    // LiftSourceTinyMode's tail publish.
    PublishGripTransparency();
}

void UnityStereoRenderer::SuppressSourceCamera(void* sourceCamera) noexcept {
    if (sourceCamera == nullptr || !IsUnityManagedObjectAlive(sourceCamera)) {
        return;
    }
    if (sourceCameraSuppressed_ && suppressedSourceCamera_ == sourceCamera) {
        bool enabled = true;
        if (ReadCameraEnabled(sourceCamera, &enabled) && enabled) {
            if (!sourceReenableFightLogged_) {
                sourceReenableFightLogged_ = true;
                Log("[VR][stereo] SOURCE_CAMERA_REENABLE_FIGHT");
            }
            if (!WriteSourceCameraEnabled(sourceCamera, false)) {
                return;
            }
        }
        expectOwnedEyeDof_ = true;
        return;
    }
    if (sourceCameraSuppressed_ && suppressedSourceCamera_ != sourceCamera) {
        RestoreSourceCamera("source-boundary");
    }
    if (!WriteSourceCameraEnabled(sourceCamera, false)) {
        Log("[VR][stereo] SOURCE_CAMERA_SUPPRESSED failed=write");
        return;
    }
    suppressedSourceCamera_ = sourceCamera;
    sourceCameraSuppressed_ = true;
    expectOwnedEyeDof_ = true;
    if (!sourceSuppressLogged_) {
        sourceSuppressLogged_ = true;
        Log("[VR][stereo] SOURCE_CAMERA_SUPPRESSED camera=0x" +
            std::to_string(reinterpret_cast<std::uintptr_t>(sourceCamera)));
        ResetSkyTaaSamples();
    }
    SyncEyeAsMainCameraTag();
}

void UnityStereoRenderer::SyncSourceCameraSuppression(void* sourceCamera) noexcept {
    RefreshLiveSourcePhotoProtection();
    if (LiveSourcePhotoProtectionActive()) {
        // Preserve the official full-size capture surface during the Produce
        // automatic-photo task. Reuse the established
        // target/enabled/tag restore path so the tracked source stays alive.
        RestoreSourceCamera("produce-auto-photo-protection");
        PublishGripTransparency();
        return;
    }
    const bool healthy =
        stage_ == LadderStage::StereoFull &&
        (continuousStereo_ || publishedFrames_ != 0U || stageArmed_) &&
        SceneReadyAllowsStereoPublish();
    const bool wantDisable =
        healthy && GakumasLocal::Config::vrDisableSourceCamera;
    const bool wantTiny = healthy && !wantDisable &&
        GakumasLocal::Config::vrSourceCameraTiny;
    if (wantDisable) {
        // Full disable wins over tiny mode; SuppressSourceCamera's
        // RestoreSourceCamera("source-boundary") path lifts tiny too, but
        // the steady case never reaches it, so lift explicitly.
        LiftSourceTinyMode("full-disable");
        SuppressSourceCamera(sourceCamera);
        SyncEyeAsMainCameraTag();
        PublishGripTransparency();
        return;
    }
    if (wantTiny) {
        if (sourceCameraSuppressed_) {
            RestoreSourceCamera("tiny-handoff");
        }
        ApplySourceTinyMode(sourceCamera);
        PublishGripTransparency();
        return;
    }
    LiftSourceTinyMode(!GakumasLocal::Config::vrSourceCameraTiny
                           ? "toggle-off"
                           : "stereo-unhealthy");
    RestoreSourceCamera(
        !GakumasLocal::Config::vrDisableSourceCamera ? "toggle-off"
                                                     : "stereo-unhealthy");
    PublishGripTransparency();
}

void UnityStereoRenderer::PublishGripTransparency() noexcept {
    // Reads the suppression state the apply/lift paths maintain; never
    // writes source-camera or target state itself. Config off, tiny lifted,
    // or source restored all publish "opaque" on the next pass. The UI
    // render-pass clear override below owns its own saved-state writes.
    const bool armed = GakumasLocal::Config::vrGripPanelTransparent &&
        (tinyModeActive_ || sourceCameraSuppressed_);
    // Runs on every publish pass (armed-state flips are rarer): keeps the
    // clear override applied across pass recreation and restores it the
    // moment transparency disarms, including the Lift/Restore tail calls.
    SyncGripUiPassClear(armed);
    if (armed == gripTransparencyArmed_.load(std::memory_order_relaxed)) {
        return;
    }
    if (armed) {
        // A few Execute-hook fixup lines per arm prove which branch the
        // game chose; steady state stays quiet. Diagnostics sessions get
        // ~2 s of per-frame branch sequence instead (.254: measures the
        // fb/native alternation pattern per screen).
        gripExecFixupLogBudget_.store(
            GakumasLocal::Config::vrDiagnosticsStartupEnabled ? 120 : 4,
            std::memory_order_relaxed);
        if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
            // Plate census: immediate pass plus a settled pass ~3 s later.
            gripPlateCensusRuns_ = 2;
            gripPlateCensusDelay_ = 0;
        }
    }
    gripTransparencyArmed_.store(armed, std::memory_order_release);
    // Relay to the VR runtime: the Present hook clears the backbuffer to
    // transparent black and the worker flags the quad for source-alpha
    // blending while this is armed.
    SetGripPanelTransparent(armed);
    std::ostringstream stream;
    stream << "[VR][stereo] "
           << (armed ? "GRIP_TRANSPARENCY_APPLIED"
                     : "GRIP_TRANSPARENCY_LIFTED")
           << " config="
           << (GakumasLocal::Config::vrGripPanelTransparent ? 1 : 0)
           << " tiny=" << (tinyModeActive_ ? 1 : 0)
           << " suppressed=" << (sourceCameraSuppressed_ ? 1 : 0);
    Log(stream.str());
}

// Defined later in this translation unit.
std::string ManagedObjectName(void* object) noexcept;
std::vector<void*> FindManagedObjectsOfType(UnityResolve::Class* klass) noexcept;

void UnityStereoRenderer::SyncGripUiPassClear(bool armed) noexcept {
    if (!armed) {
        if (gripIntermediateLastPublished_ != 0U) {
            // Release the Present-hook clear targets the moment
            // transparency disarms so live rendering is never cleared.
            SetGripPanelClearTargets(nullptr, 0);
            gripIntermediateLastPublished_ = 0U;
            Log("[VR][stereo] GRIP_TRANSPARENCY_RT_PUBLISH count=0 "
                "reason=disarm");
        }
        gripPlateCensusRuns_ = 0;
        if (!gripPlateHides_.empty()) {
            RestoreGripPlateHide("disarm");
        }
        if (!gripScreenBackgroundMutes_.empty()) {
            RestoreGripScreenBackgroundMute("disarm");
        }
        if (!gripAdvUiMutes_.empty()) {
            RestoreGripAdvUiMute("disarm");
        }
        // .261: the force-all tier must never outlive the armed state.
        if (gripForceAllForExec_.exchange(false, std::memory_order_acq_rel)) {
            Log("[VR][stereo] GRIP_TRANSPARENCY_FORCE_CONTEXT forceAll=0 "
                "reason=disarm");
        }
        gripUiPassRescanCountdown_ = 0;
        return;
    }
    if (gripUiPassRescanCountdown_ > 0) {
        --gripUiPassRescanCountdown_;
        return;
    }
    // Publish passes run at Tick cadence: rescan roughly twice a second so
    // the Execute-hook offsets stay resolved, late-allocated VL surfaces
    // get published, and the plate hides / official mutes re-assert.
    gripUiPassRescanCountdown_ = 30;
    ApplyGripUiPassClear();
    MaybeRunGripPlateCensus();
}

void UnityStereoRenderer::MaybeRunGripPlateCensus() noexcept {
    if (gripPlateCensusRuns_ <= 0) {
        return;
    }
    if (gripPlateCensusDelay_ > 0) {
        --gripPlateCensusDelay_;
        return;
    }
    --gripPlateCensusRuns_;
    // ~3 s between passes: the second one catches late-loaded screen UI.
    gripPlateCensusDelay_ = 6;
    RunGripPlateCensus(gripPlateCensusRuns_ == 1 ? "arm" : "settled");
}

namespace {

bool Il2cppClassDerivesFrom(void* klass, void* baseKlass) noexcept {
    void* current = klass;
    for (int depth = 0; current != nullptr && depth < 12; ++depth) {
        if (current == baseKlass) {
            return true;
        }
        current = UnityResolve::Invoke<void*>(
            "il2cpp_class_get_parent", current);
    }
    return false;
}

} // namespace

void UnityStereoRenderer::RunGripPlateCensus(const char* phase) noexcept {
    auto* graphicClass = Il2cppUtils::GetClass(
        "UnityEngine.UI.dll", "UnityEngine.UI", "Graphic");
    auto* imageClass = Il2cppUtils::GetClass(
        "UnityEngine.UI.dll", "UnityEngine.UI", "Image");
    auto* rawImageClass = Il2cppUtils::GetClass(
        "UnityEngine.UI.dll", "UnityEngine.UI", "RawImage");
    if (graphicClass == nullptr) {
        return;
    }
    static const std::int32_t colorOffset = FindNamedFieldOffset(
        graphicClass, {"m_Color"}, {"UnityEngine.Color", "Color"});
    static MethodRef imageGetSprite = MethodReference(ResolveStrictMethodAny(
        "UnityEngine.UI.dll", "UnityEngine.UI", "Image", "get_sprite",
        {
            {false, "UnityEngine.Sprite", {}},
            {false, "Sprite", {}},
        }));
    static MethodRef rawImageGetTexture =
        MethodReference(ResolveStrictMethodAny(
            "UnityEngine.UI.dll", "UnityEngine.UI", "RawImage", "get_texture",
            {
                {false, "UnityEngine.Texture", {}},
                {false, "Texture", {}},
            }));
    static MethodRef gameObjectActiveInHierarchy =
        MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
            "get_activeInHierarchy", false, "System.Boolean", {}));
    // .243: full-stretch detection. The lobby census drowned in widget
    // noise (hits=165, caps exhausted by pager dots and buttons) and the
    // real board never got logged. Anchors (0,0)-(1,1) mark full-screen
    // boards; Vector2 returns are 8 bytes → RAX on win64, ABI-safe.
    static MethodRef rectGetAnchorMin = MethodReference(ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RectTransform",
        "get_anchorMin",
        {
            {false, "UnityEngine.Vector2", {}},
            {false, "Vector2", {}},
        }));
    static MethodRef rectGetAnchorMax = MethodReference(ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RectTransform",
        "get_anchorMax",
        {
            {false, "UnityEngine.Vector2", {}},
            {false, "Vector2", {}},
        }));
    if (colorOffset < 0 || !api_.behaviourGetEnabled.Ready() ||
        !api_.componentGetGameObject.Ready()) {
        Log("[VR][stereo] GRIP_TRANSPARENCY_PLATE_API miss colorOffset=" +
            std::to_string(colorOffset));
        return;
    }
    using GetPtr = void* (*)(void*, void*);
    using GetBool = bool (*)(void*, void*);
    struct PlateColor {
        float r = 0.0F;
        float g = 0.0F;
        float b = 0.0F;
        float a = 0.0F;
    };
    std::size_t scanned = 0;
    std::size_t hits = 0;
    std::size_t logged = 0;
    std::size_t advLogged = 0;
    std::size_t boardLogged = 0;
    constexpr std::size_t kLogCap = 24;
    // .237: the gray cinematic board and the still-black vertical dialogue
    // proved the color-gated census misses mid-tone plates. Everything
    // under an Adv wrapper/engine gets logged regardless of color.
    constexpr std::size_t kAdvLogCap = 40;
    constexpr std::size_t kBoardLogCap = 16;
    for (void* graphic : FindManagedObjectsOfType(graphicClass)) {
        if (graphic == nullptr || !IsUnityManagedObjectAlive(graphic)) {
            continue;
        }
        ++scanned;
        PlateColor color{};
        if (!ReadManagedField(graphic, colorOffset, &color)) {
            continue;
        }
        void* klass =
            UnityResolve::Invoke<void*>("il2cpp_object_get_class", graphic);
        const char* className = klass != nullptr
            ? UnityResolve::Invoke<const char*>(
                  "il2cpp_class_get_name", klass)
            : nullptr;
        // White text is the biggest false-positive class; plates are
        // Image/RawImage-derived boards.
        if (className != nullptr &&
            std::strstr(className, "Text") != nullptr) {
            continue;
        }
        bool enabled = false;
        if (!InvokeManagedResult<bool, GetBool>(
                api_.behaviourGetEnabled, &enabled, graphic) ||
            !enabled) {
            continue;
        }
        void* gameObject = nullptr;
        if (!InvokeManagedResult<void*, GetPtr>(
                api_.componentGetGameObject, &gameObject, graphic) ||
            gameObject == nullptr) {
            continue;
        }
        bool activeInHierarchy = true;
        if (gameObjectActiveInHierarchy.Ready()) {
            InvokeManagedResult<bool, GetBool>(
                gameObjectActiveInHierarchy, &activeInHierarchy, gameObject);
        }
        if (!activeInHierarchy) {
            continue;
        }
        // Parent path (up to 6 hops, reaches the Adv roots): Object.name on
        // a Transform yields the GameObject name.
        std::string path = ManagedObjectName(graphic);
        if (api_.componentGetTransform.Ready() &&
            api_.transformGetParent.Ready()) {
            void* transform = nullptr;
            InvokeManagedResult<void*, GetPtr>(
                api_.componentGetTransform, &transform, graphic);
            for (int hop = 0; hop < 6 && transform != nullptr; ++hop) {
                void* parent = nullptr;
                if (!InvokeManagedResult<void*, GetPtr>(
                        api_.transformGetParent, &parent, transform) ||
                    parent == nullptr) {
                    break;
                }
                path = ManagedObjectName(parent) + "/" + path;
                transform = parent;
            }
        }
        // Home joins the color-blind subtree census (.240: the lobby white
        // survives direct-fb forcing, so it is content inside the home UI
        // that every color gate missed — same lesson as BackgroundBase).
        const bool advScoped = path.find("Adv") != std::string::npos ||
            path.find("ADVEngine") != std::string::npos ||
            (path.find("Home") != std::string::npos && color.a >= 0.3F);
        // .243: full-stretch boards get their own priority lane; the caps
        // above are routinely exhausted by widget noise before any real
        // backdrop board is reached. .244: anchors are PARENT-relative, so
        // a gauge fill stretched inside its small container false-hits —
        // require the graphic AND its parent chain to stretch, up to a
        // Canvas-named ancestor (fixed-size widget containers break the
        // chain and get filtered).
        bool fullStretch = false;
        if (rectGetAnchorMin.Ready() && rectGetAnchorMax.Ready() &&
            api_.componentGetTransform.Ready() &&
            api_.transformGetParent.Ready()) {
            struct Vec2 {
                float x;
                float y;
            };
            using GetVec2 = Vec2 (*)(void*, void*);
            const auto isStretched = [&](void* rectTransform) -> bool {
                Vec2 anchorMin{1.0F, 1.0F};
                Vec2 anchorMax{0.0F, 0.0F};
                return InvokeManagedResult<Vec2, GetVec2>(
                           rectGetAnchorMin, &anchorMin, rectTransform) &&
                    InvokeManagedResult<Vec2, GetVec2>(
                        rectGetAnchorMax, &anchorMax, rectTransform) &&
                    anchorMin.x <= 0.01F && anchorMin.y <= 0.01F &&
                    anchorMax.x >= 0.99F && anchorMax.y >= 0.99F;
            };
            void* rectTransform = nullptr;
            InvokeManagedResult<void*, GetPtr>(
                api_.componentGetTransform, &rectTransform, graphic);
            if (rectTransform != nullptr && isStretched(rectTransform)) {
                // .245: button effects nest 5+ stretched containers deep,
                // so the walk must actually REACH a Canvas ancestor with
                // every level stretched — otherwise it is a widget.
                bool chainStretched = true;
                bool reachedCanvas = false;
                void* current = rectTransform;
                for (int hop = 0; hop < 10 && chainStretched; ++hop) {
                    void* parent = nullptr;
                    if (!InvokeManagedResult<void*, GetPtr>(
                            api_.transformGetParent, &parent, current) ||
                        parent == nullptr) {
                        break;
                    }
                    const std::string parentName = ManagedObjectName(parent);
                    if (parentName.find("Canvas") != std::string::npos) {
                        reachedCanvas = true;
                        break;
                    }
                    if (!isStretched(parent)) {
                        chainStretched = false;
                    }
                    current = parent;
                }
                fullStretch = chainStretched && reachedCanvas;
            }
        }
        const bool boardHit =
            fullStretch && color.a >= 0.3F && boardLogged < kBoardLogCap;
        // .236: the lobby's white wash never showed at the .235 a>0.85
        // threshold — translucent white boards stacked over the cleared
        // black base read as white. Census down to a>=0.3, tone marked
        // -translucent below 0.85.
        const bool nearBlack =
            color.r < 0.2F && color.g < 0.2F && color.b < 0.2F;
        const bool nearWhite =
            color.r > 0.85F && color.g > 0.85F && color.b > 0.85F;
        const bool plateHit =
            color.a >= 0.3F && (nearBlack || nearWhite);
        if (!plateHit && !boardHit &&
            !(advScoped && advLogged < kAdvLogCap)) {
            continue;
        }
        // Sprite / texture identity: the "universal plate" hypothesis says
        // these boards share an asset.
        std::string assetName = "-";
        if (imageClass != nullptr && imageGetSprite.Ready() &&
            Il2cppClassDerivesFrom(klass, imageClass->address)) {
            void* sprite = nullptr;
            if (InvokeManagedResult<void*, GetPtr>(
                    imageGetSprite, &sprite, graphic) &&
                sprite != nullptr) {
                assetName = ManagedObjectName(sprite);
            }
        } else if (
            rawImageClass != nullptr && rawImageGetTexture.Ready() &&
            Il2cppClassDerivesFrom(klass, rawImageClass->address)) {
            void* texture = nullptr;
            if (InvokeManagedResult<void*, GetPtr>(
                    rawImageGetTexture, &texture, graphic) &&
                texture != nullptr) {
                assetName = ManagedObjectName(texture);
            }
        }
        if (boardHit) {
            ++boardLogged;
            std::ostringstream stream;
            stream << "[VR][stereo] GRIP_TRANSPARENCY_BOARD phase=" << phase
                   << " class=" << (className != nullptr ? className : "?")
                   << " color=" << color.r << ',' << color.g << ',' << color.b
                   << ',' << color.a << " asset=\"" << assetName
                   << "\" path=\"" << path << "\"";
            Log(stream.str());
        }
        if (advScoped && advLogged < kAdvLogCap) {
            ++advLogged;
            std::ostringstream stream;
            stream << "[VR][stereo] GRIP_TRANSPARENCY_ADV phase=" << phase
                   << " class=" << (className != nullptr ? className : "?")
                   << " color=" << color.r << ',' << color.g << ',' << color.b
                   << ',' << color.a << " asset=\"" << assetName
                   << "\" path=\"" << path << "\"";
            Log(stream.str());
        }
        if (!plateHit) {
            continue;
        }
        ++hits;
        if (logged >= kLogCap) {
            continue;
        }
        ++logged;
        std::ostringstream stream;
        stream << "[VR][stereo] GRIP_TRANSPARENCY_PLATE phase=" << phase
               << " class=" << (className != nullptr ? className : "?")
               << " tone=" << (nearBlack ? "black" : "white")
               << (color.a < 0.85F ? "-translucent" : "")
               << " color=" << color.r << ',' << color.g << ',' << color.b
               << ',' << color.a << " asset=\"" << assetName << "\" path=\""
               << path << "\"";
        Log(stream.str());
    }
    std::ostringstream stream;
    stream << "[VR][stereo] GRIP_TRANSPARENCY_PLATE_SUMMARY phase=" << phase
           << " scanned=" << scanned << " hits=" << hits
           << " logged=" << logged << " adv=" << advLogged
           << " board=" << boardLogged;
    Log(stream.str());
    RunGripCameraCensus(phase);
}

void UnityStereoRenderer::RunGripCameraCensus(const char* phase) noexcept {
    // .237: the cinematic-mode probe showed the whole backbuffer painted a
    // uniform opaque gray-blue (0xff585549 = RGB 73,85,88) every frame with
    // all our clears active — the painter is a camera SolidColor clear.
    // Census every camera's clear setup to name it.
    auto* cameraClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera");
    if (cameraClass == nullptr || !api_.behaviourGetEnabled.Ready() ||
        !api_.cameraGetTargetTexture.Ready()) {
        return;
    }
    static UnityResolve::Method* clearFlagsMethod = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_clearFlags",
        {
            {false, "UnityEngine.CameraClearFlags", {}},
            {false, "CameraClearFlags", {}},
        });
    static UnityResolve::Method* backgroundMethod = ResolveStrictMethodAny(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Camera",
        "get_backgroundColor_Injected",
        {
            {false, "System.Void", {"UnityEngine.Color&"}},
            {true, "System.Void", {"System.IntPtr", "UnityEngine.Color&"}},
        });
    static const MethodRef getClearFlags = MethodReference(clearFlagsMethod);
    static const MethodRef getBackground = MethodReference(backgroundMethod);
    static const bool backgroundUsesNativeSelf =
        backgroundMethod != nullptr && backgroundMethod->static_function;
    using GetInt = std::int32_t (*)(void*, void*);
    using GetFloat = float (*)(void*, void*);
    using GetPtr = void* (*)(void*, void*);
    using GetBool = bool (*)(void*, void*);
    using ColorAccess = void (*)(void*, void*, void*);
    for (void* camera : FindManagedObjectsOfType(cameraClass)) {
        if (camera == nullptr || !IsUnityManagedObjectAlive(camera)) {
            continue;
        }
        bool enabled = false;
        InvokeManagedResult<bool, GetBool>(
            api_.behaviourGetEnabled, &enabled, camera);
        void* target = nullptr;
        InvokeManagedResult<void*, GetPtr>(
            api_.cameraGetTargetTexture, &target, camera);
        std::int32_t clearFlags = -1;
        if (getClearFlags.Ready()) {
            InvokeManagedResult<std::int32_t, GetInt>(
                getClearFlags, &clearFlags, camera);
        }
        float background[4]{};
        if (getBackground.Ready()) {
            void* self = backgroundUsesNativeSelf
                ? ReadUnityNativePointer(camera)
                : camera;
            if (self != nullptr) {
                InvokeManagedVoid<ColorAccess>(
                    getBackground, self, background);
            }
        }
        float depth = 0.0F;
        if (api_.cameraGetDepth.Ready()) {
            InvokeManagedResult<float, GetFloat>(
                api_.cameraGetDepth, &depth, camera);
        }
        std::ostringstream stream;
        stream << "[VR][stereo] GRIP_TRANSPARENCY_CAMERA phase=" << phase
               << " name=\"" << ManagedObjectName(camera) << "\" enabled="
               << (enabled ? 1 : 0) << " target=" << (target != nullptr ? 1 : 0)
               << " depth=" << depth << " clearFlags=" << clearFlags
               << " bg=" << background[0] << ',' << background[1] << ','
               << background[2] << ',' << background[3];
        Log(stream.str());
    }
    RunGripSnapshotCensus(phase);
}

void UnityStereoRenderer::RunGripSnapshotCensus(const char* phase) noexcept {
    // .257: name the frozen-frame family. Per .245/.254 the frozen popup /
    // desk-view / transition backdrops are point-in-time snapshots
    // (CaptureUtility Texture2D or per-presenter capture fields) shown by
    // ordinary RawImages — unreachable by the RT clears, invisible to the
    // color-gated plate census. Lane 1 logs every active RawImage whose
    // texture is a Texture2D (the snapshot signature); lane 2 logs every
    // VLSRPTargetImage with its Target/Captured type. Diagnostics-only.
    auto* rawImageClass = Il2cppUtils::GetClass(
        "UnityEngine.UI.dll", "UnityEngine.UI", "RawImage");
    if (rawImageClass == nullptr || !api_.behaviourGetEnabled.Ready() ||
        !api_.componentGetGameObject.Ready()) {
        return;
    }
    static MethodRef rawImageGetTexture =
        MethodReference(ResolveStrictMethodAny(
            "UnityEngine.UI.dll", "UnityEngine.UI", "RawImage", "get_texture",
            {
                {false, "UnityEngine.Texture", {}},
                {false, "Texture", {}},
            }));
    static MethodRef gameObjectActiveInHierarchy =
        MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
            "get_activeInHierarchy", false, "System.Boolean", {}));
    if (!rawImageGetTexture.Ready()) {
        return;
    }
    using GetPtr = void* (*)(void*, void*);
    using GetBool = bool (*)(void*, void*);
    const auto parentPath = [this](void* component,
                                   const std::string& leaf) -> std::string {
        std::string path = leaf;
        if (!api_.componentGetTransform.Ready() ||
            !api_.transformGetParent.Ready()) {
            return path;
        }
        void* transform = nullptr;
        InvokeManagedResult<void*, GetPtr>(
            api_.componentGetTransform, &transform, component);
        for (int hop = 0; hop < 6 && transform != nullptr; ++hop) {
            void* parent = nullptr;
            if (!InvokeManagedResult<void*, GetPtr>(
                    api_.transformGetParent, &parent, transform) ||
                parent == nullptr) {
                break;
            }
            path = ManagedObjectName(parent) + "/" + path;
            transform = parent;
        }
        return path;
    };
    std::size_t tex2dLogged = 0;
    constexpr std::size_t kTex2dLogCap = 16;
    for (void* graphic : FindManagedObjectsOfType(rawImageClass)) {
        if (graphic == nullptr || !IsUnityManagedObjectAlive(graphic)) {
            continue;
        }
        bool enabled = false;
        if (!InvokeManagedResult<bool, GetBool>(
                api_.behaviourGetEnabled, &enabled, graphic) ||
            !enabled) {
            continue;
        }
        void* gameObject = nullptr;
        if (!InvokeManagedResult<void*, GetPtr>(
                api_.componentGetGameObject, &gameObject, graphic) ||
            gameObject == nullptr) {
            continue;
        }
        bool activeInHierarchy = true;
        if (gameObjectActiveInHierarchy.Ready()) {
            InvokeManagedResult<bool, GetBool>(
                gameObjectActiveInHierarchy, &activeInHierarchy, gameObject);
        }
        if (!activeInHierarchy) {
            continue;
        }
        void* texture = nullptr;
        if (!InvokeManagedResult<void*, GetPtr>(
                rawImageGetTexture, &texture, graphic) ||
            texture == nullptr) {
            continue;
        }
        void* textureKlass =
            UnityResolve::Invoke<void*>("il2cpp_object_get_class", texture);
        const char* textureClassName = textureKlass != nullptr
            ? UnityResolve::Invoke<const char*>(
                  "il2cpp_class_get_name", textureKlass)
            : nullptr;
        if (textureClassName == nullptr ||
            std::strcmp(textureClassName, "Texture2D") != 0) {
            continue;
        }
        // .258: the first run drowned in authored icon/thumbnail art
        // (overflow>100, same lesson as the .243 BOARD lane). Snapshot
        // captures are runtime-created: unnamed, or screen-region sized.
        // Authored art always ships an asset name and icons are small.
        static MethodRef texture2dGetWidth =
            MethodReference(ResolveStrictMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D",
                "get_width", false, "System.Int32", {}));
        static MethodRef texture2dGetHeight =
            MethodReference(ResolveStrictMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Texture2D",
                "get_height", false, "System.Int32", {}));
        using GetInt = std::int32_t (*)(void*, void*);
        std::int32_t width = 0;
        std::int32_t height = 0;
        if (texture2dGetWidth.Ready() && texture2dGetHeight.Ready()) {
            InvokeManagedResult<std::int32_t, GetInt>(
                texture2dGetWidth, &width, texture);
            InvokeManagedResult<std::int32_t, GetInt>(
                texture2dGetHeight, &height, texture);
        }
        const std::string textureName = ManagedObjectName(texture);
        const bool snapshotSignature =
            textureName.empty() || (width >= 400 && height >= 400);
        if (!snapshotSignature) {
            continue;
        }
        if (++tex2dLogged > kTex2dLogCap) {
            continue;
        }
        std::ostringstream stream;
        stream << "[VR][stereo] GRIP_TRANSPARENCY_TEX2D phase=" << phase
               << " tex=\"" << textureName << "\" size=" << width << 'x'
               << height << " path=\""
               << parentPath(graphic, ManagedObjectName(graphic)) << "\"";
        Log(stream.str());
    }
    if (tex2dLogged > kTex2dLogCap) {
        Log("[VR][stereo] GRIP_TRANSPARENCY_TEX2D overflow=" +
            std::to_string(tex2dLogged - kTex2dLogCap));
    }
    // Lane 2: VLSRPTargetImage type census (Target=0 live composite,
    // Captured=1 snapshot display).
    // .258: TypeDefIndex 40454 falls in the vl-unity.Runtime.dll image
    // (starts at 40267 per the dump image table) — the .257 guess list
    // missed it (VLTI_API class=0 on hardware).
    static UnityResolve::Class* targetImageClass = [] {
        for (const char* assembly :
             {"vl-unity.Runtime.dll", "VL.Rendering.dll",
              "Assembly-CSharp.dll", "campus-submodule.Runtime.dll"}) {
            auto* klass = Il2cppUtils::GetClass(
                assembly, "VL.Rendering", "VLSRPTargetImage");
            if (klass != nullptr) {
                return klass;
            }
        }
        return static_cast<UnityResolve::Class*>(nullptr);
    }();
    static bool vltiLogged = false;
    if (!vltiLogged) {
        vltiLogged = true;
        Log(std::string("[VR][stereo] GRIP_TRANSPARENCY_VLTI_API class=") +
            (targetImageClass != nullptr ? "1" : "0"));
    }
    if (targetImageClass == nullptr) {
        return;
    }
    static const std::int32_t textureTypeOffset = FindNamedFieldOffset(
        targetImageClass, {"_textureType"}, {});
    static const std::int32_t imageOffset =
        FindNamedFieldOffset(targetImageClass, {"_image"}, {});
    if (textureTypeOffset < 0) {
        return;
    }
    std::size_t vltiCount = 0;
    constexpr std::size_t kVltiLogCap = 16;
    for (void* targetImage : FindManagedObjectsOfType(targetImageClass)) {
        if (targetImage == nullptr ||
            !IsUnityManagedObjectAlive(targetImage)) {
            continue;
        }
        std::int32_t textureType = -1;
        ReadManagedField(targetImage, textureTypeOffset, &textureType);
        bool imageEnabled = false;
        std::string leaf = ManagedObjectName(targetImage);
        if (imageOffset >= 0) {
            void* image = nullptr;
            if (ReadManagedField(targetImage, imageOffset, &image) &&
                image != nullptr && IsUnityManagedObjectAlive(image)) {
                InvokeManagedResult<bool, GetBool>(
                    api_.behaviourGetEnabled, &imageEnabled, image);
            }
        }
        if (++vltiCount > kVltiLogCap) {
            continue;
        }
        std::ostringstream stream;
        stream << "[VR][stereo] GRIP_TRANSPARENCY_VLTI phase=" << phase
               << " type=" << textureType
               << " imgEnabled=" << (imageEnabled ? 1 : 0) << " path=\""
               << parentPath(targetImage, leaf) << "\"";
        Log(stream.str());
    }
    if (vltiCount > kVltiLogCap) {
        Log("[VR][stereo] GRIP_TRANSPARENCY_VLTI overflow=" +
            std::to_string(vltiCount - kVltiLogCap));
    }
    // Lane 3 (.260): full Graphic dump of the HomePage background subtree
    // — no color or class gates at all. Four census generations missed
    // the lobby white; .259 proved it is neither the 3dTargetImage nor a
    // Root RawImage, so name EVERYTHING that draws under BackgroundCanvas
    // in one pass (the subtree is small; cap only as a safety net).
    auto* graphicClass = Il2cppUtils::GetClass(
        "UnityEngine.UI.dll", "UnityEngine.UI", "Graphic");
    if (graphicClass == nullptr) {
        return;
    }
    static const std::int32_t homeColorOffset = FindNamedFieldOffset(
        graphicClass, {"m_Color"}, {"UnityEngine.Color", "Color"});
    struct HomeColor {
        float r = 0.0F;
        float g = 0.0F;
        float b = 0.0F;
        float a = 0.0F;
    };
    std::size_t homeLogged = 0;
    constexpr std::size_t kHomeLogCap = 40;
    for (void* graphic : FindManagedObjectsOfType(graphicClass)) {
        if (graphic == nullptr || !IsUnityManagedObjectAlive(graphic)) {
            continue;
        }
        const std::string path =
            parentPath(graphic, ManagedObjectName(graphic));
        if (path.find("BackgroundCanvas") == std::string::npos ||
            path.find("Home") == std::string::npos) {
            continue;
        }
        bool enabled = false;
        InvokeManagedResult<bool, GetBool>(
            api_.behaviourGetEnabled, &enabled, graphic);
        HomeColor color{};
        if (homeColorOffset >= 0) {
            ReadManagedField(graphic, homeColorOffset, &color);
        }
        void* klass =
            UnityResolve::Invoke<void*>("il2cpp_object_get_class", graphic);
        const char* className = klass != nullptr
            ? UnityResolve::Invoke<const char*>(
                  "il2cpp_class_get_name", klass)
            : "?";
        if (++homeLogged > kHomeLogCap) {
            continue;
        }
        std::ostringstream stream;
        stream << "[VR][stereo] GRIP_TRANSPARENCY_HOMEBG_CENSUS phase="
               << phase << " class=" << (className != nullptr ? className : "?")
               << " enabled=" << (enabled ? 1 : 0) << " color=" << color.r
               << ',' << color.g << ',' << color.b << ',' << color.a
               << " path=\"" << path << "\"";
        Log(stream.str());
    }
    if (homeLogged > kHomeLogCap) {
        Log("[VR][stereo] GRIP_TRANSPARENCY_HOMEBG_CENSUS overflow=" +
            std::to_string(homeLogged - kHomeLogCap));
    }
}

namespace {

// Campus code ships in campus-submodule.Runtime.dll with an Assembly-CSharp
// fallback (same pattern as the Campus.Rendering lookups).
UnityResolve::Class* ResolveCampusUiRendererClass(const char* name) noexcept {
    auto* klass = Il2cppUtils::GetClass(
        "campus-submodule.Runtime.dll", "Campus.Common.UIRenderer", name);
    if (klass == nullptr) {
        klass = Il2cppUtils::GetClass(
            "Assembly-CSharp.dll", "Campus.Common.UIRenderer", name);
    }
    return klass;
}

// Il2CppArray ABI: klass + monitor + bounds + max_length, elements at 0x20.
constexpr std::int32_t kIl2cppArrayDataOffset = 0x20;

struct GripUiPassColor {
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 0.0F;
};

// Published (release) once the pass field offsets are resolved so the
// render-thread Execute-hook prefix never reads half-initialized offsets.
std::atomic<UnityStereoRenderer*> g_gripUiPassFixupOwner{nullptr};

} // namespace

bool GripUiTransparencyArmed() noexcept {
    auto* owner = g_gripUiPassFixupOwner.load(std::memory_order_acquire);
    return owner != nullptr && owner->GripTransparencyArmed();
}

bool GripUiPassExecuteEnter(
    void* pass, UnityStereoRenderer::GripUiPassExecState& saved) noexcept {
    auto* owner = g_gripUiPassFixupOwner.load(std::memory_order_acquire);
    if (owner == nullptr) {
        return false;
    }
    return owner->GripUiPassExecuteEnter(pass, saved);
}

void GripUiPassExecuteExit(
    void* pass,
    const UnityStereoRenderer::GripUiPassExecState& saved) noexcept {
    auto* owner = g_gripUiPassFixupOwner.load(std::memory_order_acquire);
    if (owner != nullptr) {
        owner->GripUiPassExecuteExit(pass, saved);
    }
}

bool UnityStereoRenderer::ResolveGripUiPassApi() noexcept {
    // .256: the .224 feature-field writes and live-pass stamping are gone
    // (superseded — the Execute wrapper writes the pass fields ahead of
    // every use and restores them after, so nothing persists and disarm
    // needs no restore). Only the four pass-instance offsets the wrapper
    // reads/writes are resolved here; drawFb is required (the whole .240+
    // regime lives on that branch flip).
    if (gripUiPassApiResolved_) {
        return gripUiPassIndexOffset_ >= 0 &&
            gripUiPassNeedsClearOffset_ >= 0 &&
            gripUiPassClearColorOffset_ >= 0 && gripUiPassDrawFbOffset_ >= 0;
    }
    auto* passClass = ResolveCampusUiRendererClass("UIRenderPass");
    if (passClass == nullptr) {
        // The class may not be loaded yet; keep retrying, complain once.
        if (!gripUiPassApiLogged_) {
            gripUiPassApiLogged_ = true;
            Log("[VR][stereo] GRIP_TRANSPARENCY_UIPASS_API miss pass=0");
        }
        return false;
    }
    gripUiPassIndexOffset_ = FindNamedFieldOffset(
        passClass, {"_passIndex"}, {"System.Int32", "Int32"});
    gripUiPassNeedsClearOffset_ = FindNamedFieldOffset(
        passClass, {"_needsClear"}, {"System.Boolean", "Boolean"});
    gripUiPassClearColorOffset_ = FindNamedFieldOffset(
        passClass, {"_clearColor"}, {"UnityEngine.Color", "Color"});
    gripUiPassDrawFbOffset_ = FindNamedFieldOffset(
        passClass, {"<DrawFrameBuffer>k__BackingField", "_drawFrameBuffer"},
        {"System.Boolean", "Boolean"});
    gripUiPassApiResolved_ = true;
    std::ostringstream stream;
    stream << "[VR][stereo] GRIP_TRANSPARENCY_UIPASS_API passIndex="
           << gripUiPassIndexOffset_
           << " passNeedsClear=" << gripUiPassNeedsClearOffset_
           << " passClearColor=" << gripUiPassClearColorOffset_
           << " passDrawFb=" << gripUiPassDrawFbOffset_;
    Log(stream.str());
    const bool ready = gripUiPassIndexOffset_ >= 0 &&
        gripUiPassNeedsClearOffset_ >= 0 &&
        gripUiPassClearColorOffset_ >= 0 && gripUiPassDrawFbOffset_ >= 0;
    if (ready) {
        g_gripUiPassFixupOwner.store(this, std::memory_order_release);
    }
    return ready;
}

bool UnityStereoRenderer::GripUiPassExecuteEnter(
    void* pass, GripUiPassExecState& saved) noexcept {
    saved.modified = false;
    if (pass == nullptr ||
        !gripTransparencyArmed_.load(std::memory_order_relaxed)) {
        return false;
    }
    const std::int32_t indexOffset = gripUiPassIndexOffset_;
    const std::int32_t needsClearOffset = gripUiPassNeedsClearOffset_;
    const std::int32_t clearColorOffset = gripUiPassClearColorOffset_;
    const std::int32_t drawFbOffset = gripUiPassDrawFbOffset_;
    if (indexOffset < 0 || needsClearOffset < 0 || clearColorOffset < 0 ||
        drawFbOffset < 0) {
        return false;
    }
    if (!ReadManagedField(pass, indexOffset, &saved.passIndex) ||
        !ReadManagedField(pass, needsClearOffset, &saved.needsClear) ||
        !ReadManagedField(
            pass, clearColorOffset,
            reinterpret_cast<GripUiPassColor*>(saved.clearColor)) ||
        !ReadManagedField(pass, drawFbOffset, &saved.drawFrameBuffer)) {
        return false;
    }
    GripTraceUiPolicy(pass, saved.passIndex, saved.drawFrameBuffer,
                      saved.needsClear, saved.clearColor);
    // .262: back to the .240 unconditional forcing — the ONLY regime that
    // never ghosted. The .261 fb-frame-only stamp produced a .249-class
    // mix ghost on the 特別指導 card-upgrade exit (screens whose branch
    // alternates per frame show stamped-transparent fb frames against
    // native frames' stale intermediate), experimentally confirming the
    // .253 mixing verdict. Every pass goes onto the direct framebuffer
    // path: pass0 clears transparent, later passes get _passIndex=0 (the
    // .225-disasm blit gate skips the stale-intermediate blit) and
    // needsClear=false so they do not erase the layers below. The tier
    // flags stay published as diagnostics only.
    WriteManagedField(pass, drawFbOffset, true);
    if (saved.passIndex == 0) {
        WriteManagedField(pass, needsClearOffset, true);
        WriteManagedField(
            pass, clearColorOffset, GripUiPassColor{0.0F, 0.0F, 0.0F, 0.0F});
    } else {
        WriteManagedField(pass, indexOffset, std::int32_t{0});
        WriteManagedField(pass, needsClearOffset, false);
    }
    saved.modified = true;
    int budget = gripExecFixupLogBudget_.load(std::memory_order_relaxed);
    while (budget > 0 &&
           !gripExecFixupLogBudget_.compare_exchange_weak(
               budget, budget - 1, std::memory_order_relaxed)) {
    }
    if (budget > 0) {
        std::ostringstream stream;
        stream << "[VR][stereo] GRIP_TRANSPARENCY_EXEC passIndex="
               << saved.passIndex << " drawFb="
               << (saved.drawFrameBuffer ? 1 : 0) << " forced=direct-fb";
        Log(stream.str());
    }
    return true;
}

void UnityStereoRenderer::GripUiPassExecuteExit(
    void* pass, const GripUiPassExecState& saved) noexcept {
    if (pass == nullptr || !saved.modified) {
        return;
    }
    // Restore the authored fields after the original Execute consumed the
    // forced ones; enqueue rewrites DrawFrameBuffer anyway, but _passIndex
    // must be exact for the blit gate on unarmed frames.
    WriteManagedField(pass, gripUiPassIndexOffset_, saved.passIndex);
    WriteManagedField(pass, gripUiPassNeedsClearOffset_, saved.needsClear);
    WriteManagedField(
        pass, gripUiPassClearColorOffset_,
        GripUiPassColor{
            saved.clearColor[0], saved.clearColor[1], saved.clearColor[2],
            saved.clearColor[3]});
    WriteManagedField(pass, gripUiPassDrawFbOffset_, saved.drawFrameBuffer);
}

void UnityStereoRenderer::ApplyGripUiPassClear() noexcept {
    // .256: keeps the Execute-hook offsets resolved (arms the wrapper via
    // g_gripUiPassFixupOwner) and drives the armed-state consumers. The
    // .224 feature writes / live-pass stamping / disarm restore that used
    // to live here were removed as superseded: the Execute wrapper writes
    // the pass fields ahead of every use and restores them right after.
    if (!ResolveGripUiPassApi()) {
        return;
    }
    PublishGripIntermediateClearTargets();
    ApplyGripPlateHide();
    ApplyGripScreenBackgroundMute();
    ApplyGripAdvUiMute();
}

void UnityStereoRenderer::ApplyGripAdvUiMute() noexcept {
    // Official getters on Campus.ADV.UIManager (dump TypeDefIndex 34934).
    // .255 model (evidence/stereo.254-latched-blur/): the produce-context
    // white column is the aspect-fitted ADV canvas background stack —
    // ContentBackground shows an on-demand capture we zero, uncovering the
    // authored null-texture white "Root" RawImages on the _root /
    // _playerControlRoot containers. CapturedImage is the crossfade
    // element (mutes a white transition flash while armed).
    // .255: UIManager lives in ADV.Runtime.dll (TypeDefIndex 34934 in the
    // 34655+ image) — the .248 resolves targeted the wrong assembly and
    // never fired (ADVUI_API captured=0; proven live in the .249 run).
    static UnityResolve::Class* uiManagerClass = [] {
        auto* klass = Il2cppUtils::GetClass(
            "ADV.Runtime.dll", "Campus.ADV", "UIManager");
        if (klass == nullptr) {
            klass = Il2cppUtils::GetClass(
                "Assembly-CSharp.dll", "Campus.ADV", "UIManager");
        }
        return klass;
    }();
    static MethodRef getCapturedImage = MethodReference(ResolveStrictMethodAny(
        "ADV.Runtime.dll", "Campus.ADV", "UIManager",
        "get_CapturedImage",
        {
            {false, "UnityEngine.UI.RawImage", {}},
            {false, "RawImage", {}},
        }));
    static MethodRef getContentBackground =
        MethodReference(ResolveStrictMethodAny(
            "ADV.Runtime.dll", "Campus.ADV", "UIManager",
            "get_ContentBackground",
            {
                {false, "UnityEngine.UI.RawImage", {}},
                {false, "RawImage", {}},
            }));
    // The white Root quads are plain RawImages on the container
    // GameObjects, unreachable through any getter — read the serialized
    // container fields and GetComponent(RawImage) on them.
    static const std::int32_t rootOffset =
        FindNamedFieldOffset(uiManagerClass, {"_root"}, {});
    static const std::int32_t playerControlRootOffset =
        FindNamedFieldOffset(uiManagerClass, {"_playerControlRoot"}, {});
    static void* rawImageReflectionType = [] {
        auto* rawImageClass = Il2cppUtils::GetClass(
            "UnityEngine.UI.dll", "UnityEngine.UI", "RawImage");
        return rawImageClass != nullptr ? rawImageClass->GetType() : nullptr;
    }();
    static bool apiLogged = false;
    if (!apiLogged) {
        apiLogged = true;
        std::ostringstream stream;
        stream << "[VR][stereo] GRIP_TRANSPARENCY_ADVUI_API class="
               << (uiManagerClass != nullptr ? 1 : 0)
               << " captured=" << (getCapturedImage.Ready() ? 1 : 0)
               << " contentBg=" << (getContentBackground.Ready() ? 1 : 0)
               << " rootOffset=" << rootOffset
               << " pcRootOffset=" << playerControlRootOffset
               << " rawImageType=" << (rawImageReflectionType != nullptr ? 1 : 0)
               << " getComponent="
               << (api_.componentGetComponent.Ready() ? 1 : 0);
        Log(stream.str());
    }
    if (uiManagerClass == nullptr || !api_.behaviourSetEnabled.Ready() ||
        !api_.behaviourGetEnabled.Ready()) {
        return;
    }
    gripAdvUiMutes_.erase(
        std::remove_if(
            gripAdvUiMutes_.begin(),
            gripAdvUiMutes_.end(),
            [](const GripAdvUiMute& entry) {
                return !IsUnityManagedObjectAlive(entry.graphic);
            }),
        gripAdvUiMutes_.end());
    using GetPtr = void* (*)(void*, void*);
    using GetComponentFn = void* (*)(void*, void*, void*);
    using GetBool = bool (*)(void*, void*);
    using SetBool = void (*)(void*, bool, void*);
    static MethodRef advGameObjectActiveInHierarchy =
        MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
            "get_activeInHierarchy", false, "System.Boolean", {}));
    const std::array<const MethodRef*, 2> getters{
        &getCapturedImage, &getContentBackground};
    const std::array<std::int32_t, 2> containerOffsets{
        rootOffset, playerControlRootOffset};
    // .260: while enumerating, decide whether any ADV is visibly open
    // (root container activeInHierarchy) — the Execute-forcing gate.
    // Dormant engines parked on schedule screens keep instances alive
    // but their roots inactive, which is exactly the .253 gate trap.
    bool anyAdvVisible = false;
    for (void* uiManager : FindManagedObjectsOfType(uiManagerClass)) {
        if (uiManager == nullptr || !IsUnityManagedObjectAlive(uiManager)) {
            continue;
        }
        if (!anyAdvVisible && rootOffset >= 0 &&
            api_.componentGetGameObject.Ready() &&
            advGameObjectActiveInHierarchy.Ready()) {
            void* rootContainer = nullptr;
            if (ReadManagedField(uiManager, rootOffset, &rootContainer) &&
                rootContainer != nullptr &&
                IsUnityManagedObjectAlive(rootContainer)) {
                void* rootGameObject = nullptr;
                bool rootActive = false;
                if (InvokeManagedResult<void*, GetPtr>(
                        api_.componentGetGameObject, &rootGameObject,
                        rootContainer) &&
                    rootGameObject != nullptr &&
                    InvokeManagedResult<bool, GetBool>(
                        advGameObjectActiveInHierarchy, &rootActive,
                        rootGameObject) &&
                    rootActive) {
                    anyAdvVisible = true;
                }
            }
        }
        // Candidate graphics: the two official getters plus the RawImage
        // component on each authored container.
        std::array<void*, 4> graphics{};
        std::size_t graphicCount = 0;
        for (const MethodRef* getter : getters) {
            if (!getter->Ready()) {
                continue;
            }
            void* graphic = nullptr;
            if (InvokeManagedResult<void*, GetPtr>(
                    *getter, &graphic, uiManager) &&
                graphic != nullptr && IsUnityManagedObjectAlive(graphic)) {
                graphics[graphicCount++] = graphic;
            }
        }
        if (rawImageReflectionType != nullptr &&
            api_.componentGetComponent.Ready()) {
            for (const std::int32_t offset : containerOffsets) {
                if (offset < 0) {
                    continue;
                }
                void* container = nullptr;
                if (!ReadManagedField(uiManager, offset, &container) ||
                    container == nullptr ||
                    !IsUnityManagedObjectAlive(container)) {
                    continue;
                }
                void* graphic = nullptr;
                if (InvokeManagedResult<void*, GetComponentFn>(
                        api_.componentGetComponent, &graphic, container,
                        rawImageReflectionType) &&
                    graphic != nullptr && IsUnityManagedObjectAlive(graphic)) {
                    graphics[graphicCount++] = graphic;
                }
            }
        }
        for (std::size_t index = 0; index < graphicCount; ++index) {
            void* graphic = graphics[index];
            const auto tracked = std::find_if(
                gripAdvUiMutes_.begin(),
                gripAdvUiMutes_.end(),
                [graphic](const GripAdvUiMute& entry) {
                    return entry.graphic == graphic;
                });
            if (tracked == gripAdvUiMutes_.end()) {
                GripAdvUiMute entry{};
                entry.graphic = graphic;
                InvokeManagedResult<bool, GetBool>(
                    api_.behaviourGetEnabled, &entry.wasEnabled, graphic);
                gripAdvUiMutes_.push_back(entry);
                Log("[VR][stereo] GRIP_TRANSPARENCY_ADVUI mute=\"" +
                    ManagedObjectName(graphic) + "\" wasEnabled=" +
                    (entry.wasEnabled ? "1" : "0"));
            }
            // Re-mute per rescan: the engine re-enables these on scene
            // transitions and capture requests.
            InvokeManagedVoid<SetBool>(
                api_.behaviourSetEnabled, graphic, false);
        }
    }
    // .259: the lobby stage. The VLTI census named the home 3D as a
    // UI element — Campus.OutGame.HomePageViewBase._backgroundImage
    // (VLSRPTargetImage "3dTargetImage" under BackgroundCanvas/Root)
    // displays the VL composite we clear, and the authored Root plate
    // behind it is the white suspect. While armed, the RawImages on
    // _backgroundRoot and on the _backgroundImage GameObject go dark
    // through the same tracked-mute list; disarm restores them and the
    // desktop home view returns.
    static UnityResolve::Class* homePageClass = [] {
        auto* klass = Il2cppUtils::GetClass(
            "Assembly-CSharp.dll", "Campus.OutGame", "HomePageViewBase");
        if (klass == nullptr) {
            klass = Il2cppUtils::GetClass(
                "campus-submodule.Runtime.dll", "Campus.OutGame",
                "HomePageViewBase");
        }
        return klass;
    }();
    static const std::int32_t homeBackgroundRootOffset =
        FindNamedFieldOffset(homePageClass, {"_backgroundRoot"}, {});
    static const std::int32_t homeBackgroundImageOffset =
        FindNamedFieldOffset(homePageClass, {"_backgroundImage"}, {});
    static const std::int32_t homeIsShowOffset = FindNamedFieldOffset(
        homePageClass, {"_isShow"}, {"System.Boolean", "Boolean"});
    static bool homeApiLogged = false;
    if (!homeApiLogged) {
        homeApiLogged = true;
        std::ostringstream stream;
        stream << "[VR][stereo] GRIP_TRANSPARENCY_HOMEBG_API class="
               << (homePageClass != nullptr ? 1 : 0)
               << " rootOffset=" << homeBackgroundRootOffset
               << " imageOffset=" << homeBackgroundImageOffset
               << " isShowOffset=" << homeIsShowOffset;
        Log(stream.str());
    }
    bool anyHomeVisible = false;
    if (homePageClass != nullptr && rawImageReflectionType != nullptr &&
        api_.componentGetComponent.Ready()) {
        for (void* homePage : FindManagedObjectsOfType(homePageClass)) {
            if (homePage == nullptr || !IsUnityManagedObjectAlive(homePage)) {
                continue;
            }
            if (!anyHomeVisible && homeIsShowOffset >= 0) {
                bool isShow = false;
                if (ReadManagedField(homePage, homeIsShowOffset, &isShow) &&
                    isShow) {
                    anyHomeVisible = true;
                }
            }
            const std::array<std::int32_t, 2> homeOffsets{
                homeBackgroundRootOffset, homeBackgroundImageOffset};
            for (const std::int32_t offset : homeOffsets) {
                if (offset < 0) {
                    continue;
                }
                void* component = nullptr;
                if (!ReadManagedField(homePage, offset, &component) ||
                    component == nullptr ||
                    !IsUnityManagedObjectAlive(component)) {
                    continue;
                }
                void* graphic = nullptr;
                if (!InvokeManagedResult<void*, GetComponentFn>(
                        api_.componentGetComponent, &graphic, component,
                        rawImageReflectionType) ||
                    graphic == nullptr ||
                    !IsUnityManagedObjectAlive(graphic)) {
                    continue;
                }
                const auto tracked = std::find_if(
                    gripAdvUiMutes_.begin(),
                    gripAdvUiMutes_.end(),
                    [graphic](const GripAdvUiMute& entry) {
                        return entry.graphic == graphic;
                    });
                if (tracked == gripAdvUiMutes_.end()) {
                    GripAdvUiMute entry{};
                    entry.graphic = graphic;
                    InvokeManagedResult<bool, GetBool>(
                        api_.behaviourGetEnabled, &entry.wasEnabled, graphic);
                    gripAdvUiMutes_.push_back(entry);
                    Log("[VR][stereo] GRIP_TRANSPARENCY_HOMEBG mute=\"" +
                        ManagedObjectName(graphic) + "\" wasEnabled=" +
                        (entry.wasEnabled ? "1" : "0"));
                }
                InvokeManagedVoid<SetBool>(
                    api_.behaviourSetEnabled, graphic, false);
            }
        }
    }
    // Legacy tier is diagnostic only since .262.
    const bool forceAll = anyAdvVisible || anyHomeVisible;
    const bool wasForceAll = gripForceAllForExec_.exchange(
        forceAll, std::memory_order_acq_rel);
    if (wasForceAll != forceAll) {
        Log(std::string("[VR][stereo] GRIP_TRANSPARENCY_FORCE_CONTEXT adv=") +
            (anyAdvVisible ? "1" : "0") + " home=" +
            (anyHomeVisible ? "1" : "0") + " forceAll=" +
            (forceAll ? "1" : "0"));
    }
}

void UnityStereoRenderer::RestoreGripAdvUiMute(const char* reason) noexcept {
    if (gripAdvUiMutes_.empty()) {
        return;
    }
    std::size_t restored = 0;
    std::size_t dead = 0;
    using SetBool = void (*)(void*, bool, void*);
    for (auto& entry : gripAdvUiMutes_) {
        if (entry.graphic == nullptr ||
            !IsUnityManagedObjectAlive(entry.graphic)) {
            ++dead;
            continue;
        }
        if (api_.behaviourSetEnabled.Ready() &&
            InvokeManagedVoid<SetBool>(
                api_.behaviourSetEnabled, entry.graphic, entry.wasEnabled)) {
            ++restored;
        }
    }
    gripAdvUiMutes_.clear();
    std::ostringstream stream;
    stream << "[VR][stereo] GRIP_TRANSPARENCY_ADVUI_RESTORE reason="
           << (reason != nullptr ? reason : "unknown")
           << " restored=" << restored << " dead=" << dead;
    Log(stream.str());
}

void UnityStereoRenderer::ApplyGripScreenBackgroundMute() noexcept {
    // Official API path: CampusScreenCommonView.SetBackgroundRootActive is
    // the game's own backdrop toggle (evidence/stereo.219-grip-transparency
    // dossier, .247). Covers every plate color the name-matched hides
    // could not enumerate.
    // .255: CampusScreenCommonView lives in Assembly-CSharp.dll
    // (TypeDefIndex 286 < campus-submodule's 28555 start) — the .247
    // method resolve targeted the wrong assembly and never fired
    // (SCREENBG_API set=0; proven live in the .249 run).
    static UnityResolve::Class* commonViewClass = [] {
        auto* klass = Il2cppUtils::GetClass(
            "Assembly-CSharp.dll", "Campus", "CampusScreenCommonView");
        if (klass == nullptr) {
            klass = Il2cppUtils::GetClass(
                "campus-submodule.Runtime.dll", "Campus",
                "CampusScreenCommonView");
        }
        return klass;
    }();
    static MethodRef setBackgroundRootActive = [] {
        auto* method = ResolveStrictMethod(
            "Assembly-CSharp.dll", "Campus", "CampusScreenCommonView",
            "SetBackgroundRootActive", false, "System.Void",
            {"System.Boolean"});
        if (method == nullptr) {
            method = ResolveStrictMethod(
                "campus-submodule.Runtime.dll", "Campus",
                "CampusScreenCommonView", "SetBackgroundRootActive", false,
                "System.Void", {"System.Boolean"});
        }
        return MethodReference(method);
    }();
    static const std::int32_t backgroundRootOffset =
        FindNamedFieldOffset(commonViewClass, {"_backgroundRoot"}, {});
    static bool apiLogged = false;
    if (!apiLogged) {
        apiLogged = true;
        Log(std::string("[VR][stereo] GRIP_TRANSPARENCY_SCREENBG_API class=") +
            (commonViewClass != nullptr ? "1" : "0") + " set=" +
            (setBackgroundRootActive.Ready() ? "1" : "0") +
            " rootOffset=" + std::to_string(backgroundRootOffset));
    }
    if (commonViewClass == nullptr || !setBackgroundRootActive.Ready() ||
        !api_.gameObjectGetActiveSelf.Ready()) {
        return;
    }
    gripScreenBackgroundMutes_.erase(
        std::remove_if(
            gripScreenBackgroundMutes_.begin(),
            gripScreenBackgroundMutes_.end(),
            [](const GripScreenBackgroundMute& entry) {
                return !IsUnityManagedObjectAlive(entry.commonView);
            }),
        gripScreenBackgroundMutes_.end());
    using SetBool = void (*)(void*, bool, void*);
    using GetBool = bool (*)(void*, void*);
    for (void* commonView : FindManagedObjectsOfType(commonViewClass)) {
        if (commonView == nullptr || !IsUnityManagedObjectAlive(commonView)) {
            continue;
        }
        const auto tracked = std::find_if(
            gripScreenBackgroundMutes_.begin(),
            gripScreenBackgroundMutes_.end(),
            [commonView](const GripScreenBackgroundMute& entry) {
                return entry.commonView == commonView;
            });
        if (tracked == gripScreenBackgroundMutes_.end()) {
            GripScreenBackgroundMute entry{};
            entry.commonView = commonView;
            entry.wasActive = true;
            if (backgroundRootOffset >= 0) {
                void* backgroundRoot = nullptr;
                if (ReadManagedField(
                        commonView, backgroundRootOffset, &backgroundRoot) &&
                    backgroundRoot != nullptr &&
                    IsUnityManagedObjectAlive(backgroundRoot)) {
                    InvokeManagedResult<bool, GetBool>(
                        api_.gameObjectGetActiveSelf, &entry.wasActive,
                        backgroundRoot);
                }
            }
            gripScreenBackgroundMutes_.push_back(entry);
            Log("[VR][stereo] GRIP_TRANSPARENCY_SCREENBG mute=\"" +
                ManagedObjectName(commonView) + "\" wasActive=" +
                (entry.wasActive ? "1" : "0"));
        }
        // Re-mute every rescan: screens re-activate their backdrops on
        // transitions.
        InvokeManagedVoid<SetBool>(
            setBackgroundRootActive, commonView, false);
    }
}

void UnityStereoRenderer::RestoreGripScreenBackgroundMute(
    const char* reason) noexcept {
    if (gripScreenBackgroundMutes_.empty()) {
        return;
    }
    static MethodRef setBackgroundRootActive = [] {
        auto* method = ResolveStrictMethod(
            "Assembly-CSharp.dll", "Campus", "CampusScreenCommonView",
            "SetBackgroundRootActive", false, "System.Void",
            {"System.Boolean"});
        if (method == nullptr) {
            method = ResolveStrictMethod(
                "campus-submodule.Runtime.dll", "Campus",
                "CampusScreenCommonView", "SetBackgroundRootActive", false,
                "System.Void", {"System.Boolean"});
        }
        return MethodReference(method);
    }();
    std::size_t restored = 0;
    std::size_t dead = 0;
    using SetBool = void (*)(void*, bool, void*);
    for (auto& entry : gripScreenBackgroundMutes_) {
        if (entry.commonView == nullptr ||
            !IsUnityManagedObjectAlive(entry.commonView)) {
            ++dead;
            continue;
        }
        if (setBackgroundRootActive.Ready() &&
            InvokeManagedVoid<SetBool>(
                setBackgroundRootActive, entry.commonView,
                entry.wasActive)) {
            ++restored;
        }
    }
    gripScreenBackgroundMutes_.clear();
    std::ostringstream stream;
    stream << "[VR][stereo] GRIP_TRANSPARENCY_SCREENBG_RESTORE reason="
           << (reason != nullptr ? reason : "unknown")
           << " restored=" << restored << " dead=" << dead;
    Log(stream.str());
}

void UnityStereoRenderer::ApplyGripPlateHide() noexcept {
    // Census-identified backdrop plates only — no heuristics beyond the
    // exact signature seen in the .235 log: an opaque black RawImage named
    // "Background" under the ADV engine's Content Canvas.
    if (!api_.behaviourSetEnabled.Ready() ||
        !api_.behaviourGetEnabled.Ready()) {
        return;
    }
    auto* graphicClass = Il2cppUtils::GetClass(
        "UnityEngine.UI.dll", "UnityEngine.UI", "Graphic");
    if (graphicClass == nullptr) {
        return;
    }
    static const std::int32_t colorOffset = FindNamedFieldOffset(
        graphicClass, {"m_Color"}, {"UnityEngine.Color", "Color"});
    if (colorOffset < 0) {
        return;
    }
    gripPlateHides_.erase(
        std::remove_if(
            gripPlateHides_.begin(),
            gripPlateHides_.end(),
            [](const GripPlateHide& entry) {
                return !IsUnityManagedObjectAlive(entry.graphic);
            }),
        gripPlateHides_.end());
    using GetPtr = void* (*)(void*, void*);
    using GetBool = bool (*)(void*, void*);
    using SetEnabled = void (*)(void*, bool, void*);
    struct PlateColor {
        float r = 0.0F;
        float g = 0.0F;
        float b = 0.0F;
        float a = 0.0F;
    };
    // Census-proven signatures (.235 log): the ADV engines' opaque black
    // "Background" RawImage, and the ADV wrappers' opaque black "Viewport"
    // Image under ViewportRoot (.236 hardware: hiding Background alone left
    // the vertical dialogue ADV black).
    for (void* graphic : FindManagedObjectsOfType(graphicClass)) {
        if (graphic == nullptr || !IsUnityManagedObjectAlive(graphic)) {
            continue;
        }
        const auto tracked = std::find_if(
            gripPlateHides_.begin(),
            gripPlateHides_.end(),
            [graphic](const GripPlateHide& entry) {
                return entry.graphic == graphic;
            });
        if (tracked != gripPlateHides_.end()) {
            // Keep-alive: the game may re-enable the plate per screen.
            InvokeManagedVoid<SetEnabled>(
                api_.behaviourSetEnabled, graphic, false);
            continue;
        }
        const std::string name = ManagedObjectName(graphic);
        const bool isBackground = name == "Background";
        const bool isViewport = name == "Viewport";
        // .237 hardware: the story wrapper (StoryPlayerScreen) carries its
        // own full-screen black Fade plate outside the ADV engine chain.
        const bool isFade = name == "Fade";
        // .238 hardware: with all three plates hidden the story probe
        // stayed all-black — the last painter is the ADV "Render Target"
        // RawImage itself, drawing the (transparently cleared) VL target
        // texture opaquely. It is the frozen-3D monitor this feature
        // exists to remove; the stereo projection shows the live scene.
        const bool isRenderTarget = name == "Render Target";
        // .260: the .259 "Capture Target" rule was removed — the .259 run
        // proved that RawImage IS UIManager._capturedImage (ADVUI mute
        // line "Capture Target"), so the rule was redundant AND a
        // restore-clobber hazard (two tracked lists saving each other's
        // muted state as the original).
        // .240 hardware: the cinematic gray board is the mid-tone
        // `BackgroundBase` Image (0.286,0.333,0.345 = probe 0xff585549)
        // under the HorizontalAdv canvas — invisible to every color-gated
        // census; matched by name+chain instead.
        const bool isBackgroundBase = name == "BackgroundBase";
        // .241 hardware: the option-style dialogues (produce choices) keep
        // an opaque black `FadeImage "Content Cover"` over the content
        // area — the gray/white wash behind the option bars.
        const bool isContentCover = name == "Content Cover";
        // .245 BOARD census: a DontDestroy singleton full-screen opaque
        // black plate lives under ScreenOrientationManager on EVERY screen
        // — the universal bottom black board.
        const bool isFadeFitScreen = name == "FadeFitScreen";
        if (!isBackground && !isViewport && !isFade && !isRenderTarget &&
            !isBackgroundBase && !isContentCover && !isFadeFitScreen) {
            continue;
        }
        PlateColor color{};
        if (!ReadManagedField(graphic, colorOffset, &color)) {
            continue;
        }
        // The board plates are authored opaque black; Render Target is a
        // white-tinted RawImage whose blackness lives in the texture, and
        // BackgroundBase is authored mid-gray.
        if (!isRenderTarget && !isBackgroundBase &&
            (color.a < 0.9F || color.r > 0.1F || color.g > 0.1F ||
             color.b > 0.1F)) {
            continue;
        }
        // Parent-chain verification keeps this surgical: Background must
        // sit under an ADV engine content canvas, Viewport under a
        // ViewportRoot wrapper.
        bool chainMatch = false;
        std::string path = name;
        if (api_.componentGetTransform.Ready() &&
            api_.transformGetParent.Ready()) {
            void* transform = nullptr;
            InvokeManagedResult<void*, GetPtr>(
                api_.componentGetTransform, &transform, graphic);
            for (int hop = 0; hop < 4 && transform != nullptr; ++hop) {
                void* parent = nullptr;
                if (!InvokeManagedResult<void*, GetPtr>(
                        api_.transformGetParent, &parent, transform) ||
                    parent == nullptr) {
                    break;
                }
                const std::string parentName = ManagedObjectName(parent);
                path = parentName + "/" + path;
                if (isBackground &&
                    (parentName.find("Content Canvas") != std::string::npos ||
                     parentName.find("ADVEngine") != std::string::npos ||
                     parentName.find("ScreenCanvas") != std::string::npos ||
                     parentName.find("StoryPlayerScreen") !=
                         std::string::npos)) {
                    chainMatch = true;
                }
                if (isViewport &&
                    parentName.find("ViewportRoot") != std::string::npos) {
                    chainMatch = true;
                }
                if (isFade &&
                    (parentName.find("ScreenHeaderFooterCanvas") !=
                         std::string::npos ||
                     parentName.find("StoryPlayerScreen") !=
                         std::string::npos)) {
                    chainMatch = true;
                }
                if (isRenderTarget &&
                    (parentName.find("Content Canvas") != std::string::npos ||
                     parentName.find("Main Layer") != std::string::npos)) {
                    chainMatch = true;
                }
                if (isBackgroundBase &&
                    parentName.find("Adv") != std::string::npos) {
                    chainMatch = true;
                }
                if (isContentCover &&
                    (parentName.find("Content Root") != std::string::npos ||
                     parentName.find("Content Canvas") != std::string::npos)) {
                    chainMatch = true;
                }
                if (isFadeFitScreen &&
                    parentName.find("ScreenOrientationManager") !=
                        std::string::npos) {
                    chainMatch = true;
                }
                transform = parent;
            }
        }
        if (!chainMatch) {
            continue;
        }
        bool wasEnabled = false;
        if (!InvokeManagedResult<bool, GetBool>(
                api_.behaviourGetEnabled, &wasEnabled, graphic)) {
            continue;
        }
        InvokeManagedVoid<SetEnabled>(api_.behaviourSetEnabled, graphic, false);
        gripPlateHides_.push_back(GripPlateHide{graphic, wasEnabled});
        Log("[VR][stereo] GRIP_TRANSPARENCY_PLATE_HIDE path=\"" + path +
            "\" enabledBefore=" + (wasEnabled ? "1" : "0"));
    }
}

void UnityStereoRenderer::RestoreGripPlateHide(const char* reason) noexcept {
    if (gripPlateHides_.empty()) {
        return;
    }
    std::size_t restored = 0;
    std::size_t dead = 0;
    using SetEnabled = void (*)(void*, bool, void*);
    for (auto& entry : gripPlateHides_) {
        if (entry.graphic == nullptr ||
            !IsUnityManagedObjectAlive(entry.graphic)) {
            ++dead;
            continue;
        }
        if (api_.behaviourSetEnabled.Ready() &&
            InvokeManagedVoid<SetEnabled>(
                api_.behaviourSetEnabled, entry.graphic,
                entry.wasEnabled)) {
            ++restored;
        }
    }
    gripPlateHides_.clear();
    std::ostringstream stream;
    stream << "[VR][stereo] GRIP_TRANSPARENCY_PLATE_RESTORE reason="
           << (reason != nullptr ? reason : "unknown")
           << " restored=" << restored << " dead=" << dead;
    Log(stream.str());
}

void UnityStereoRenderer::PublishGripIntermediateClearTargets() noexcept {
    // .226: hand the native pointers of the live Unity RenderTextures to
    // the Present hook, which adopts the swapchain-sized color attachments
    // and clears them to transparent black each frame while armed. The
    // frozen 3D lives in one of these pooled intermediates; the UI pass
    // loads it and the final blit re-composites it over the backbuffer, so
    // clearing the backbuffer alone never sticks.
    if (!api_.textureGetNativeTexturePtr.Ready()) {
        return;
    }
    auto* renderTextureClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture");
    if (renderTextureClass == nullptr) {
        return;
    }
    // .227: adoption is decided D3D-side by size, but the NAMES prove what
    // the internal compositing surfaces actually are (and protect us from
    // adopting live content like Live video RTs later). Resolved lazily —
    // the getters live on RenderTexture itself, not the Texture base.
    static MethodRef renderTextureGetWidth = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture",
        "get_width", false, "System.Int32", {}));
    static MethodRef renderTextureGetHeight = MethodReference(ResolveStrictMethod(
        "UnityEngine.CoreModule.dll", "UnityEngine", "RenderTexture",
        "get_height", false, "System.Int32", {}));
    struct ScanEntry {
        void* native = nullptr;
        std::int32_t width = 0;
        std::int32_t height = 0;
        std::string name;
    };
    std::vector<void*> natives;
    std::vector<ScanEntry> scan;
    std::size_t setHash = 0;
    using GetNativePtr = void* (*)(void*, void*);
    using GetInt = std::int32_t (*)(void*, void*);
    for (void* texture : FindManagedObjectsOfType(renderTextureClass)) {
        if (texture == nullptr || !IsUnityManagedObjectAlive(texture)) {
            continue;
        }
        // .236: publish only VLSRP compositing surfaces by name. The .227
        // adoption log proved these are exactly what gets adopted anyway;
        // the name allowlist lets the Present hook also accept the
        // swapped-aspect (landscape) targets ADV composits into, without
        // ever risking a Live video RT that shares the aspect ratio.
        // .241: _VLCapturedTexture is cleared again. The .239 experiment
        // (stop clearing it to chase the lobby white) was falsified — the
        // white persisted — and it regressed produce sub-screens that show
        // the frozen capture through Captured-type VLSRPTargetImages.
        const std::string textureName = ManagedObjectName(texture);
        const bool vlSurface =
            textureName.rfind("_VLTargetTexture", 0) == 0 ||
            textureName.rfind("_VLCapturedTexture", 0) == 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
        if (renderTextureGetWidth.Ready() && renderTextureGetHeight.Ready()) {
            InvokeManagedResult<std::int32_t, GetInt>(
                renderTextureGetWidth, &width, texture);
            InvokeManagedResult<std::int32_t, GetInt>(
                renderTextureGetHeight, &height, texture);
        }
        void* nativePointer = nullptr;
        if (vlSurface) {
            if (!InvokeManagedResult<void*, GetNativePtr>(
                    api_.textureGetNativeTexturePtr, &nativePointer,
                    texture) ||
                nativePointer == nullptr) {
                continue;
            }
            natives.push_back(nativePointer);
            setHash ^= std::hash<void*>{}(nativePointer) +
                0x9E3779B97F4A7C15ULL + (setHash << 6) + (setHash >> 2);
        }
        if ((width >= 512 || height >= 512) && nativePointer != nullptr) {
            scan.push_back(
                ScanEntry{nativePointer, width, height, textureName});
        }
    }
    // The Present hook dedupes identical sets; sending every rescan keeps
    // late-allocated attachments covered without extra bookkeeping here.
    SetGripPanelClearTargets(natives.data(), natives.size());
    if (setHash != gripIntermediateLastPublished_) {
        gripIntermediateLastPublished_ = setHash;
        std::ostringstream stream;
        stream << "[VR][stereo] GRIP_TRANSPARENCY_RT_PUBLISH count="
               << natives.size() << " big=" << scan.size();
        Log(stream.str());
        for (const auto& entry : scan) {
            std::ostringstream line;
            line << "[VR][stereo] GRIP_TRANSPARENCY_RT_SCAN name=\""
                 << entry.name << "\" size=" << entry.width << 'x'
                 << entry.height << " native=0x" << std::hex
                 << reinterpret_cast<std::uintptr_t>(entry.native);
            Log(line.str());
        }
    }
}

void UnityStereoRenderer::ClearSourceTemporalReset(
    void* sourceUniversalData) noexcept {
    if (sourceUniversalData == nullptr ||
        !IsUnityManagedObjectAlive(sourceUniversalData) ||
        api_.universalTaaSettingsOffset < 0) {
        return;
    }
    // URP's resetHistory setter only accumulates
    // (m_TaaSettings.resetHistoryFrames += value ? N : 0), so writing
    // false through set_resetHistory drops nothing. The .103 log shows
    // 45 queued frames draining through Grip after restore (fat
    // outlines ~0.75 s). Zero the counter field directly.
    constexpr std::int32_t kResetHistoryFramesOffset = 24;
    WriteManagedField(
        sourceUniversalData,
        api_.universalTaaSettingsOffset + kResetHistoryFramesOffset,
        std::int32_t{0});
}

void UnityStereoRenderer::RequestOneFrameSourceTemporalReset(
    void* sourceUniversalData) noexcept {
    if (sourceUniversalData == nullptr ||
        !IsUnityManagedObjectAlive(sourceUniversalData)) {
        return;
    }
    using SetBool = void (*)(void*, bool, void*);
    InvokeManagedVoid<SetBool>(
        api_.universalSetResetHistory, sourceUniversalData, true);
}

bool UnityStereoRenderer::ReadSourceCutSignature(
    void* sourceCamera,
    float* fieldOfView,
    float* posX,
    float* posY,
    float* posZ) noexcept {
    if (sourceCamera == nullptr || fieldOfView == nullptr || posX == nullptr ||
        posY == nullptr || posZ == nullptr ||
        !api_.cameraGetFieldOfView.Ready() ||
        !api_.componentGetTransform.Ready() ||
        !api_.transformGetPositionInjected.Ready() ||
        !IsUnityManagedObjectAlive(sourceCamera)) {
        return false;
    }
    using GetFloat = float (*)(void*, void*);
    using GetTransform = void* (*)(void*, void*);
    using GetVector3 = void (*)(void*, UnityVector3*, void*);
    float fov = 0.0F;
    if (!InvokeManagedResult<float, GetFloat>(
            api_.cameraGetFieldOfView, &fov, sourceCamera) ||
        !std::isfinite(fov) || fov <= 1.0F) {
        return false;
    }
    void* transform = nullptr;
    if (!InvokeManagedResult<void*, GetTransform>(
            api_.componentGetTransform, &transform, sourceCamera) ||
        transform == nullptr) {
        return false;
    }
    void* positionSelf = transform;
    if (api_.transformPositionGetterUsesNativeSelf) {
        void* nativeTransform = ReadUnityNativePointer(transform);
        if (nativeTransform == nullptr) {
            return false;
        }
        positionSelf = nativeTransform;
    }
    UnityVector3 position{};
    if (!InvokeManagedVoid<GetVector3>(
            api_.transformGetPositionInjected, positionSelf, &position) ||
        !std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
        return false;
    }
    *fieldOfView = fov;
    *posX = position.x;
    *posY = position.y;
    *posZ = position.z;
    return true;
}

bool UnityStereoRenderer::ConsumeSourceTemporalCut(
    void* sourceCamera,
    void* sourceUniversalData,
    bool sourceResetHistory) noexcept {
    float fov = 0.0F;
    float posX = 0.0F;
    float posY = 0.0F;
    float posZ = 0.0F;
    const bool haveSignature =
        ReadSourceCutSignature(sourceCamera, &fov, &posX, &posY, &posZ);
    bool poseCut = false;
    if (haveSignature && sourceCutSignatureValid_) {
        const float dFov = std::fabs(fov - lastSourceCutFov_);
        const float dx = posX - lastSourceCutPosX_;
        const float dy = posY - lastSourceCutPosY_;
        const float dz = posZ - lastSourceCutPosZ_;
        // Live hard cuts jump tens of metres / tens of degrees. A 2 s
        // zoom never moves 4° or 2 m in one Tick.
        if (dFov >= 4.0F || (dx * dx + dy * dy + dz * dz) >= 4.0F) {
            poseCut = true;
        }
    }
    if (haveSignature) {
        lastSourceCutFov_ = fov;
        lastSourceCutPosX_ = posX;
        lastSourceCutPosY_ = posY;
        lastSourceCutPosZ_ = posZ;
        sourceCutSignatureValid_ = true;
    }

    bool historyRise = false;
    if (sourceResetHistory) {
        historyRise = !sourceHistoryResetLatched_;
        sourceHistoryResetLatched_ = true;
        // A disabled source never runs URP, so resetHistoryFrames only
        // ever accumulates. Clear it so Cinemachine can pulse again on
        // the next shot and the backlog does not drain through Grip
        // after restore.
        ClearSourceTemporalReset(sourceUniversalData);
    } else {
        sourceHistoryResetLatched_ = false;
    }

    if (!historyRise && !poseCut) {
        return false;
    }
    std::ostringstream pulse;
    pulse << "[VR][stereo] SOURCE_CUT_PULSE suppressed="
          << (sourceCameraSuppressed_ ? "1" : "0")
          << " historyRise=" << (historyRise ? "1" : "0")
          << " poseCut=" << (poseCut ? "1" : "0")
          << " fov=" << fov
          << " pos=" << posX << "," << posY << "," << posZ;
    Log(pulse.str());
    return true;
}

bool UnityStereoRenderer::ApplyEyePoseAndProjection(
    std::size_t eye,
    const UnityStereoCameraFrame& frame,
    float nearClip,
    float farClip) noexcept {
    if (eye >= 2U || !IsFinitePose(frame.composedPose.eyes[eye]) ||
        !IsFiniteFov(frame.trackingSample.eyes[eye].fov) ||
        !std::isfinite(nearClip) || !std::isfinite(farClip) ||
        nearClip < 0.001F || farClip <= nearClip + 0.01F) {
        return FailStage("configure.eye-input", eye);
    }
    using GetTransform = void* (*)(void*, void*);
    using SetPositionAndRotation = void (*)(
        void*, const UnityVector3*, const UnityQuaternion*, void*);
    using Frustum = void (*)(float, float, float, float, float, float,
                             UnityMatrix4x4*, void*);
    using GetFloat = float (*)(void*, void*);
    using SetFloat = void (*)(void*, float, void*);
    using GetBool = bool (*)(void*, void*);
    using GetLensShift = void (*)(void*, UnityVector2*, void*);
    using SetLensShift = void (*)(void*, const UnityVector2*, void*);
    using GetNativeFloat = float (*)(void*, void*);
    using SetNativeFloat = void (*)(void*, float, void*);
    using GetNativeVector2 = void (*)(void*, UnityVector2*, void*);
    using SetNativeVector2 = void (*)(void*, const UnityVector2*, void*);
    using SetProjection = void (*)(void*, const UnityMatrix4x4*, void*);
    void* transform = nullptr;
    Log("[VR][stereo] CALL_BEGIN stage=configure.get-transform eye=" +
        std::string(eye == 0U ? "left" : "right"));
    if (!InvokeManagedResult<void*, GetTransform>(api_.componentGetTransform,
            &transform, eyeCameras_[eye]) || transform == nullptr) {
        return FailStage("configure.get-transform", eye);
    }
    Log("[VR][stereo] CALL_OK stage=configure.get-transform eye=" +
        std::string(eye == 0U ? "left" : "right"));
    const auto& poseValue = frame.composedPose.eyes[eye];
    const UnityVector3 position(
        poseValue.position.x, poseValue.position.y, poseValue.position.z);
    const UnityQuaternion rotation(
        poseValue.orientation.x, poseValue.orientation.y,
        poseValue.orientation.z, poseValue.orientation.w);
    Log("[VR][stereo] CALL_BEGIN stage=configure.transform-self eye=" +
        std::string(eye == 0U ? "left" : "right"));
    void* transformSelf = api_.transformUsesNativeSelf
        ? ReadUnityNativePointer(transform) : transform;
    if (transformSelf == nullptr) {
        return FailStage("configure.transform-self", eye);
    }
    Log("[VR][stereo] CALL_OK stage=configure.transform-self eye=" +
        std::string(eye == 0U ? "left" : "right"));
    Log("[VR][stereo] CALL_BEGIN stage=configure.pose eye=" +
        std::string(eye == 0U ? "left" : "right"));
    if (!InvokeManagedVoid<SetPositionAndRotation>(
            api_.transformSetPositionAndRotationInjected, transformSelf,
            &position, &rotation)) {
        return FailStage("configure.pose", eye);
    }
    Log("[VR][stereo] CALL_OK stage=configure.pose eye=" +
        std::string(eye == 0U ? "left" : "right") + " orientation=raw");
    const auto& fov = frame.trackingSample.eyes[eye].fov;
    const float left = std::tan(fov.angleLeft) * nearClip;
    const float right = std::tan(fov.angleRight) * nearClip;
    const float bottom = std::tan(fov.angleDown) * nearClip;
    const float top = std::tan(fov.angleUp) * nearClip;
    UnityMatrix4x4 projection{};
    Log("[VR][stereo] CALL_BEGIN stage=configure.frustum eye=" +
        std::string(eye == 0U ? "left" : "right"));
    if (!InvokeManagedVoid<Frustum>(api_.matrixFrustumInjected, left, right,
            bottom, top, nearClip, farClip, &projection)) {
        return FailStage("configure.frustum", eye);
    }
    const UnityMatrix4x4 nonJitteredProjection = projection;
    if (smaaT2xActiveForPair_) {
        if (fullWidth_ == 0U || fullHeight_ == 0U) {
            return FailStage("configure.smaa-t2x-jitter-size", eye);
        }
        const auto& t2xPhase = d3d11::kSmaaT2xPhases[smaaT2xPairPhase_];
        const auto clipOffset = d3d11::SmaaT2xClipOffsetForPhase(
            smaaT2xPairPhase_, fullWidth_, fullHeight_);
        if (!d3d11::ApplySmaaT2xProjectionJitter(
                &projection.m[0][0], smaaT2xPairPhase_, fullWidth_,
                fullHeight_)) {
            return FailStage("configure.smaa-t2x-jitter", eye);
        }
        if (eye == 0U) {
            Log("[VR][smaa-t2x] SMAA_T2X_FRAME token=" +
                std::to_string(smaaT2xPairToken_) + " phase=" +
                std::to_string(smaaT2xPairPhase_) + " jitter=" +
                std::to_string(t2xPhase.jitterX) + "," +
                std::to_string(t2xPhase.jitterY) + " ndc=" +
                std::to_string(clipOffset.x) + "," +
                std::to_string(clipOffset.y));
        }
    }
    Log("[VR][stereo] CALL_OK stage=configure.frustum eye=" +
        std::string(eye == 0U ? "left" : "right"));
    Log("[VR][stereo] CALL_BEGIN stage=configure.camera-self eye=" +
        std::string(eye == 0U ? "left" : "right"));
    void* nativeCamera = nullptr;
    if (api_.projectionUsesNativeSelf ||
        api_.lensShiftGetterUsesNativeSelf ||
        api_.lensShiftSetterUsesNativeSelf) {
        nativeCamera = ReadUnityNativePointer(eyeCameras_[eye]);
    }
    void* cameraSelf = api_.projectionUsesNativeSelf
        ? nativeCamera : eyeCameras_[eye];
    if (cameraSelf == nullptr) {
        return FailStage("configure.camera-self", eye);
    }
    Log("[VR][stereo] CALL_OK stage=configure.camera-self eye=" +
        std::string(eye == 0U ? "left" : "right"));
    camera::ProjectionIntrinsics intrinsics{};
    if (!camera::TryDeriveProjectionIntrinsics(
            fov.angleLeft, fov.angleRight, fov.angleDown, fov.angleUp,
            intrinsics)) {
        return FailStage("configure.eye-intrinsics", eye);
    }
    float copiedFieldOfView = 0.0F;
    float copiedAspect = 0.0F;
    bool usePhysicalProperties = false;
    UnityVector2 copiedLensShift{};
    void* lensShiftGetterSelf = api_.lensShiftGetterUsesNativeSelf
        ? nativeCamera : eyeCameras_[eye];
    if (lensShiftGetterSelf == nullptr ||
        !InvokeManagedResult<float, GetFloat>(
            api_.cameraGetFieldOfView, &copiedFieldOfView,
            eyeCameras_[eye]) ||
        !InvokeManagedResult<float, GetFloat>(
            api_.cameraGetAspect, &copiedAspect, eyeCameras_[eye]) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.cameraGetUsePhysicalProperties, &usePhysicalProperties,
            eyeCameras_[eye]) ||
        !InvokeManagedVoid<GetLensShift>(
            api_.cameraGetLensShiftInjected, lensShiftGetterSelf,
            &copiedLensShift)) {
        return FailStage("configure.eye-intrinsics-read", eye);
    }
    if (usePhysicalProperties) {
        if (nativeCamera == nullptr) {
            nativeCamera = ReadUnityNativePointer(eyeCameras_[eye]);
        }
        float copiedAperture = 0.0F;
        float copiedFocusDistance = 0.0F;
        float copiedFocalLength = 0.0F;
        UnityVector2 copiedSensorSize{};
        if (nativeCamera == nullptr ||
            !InvokeManagedResult<float, GetNativeFloat>(
                api_.cameraGetApertureInjected, &copiedAperture,
                nativeCamera) ||
            !InvokeManagedResult<float, GetNativeFloat>(
                api_.cameraGetFocusDistanceInjected, &copiedFocusDistance,
                nativeCamera) ||
            !InvokeManagedResult<float, GetNativeFloat>(
                api_.cameraGetFocalLengthInjected, &copiedFocalLength,
                nativeCamera) ||
            !InvokeManagedVoid<GetNativeVector2>(
                api_.cameraGetSensorSizeInjected, nativeCamera,
                &copiedSensorSize)) {
            return FailStage("configure.eye-physical-intrinsics-read", eye);
        }
        camera::PhysicalCameraIntrinsics physicalIntrinsics{};
        if (!std::isfinite(copiedAperture) ||
            !std::isfinite(copiedFocusDistance) ||
            !std::isfinite(copiedFocalLength) ||
            !std::isfinite(copiedSensorSize.x) ||
            copiedFocalLength <= 0.0F || copiedSensorSize.x <= 0.0F ||
            !camera::TryDerivePhysicalCameraIntrinsics(
                intrinsics, copiedSensorSize.y, physicalIntrinsics)) {
            return FailStage("configure.eye-physical-intrinsics-input", eye);
        }

        const UnityVector2 eyeSensorSize(
            physicalIntrinsics.sensorWidthMillimeters,
            physicalIntrinsics.sensorHeightMillimeters);
        const UnityVector2 eyeLensShift(
            intrinsics.lensShiftX, intrinsics.lensShiftY);
        void* lensShiftSetterSelf = api_.lensShiftSetterUsesNativeSelf
            ? nativeCamera : eyeCameras_[eye];
        if (lensShiftSetterSelf == nullptr) {
            return FailStage("configure.eye-physical-intrinsics-self", eye);
        }
        const auto restoreCopiedPhysicalIntrinsics = [&]() noexcept {
            Log("[VR][stereo] CALL_BEGIN stage=configure.eye-physical-intrinsics-rollback eye=" +
                std::string(eye == 0U ? "left" : "right"));
            bool restored = true;
            restored = InvokeManagedVoid<SetFloat>(
                api_.cameraSetAspect, eyeCameras_[eye], copiedAspect) &&
                restored;
            restored = InvokeManagedVoid<SetNativeVector2>(
                api_.cameraSetSensorSizeInjected, nativeCamera,
                &copiedSensorSize) && restored;
            restored = InvokeManagedVoid<SetNativeFloat>(
                api_.cameraSetFocalLengthInjected, nativeCamera,
                copiedFocalLength) && restored;
            restored = InvokeManagedVoid<SetLensShift>(
                api_.cameraSetLensShiftInjected, lensShiftSetterSelf,
                &copiedLensShift) && restored;
            Log("[VR][stereo] " + std::string(restored ? "CALL_OK" : "CALL_FAILED") +
                " stage=configure.eye-physical-intrinsics-rollback eye=" +
                std::string(eye == 0U ? "left" : "right"));
            return restored;
        };
        Log("[VR][stereo] CALL_BEGIN stage=configure.eye-physical-intrinsics eye=" +
            std::string(eye == 0U ? "left" : "right"));
        if (!InvokeManagedVoid<SetFloat>(
                api_.cameraSetAspect, eyeCameras_[eye], intrinsics.aspect) ||
            !InvokeManagedVoid<SetNativeVector2>(
                api_.cameraSetSensorSizeInjected, nativeCamera,
                &eyeSensorSize) ||
            !InvokeManagedVoid<SetNativeFloat>(
                api_.cameraSetFocalLengthInjected, nativeCamera,
                physicalIntrinsics.focalLengthMillimeters) ||
            !InvokeManagedVoid<SetLensShift>(
                api_.cameraSetLensShiftInjected, lensShiftSetterSelf,
                &eyeLensShift)) {
            const bool restored = restoreCopiedPhysicalIntrinsics();
            return FailStage(restored
                ? "configure.eye-physical-intrinsics-write"
                : "configure.eye-physical-intrinsics-write-rollback", eye);
        }

        float appliedFieldOfView = 0.0F;
        float appliedAspect = 0.0F;
        bool appliedPhysicalProperties = false;
        float appliedAperture = 0.0F;
        float appliedFocusDistance = 0.0F;
        float appliedFocalLength = 0.0F;
        float fittedFieldOfView = 0.0F;
        UnityVector2 appliedSensorSize{};
        UnityVector2 appliedLensShift{};
        UnityVector2 fittedLensShift{};
        if (!InvokeManagedResult<float, GetFloat>(
                api_.cameraGetFieldOfView, &appliedFieldOfView,
                eyeCameras_[eye]) ||
            !InvokeManagedResult<float, GetFloat>(
                api_.cameraGetAspect, &appliedAspect, eyeCameras_[eye]) ||
            !InvokeManagedResult<bool, GetBool>(
                api_.cameraGetUsePhysicalProperties,
                &appliedPhysicalProperties, eyeCameras_[eye]) ||
            !InvokeManagedResult<float, GetNativeFloat>(
                api_.cameraGetApertureInjected, &appliedAperture,
                nativeCamera) ||
            !InvokeManagedResult<float, GetNativeFloat>(
                api_.cameraGetFocusDistanceInjected, &appliedFocusDistance,
                nativeCamera) ||
            !InvokeManagedResult<float, GetNativeFloat>(
                api_.cameraGetFocalLengthInjected, &appliedFocalLength,
                nativeCamera) ||
            !InvokeManagedVoid<GetNativeVector2>(
                api_.cameraGetSensorSizeInjected, nativeCamera,
                &appliedSensorSize) ||
            !InvokeManagedVoid<GetLensShift>(
                api_.cameraGetLensShiftInjected, lensShiftGetterSelf,
                &appliedLensShift) ||
            !InvokeManagedResult<float, GetNativeFloat>(
                api_.cameraGetGateFittedFieldOfViewInjected,
                &fittedFieldOfView, nativeCamera) ||
            !InvokeManagedVoid<GetNativeVector2>(
                api_.cameraGetGateFittedLensShiftInjected, nativeCamera,
                &fittedLensShift)) {
            const bool restored = restoreCopiedPhysicalIntrinsics();
            return FailStage(restored
                ? "configure.eye-physical-intrinsics-verify-read"
                : "configure.eye-physical-intrinsics-verify-read-rollback", eye);
        }

        constexpr float kPi = 3.14159265358979323846F;
        const float fittedHalfAngle =
            fittedFieldOfView * kPi / 360.0F;
        const float fittedTangent = std::tan(fittedHalfAngle);
        const float fittedM11 = 1.0F / fittedTangent;
        const float fittedM00 = fittedM11 / appliedAspect;
        const float fittedM02 = 2.0F * fittedLensShift.x;
        const float fittedM12 = 2.0F * fittedLensShift.y;
        const float focalTolerance = std::max(
            0.001F, physicalIntrinsics.focalLengthMillimeters * 0.001F);
        const float sensorTolerance = std::max(
            0.001F, physicalIntrinsics.sensorHeightMillimeters * 0.001F);
        if (!appliedPhysicalProperties ||
            !std::isfinite(appliedFieldOfView) ||
            !std::isfinite(appliedAspect) ||
            !std::isfinite(appliedAperture) ||
            !std::isfinite(appliedFocusDistance) ||
            !std::isfinite(appliedFocalLength) ||
            !std::isfinite(appliedSensorSize.x) ||
            !std::isfinite(appliedSensorSize.y) ||
            !std::isfinite(appliedLensShift.x) ||
            !std::isfinite(appliedLensShift.y) ||
            !std::isfinite(fittedFieldOfView) ||
            !std::isfinite(fittedLensShift.x) ||
            !std::isfinite(fittedLensShift.y) ||
            !std::isfinite(fittedM00) || !std::isfinite(fittedM11) ||
            fittedTangent <= 0.0F ||
            std::abs(appliedFieldOfView -
                intrinsics.verticalFieldOfViewDegrees) > 0.02F ||
            std::abs(appliedAspect - intrinsics.aspect) > 0.001F ||
            std::abs(appliedAperture - copiedAperture) > 0.001F ||
            std::abs(appliedFocusDistance - copiedFocusDistance) > 0.001F ||
            std::abs(appliedFocalLength -
                physicalIntrinsics.focalLengthMillimeters) > focalTolerance ||
            std::abs(appliedSensorSize.x -
                physicalIntrinsics.sensorWidthMillimeters) > sensorTolerance ||
            std::abs(appliedSensorSize.y -
                physicalIntrinsics.sensorHeightMillimeters) > sensorTolerance ||
            std::abs(appliedLensShift.x - intrinsics.lensShiftX) > 0.001F ||
            std::abs(appliedLensShift.y - intrinsics.lensShiftY) > 0.001F ||
            std::abs(fittedFieldOfView -
                intrinsics.verticalFieldOfViewDegrees) > 0.02F ||
            std::abs(fittedLensShift.x - intrinsics.lensShiftX) > 0.001F ||
            std::abs(fittedLensShift.y - intrinsics.lensShiftY) > 0.001F ||
            std::abs(fittedM00 - intrinsics.projectionM00) > 0.002F ||
            std::abs(fittedM02 - intrinsics.projectionM02) > 0.002F ||
            std::abs(fittedM11 - intrinsics.projectionM11) > 0.002F ||
            std::abs(fittedM12 - intrinsics.projectionM12) > 0.002F) {
            const bool restored = restoreCopiedPhysicalIntrinsics();
            return FailStage(restored
                ? "configure.eye-physical-intrinsics-verify"
                : "configure.eye-physical-intrinsics-verify-rollback", eye);
        }

        std::ostringstream physicalState;
        physicalState
            << "[VR][stereo] EYE_PHYSICAL_INTRINSICS_APPLIED eye="
            << (eye == 0U ? "left" : "right")
            << " copiedFov=" << copiedFieldOfView
            << " copiedAspect=" << copiedAspect
            << " copiedFocal=" << copiedFocalLength
            << " copiedSensor=" << copiedSensorSize.x << ','
            << copiedSensorSize.y
            << " copiedLensShift=" << copiedLensShift.x << ','
            << copiedLensShift.y
            << " aperture=" << appliedAperture
            << " focusDistance=" << appliedFocusDistance
            << " eyeFov=" << appliedFieldOfView
            << " fittedFov=" << fittedFieldOfView
            << " eyeAspect=" << appliedAspect
            << " eyeFocal=" << appliedFocalLength
            << " eyeSensor=" << appliedSensorSize.x << ','
            << appliedSensorSize.y
            << " eyeLensShift=" << appliedLensShift.x << ','
            << appliedLensShift.y
            << " fittedLensShift=" << fittedLensShift.x << ','
            << fittedLensShift.y
            << " p00=" << fittedM00
            << " p02=" << fittedM02
            << " p11=" << fittedM11
            << " p12=" << fittedM12;
        Log(physicalState.str());
    } else {
        const UnityVector2 eyeLensShift(
            intrinsics.lensShiftX, intrinsics.lensShiftY);
        void* lensShiftSetterSelf = api_.lensShiftSetterUsesNativeSelf
            ? nativeCamera : eyeCameras_[eye];
        Log("[VR][stereo] CALL_BEGIN stage=configure.eye-intrinsics eye=" +
            std::string(eye == 0U ? "left" : "right"));
        if (lensShiftSetterSelf == nullptr ||
            !InvokeManagedVoid<SetFloat>(
                api_.cameraSetFieldOfView, eyeCameras_[eye],
                intrinsics.verticalFieldOfViewDegrees) ||
            !InvokeManagedVoid<SetFloat>(
                api_.cameraSetAspect, eyeCameras_[eye], intrinsics.aspect) ||
            !InvokeManagedVoid<SetLensShift>(
                api_.cameraSetLensShiftInjected, lensShiftSetterSelf,
                &eyeLensShift)) {
            return FailStage("configure.eye-intrinsics-write", eye);
        }
        float appliedFieldOfView = 0.0F;
        float appliedAspect = 0.0F;
        UnityVector2 appliedLensShift{};
        if (!InvokeManagedResult<float, GetFloat>(
                api_.cameraGetFieldOfView, &appliedFieldOfView,
                eyeCameras_[eye]) ||
            !InvokeManagedResult<float, GetFloat>(
                api_.cameraGetAspect, &appliedAspect, eyeCameras_[eye]) ||
            !InvokeManagedVoid<GetLensShift>(
                api_.cameraGetLensShiftInjected, lensShiftGetterSelf,
                &appliedLensShift) ||
            std::abs(
                appliedFieldOfView - intrinsics.verticalFieldOfViewDegrees) >
                0.01F ||
            std::abs(appliedAspect - intrinsics.aspect) > 0.001F ||
            std::abs(appliedLensShift.x - intrinsics.lensShiftX) > 0.001F ||
            std::abs(appliedLensShift.y - intrinsics.lensShiftY) > 0.001F) {
            return FailStage("configure.eye-intrinsics-verify", eye);
        }
        std::ostringstream intrinsicsState;
        intrinsicsState
            << "[VR][stereo] EYE_PROJECTION_INTRINSICS eye="
            << (eye == 0U ? "left" : "right")
            << " copiedFov=" << copiedFieldOfView
            << " copiedAspect=" << copiedAspect
            << " copiedLensShift=" << copiedLensShift.x << ','
            << copiedLensShift.y
            << " physical=0"
            << " eyeFov=" << appliedFieldOfView
            << " eyeAspect=" << appliedAspect
            << " eyeLensShift=" << appliedLensShift.x << ','
            << appliedLensShift.y
            << " p00=" << intrinsics.projectionM00
            << " p02=" << intrinsics.projectionM02
            << " p11=" << intrinsics.projectionM11
            << " p12=" << intrinsics.projectionM12;
        Log(intrinsicsState.str());
    }
    Log("[VR][stereo] CALL_BEGIN stage=configure.projection eye=" +
        std::string(eye == 0U ? "left" : "right"));
    if (!InvokeManagedVoid<SetProjection>(api_.cameraSetProjectionMatrixInjected,
                                         cameraSelf, &projection)) {
        return FailStage("configure.projection", eye);
    }
    // Only needed while CopyFrom is skipped (suppressed source): the eye
    // then keeps stale explicit temporal matrices. With a live source the
    // per-frame CopyFrom already resets this state; writing it anyway is
    // a .99 experiment that changed nothing on hardware.
    if (smaaT2xActiveForPair_ &&
        !api_.cameraSetNonJitteredProjectionMatrixInjected.Ready()) {
        return FailStage("configure.non-jittered-projection-api", eye);
    }
    if ((sourceCameraSuppressed_ || smaaT2xActiveForPair_) &&
        api_.cameraSetNonJitteredProjectionMatrixInjected.Ready()) {
        void* nonJitteredSelf = api_.nonJitteredProjectionUsesNativeSelf
            ? nativeCamera : eyeCameras_[eye];
        if (api_.nonJitteredProjectionUsesNativeSelf &&
            nonJitteredSelf == nullptr) {
            nonJitteredSelf = ReadUnityNativePointer(eyeCameras_[eye]);
        }
        if (nonJitteredSelf == nullptr ||
            !InvokeManagedVoid<SetProjection>(
                api_.cameraSetNonJitteredProjectionMatrixInjected,
                nonJitteredSelf, &nonJitteredProjection)) {
            return FailStage("configure.non-jittered-projection", eye);
        }
    }
    if (smaaT2xActiveForPair_) {
        std::copy_n(
            &projection.m[0][0], 16,
            smaaT2xExpectedProjection_[eye].begin());
        std::copy_n(
            &nonJitteredProjection.m[0][0], 16,
            smaaT2xExpectedNonJitteredProjection_[eye].begin());
        smaaT2xExpectedProjectionValid_[eye] = true;
        if (smaaT2xComprehensiveProbeForPair_) {
            LogSmaaT2xProjectionReadback(eye, "configure");
        }
    }
    Log("[VR][stereo] CALL_OK stage=configure.projection eye=" +
        std::string(eye == 0U ? "left" : "right") + " reflected=0 pixelFlip=deferred");
    return true;
}

void UnityStereoRenderer::LogSmaaT2xProjectionReadback(
    std::size_t eye,
    const char* stage) noexcept {
    if (!smaaT2xComprehensiveProbeForPair_ || eye >= eyeCameras_.size() ||
        !smaaT2xExpectedProjectionValid_[eye]) {
        return;
    }
    using GetProjection = void (*)(void*, UnityMatrix4x4*, void*);
    UnityMatrix4x4 actualProjection{};
    UnityMatrix4x4 actualNonJittered{};
    void* nativeCamera = ReadUnityNativePointer(eyeCameras_[eye]);
    void* projectionSelf = api_.projectionGetterUsesNativeSelf
        ? nativeCamera
        : eyeCameras_[eye];
    void* nonJitteredSelf = api_.nonJitteredProjectionGetterUsesNativeSelf
        ? nativeCamera
        : eyeCameras_[eye];
    const bool projectionReady = projectionSelf != nullptr &&
        InvokeManagedVoid<GetProjection>(
            api_.cameraGetProjectionMatrixInjected, projectionSelf,
            &actualProjection);
    const bool nonJitteredReady = nonJitteredSelf != nullptr &&
        InvokeManagedVoid<GetProjection>(
            api_.cameraGetNonJitteredProjectionMatrixInjected,
            nonJitteredSelf, &actualNonJittered);
    const auto maxDifference = [](const float* left, const float* right) {
        float maximum = 0.0F;
        for (std::size_t index = 0; index < 16U; ++index) {
            maximum = std::max(maximum, std::abs(left[index] - right[index]));
        }
        return maximum;
    };
    const float* actual = &actualProjection.m[0][0];
    const float* actualNonJitter = &actualNonJittered.m[0][0];
    const float projectionError = projectionReady
        ? maxDifference(actual, smaaT2xExpectedProjection_[eye].data())
        : -1.0F;
    const float nonJitteredError = nonJitteredReady
        ? maxDifference(
              actualNonJitter,
              smaaT2xExpectedNonJitteredProjection_[eye].data())
        : -1.0F;
    const float actualSeparation = projectionReady && nonJitteredReady
        ? maxDifference(actual, actualNonJitter)
        : -1.0F;
    std::ostringstream line;
    line << std::fixed << std::setprecision(9)
         << "[VR][smaa-t2x] SMAA_T2X_PROJECTION token="
         << smaaT2xPairToken_ << " eye=" << (eye == 0U ? "left" : "right")
         << " stage=" << (stage != nullptr ? stage : "unknown")
         << " phase=" << smaaT2xPairPhase_
         << " projectionReady=" << (projectionReady ? 1 : 0)
         << " nonJitteredReady=" << (nonJitteredReady ? 1 : 0)
         << " projectionMaxError=" << projectionError
         << " nonJitteredMaxError=" << nonJitteredError
         << " actualSeparation=" << actualSeparation
         << " expectedP02=" << smaaT2xExpectedProjection_[eye][8]
         << " actualP02=" << (projectionReady ? actual[8] : 0.0F)
         << " expectedP12=" << smaaT2xExpectedProjection_[eye][9]
         << " actualP12=" << (projectionReady ? actual[9] : 0.0F)
         << " expectedNjP02="
         << smaaT2xExpectedNonJitteredProjection_[eye][8]
         << " actualNjP02=" << (nonJitteredReady ? actualNonJitter[8] : 0.0F)
         << " expectedNjP12="
         << smaaT2xExpectedNonJitteredProjection_[eye][9]
         << " actualNjP12=" << (nonJitteredReady ? actualNonJitter[9] : 0.0F);
    Log(line.str());
}

bool UnityStereoRenderer::CorrectSmaaT2xMotionHistoryProjection(
    void* camera,
    float* cameraDataProjection,
    bool* required) noexcept {
    if (required != nullptr) {
        *required = false;
    }
    if (!smaaT2xActiveForPair_ || camera == nullptr) {
        return true;
    }
    std::size_t eye = eyeCameras_.size();
    for (std::size_t index = 0; index < eyeCameras_.size(); ++index) {
        if (camera == eyeCameras_[index]) {
            eye = index;
            break;
        }
    }
    if (eye >= eyeCameras_.size()) {
        return true;
    }
    if (required != nullptr) {
        *required = true;
    }

    constexpr float kProjectionTolerance = 0.000002F;
    float incomingError = -1.0F;
    float correctedError = -1.0F;
    bool corrected = false;
    if (cameraDataProjection != nullptr &&
        smaaT2xExpectedProjectionValid_[eye]) {
        corrected = TryPrepareSmaaT2xMotionHistoryProjection(
            cameraDataProjection,
            smaaT2xExpectedProjection_[eye].data(),
            smaaT2xExpectedNonJitteredProjection_[eye].data(),
            kProjectionTolerance,
            &incomingError,
            &correctedError);
    }
    if (!corrected) {
        const std::uint64_t count = ++smaaT2xMotionHistoryFaultCount_[eye];
        if (count <= 2U || count % 300U == 0U) {
            std::ostringstream fault;
            fault << std::fixed << std::setprecision(9)
                  << "[VR][smaa-t2x] SMAA_T2X_FAULT mv-history-projection"
                  << " eye=" << (eye == 0U ? "left" : "right")
                  << " token=" << smaaT2xPairToken_
                  << " phase=" << smaaT2xPairPhase_
                  << " expectedReady="
                  << (smaaT2xExpectedProjectionValid_[eye] ? 1 : 0)
                  << " incomingMaxError=" << incomingError
                  << " count=" << count;
            Log(fault.str());
        }
        return false;
    }

    smaaT2xMotionHistoryCorrectedForPair_[eye] = true;
    const std::uint64_t count = ++smaaT2xMotionHistoryCorrectionCount_[eye];
    if (smaaT2xComprehensiveProbeForPair_ || count <= 2U || count % 300U == 0U) {
        std::ostringstream line;
        line << std::fixed << std::setprecision(9)
             << "[VR][smaa-t2x] SMAA_T2X_MV_MATRIX eye="
             << (eye == 0U ? "left" : "right")
             << " token=" << smaaT2xPairToken_
             << " phase=" << smaaT2xPairPhase_
             << " incomingMaxError=" << incomingError
             << " correctedMaxError=" << correctedError
             << " projectionSeparation="
             << (std::max)(
                    std::abs(
                        smaaT2xExpectedProjection_[eye][8] -
                        smaaT2xExpectedNonJitteredProjection_[eye][8]),
                    std::abs(
                        smaaT2xExpectedProjection_[eye][9] -
                        smaaT2xExpectedNonJitteredProjection_[eye][9]))
             << " count=" << count;
        Log(line.str());
    }
    return true;
}

bool UnityStereoRenderer::ConfigureCameraData(
    std::size_t eye,
    void* sourceCamera,
    void* target) noexcept {
    if (eye >= eyeCameras_.size() || sourceCamera == nullptr || target == nullptr ||
        eyeUniversalCameraData_[eye] == nullptr ||
        eyeVlAdditionalCameraData_[eye] == nullptr) {
        return FailStage("configure.camera-data-input", eye);
    }
    using GetComponent = void* (*)(void*, void*, void*);
    using GetInt = int (*)(void*, void*);
    using SetInt = void (*)(void*, int, void*);
    using GetBool = bool (*)(void*, void*);
    using SetBool = void (*)(void*, bool, void*);
    using GetObject = void* (*)(void*, void*);
    using SetObject = void (*)(void*, void*, void*);
    using SetTarget = void (*)(void*, void*, void*);
    void* sourceUniversalData = nullptr;
    void* sourceVlAdditionalData = nullptr;
    void* sourceVlController = nullptr;
    const std::string eyeName = eye == 0U ? "left" : "right";
    Log("[VR][stereo] CALL_BEGIN stage=configure.camera-data-discover eye=" +
        eyeName);
    if (!InvokeManagedResult<void*, GetComponent>(
            api_.componentGetComponent, &sourceUniversalData, sourceCamera,
            api_.universalCameraDataReflectionType) ||
        !InvokeManagedResult<void*, GetComponent>(
            api_.componentGetComponent, &sourceVlAdditionalData, sourceCamera,
            api_.vlAdditionalCameraDataReflectionType) ||
        !InvokeManagedResult<void*, GetComponent>(
            api_.componentGetComponent, &sourceVlController, sourceCamera,
            api_.vlCameraControllerReflectionType)) {
        return FailStage("configure.camera-data-discover", eye);
    }
    if (sourceUniversalData == nullptr) {
        Log("[VR][stereo] SOURCE_CAMERA_DATA_REJECTED reason=universal-data-missing");
        return FailStage("configure.source-universal-data", eye);
    }
    latestSourceUniversalData_ = sourceUniversalData;
    Log("[VR][stereo] CALL_OK stage=configure.camera-data-discover eye=" +
        eyeName);

    std::int32_t rendererIndex = -1;
    int renderType = 0;
    bool renderPostProcessing = false;
    int sourceAntialiasing = 0;
    int antialiasingQuality = 0;
    int requiresDepthOption = 0;
    int requiresColorOption = 0;
    bool requiresDepthTexture = false;
    bool requiresColorTexture = false;
    UnityLayerMaskValue volumeLayerMask{};
    void* sourceVolumeTrigger = nullptr;
    void* eyeVolumeTrigger = nullptr;
    int volumeFrameworkUpdateMode = 0;
    bool stopNan = false;
    bool dithering = false;
    bool allowHdrOutput = false;
    bool renderShadows = false;
    bool fastRendering = false;
    bool needsAlphaChannel = false;
    bool clearDepth = true;
    bool useScreenCoordOverride = false;
    UnityVector4 screenSizeOverride{};
    UnityVector4 screenCoordScaleBias{};
    float renderScale = 1.0F;
    bool requiresVolumeFrameworkUpdate = false;
    bool sourceResetHistory = false;
    void* sourceVolumeStack = nullptr;
    void* sourceTaaPersistentData = nullptr;
    void* sourceMotionVectorsPersistentData = nullptr;
    bool sourceCameraAllowHdr = false;
    bool sourceCameraAllowMsaa = false;
    int sourceDepthTextureMode = 0;
    if (!ReadInt32Field(sourceUniversalData,
                        api_.universalRendererIndexOffset, &rendererIndex) ||
        !InvokeManagedResult<int, GetInt>(
            api_.universalGetRenderType, &renderType, sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetRenderPostProcessing, &renderPostProcessing,
            sourceUniversalData) ||
        !InvokeManagedResult<int, GetInt>(
            api_.universalGetAntialiasing, &sourceAntialiasing,
            sourceUniversalData) ||
        !InvokeManagedResult<int, GetInt>(
            api_.universalGetAntialiasingQuality, &antialiasingQuality,
            sourceUniversalData) ||
        !InvokeManagedResult<int, GetInt>(
            api_.universalGetRequiresDepthOption, &requiresDepthOption,
            sourceUniversalData) ||
        !InvokeManagedResult<int, GetInt>(
            api_.universalGetRequiresColorOption, &requiresColorOption,
            sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetRequiresDepthTexture, &requiresDepthTexture,
            sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetRequiresColorTexture, &requiresColorTexture,
            sourceUniversalData) ||
        !InvokeManagedResult<UnityLayerMaskValue, UnityLayerMaskValue (*)(void*, void*)>(
            api_.universalGetVolumeLayerMask, &volumeLayerMask,
            sourceUniversalData) ||
        !InvokeManagedResult<void*, GetObject>(
            api_.universalGetVolumeTrigger, &sourceVolumeTrigger,
            sourceUniversalData) ||
        !InvokeManagedResult<int, GetInt>(
            api_.universalGetVolumeFrameworkUpdateMode,
            &volumeFrameworkUpdateMode, sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetStopNan, &stopNan, sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetDithering, &dithering, sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetAllowHdrOutput, &allowHdrOutput,
            sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetRenderShadows, &renderShadows,
            sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetFastRendering, &fastRendering,
            sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetNeedsAlphaChannel, &needsAlphaChannel,
            sourceUniversalData) ||
        !ReadManagedField(sourceUniversalData,
                          api_.universalClearDepthOffset, &clearDepth) ||
        !ReadManagedField(sourceUniversalData,
                          api_.universalUseScreenCoordOverrideOffset,
                          &useScreenCoordOverride) ||
        !ReadManagedField(sourceUniversalData,
                          api_.universalScreenSizeOverrideOffset,
                          &screenSizeOverride) ||
        !ReadManagedField(sourceUniversalData,
                          api_.universalScreenCoordScaleBiasOffset,
                          &screenCoordScaleBias) ||
        !ReadManagedField(sourceUniversalData,
                          api_.universalRenderScaleOffset, &renderScale) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetRequiresVolumeFrameworkUpdate,
            &requiresVolumeFrameworkUpdate, sourceUniversalData) ||
        !InvokeManagedResult<void*, GetObject>(
            api_.universalGetVolumeStack, &sourceVolumeStack,
            sourceUniversalData) ||
        !InvokeManagedResult<void*, GetObject>(
            api_.universalGetTaaPersistentData, &sourceTaaPersistentData,
            sourceUniversalData) ||
        !InvokeManagedResult<void*, GetObject>(
            api_.universalGetMotionVectorsPersistentData,
            &sourceMotionVectorsPersistentData, sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetResetHistory, &sourceResetHistory,
            sourceUniversalData) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.cameraGetAllowHdr, &sourceCameraAllowHdr, sourceCamera) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.cameraGetAllowMsaa, &sourceCameraAllowMsaa, sourceCamera) ||
        !InvokeManagedResult<int, GetInt>(
            api_.cameraGetDepthTextureMode, &sourceDepthTextureMode,
            sourceCamera)) {
        return FailStage("configure.camera-data-read", eye);
    }
    if (eye == 0U) {
        if (sourceCameraSuppressed_) {
            // Keep consuming the source's own pulse so it does not go stale
            // and re-fire when the source camera is restored.
            const bool sourceCutPulse = ConsumeSourceTemporalCut(
                sourceCamera, sourceUniversalData, sourceResetHistory);
            if (GakumasLocal::Config::vrSourceOffResetPerFrame) {
                // .107 fix: per-frame resetHistory kills the source-off
                // character trail, but resetHistoryFrames > 0 promotes the
                // TAA blend to 1.0 → effectively no TAA while suppressed.
                ResetTemporalHistories("source-off-per-frame");
            } else if (sourceCutPulse) {
                // Probe mode (.108): pulse-driven cadence only, reproduces
                // the trail so TAA_INPUT forensics can catch the dirty
                // prev-frame data feeding the accumulator.
                ResetTemporalHistories("source-cut-pulse");
            }
        } else if (sourceResetHistory) {
            // Source is live: propagate its own Cinemachine pulse only.
            // The .101 poseCut heuristic must stay out of this path — a
            // fast dolly/zoom trips it every Tick and keeps flushing eye
            // TAA history mid-shot (blurry / unresolved shots).
            ResetTemporalHistories("source-reset-history");
            Log("[VR][stereo] SOURCE_HISTORY_RESET_PROPAGATED source=" +
                std::to_string(reinterpret_cast<std::uintptr_t>(sourceCamera)));
        }
    }
    if (sourceVolumeTrigger != nullptr) {
        eyeVolumeTrigger = sourceVolumeTrigger;
    } else if (!InvokeManagedResult<void*, GetObject>(
                   api_.componentGetTransform, &eyeVolumeTrigger,
                   eyeCameras_[eye]) ||
               eyeVolumeTrigger == nullptr) {
        return FailStage("configure.volume-trigger-fallback", eye);
    }

    // Default: copy the source camera's AA so sub-pixel material, hair,
    // highlight, shadow, and LOD stay temporally stable. Menu override
    // (TAA / SMAA) can replace that for phase-2 quality A/B without
    // changing the inherit baseline.
    UnityTaaSettingsPrefix sourceTaaSettings{};
    const bool sourceTaaValid =
        ReadTaaSettingsPrefix(sourceUniversalData,
                              api_.universalTaaSettingsOffset,
                              &sourceTaaSettings) &&
        IsValidTaaSettings(sourceTaaSettings);
    const EyeAaResolve eyeAa = ResolveEyeAa(
        sourceAntialiasing, antialiasingQuality, sourceTaaSettings,
        sourceTaaValid, smaaT2xActiveForPair_ || tscmaaActiveForPair_);
    const int eyeAntialiasing = eyeAa.antialiasing;
    const int eyeAntialiasingQuality = eyeAa.antialiasingQuality;

    // Camera::CopyFrom still preserves the source Camera.allowHDR setting for
    // HDR intermediates and SDR tone mapping. UniversalAdditionalCameraData's
    // allowHDROutput flag is different: it permits final HDR-display output.
    // The OpenXR projection target used here is an 8-bit sRGB swapchain, so do
    // not inherit the desktop camera's display-HDR permission into the eyes.
    constexpr bool kVrAllowHdrOutput = false;
    // URP VolumeFrameworkUpdateMode.ViaScripting. Source cameras use
    // UsePipelineSettings (2) and the pipeline reblends VLDOF after we
    // write `active=false`. Own the eye stacks so the zeroed CoC sticks.
    constexpr int kVolumeUpdateViaScripting = 1;

    UnityVector4 eyeScreenSizeOverride = screenSizeOverride;
    if (useScreenCoordOverride && fullWidth_ != 0U && fullHeight_ != 0U) {
        eyeScreenSizeOverride = UnityVector4(
            static_cast<float>(fullWidth_),
            static_cast<float>(fullHeight_),
            1.0F / static_cast<float>(fullWidth_),
            1.0F / static_cast<float>(fullHeight_));
    }

    Log("[VR][stereo] CALL_BEGIN stage=configure.camera-data-copy eye=" +
        eyeName);
    if (!InvokeManagedVoid<SetInt>(
            api_.universalSetRenderer, eyeUniversalCameraData_[eye],
            rendererIndex) ||
        !InvokeManagedVoid<SetInt>(
            api_.universalSetRenderType, eyeUniversalCameraData_[eye],
            renderType) ||
        !InvokeManagedVoid<SetBool>(
            api_.universalSetRenderPostProcessing,
            eyeUniversalCameraData_[eye], renderPostProcessing) ||
        !InvokeManagedVoid<SetInt>(
            api_.universalSetAntialiasing,
            eyeUniversalCameraData_[eye], eyeAntialiasing) ||
        !InvokeManagedVoid<SetInt>(
            api_.universalSetAntialiasingQuality,
            eyeUniversalCameraData_[eye], eyeAntialiasingQuality) ||
        !InvokeManagedVoid<SetInt>(
            api_.universalSetRequiresDepthOption,
            eyeUniversalCameraData_[eye], requiresDepthOption) ||
        !InvokeManagedVoid<SetInt>(
            api_.universalSetRequiresColorOption,
            eyeUniversalCameraData_[eye], requiresColorOption) ||
        !InvokeManagedVoid<void (*)(void*, UnityLayerMaskValue, void*)>(
            api_.universalSetVolumeLayerMask,
            eyeUniversalCameraData_[eye], volumeLayerMask) ||
        !InvokeManagedVoid<SetObject>(
            api_.universalSetVolumeTrigger,
            eyeUniversalCameraData_[eye], eyeVolumeTrigger) ||
        !InvokeManagedVoid<SetInt>(
            api_.universalSetVolumeFrameworkUpdateMode,
            eyeUniversalCameraData_[eye], kVolumeUpdateViaScripting) ||
        !InvokeManagedVoid<SetBool>(
            api_.universalSetStopNan,
            eyeUniversalCameraData_[eye], stopNan) ||
        !InvokeManagedVoid<SetBool>(
            api_.universalSetDithering,
            eyeUniversalCameraData_[eye], dithering) ||
        !InvokeManagedVoid<SetBool>(
            api_.universalSetAllowHdrOutput,
            eyeUniversalCameraData_[eye], kVrAllowHdrOutput) ||
        !InvokeManagedVoid<SetBool>(
            api_.universalSetFastRendering,
            eyeUniversalCameraData_[eye], fastRendering) ||
        !InvokeManagedVoid<SetBool>(
            api_.universalSetNeedsAlphaChannel,
            eyeUniversalCameraData_[eye], needsAlphaChannel) ||
        !InvokeManagedVoid<SetBool>(
            api_.universalSetAllowXrRendering,
            eyeUniversalCameraData_[eye], false) ||
        !InvokeManagedVoid<SetBool>(
            api_.universalSetRenderShadows,
            eyeUniversalCameraData_[eye], renderShadows) ||
        !InvokeManagedVoid<SetTarget>(
            api_.vlAdditionalSetTargetTexture,
            eyeVlAdditionalCameraData_[eye], target) ||
        !WriteManagedField(eyeUniversalCameraData_[eye],
                           api_.universalClearDepthOffset, clearDepth) ||
        !WriteManagedField(eyeUniversalCameraData_[eye],
                           api_.universalUseScreenCoordOverrideOffset,
                           useScreenCoordOverride) ||
        !WriteManagedField(eyeUniversalCameraData_[eye],
                           api_.universalScreenSizeOverrideOffset,
                           eyeScreenSizeOverride) ||
        !WriteManagedField(eyeUniversalCameraData_[eye],
                           api_.universalScreenCoordScaleBiasOffset,
                           screenCoordScaleBias) ||
        !WriteManagedField(eyeUniversalCameraData_[eye],
                           api_.universalRenderScaleOffset, renderScale)) {
        return FailStage("configure.camera-data-copy", eye);
    }

    using NoArgumentVoid = void (*)(void*, void*);
    if (!InvokeManagedVoid<NoArgumentVoid>(
            api_.universalGetOrCreateVolumeStack,
            eyeUniversalCameraData_[eye])) {
        return FailStage("configure.volume-stack-create", eye);
    }

    // Unity's live API table in stereo.192 proved this exact public setter.
    // CopyFrom may preserve the source flags, but source-off mode skips
    // CopyFrom, so write and verify the complete desired value every pair.
    constexpr int kDepthTextureModeMotionVectors = 4;
    const bool nativeTemporalAaRequested =
        smaaT2xRequestedForPair_ || tscmaaRequestedForPair_;
    const int desiredDepthTextureMode = nativeTemporalAaRequested
        ? sourceDepthTextureMode | kDepthTextureModeMotionVectors
        : sourceDepthTextureMode;
    if (!InvokeManagedVoid<SetInt>(
            api_.cameraSetDepthTextureMode, eyeCameras_[eye],
            desiredDepthTextureMode)) {
        return FailStage("configure.eye-depth-texture-mode", eye);
    }

    bool eyeRequiresVolumeFrameworkUpdate = false;
    if (!InvokeManagedResult<bool, GetBool>(
            api_.universalGetRequiresVolumeFrameworkUpdate,
            &eyeRequiresVolumeFrameworkUpdate,
            eyeUniversalCameraData_[eye])) {
        return FailStage("configure.volume-update-mode", eye);
    }
    bool volumeManuallyUpdated = false;
    {
        ClearEyeBloomOverrides(eye);
        using UpdateVolumeStack = void (*)(void*, void*);
        if (!InvokeManagedVoid<UpdateVolumeStack>(
                api_.cameraExtensionsUpdateVolumeStack,
                eyeCameras_[eye])) {
            return FailStage("configure.volume-stack-update", eye);
        }
        volumeManuallyUpdated = true;
    }

    bool eyeRequiresDepthTexture = false;
    bool eyeRequiresColorTexture = false;
    bool eyeCameraAllowHdr = false;
    bool eyeCameraAllowMsaa = false;
    bool eyeRenderPostProcessing = true;
    int eyeDepthTextureMode = 0;
    if (!InvokeManagedResult<bool, GetBool>(
            api_.universalGetRequiresDepthTexture,
            &eyeRequiresDepthTexture, eyeUniversalCameraData_[eye]) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetRequiresColorTexture,
            &eyeRequiresColorTexture, eyeUniversalCameraData_[eye]) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.universalGetRenderPostProcessing,
            &eyeRenderPostProcessing, eyeUniversalCameraData_[eye]) ||
        !InvokeManagedResult<void*, GetObject>(
            api_.universalGetVolumeStack, &eyeVolumeStacks_[eye],
            eyeUniversalCameraData_[eye]) ||
        !InvokeManagedResult<void*, GetObject>(
            api_.universalGetTaaPersistentData,
            &eyeTaaPersistentData_[eye], eyeUniversalCameraData_[eye]) ||
        !InvokeManagedResult<void*, GetObject>(
            api_.universalGetMotionVectorsPersistentData,
            &eyeMotionVectorsPersistentData_[eye],
            eyeUniversalCameraData_[eye]) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.cameraGetAllowHdr, &eyeCameraAllowHdr,
            eyeCameras_[eye]) ||
        !InvokeManagedResult<bool, GetBool>(
            api_.cameraGetAllowMsaa, &eyeCameraAllowMsaa,
            eyeCameras_[eye]) ||
        !InvokeManagedResult<int, GetInt>(
            api_.cameraGetDepthTextureMode, &eyeDepthTextureMode,
            eyeCameras_[eye])) {
        return FailStage("configure.camera-data-verify", eye);
    }
    if (eyeRenderPostProcessing != renderPostProcessing) {
        return FailStage("configure.eye-postprocess-restore", eye);
    }
    if (eyeDepthTextureMode != desiredDepthTextureMode) {
        const std::string aaTag = tscmaaRequestedForPair_
            ? "[VR][tscmaa] TSCMAA_FAULT"
            : "[VR][smaa-t2x] SMAA_T2X_FAULT";
        Log(aaTag + " depth-mode-readback eye=" + eyeName + " requested=" +
            std::to_string(desiredDepthTextureMode) + " readback=" +
            std::to_string(eyeDepthTextureMode));
        return FailStage("configure.eye-depth-texture-mode-readback", eye);
    }
    if (nativeTemporalAaRequested &&
        (verboseFrameLog_ || publishedFrames_ < 2U)) {
        const std::string aaTag = tscmaaRequestedForPair_
            ? "[VR][tscmaa] TSCMAA_API"
            : "[VR][smaa-t2x] SMAA_T2X_API";
        Log(aaTag + " depth-mode eye=" + eyeName +
            " source=" + std::to_string(sourceDepthTextureMode) +
            " applied=" + std::to_string(eyeDepthTextureMode));
    }
    const bool volumeAliased = eyeVolumeStacks_[eye] == nullptr ||
        (sourceVolumeStack != nullptr &&
         eyeVolumeStacks_[eye] == sourceVolumeStack) ||
        (eye == 1U && eyeVolumeStacks_[0] != nullptr &&
         eyeVolumeStacks_[eye] == eyeVolumeStacks_[0]);
    const bool taaAliased =
        (sourceTaaPersistentData != nullptr &&
         eyeTaaPersistentData_[eye] == sourceTaaPersistentData) ||
        (eye == 1U && eyeTaaPersistentData_[0] != nullptr &&
         eyeTaaPersistentData_[eye] == eyeTaaPersistentData_[0]);
    const bool motionAliased =
        (sourceMotionVectorsPersistentData != nullptr &&
         eyeMotionVectorsPersistentData_[eye] ==
             sourceMotionVectorsPersistentData) ||
        (eye == 1U && eyeMotionVectorsPersistentData_[0] != nullptr &&
         eyeMotionVectorsPersistentData_[eye] ==
             eyeMotionVectorsPersistentData_[0]);
    if (volumeAliased || taaAliased || motionAliased) {
        Log("[VR][stereo] PER_EYE_STATE_ALIAS_FAULT eye=" + eyeName +
            " volume=" + (volumeAliased ? "1" : "0") +
            " taa=" + (taaAliased ? "1" : "0") +
            " motion=" + (motionAliased ? "1" : "0"));
        return FailStage("configure.per-eye-state-alias", eye);
    }
    BindEyeDepthOfField(eye, sourceVolumeStack);
    BindEyeUrpDepthOfField(eye, sourceVolumeStack);
    latestSourceVolumeStack_ = sourceVolumeStack;
    BindEyeVolumeComponent(
        eye, sourceVolumeStack, api_.vlBloomReflectionType,
        eyeVlBloomComponents_, eyeVlBloomBindLogged_,
        "EYE_VL_BLOOM_SUPPRESS_ARMED", "EYE_VL_BLOOM_SUPPRESS_SKIPPED");
    BindEyeVolumeComponent(
        eye, sourceVolumeStack, api_.urpBloomReflectionType,
        eyeUrpBloomComponents_, eyeUrpBloomBindLogged_,
        "EYE_URP_BLOOM_SUPPRESS_ARMED", "EYE_URP_BLOOM_SUPPRESS_SKIPPED");
    CacheAuthoredBloom(eye);
    ApplyModeBodyBloom(eye);
    DeactivateBoundEyeDepthOfField();

    UnityTaaSettingsPrefix eyeTaaSettings{};
    bool taaSettingsCopied = false;
    if (eyeAa.writeTaa &&
        WriteTaaSettingsPrefix(eyeUniversalCameraData_[eye],
                               api_.universalTaaSettingsOffset,
                               eyeAa.taa) &&
        ReadTaaSettingsPrefix(eyeUniversalCameraData_[eye],
                              api_.universalTaaSettingsOffset,
                              &eyeTaaSettings)) {
        taaSettingsCopied = true;
    }
    const bool aaSignatureChanged =
        lastAppliedAaMode_ != eyeAa.mode ||
        lastAppliedEyeAntialiasing_ != eyeAntialiasing ||
        lastAppliedEyeAntialiasingQuality_ != eyeAntialiasingQuality ||
        lastAppliedTaaQuality_ != eyeTaaSettings.quality ||
        lastAppliedTaaFrameInfluence_ != eyeTaaSettings.frameInfluence ||
        lastAppliedTaaJitterScale_ != eyeTaaSettings.jitterScale ||
        lastAppliedTaaMipBias_ != eyeTaaSettings.mipBias ||
        lastAppliedTaaVarianceClamp_ != eyeTaaSettings.varianceClampScale ||
        lastAppliedTaaSharpen_ != eyeTaaSettings.contrastAdaptiveSharpening;
    if (aaSignatureChanged) {
        ResetTemporalHistories("aa-signature-change");
        lastAppliedAaMode_ = eyeAa.mode;
        lastAppliedEyeAntialiasing_ = eyeAntialiasing;
        lastAppliedEyeAntialiasingQuality_ = eyeAntialiasingQuality;
        lastAppliedTaaQuality_ = eyeTaaSettings.quality;
        lastAppliedTaaFrameInfluence_ = eyeTaaSettings.frameInfluence;
        lastAppliedTaaJitterScale_ = eyeTaaSettings.jitterScale;
        lastAppliedTaaMipBias_ = eyeTaaSettings.mipBias;
        lastAppliedTaaVarianceClamp_ = eyeTaaSettings.varianceClampScale;
        lastAppliedTaaSharpen_ = eyeTaaSettings.contrastAdaptiveSharpening;
        Log(std::string("[VR][stereo] EYE_AA_OVERRIDE mode=") +
            EyeAaModeName(eyeAa.mode) +
            " override=" + (eyeAa.overrideApplied ? "1" : "0") +
            " eyeAntialiasing=" + std::to_string(eyeAntialiasing) +
            " eyeAntialiasingQuality=" +
                std::to_string(eyeAntialiasingQuality) +
            " taaSettingsCopied=" + (taaSettingsCopied ? "1" : "0") +
            " taaQuality=" + std::to_string(eyeTaaSettings.quality) +
            " taaFrameInfluence=" +
                std::to_string(eyeTaaSettings.frameInfluence) +
            " taaJitterScale=" + std::to_string(eyeTaaSettings.jitterScale) +
            " taaMipBias=" + std::to_string(eyeTaaSettings.mipBias) +
            " taaVarianceClamp=" +
                std::to_string(eyeTaaSettings.varianceClampScale) +
            " taaSharpen=" +
                std::to_string(eyeTaaSettings.contrastAdaptiveSharpening));
    }
    if (taaHistoryResetPending_[eye]) {
        if (!InvokeManagedVoid<SetBool>(
                api_.universalSetResetHistory,
                eyeUniversalCameraData_[eye], true)) {
            return FailStage("configure.temporal-history-reset", eye);
        }
        taaHistoryResetPending_[eye] = false;
        // Suppressed mode resets every frame (80+ lines/s); keep the
        // pulse-driven cadence verbose and throttle the per-frame flood.
        const bool perFrameReset = sourceCameraSuppressed_;
        ++historyResetWrites_[eye];
        if (!perFrameReset || historyResetWrites_[eye] % 300U == 1U) {
            Log("[VR][stereo] VLSRP_HISTORY_RESET eye=" + eyeName +
                " motion=1 taa=" +
                (eyeAntialiasing == kAntialiasingTaa ? "1" : "0") +
                " resetApi=UniversalAdditionalCameraData" +
                (perFrameReset ? " perFrame=1" : ""));
        }
    }
    Log("[VR][stereo] CALL_OK stage=configure.camera-data-copy eye=" +
        eyeName + " renderShadows=" + (renderShadows ? "1" : "0") +
        " allowXR=0");
    Log("[VR][stereo] CAMERA_DATA_COPIED eye=" + eyeName +
        " aaMode=" + std::string(EyeAaModeName(eyeAa.mode)) +
        " aaOverride=" + (eyeAa.overrideApplied ? "1" : "0") +
        " sourceAntialiasing=" + std::to_string(sourceAntialiasing) +
        " eyeAntialiasing=" + std::to_string(eyeAntialiasing) +
        " eyeAntialiasingQuality=" + std::to_string(eyeAntialiasingQuality) +
        " taaSettingsCopied=" + (taaSettingsCopied ? "1" : "0") +
        " taaQuality=" + std::to_string(eyeTaaSettings.quality) +
        " taaFrameInfluence=" + std::to_string(eyeTaaSettings.frameInfluence) +
        " taaJitterScale=" + std::to_string(eyeTaaSettings.jitterScale) +
        " taaMipBias=" + std::to_string(eyeTaaSettings.mipBias) +
        " taaVarianceClamp=" + std::to_string(eyeTaaSettings.varianceClampScale) +
        " taaSharpen=" + std::to_string(
            eyeTaaSettings.contrastAdaptiveSharpening) +
        " volumeLayerMask=" + std::to_string(volumeLayerMask.mask) +
        " sourceVolumeTrigger=" + (sourceVolumeTrigger != nullptr ? "1" : "0") +
        " eyeVolumeTrigger=" + (sourceVolumeTrigger != nullptr ? "source" : "eye") +
        " volumeUpdateMode=" + std::to_string(volumeFrameworkUpdateMode) +
        " eyeVolumeUpdateMode=" + std::to_string(kVolumeUpdateViaScripting) +
        " volumeEffectiveSource=" +
            (requiresVolumeFrameworkUpdate ? "1" : "0") +
        " volumeEffectiveEye=" +
            (eyeRequiresVolumeFrameworkUpdate ? "1" : "0") +
        " volumeManualUpdate=" + (volumeManuallyUpdated ? "1" : "0") +
        " volumeStack=" + std::to_string(
            reinterpret_cast<std::uintptr_t>(eyeVolumeStacks_[eye])) +
        " taaState=" + std::to_string(
            reinterpret_cast<std::uintptr_t>(eyeTaaPersistentData_[eye])) +
        " motionState=" + std::to_string(
            reinterpret_cast<std::uintptr_t>(eyeMotionVectorsPersistentData_[eye])) +
        " depthSource=" + (requiresDepthTexture ? "1" : "0") +
        " depthEye=" + (eyeRequiresDepthTexture ? "1" : "0") +
        " colorSource=" + (requiresColorTexture ? "1" : "0") +
        " colorEye=" + (eyeRequiresColorTexture ? "1" : "0") +
        " clearDepth=" + (clearDepth ? "1" : "0") +
        " renderScale=" + std::to_string(renderScale) +
        " screenOverride=" + (useScreenCoordOverride ? "1" : "0") +
        " sourceAllowHDROutput=" + (allowHdrOutput ? "1" : "0") +
        " eyeAllowHDROutput=0" +
        " cameraHdrSource=" + (sourceCameraAllowHdr ? "1" : "0") +
        " cameraHdrEye=" + (eyeCameraAllowHdr ? "1" : "0") +
        " cameraMsaaSource=" + (sourceCameraAllowMsaa ? "1" : "0") +
        " cameraMsaaEye=" + (eyeCameraAllowMsaa ? "1" : "0") +
        " depthModeSource=" + std::to_string(sourceDepthTextureMode) +
        " depthModeEye=" + std::to_string(eyeDepthTextureMode) +
        " allowXR=0 renderShadows=" + (renderShadows ? "1" : "0") +
        " sourcePost=" + (renderPostProcessing ? "1" : "0") +
        " eyePost=" + (eyeRenderPostProcessing ? "1" : "0") +
        " vlDofSuppress=" + (eyeDofComponents_[eye] != nullptr ? "1" : "0") +
        " urpDofSuppress=" +
            (eyeUrpDofComponents_[eye] != nullptr ? "1" : "0") +
        " vlBloomSuppress=" +
            (eyeVlBloomComponents_[eye] != nullptr ? "1" : "0") +
        " urpBloomSuppress=" +
            (eyeUrpBloomComponents_[eye] != nullptr ? "1" : "0") +
        " caScale=" +
            (eyeChromaticAberrationComponents_[eye] != nullptr ? "1" : "0") +
        " distortionScale=" +
            (eyeLensDistortionComponents_[eye] != nullptr ? "1" : "0") +
        " motionBlurScale=" +
            (eyeMotionBlurComponents_[eye] != nullptr ? "1" : "0") +
        " vignetteScale=" +
            (eyeVignetteComponents_[eye] != nullptr ? "1" : "0") +
        " modeFov=" + std::to_string(bloomSourceFovDegrees_) +
        " liveFov=" + std::to_string(sourceLiveFovDegrees_) +
        " fittedFov=" + std::to_string(sourceFittedFovDegrees_) +
        " sourceFocal=" + std::to_string(sourceFocalLengthMillimeters_) +
        " dofActiveOffset=" +
            std::to_string(api_.volumeComponentActiveOffset));
    if (!eyePostProcessRestoredLogged_) {
        eyePostProcessRestoredLogged_ = true;
        Log(std::string("[VR][stereo] EYE_POSTPROCESS_RESTORED sourcePost=") +
            (renderPostProcessing ? "1" : "0") +
            " eyePost=" + (eyeRenderPostProcessing ? "1" : "0"));
    }

    if (additionalDataLoggedSource_ != sourceCamera ||
        additionalDataLoggedGeneration_ != latestTargetSpec_.generation) {
        additionalDataLoggedSource_ = sourceCamera;
        additionalDataLoggedGeneration_ = latestTargetSpec_.generation;
        UnityRenderTextureDescriptorValue controllerFinalDescriptor{};
        UnityRenderTextureDescriptorValue controllerHdrDescriptor{};
        bool controllerDescriptorsReady = false;
        if (sourceVlController != nullptr &&
            api_.vlControllerGetTargetTextureDescriptor.Ready() &&
            api_.vlControllerGetHdrTargetTextureDescriptor.Ready()) {
            std::string descriptorException;
            if (!ReadRenderTextureDescriptor(
                    api_.vlControllerGetTargetTextureDescriptor,
                    sourceVlController, api_.renderTextureDescriptorValueSize,
                    &controllerFinalDescriptor, &descriptorException) ||
                !ReadRenderTextureDescriptor(
                    api_.vlControllerGetHdrTargetTextureDescriptor,
                    sourceVlController, api_.renderTextureDescriptorValueSize,
                    &controllerHdrDescriptor, &descriptorException)) {
                Log("[VR][stereo] SOURCE_VLSRP_DESCRIPTOR_REJECTED detail=\"" +
                    descriptorException + '"');
            } else {
                controllerDescriptorsReady = true;
                Log("[VR][stereo] SOURCE_VLSRP_DESCRIPTOR final={" +
                    DescriptorState(controllerFinalDescriptor) + "} hdr={" +
                    DescriptorState(controllerHdrDescriptor) + "}");
            }
        } else if (sourceVlController != nullptr) {
            Log("[VR][stereo] SOURCE_VLSRP_DESCRIPTOR_SKIPPED reason=diagnostic-api-unavailable");
        }
        std::ostringstream state;
        state << "[VR][stereo] SOURCE_CAMERA_DATA source=" << sourceCamera
              << " universal=1 vlsrpAdditional="
              << (sourceVlAdditionalData != nullptr)
              << " vlsrpController=" << (sourceVlController != nullptr)
              << " rendererIndex=" << rendererIndex
              << " renderType=" << renderType
              << " post=" << renderPostProcessing
              << " eyePost=" << eyeRenderPostProcessing
              << " aaMode=" << EyeAaModeName(eyeAa.mode)
              << " aaOverride=" << (eyeAa.overrideApplied ? "1" : "0")
              << " sourceAntialiasing=" << sourceAntialiasing
              << " antialiasingQuality=" << antialiasingQuality
              << " eyeAntialiasing=" << eyeAntialiasing
              << " eyeAntialiasingQuality=" << eyeAntialiasingQuality
              << " taaSettingsCopied=" << taaSettingsCopied
              << " taaQuality=" << eyeTaaSettings.quality
              << " taaFrameInfluence=" << eyeTaaSettings.frameInfluence
              << " taaJitterScale=" << eyeTaaSettings.jitterScale
              << " taaMipBias=" << eyeTaaSettings.mipBias
              << " taaVarianceClamp=" << eyeTaaSettings.varianceClampScale
              << " taaSharpen="
              << eyeTaaSettings.contrastAdaptiveSharpening
              << " depthOption=" << requiresDepthOption
              << " colorOption=" << requiresColorOption
              << " depthTexture=" << requiresDepthTexture
              << " colorTexture=" << requiresColorTexture
              << " eyeDepthTexture=" << eyeRequiresDepthTexture
              << " eyeColorTexture=" << eyeRequiresColorTexture
              << " volumeLayerMask=" << volumeLayerMask.mask
              << " sourceVolumeTrigger=" << (sourceVolumeTrigger != nullptr)
              << " eyeVolumeTrigger="
              << (sourceVolumeTrigger != nullptr ? "source" : "eye")
              << " volumeUpdateMode=" << volumeFrameworkUpdateMode
              << " volumeEffectiveSource="
              << requiresVolumeFrameworkUpdate
              << " volumeEffectiveEye="
               << eyeRequiresVolumeFrameworkUpdate
              << " volumeManualUpdate=" << volumeManuallyUpdated
              << " sourceVolumeStack=" << sourceVolumeStack
              << " eyeVolumeStack=" << eyeVolumeStacks_[eye]
              << " sourceTaaState=" << sourceTaaPersistentData
              << " eyeTaaState=" << eyeTaaPersistentData_[eye]
              << " sourceMotionState=" << sourceMotionVectorsPersistentData
              << " eyeMotionState=" << eyeMotionVectorsPersistentData_[eye]
              << " clearDepth=" << clearDepth
              << " screenOverride=" << useScreenCoordOverride
              << " screenSize=" << eyeScreenSizeOverride.x << ','
              << eyeScreenSizeOverride.y << ',' << eyeScreenSizeOverride.z
              << ',' << eyeScreenSizeOverride.w
              << " screenScaleBias=" << screenCoordScaleBias.x << ','
              << screenCoordScaleBias.y << ',' << screenCoordScaleBias.z
              << ',' << screenCoordScaleBias.w
              << " renderScale=" << renderScale
              << " stopNaN=" << stopNan
              << " dithering=" << dithering
              << " sourceAllowHDROutput=" << allowHdrOutput
              << " eyeAllowHDROutput=0"
              << " sourceShadows=" << renderShadows
              << " eyeShadows=" << renderShadows
              << " cameraHdrSource=" << sourceCameraAllowHdr
              << " cameraHdrEye=" << eyeCameraAllowHdr
              << " cameraMsaaSource=" << sourceCameraAllowMsaa
              << " cameraMsaaEye=" << eyeCameraAllowMsaa
              << " depthModeSource=" << sourceDepthTextureMode
              << " depthModeEye=" << eyeDepthTextureMode
              << " controllerDescriptors=" << controllerDescriptorsReady
              << " controllerInstances=source-only fast=" << fastRendering
              << " alpha=" << needsAlphaChannel
              << " generation=" << latestTargetSpec_.generation;
        Log(state.str());
    }
    return true;
}

void UnityStereoRenderer::BindEyeDepthOfField(
    std::size_t eye,
    void* sourceVolumeStack) noexcept {
    if (eye >= eyeDofComponents_.size()) {
        return;
    }
    const char* eyeName = eye == 0U ? "left" : "right";
    void* next = nullptr;
    const char* skipReason = nullptr;
    if (!api_.volumeStackGetComponent.Ready() ||
        api_.vlDofReflectionType == nullptr) {
        skipReason = "diagnostic-api-unavailable";
    } else if (eyeVolumeStacks_[eye] == nullptr) {
        skipReason = "eye-stack-missing";
    } else {
        using GetComponent = void* (*)(void*, void*, void*);
        if (!InvokeManagedResult<void*, GetComponent>(
                api_.volumeStackGetComponent, &next, eyeVolumeStacks_[eye],
                api_.vlDofReflectionType)) {
            skipReason = "get-component-failed";
            next = nullptr;
        } else if (next == nullptr) {
            skipReason = "component-missing";
        } else if (sourceVolumeStack != nullptr) {
            void* sourceDof = nullptr;
            if (InvokeManagedResult<void*, GetComponent>(
                    api_.volumeStackGetComponent, &sourceDof, sourceVolumeStack,
                    api_.vlDofReflectionType)) {
                sourceDofComponent_ = sourceDof;
                if (sourceDof != nullptr && sourceDof == next) {
                    skipReason = "source-alias";
                    next = nullptr;
                }
            }
        }
        if (next != nullptr && eye == 1U &&
            eyeDofComponents_[0] != nullptr &&
            next == eyeDofComponents_[0]) {
            skipReason = "left-alias";
            next = nullptr;
        }
    }
    eyeDofComponents_[eye] = next;
    if (next == nullptr) {
        if (!eyeDofBindLogged_[eye]) {
            eyeDofBindLogged_[eye] = true;
            Log("[VR][stereo] EYE_VLDOF_SUPPRESS_SKIPPED eye=" +
                std::string(eyeName) + " reason=" +
                (skipReason != nullptr ? skipReason : "unknown"));
        }
        return;
    }
    if (!eyeDofBindLogged_[eye]) {
        eyeDofBindLogged_[eye] = true;
        Log("[VR][stereo] EYE_VLDOF_SUPPRESS_ARMED eye=" +
            std::string(eyeName) + " instance=" +
            std::to_string(reinterpret_cast<std::uintptr_t>(next)) +
            " sourceInstance=" +
            std::to_string(reinterpret_cast<std::uintptr_t>(sourceDofComponent_)));
    }
    if (api_.volumeComponentSetActive.Ready()) {
        using SetActive = void (*)(void*, bool, void*);
        if (!InvokeManagedVoid<SetActive>(
                api_.volumeComponentSetActive, next, false)) {
            Log("[VR][stereo] EYE_VLDOF_SET_ACTIVE_FAILED eye=" +
                std::string(eyeName));
        }
    }
}

void UnityStereoRenderer::BindEyeUrpDepthOfField(
    std::size_t eye,
    void* sourceVolumeStack) noexcept {
    if (eye >= eyeUrpDofComponents_.size()) {
        return;
    }
    const char* eyeName = eye == 0U ? "left" : "right";
    void* next = nullptr;
    const char* skipReason = nullptr;
    if (!api_.volumeStackGetComponent.Ready() ||
        api_.urpDofReflectionType == nullptr) {
        skipReason = "diagnostic-api-unavailable";
    } else if (eyeVolumeStacks_[eye] == nullptr) {
        skipReason = "eye-stack-missing";
    } else {
        using GetComponent = void* (*)(void*, void*, void*);
        if (!InvokeManagedResult<void*, GetComponent>(
                api_.volumeStackGetComponent, &next, eyeVolumeStacks_[eye],
                api_.urpDofReflectionType)) {
            skipReason = "get-component-failed";
            next = nullptr;
        } else if (next == nullptr) {
            skipReason = "component-missing";
        } else if (sourceVolumeStack != nullptr) {
            void* sourceDof = nullptr;
            if (InvokeManagedResult<void*, GetComponent>(
                    api_.volumeStackGetComponent, &sourceDof, sourceVolumeStack,
                    api_.urpDofReflectionType) &&
                sourceDof != nullptr && sourceDof == next) {
                skipReason = "source-alias";
                next = nullptr;
            }
        }
        if (next != nullptr && eye == 1U &&
            eyeUrpDofComponents_[0] != nullptr &&
            next == eyeUrpDofComponents_[0]) {
            skipReason = "left-alias";
            next = nullptr;
        }
    }
    eyeUrpDofComponents_[eye] = next;
    if (next == nullptr) {
        if (!eyeUrpDofBindLogged_[eye]) {
            eyeUrpDofBindLogged_[eye] = true;
            Log("[VR][stereo] EYE_URP_DOF_SUPPRESS_SKIPPED eye=" +
                std::string(eyeName) + " reason=" +
                (skipReason != nullptr ? skipReason : "unknown"));
        }
        return;
    }
    if (!eyeUrpDofBindLogged_[eye]) {
        eyeUrpDofBindLogged_[eye] = true;
        Log("[VR][stereo] EYE_URP_DOF_SUPPRESS_ARMED eye=" +
            std::string(eyeName) + " instance=" +
            std::to_string(reinterpret_cast<std::uintptr_t>(next)));
    }
}

void UnityStereoRenderer::BindEyeVolumeComponent(
    std::size_t eye,
    void* sourceVolumeStack,
    void* reflectionType,
    std::array<void*, 2>& slots,
    std::array<bool, 2>& logged,
    const char* armedTag,
    const char* skippedTag) noexcept {
    if (eye >= slots.size() || armedTag == nullptr || skippedTag == nullptr) {
        return;
    }
    const char* eyeName = eye == 0U ? "left" : "right";
    void* next = nullptr;
    const char* skipReason = nullptr;
    if (!api_.volumeStackGetComponent.Ready() || reflectionType == nullptr) {
        skipReason = "diagnostic-api-unavailable";
    } else if (eyeVolumeStacks_[eye] == nullptr) {
        skipReason = "eye-stack-missing";
    } else {
        using GetComponent = void* (*)(void*, void*, void*);
        if (!InvokeManagedResult<void*, GetComponent>(
                api_.volumeStackGetComponent, &next, eyeVolumeStacks_[eye],
                reflectionType)) {
            skipReason = "get-component-failed";
            next = nullptr;
        } else if (next == nullptr) {
            skipReason = "component-missing";
        } else if (sourceVolumeStack != nullptr) {
            void* sourceComponent = nullptr;
            if (InvokeManagedResult<void*, GetComponent>(
                    api_.volumeStackGetComponent, &sourceComponent,
                    sourceVolumeStack, reflectionType) &&
                sourceComponent != nullptr && sourceComponent == next) {
                skipReason = "source-alias";
                next = nullptr;
            }
        }
        if (next != nullptr && eye == 1U && slots[0] != nullptr &&
            next == slots[0]) {
            skipReason = "left-alias";
            next = nullptr;
        }
    }
    slots[eye] = next;
    if (next == nullptr) {
        if (!logged[eye]) {
            logged[eye] = true;
            Log(std::string("[VR][stereo] ") + skippedTag + " eye=" +
                eyeName + " reason=" +
                (skipReason != nullptr ? skipReason : "unknown"));
        }
        return;
    }
    if (!logged[eye]) {
        logged[eye] = true;
        Log(std::string("[VR][stereo] ") + armedTag + " eye=" + eyeName +
            " instance=" +
            std::to_string(reinterpret_cast<std::uintptr_t>(next)));
    }
}

void UnityStereoRenderer::DeactivateVolumeComponent(
    void* component,
    const char* label) noexcept {
    if (component == nullptr || api_.volumeComponentActiveOffset < 0) {
        return;
    }
    bool before = true;
    bool after = true;
    const bool readBefore = ReadManagedField(
        component, api_.volumeComponentActiveOffset, &before);
    if (!WriteManagedField(
            component, api_.volumeComponentActiveOffset, false)) {
        if (!eyeDofDeactivatedLogged_) {
            Log(std::string("[VR][stereo] EYE_DOF_ACTIVE_WRITE_FAILED kind=") +
                label);
        }
        return;
    }
    const bool readAfter = ReadManagedField(
        component, api_.volumeComponentActiveOffset, &after);
    if (!eyeDofDeactivatedLogged_) {
        Log(std::string("[VR][stereo] EYE_DOF_ACTIVE_WRITTEN kind=") + label +
            " before=" + (readBefore && before ? "1" : "0") +
            " after=" + (readAfter && after ? "1" : "0") +
            " offset=" + std::to_string(api_.volumeComponentActiveOffset));
        if (readAfter && after) {
            Log("[VR][stereo] EYE_DOF_ACTIVE_WRITE_STUCK kind=" +
                std::string(label));
        }
    }
}

void UnityStereoRenderer::NeutralizeDepthOfFieldParameters(
    void* component,
    const char* label) noexcept {
    if (component == nullptr || label == nullptr) {
        return;
    }
    const std::string_view kind(label);
    const bool isVl = kind.find("vl") != std::string_view::npos;
    const bool isUrp = kind.find("urp") != std::string_view::npos;
    auto readPointer = [component](std::int32_t offset) -> void* {
        void* value = nullptr;
        if (offset < 0 ||
            !ReadManagedField(component, offset, &value)) {
            return nullptr;
        }
        return value;
    };
    auto resolveFromObject = [](void* parameter,
                                std::initializer_list<const char*> names,
                                std::initializer_list<const char*> typeNames)
        -> std::int32_t {
        if (parameter == nullptr) {
            return -1;
        }
        void* klass = nullptr;
        if (!ReadManagedField(parameter, 0, &klass) || klass == nullptr) {
            return -1;
        }
        return FindFieldOffsetOnIl2CppClass(klass, names, typeNames);
    };
    auto writeFloat = [this, &resolveFromObject](
                          void* parameter, float* beforeOut) -> bool {
        if (parameter == nullptr) {
            return false;
        }
        if (api_.volumeParameterFloatValueOffset < 0) {
            api_.volumeParameterFloatValueOffset = resolveFromObject(
                parameter, {"m_Value", "value"},
                {"System.Single", "Single"});
        }
        if (api_.volumeParameterOverrideOffset < 0) {
            api_.volumeParameterOverrideOffset = resolveFromObject(
                parameter, {"m_OverrideState", "overrideState"},
                {"System.Boolean", "Boolean"});
        }
        if (api_.volumeParameterFloatValueOffset < 0) {
            return false;
        }
        float before = -1.0F;
        ReadManagedField(
            parameter, api_.volumeParameterFloatValueOffset, &before);
        if (beforeOut != nullptr) {
            *beforeOut = before;
        }
        if (api_.volumeParameterOverrideOffset >= 0) {
            WriteManagedField(
                parameter, api_.volumeParameterOverrideOffset, true);
        }
        if (!WriteManagedField(
                parameter, api_.volumeParameterFloatValueOffset, 0.0F)) {
            return false;
        }
        float after = -1.0F;
        return ReadManagedField(
                   parameter, api_.volumeParameterFloatValueOffset,
                   &after) &&
            after == 0.0F;
    };
    auto writeInt = [this, &resolveFromObject](
                        void* parameter, std::int32_t value,
                        std::int32_t* beforeOut) -> bool {
        if (parameter == nullptr) {
            return false;
        }
        if (api_.volumeParameterIntValueOffset < 0) {
            api_.volumeParameterIntValueOffset = resolveFromObject(
                parameter, {"m_Value", "value"},
                {"System.Int32", "Int32",
                 "UnityEngine.Rendering.Universal.DepthOfFieldMode"});
        }
        if (api_.volumeParameterOverrideOffset < 0) {
            api_.volumeParameterOverrideOffset = resolveFromObject(
                parameter, {"m_OverrideState", "overrideState"},
                {"System.Boolean", "Boolean"});
        }
        if (api_.volumeParameterIntValueOffset < 0) {
            return false;
        }
        std::int32_t before = -1;
        ReadInt32Field(
            parameter, api_.volumeParameterIntValueOffset, &before);
        if (beforeOut != nullptr) {
            *beforeOut = before;
        }
        if (api_.volumeParameterOverrideOffset >= 0) {
            WriteManagedField(
                parameter, api_.volumeParameterOverrideOffset, true);
        }
        return WriteManagedField(
            parameter, api_.volumeParameterIntValueOffset, value);
    };
    float maxBlurBefore = -1.0F;
    float extrudeBefore = -1.0F;
    std::int32_t modeBefore = -1;
    bool maxBlurOk = false;
    bool extrudeOk = false;
    bool modeOk = false;
    if (isVl) {
        maxBlurOk = writeFloat(
            readPointer(api_.vlDofMaxBlurSpreadFieldOffset), &maxBlurBefore);
        extrudeOk = writeFloat(
            readPointer(api_.vlDofForegroundBlurFieldOffset),
            &extrudeBefore);
        std::int32_t qualityBefore = -1;
        writeInt(readPointer(api_.vlDofQualityFieldOffset), 0, &qualityBefore);
        (void)qualityBefore;
    }
    if (isUrp) {
        modeOk = writeInt(
            readPointer(api_.urpDofModeFieldOffset), 0, &modeBefore);
    }
    if (!eyeDofParamsLogged_) {
        Log(std::string("[VR][stereo] EYE_DOF_PARAM_ZEROED kind=") + label +
            " maxBlurBefore=" + std::to_string(maxBlurBefore) +
            " maxBlurOk=" + (maxBlurOk ? "1" : "0") +
            " extrudeBefore=" + std::to_string(extrudeBefore) +
            " extrudeOk=" + (extrudeOk ? "1" : "0") +
            " modeBefore=" + std::to_string(modeBefore) +
            " modeOk=" + (modeOk ? "1" : "0") +
            " floatValueOffset=" +
            std::to_string(api_.volumeParameterFloatValueOffset) +
            " intValueOffset=" +
            std::to_string(api_.volumeParameterIntValueOffset));
    }
}

void* UnityStereoRenderer::GetVolumeComponent(
    void* stack,
    void* reflectionType) noexcept {
    if (stack == nullptr || reflectionType == nullptr ||
        !api_.volumeStackGetComponent.Ready()) {
        return nullptr;
    }
    using GetComponent = void* (*)(void*, void*, void*);
    void* component = nullptr;
    if (!InvokeManagedResult<void*, GetComponent>(
            api_.volumeStackGetComponent, &component, stack, reflectionType)) {
        return nullptr;
    }
    return component;
}

void UnityStereoRenderer::ObserveSourceCameraStateForDiagnostics(
    const UnitySourceCameraStateDiagnostic& sample) noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled || !sample.valid) {
        return;
    }
    sourceCameraStateDiagnostic_ = sample;
}

bool UnityStereoRenderer::TryGetEyeProjectionScale(
    const void* camera,
    float* scaleX,
    float* scaleY,
    std::size_t* eye) const noexcept {
    if (camera == nullptr || scaleX == nullptr || scaleY == nullptr ||
        eye == nullptr) {
        return false;
    }
    std::size_t index = 2U;
    if (camera == eyeCameras_[0]) {
        index = 0U;
    } else if (camera == eyeCameras_[1]) {
        index = 1U;
    }
    if (index >= sourceToEyeProjectionValid_.size()) {
        return false;
    }
    *eye = index;
    if (sourceToEyeProjectionCamera_ == nullptr ||
        sourceToEyeProjectionRevision_ == 0U ||
        sourceToEyeProjectionRevision_ != latestFrame_.trackingSample.revision ||
        !sourceToEyeProjectionValid_[index]) {
        return false;
    }
    const auto& map = sourceToEyeProjectionMaps_[index];
    if (!std::isfinite(map.scaleX) || !std::isfinite(map.scaleY) ||
        map.scaleX <= 0.0F || map.scaleY <= 0.0F) {
        return false;
    }
    *scaleX = map.scaleX;
    *scaleY = map.scaleY;
    return true;
}

void UnityStereoRenderer::CaptureSourceLens(void* sourceCamera) noexcept {
    using GetFloat = float (*)(void*, void*);
    using GetBool = bool (*)(void*, void*);
    using GetNativeFloat = float (*)(void*, void*);
    using GetNativeVector2 = void (*)(void*, UnityVector2*, void*);
    if (sourceCamera == nullptr) {
        return;
    }
    sourceToEyeProjectionValid_.fill(false);
    sourceToEyeProjectionRevision_ = 0U;
    sourceToEyeProjectionCamera_ = nullptr;
    float fieldOfView = 0.0F;
    if (!InvokeManagedResult<float, GetFloat>(
            api_.cameraGetFieldOfView, &fieldOfView, sourceCamera) ||
        !std::isfinite(fieldOfView) || fieldOfView <= 1.0F) {
        return;
    }
    float aspect = 0.0F;
    if (!InvokeManagedResult<float, GetFloat>(
            api_.cameraGetAspect, &aspect, sourceCamera) ||
        !std::isfinite(aspect) || aspect <= 0.0F) {
        return;
    }
    bool physical = false;
    InvokeManagedResult<bool, GetBool>(
        api_.cameraGetUsePhysicalProperties, &physical, sourceCamera);
    float fittedFov = 0.0F;
    float focalLength = 0.0F;
    UnityVector2 sensorSize{};
    UnityVector2 lensShift{};
    UnityVector2 fittedLensShift{};
    void* nativeCamera = ReadUnityNativePointer(sourceCamera);
    void* lensShiftGetterSelf = api_.lensShiftGetterUsesNativeSelf
        ? nativeCamera : sourceCamera;
    const bool lensShiftOk = lensShiftGetterSelf != nullptr &&
        InvokeManagedVoid<GetNativeVector2>(
            api_.cameraGetLensShiftInjected, lensShiftGetterSelf, &lensShift);
    bool fittedLensShiftOk = false;
    if (physical) {
        if (nativeCamera != nullptr) {
            if (api_.cameraGetFocalLengthInjected.Ready()) {
                InvokeManagedResult<float, GetNativeFloat>(
                    api_.cameraGetFocalLengthInjected, &focalLength,
                    nativeCamera);
            }
            if (api_.cameraGetSensorSizeInjected.Ready()) {
                InvokeManagedVoid<GetNativeVector2>(
                    api_.cameraGetSensorSizeInjected, nativeCamera,
                    &sensorSize);
            }
            if (api_.cameraGetGateFittedFieldOfViewInjected.Ready()) {
                InvokeManagedResult<float, GetNativeFloat>(
                    api_.cameraGetGateFittedFieldOfViewInjected, &fittedFov,
                    nativeCamera);
            }
            if (api_.cameraGetGateFittedLensShiftInjected.Ready()) {
                fittedLensShiftOk = InvokeManagedVoid<GetNativeVector2>(
                    api_.cameraGetGateFittedLensShiftInjected, nativeCamera,
                    &fittedLensShift);
            }
        }
    }
    const float computedFov =
        PhysicalVerticalFovDegrees(sensorSize.y, focalLength);
    float chosen = fieldOfView;
    if (std::isfinite(fittedFov) && fittedFov > 1.0F) {
        chosen = fittedFov;
    } else if (std::isfinite(computedFov) && computedFov > 1.0F) {
        chosen = computedFov;
    }
    bloomSourceFovDegrees_ = chosen;
    sourceLiveFovDegrees_ = chosen;
    sourceFittedFovDegrees_ = fittedFov;
    sourceAspect_ = aspect;
    sourceFocalLengthMillimeters_ = focalLength;
    sourceSensorWidthMillimeters_ = sensorSize.x;
    sourceSensorHeightMillimeters_ = sensorSize.y;
    sourcePhysicalProperties_ = physical;
    ++sourceLensSamples_;

    // This conversion is functional state, not diagnostic state.  Build it
    // for every configured eye frame from the same source lens and OpenXR
    // view sample.  Invalid/missing projection data leaves the map invalid so
    // effect hooks preserve authored values instead of falling back to the old
    // fixed 29.9-degree ratio.
    const UnityVector2 sourceShift =
        physical && fittedLensShiftOk ? fittedLensShift : lensShift;
    camera::ProjectionTerms sourceProjection{};
    std::array<camera::ProjectionTerms, 2> eyeProjections{};
    if (lensShiftOk && camera::TryBuildProjectionTerms(
            chosen, aspect, sourceShift.x, sourceShift.y,
            sourceProjection)) {
        for (std::size_t eye = 0;
             eye < sourceToEyeProjectionValid_.size(); ++eye) {
            const auto& fov = latestFrame_.trackingSample.eyes[eye].fov;
            camera::ProjectionIntrinsics intrinsics{};
            if (!camera::TryDeriveProjectionIntrinsics(
                    fov.angleLeft, fov.angleRight, fov.angleDown, fov.angleUp,
                    intrinsics)) {
                continue;
            }
            eyeProjections[eye] = {
                intrinsics.projectionM00, intrinsics.projectionM02,
                intrinsics.projectionM11, intrinsics.projectionM12};
            sourceToEyeProjectionValid_[eye] =
                camera::TryBuildProjectionMap(
                    sourceProjection, eyeProjections[eye],
                    sourceToEyeProjectionMaps_[eye]);
        }
    }
    if (sourceToEyeProjectionValid_[0] || sourceToEyeProjectionValid_[1]) {
        sourceToEyeProjectionRevision_ = latestFrame_.trackingSample.revision;
        sourceToEyeProjectionCamera_ = sourceCamera;
    }
    const bool fovMoved =
        lastLoggedSourceFovDegrees_ <= 1.0F ||
        std::abs(chosen - lastLoggedSourceFovDegrees_) >= 2.0F;
    if (fovMoved || sourceLensSamples_ % 300U == 0U) {
        lastLoggedSourceFovDegrees_ = chosen;
        eyeFovBundleLogged_ = false;
        eyeBloomParamsLogged_ = false;
        std::ostringstream lens;
        lens << "[VR][stereo] SOURCE_LENS"
             << " fov=" << fieldOfView
             << " fitted=" << fittedFov
             << " computed=" << computedFov
             << " chosen=" << chosen
             << " physical=" << (physical ? "1" : "0")
             << " focal=" << focalLength
             << " sensor=" << sensorSize.x << ',' << sensorSize.y
             << " aspect=" << aspect
             << " lensShift=" << lensShift.x << ',' << lensShift.y
             << " fittedLensShift=" << fittedLensShift.x << ','
             << fittedLensShift.y
             << " liveFov=" << chosen
             << " eyeFov=" << bloomEyeFovDegrees_
             << " modeFov=" << kModeBodyVerticalFovDegrees
             << " screenScale="
             << ModeBodyScreenScale(bloomEyeFovDegrees_)
             << " sample=" << sourceLensSamples_;
        Log(lens.str());
    }

    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    const UnitySourceCameraStateDiagnostic& state =
        sourceCameraStateDiagnostic_;
    if (!state.valid || state.sample == 0U ||
        state.sample == fovDiagnosticLastStateSample_) {
        return;
    }
    const auto& projectionMaps = sourceToEyeProjectionMaps_;
    const auto& eyeProjectionValid = sourceToEyeProjectionValid_;
    if (!eyeProjectionValid[0] && !eyeProjectionValid[1]) {
        return;
    }

    float projectionLogDelta = 0.0F;
    const bool projectionDeltaValid =
        fovDiagnosticLastProjectionM11_ > 0.0F &&
        std::isfinite(fovDiagnosticLastProjectionM11_) &&
        std::isfinite(sourceProjection.m11) && sourceProjection.m11 > 0.0F;
    if (projectionDeltaValid) {
        projectionLogDelta = std::log(
            sourceProjection.m11 / fovDiagnosticLastProjectionM11_);
    }
    const bool periodic = fovDiagnosticLastStateSample_ == 0U ||
        state.sample >= fovDiagnosticLastStateSample_ + 30U;
    const bool projectionMoved = projectionDeltaValid &&
        std::abs(projectionLogDelta) >= 0.0025F;
    if (!periodic && !projectionMoved) {
        return;
    }

    camera::MotionLogDelta motion{};
    const bool motionValid = state.hasLookAt &&
        camera::TryComputeMotionLogDelta(
            fovDiagnosticLastProjectionM11_, sourceProjection.m11,
            fovDiagnosticLastLookAtDistance_, state.lookAtDistance, motion);
    std::string changeMask = "baseline";
    if (motionValid) {
        constexpr float kReportedChangeThreshold = 0.005F;
        const bool projectionChanged =
            std::abs(motion.projection) >= kReportedChangeThreshold;
        const bool distanceChanged =
            std::abs(motion.lookAtDistance) >= kReportedChangeThreshold;
        changeMask = projectionChanged
            ? (distanceChanged ? "P+D" : "P")
            : (distanceChanged ? "D" : "none");
    } else if (!state.hasLookAt && projectionMoved) {
        changeMask = "P+no-lookat";
    }

    ++fovDiagnosticLogs_;
    std::ostringstream cameraSample;
    cameraSample << std::fixed << std::setprecision(6)
                 << "[VR][fov] CAMERA_MOTION sample=" << state.sample
                 << " log=" << fovDiagnosticLogs_
                 << " stateFov=" << state.fieldOfViewDegrees
                 << " actualFov=" << chosen
                 << " stateFocal=" << state.focalLengthMillimeters
                 << " actualFocal=" << focalLength
                 << " stateSensor=" << state.sensorWidthMillimeters << ','
                 << state.sensorHeightMillimeters
                 << " actualSensor=" << sensorSize.x << ',' << sensorSize.y
                 << " statePhysical=" << (state.physical ? 1 : 0)
                 << " actualPhysical=" << (physical ? 1 : 0)
                 << " rawPos=" << state.rawPositionX << ','
                 << state.rawPositionY << ',' << state.rawPositionZ
                 << " hasLookAt=" << (state.hasLookAt ? 1 : 0)
                 << " lookAt=" << state.referenceLookAtX << ','
                 << state.referenceLookAtY << ','
                 << state.referenceLookAtZ
                 << " distance=" << state.lookAtDistance
                 << " dLogP=" << (motionValid ? motion.projection
                                               : projectionLogDelta)
                 << " dLogD=" << (motionValid ? motion.lookAtDistance : 0.0F)
                 << " q=" << (motionValid ? motion.residual : 0.0F)
                 << " motionValid=" << (motionValid ? 1 : 0)
                 << " changeThreshold=0.005000"
                 << " changeMask=" << changeMask;
    Log(cameraSample.str());

    std::ostringstream projectionSample;
    projectionSample << std::fixed << std::setprecision(6)
                     << "[VR][fov] PROJECTION_EQ sample=" << state.sample
                     << " sourceP=" << sourceProjection.m00 << ','
                     << sourceProjection.m02 << ',' << sourceProjection.m11
                     << ',' << sourceProjection.m12
                     << " sourceAspect=" << aspect
                     << " sourceShift=" << sourceShift.x << ','
                     << sourceShift.y
                     << " fixedScale="
                     << ModeBodyScreenScale(bloomEyeFovDegrees_);
    for (std::size_t eye = 0; eye < eyeProjectionValid.size(); ++eye) {
        projectionSample << " eye" << eye << "Valid="
                         << (eyeProjectionValid[eye] ? 1 : 0);
        if (!eyeProjectionValid[eye]) {
            continue;
        }
        const auto& eyeProjection = eyeProjections[eye];
        const auto& map = projectionMaps[eye];
        projectionSample << " eye" << eye << "P=" << eyeProjection.m00
                         << ',' << eyeProjection.m02 << ','
                         << eyeProjection.m11 << ',' << eyeProjection.m12
                         << " eye" << eye << "Map=" << map.scaleX << ','
                         << map.biasX << ',' << map.scaleY << ','
                         << map.biasY
                         << " fixedOverX="
                         << ModeBodyScreenScale(bloomEyeFovDegrees_) /
                                map.scaleX
                         << " fixedOverY="
                         << ModeBodyScreenScale(bloomEyeFovDegrees_) /
                                map.scaleY;
    }
    Log(projectionSample.str());

    const std::size_t effectEye = eyeProjectionValid[0] ? 0U : 1U;
    const auto& effectMap = projectionMaps[effectEye];
    const float fixedScale = ModeBodyScreenScale(bloomEyeFovDegrees_);
    const AuthoredBloomSnapshot& bloom = authoredBloom_[effectEye];
    const float equivalentDiffusion = camera::EquivalentVlBloomDiffusion(
        static_cast<float>(bloom.vlDiffusion), effectMap.scaleY);
    std::int32_t equivalentDiffusionRounded = bloom.vlDiffusion;
    if (bloom.vlDiffusion > 0) {
        camera::TryRoundEquivalentVlBloomDiffusion(
            bloom.vlDiffusion, effectMap.scaleY, equivalentDiffusionRounded);
    }
    const int currentDiffusion = equivalentDiffusionRounded;
    const bool haveProFlare = !authoredProFlareScales_.empty();
    const AuthoredProFlareScale firstProFlare = haveProFlare
        ? authoredProFlareScales_.front() : AuthoredProFlareScale{};
    const bool haveOutline = !actorOutlineMaterials_.empty();
    const AuthoredOutlineMaterial firstOutline = haveOutline
        ? actorOutlineMaterials_.front() : AuthoredOutlineMaterial{};
    std::ostringstream effects;
    effects << std::fixed << std::setprecision(6)
            << "[VR][fov] EFFECT_EQ sample=" << state.sample
            << " eye=" << effectEye
            << " eyeDesc=" << latestTargetSpec_.width << 'x'
            << latestTargetSpec_.height
            << " scaleX=" << effectMap.scaleX
            << " scaleY=" << effectMap.scaleY
            << " proFlareCached=" << (haveProFlare ? 1 : 0)
            << " proFlareAuthoredScale=" << firstProFlare.globalScale
            << " proFlareCurrentScale="
            << firstProFlare.globalScale * fixedScale
            << " proFlareEquivalentX="
            << firstProFlare.globalScale * effectMap.scaleX
            << " proFlareEquivalentY="
            << firstProFlare.globalScale * effectMap.scaleY
            << " proFlareEquivalentGeo="
            << firstProFlare.globalScale *
                   std::sqrt(effectMap.scaleX * effectMap.scaleY)
            << " proFlareBrightnessAuthored="
            << firstProFlare.globalBrightness
            << " bloomValid=" << (bloom.valid ? 1 : 0)
            << " vlIntensityAuthored=" << bloom.vlIntensity
            << " vlDiffusionAuthored=" << bloom.vlDiffusion
            << " vlDiffusionCurrent=" << currentDiffusion
            << " vlDiffusionEquivalentFloat=" << equivalentDiffusion
            << " vlDiffusionEquivalentRound="
            << equivalentDiffusionRounded
            << " urpIntensityAuthored=" << bloom.urpIntensity
            << " urpScatterAuthored=" << bloom.urpScatter
            << " urpScatterCurrent=" << bloom.urpScatter
            << " outlineCached=" << (haveOutline ? 1 : 0)
            << " outlineAuthoredValid="
            << (firstOutline.authoredValid ? 1 : 0)
            << " outlineAuthored=" << firstOutline.authoredX << ','
            << firstOutline.authoredY << ',' << firstOutline.authoredZ << ','
            << firstOutline.authoredW
            << " outlineCurrentWidthScale="
            << GakumasLocal::Config::vrEyeOutlineWidth
            << " writeMode=observe-only";
    Log(effects.str());

    fovDiagnosticLastProjectionM11_ = sourceProjection.m11;
    fovDiagnosticLastLookAtDistance_ =
        state.hasLookAt ? state.lookAtDistance : 0.0F;
    fovDiagnosticLastStateSample_ = state.sample;
}

void UnityStereoRenderer::ClearEyeBloomOverrides(std::size_t eye) noexcept {
    if (eye >= eyeCameras_.size()) {
        return;
    }
    const auto clearField = [this](void* component, std::int32_t fieldOffset,
                                   bool integerParameter) {
        void* parameter = ReadVolumeParameter(component, fieldOffset);
        if (parameter == nullptr) {
            return;
        }
        if (integerParameter) {
            EnsureVolumeParameterOffsets(
                nullptr, &api_.volumeParameterIntValueOffset,
                &api_.volumeParameterOverrideOffset, parameter);
        } else {
            EnsureVolumeParameterOffsets(
                &api_.volumeParameterFloatValueOffset, nullptr,
                &api_.volumeParameterOverrideOffset, parameter);
        }
        ClearVolumeOverride(parameter, api_.volumeParameterOverrideOffset);
    };
    clearField(eyeVlBloomComponents_[eye], api_.vlBloomIntensityFieldOffset,
               false);
    clearField(eyeVlBloomComponents_[eye], api_.vlBloomDiffusionFieldOffset,
               true);
    clearField(eyeUrpBloomComponents_[eye], api_.urpBloomIntensityFieldOffset,
               false);
    clearField(eyeUrpBloomComponents_[eye], api_.urpBloomScatterFieldOffset,
               false);
    clearField(eyeUrpBloomComponents_[eye],
               api_.urpBloomDirtIntensityFieldOffset, false);
}

void UnityStereoRenderer::CacheAuthoredBloom(std::size_t eye) noexcept {
    if (eye >= authoredBloom_.size()) {
        return;
    }
    AuthoredBloomSnapshot snap{};
    const auto readFloat = [this](void* component, std::int32_t fieldOffset,
                                  float* value) -> bool {
        void* parameter = ReadVolumeParameter(component, fieldOffset);
        if (parameter == nullptr || value == nullptr) {
            return false;
        }
        EnsureVolumeParameterOffsets(
            &api_.volumeParameterFloatValueOffset, nullptr,
            &api_.volumeParameterOverrideOffset, parameter);
        return ReadVolumeFloat(
            parameter, api_.volumeParameterFloatValueOffset, value);
    };
    const auto readInt = [this](void* component, std::int32_t fieldOffset,
                                std::int32_t* value) -> bool {
        void* parameter = ReadVolumeParameter(component, fieldOffset);
        if (parameter == nullptr || value == nullptr) {
            return false;
        }
        EnsureVolumeParameterOffsets(
            nullptr, &api_.volumeParameterIntValueOffset,
            &api_.volumeParameterOverrideOffset, parameter);
        return ReadVolumeInt(
            parameter, api_.volumeParameterIntValueOffset, value);
    };
    const bool vlIntensityOk = readFloat(
        eyeVlBloomComponents_[eye], api_.vlBloomIntensityFieldOffset,
        &snap.vlIntensity);
    const bool urpIntensityOk = readFloat(
        eyeUrpBloomComponents_[eye], api_.urpBloomIntensityFieldOffset,
        &snap.urpIntensity);
    snap.valid = vlIntensityOk || urpIntensityOk;
    readInt(eyeVlBloomComponents_[eye], api_.vlBloomDiffusionFieldOffset,
            &snap.vlDiffusion);
    readFloat(eyeUrpBloomComponents_[eye], api_.urpBloomScatterFieldOffset,
              &snap.urpScatter);
    readFloat(eyeUrpBloomComponents_[eye],
              api_.urpBloomDirtIntensityFieldOffset, &snap.urpDirt);
    AuthoredBloomSnapshot& existing = authoredBloom_[eye];
    if (existing.valid) {
        if (std::abs(snap.vlIntensity - existing.vlIntensity) <= 0.05F &&
            snap.vlDiffusion > 0 && existing.vlDiffusion > 0 &&
            snap.vlDiffusion < existing.vlDiffusion) {
            snap.vlDiffusion = existing.vlDiffusion;
        }
        if (std::abs(snap.urpIntensity - existing.urpIntensity) <= 0.05F &&
            snap.urpScatter > 0.0F && existing.urpScatter > 0.0F &&
            snap.urpScatter + 0.01F < existing.urpScatter) {
            snap.urpScatter = existing.urpScatter;
        }
        if (std::abs(snap.urpDirt - existing.urpDirt) <= 0.05F &&
            snap.urpDirt + 0.01F < existing.urpDirt &&
            existing.urpDirt > 0.0F) {
            snap.urpDirt = existing.urpDirt;
        }
    }
    if (snap.valid) {
        existing = snap;
    }
}

bool UnityStereoRenderer::TryResolveBloomProjectionScale(
    std::size_t eye,
    float* scale,
    const char** source) const noexcept {
    if (scale == nullptr || source == nullptr) {
        return false;
    }
    float scaleX = 0.0F;
    float scaleY = 0.0F;
    std::size_t mappedEye = eye;
    if (eye < eyeCameras_.size() &&
        TryGetEyeProjectionScale(
            eyeCameras_[eye], &scaleX, &scaleY, &mappedEye)) {
        *scale = scaleY;
        *source = "projectionY";
        return true;
    }
    if (camera::TryBuildCenteredVerticalProjectionScale(
            sourceLiveFovDegrees_, bloomEyeFovDegrees_, *scale)) {
        *source = "liveTan";
        return true;
    }
    if (camera::TryBuildCenteredVerticalProjectionScale(
            kModeBodyVerticalFovDegrees, bloomEyeFovDegrees_, *scale)) {
        *source = "modeTan";
        return true;
    }
    return false;
}

void UnityStereoRenderer::ApplyModeBodyBloom(std::size_t eye) noexcept {
    if (eye >= authoredBloom_.size() || !authoredBloom_[eye].valid) {
        return;
    }
    const AuthoredBloomSnapshot& authored = authoredBloom_[eye];
    // Intensity always authored. vrBloomFollowSourceCamera
    // (default off) writes the .270 FOV-eq kernel on
    // source-follow. Off, and free camera, write the pre-.270
    // mode-body kernel. The menu row is hidden in free camera.
    constexpr bool kZeroEyeBloomIntensity = false;
    const bool freeCamActive = camera::IsVrFreeCameraLocomotionActive();
    const bool followSource =
        !freeCamActive && GakumasLocal::Config::vrBloomFollowSourceCamera;
    const bool comfortPath = !followSource;
    if (comfortPath != lastBloomComfortPath_) {
        lastBloomComfortPath_ = comfortPath;
        eyeBloomParamsLogged_ = false;
    }
    const float screenScale = ModeBodyScreenScale(bloomEyeFovDegrees_);
    float projectionScale = screenScale;
    const char* scaleSource = "none";
    const bool haveProjectionScale =
        TryResolveBloomProjectionScale(eye, &projectionScale, &scaleSource);
    const float vlIntensityWritten =
        kZeroEyeBloomIntensity ? 0.0F : authored.vlIntensity;
    const float urpIntensityWritten =
        kZeroEyeBloomIntensity ? 0.0F : authored.urpIntensity;
    const float dirtWritten =
        kZeroEyeBloomIntensity ? 0.0F : authored.urpDirt;
    bool intensityOk = false;
    bool diffusionOk = false;
    bool scatterOk = false;
    bool dirtOk = false;
    std::int32_t diffusionWritten = authored.vlDiffusion;
    std::int32_t diffusionModeBody = authored.vlDiffusion;
    float scatterWritten = authored.urpScatter;
    if (authored.vlDiffusion > 0) {
        diffusionModeBody = std::max(
            1, static_cast<std::int32_t>(std::lround(
                   static_cast<double>(authored.vlDiffusion) *
                   static_cast<double>(screenScale))));
        if (comfortPath) {
            diffusionWritten = diffusionModeBody;
        } else if (haveProjectionScale) {
            camera::TryRoundEquivalentVlBloomDiffusion(
                authored.vlDiffusion, projectionScale, diffusionWritten);
        }
    }
    if (comfortPath) {
        scatterWritten = authored.urpScatter * screenScale;
    }
    if (eyeVlBloomComponents_[eye] != nullptr) {
        void* intensityParameter = ReadVolumeParameter(
            eyeVlBloomComponents_[eye], api_.vlBloomIntensityFieldOffset);
        EnsureVolumeParameterOffsets(
            &api_.volumeParameterFloatValueOffset, nullptr,
            &api_.volumeParameterOverrideOffset, intensityParameter);
        intensityOk = WriteVolumeFloat(
            intensityParameter, api_.volumeParameterFloatValueOffset,
            api_.volumeParameterOverrideOffset, vlIntensityWritten);
        void* diffusionParameter = ReadVolumeParameter(
            eyeVlBloomComponents_[eye], api_.vlBloomDiffusionFieldOffset);
        EnsureVolumeParameterOffsets(
            nullptr, &api_.volumeParameterIntValueOffset,
            &api_.volumeParameterOverrideOffset, diffusionParameter);
        if (authored.vlDiffusion > 0) {
            diffusionOk = WriteVolumeInt(
                diffusionParameter, api_.volumeParameterIntValueOffset,
                api_.volumeParameterOverrideOffset, diffusionWritten);
        }
    }
    if (eyeUrpBloomComponents_[eye] != nullptr) {
        void* intensityParameter = ReadVolumeParameter(
            eyeUrpBloomComponents_[eye], api_.urpBloomIntensityFieldOffset);
        EnsureVolumeParameterOffsets(
            &api_.volumeParameterFloatValueOffset, nullptr,
            &api_.volumeParameterOverrideOffset, intensityParameter);
        const bool urpIntensityOk = WriteVolumeFloat(
            intensityParameter, api_.volumeParameterFloatValueOffset,
            api_.volumeParameterOverrideOffset, urpIntensityWritten);
        if (eyeVlBloomComponents_[eye] == nullptr) {
            intensityOk = urpIntensityOk;
        }
        void* scatterParameter = ReadVolumeParameter(
            eyeUrpBloomComponents_[eye], api_.urpBloomScatterFieldOffset);
        EnsureVolumeParameterOffsets(
            &api_.volumeParameterFloatValueOffset, nullptr,
            &api_.volumeParameterOverrideOffset, scatterParameter);
        scatterOk = WriteVolumeFloat(
            scatterParameter, api_.volumeParameterFloatValueOffset,
            api_.volumeParameterOverrideOffset, scatterWritten);
        void* dirtParameter = ReadVolumeParameter(
            eyeUrpBloomComponents_[eye],
            api_.urpBloomDirtIntensityFieldOffset);
        EnsureVolumeParameterOffsets(
            &api_.volumeParameterFloatValueOffset, nullptr,
            &api_.volumeParameterOverrideOffset, dirtParameter);
        dirtOk = WriteVolumeFloat(
            dirtParameter, api_.volumeParameterFloatValueOffset,
            api_.volumeParameterOverrideOffset, dirtWritten);
    }
    if (!eyeBloomParamsLogged_) {
        const char* bloomTag = "EYE_BLOOM_FOV_EQ";
        if (kZeroEyeBloomIntensity) {
            bloomTag = "EYE_BLOOM_PARAM_ZEROED";
        } else if (comfortPath) {
            bloomTag = "EYE_BLOOM_FREECAM";
        }
        Log(std::string("[VR][stereo] ") + bloomTag +
            " eye=" + (eye == 0U ? "left" : "right") +
            " policy=" + std::string(followSource ? "fovEq" : "modeBody") +
            " freeCam=" + (freeCamActive ? "1" : "0") +
            " followSource=" +
                (GakumasLocal::Config::vrBloomFollowSourceCamera ? "1" : "0") +
            " modeFov=" + std::to_string(kModeBodyVerticalFovDegrees) +
            " liveFov=" + std::to_string(sourceLiveFovDegrees_) +
            " eyeFov=" + std::to_string(bloomEyeFovDegrees_) +
            " scaleSource=" + scaleSource +
            " projectionScale=" + std::to_string(projectionScale) +
            " screenScale=" + std::to_string(screenScale) +
            " intensityScale=" +
                (kZeroEyeBloomIntensity ? "0" : "1") +
            " intensityAuthored=" + std::to_string(authored.vlIntensity) +
            " intensityWritten=" + std::to_string(vlIntensityWritten) +
            " intensityOk=" + (intensityOk ? "1" : "0") +
            " diffusionAuthored=" + std::to_string(authored.vlDiffusion) +
            " diffusionModeBody=" + std::to_string(diffusionModeBody) +
            " diffusionWritten=" + std::to_string(diffusionWritten) +
            " diffusionOk=" + (diffusionOk ? "1" : "0") +
            " urpIntensityAuthored=" +
                std::to_string(authored.urpIntensity) +
            " urpIntensityWritten=" + std::to_string(urpIntensityWritten) +
            " scatterAuthored=" + std::to_string(authored.urpScatter) +
            " scatterWritten=" + std::to_string(scatterWritten) +
            " scatterOk=" + (scatterOk ? "1" : "0") +
            " dirtAuthored=" + std::to_string(authored.urpDirt) +
            " dirtWritten=" + std::to_string(dirtWritten) +
            " dirtOk=" + (dirtOk ? "1" : "0") +
            " sourceStack=" +
                (latestSourceVolumeStack_ != nullptr ? "1" : "0"));
        if (authoredBloom_[0].valid && authoredBloom_[1].valid) {
            eyeBloomParamsLogged_ = true;
        }
    }
}

int ReadStaticIntField(UnityResolve::Class* klass, const char* name) noexcept {
    if (klass == nullptr || name == nullptr) {
        return 0;
    }
    const auto* field = klass->Get<UnityResolve::Field>(name);
    if (field == nullptr || field->address == nullptr) {
        return 0;
    }
    int value = 0;
    UnityResolve::Invoke<void, void*, int*>(
        "il2cpp_field_static_get_value", field->address, &value);
    return value;
}

void* InternIl2CppString(const char* text) noexcept {
    if (text == nullptr) {
        return nullptr;
    }
    void* value = UnityResolve::Invoke<void*, const char*>(
        "il2cpp_string_new", text);
    if (value != nullptr) {
        (void)CreateGcHandle(value);
    }
    return value;
}

int ResolveShaderPropertyId(const char* propertyName) noexcept {
    if (propertyName == nullptr) {
        return 0;
    }
    auto* shaderClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Shader");
    if (shaderClass == nullptr) {
        return 0;
    }
    UnityResolve::Method* propertyToId = nullptr;
    for (auto* method : shaderClass->methods) {
        if (method != nullptr && method->function != nullptr &&
            method->static_function && method->name == "PropertyToID" &&
            method->args.size() == 1U) {
            propertyToId = method;
            break;
        }
    }
    if (propertyToId == nullptr) {
        return 0;
    }
    void* name = UnityResolve::Invoke<void*, const char*>(
        "il2cpp_string_new", propertyName);
    int id = 0;
    return name != nullptr &&
            InvokeManagedResult<int, int (*)(void*, void*)>(
                MethodReference(propertyToId), &id, name) &&
            id != 0
        ? id
        : 0;
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

std::vector<void*> FindManagedObjectsOfType(UnityResolve::Class* klass) noexcept {
    if (klass == nullptr) {
        return {};
    }
    auto objects = klass->FindObjectsByType<void*>();
    if (!objects.empty()) {
        return objects;
    }
    if (auto* objectClass =
            UnityResolve::Get("UnityEngine.CoreModule.dll")->Get("Object")) {
        auto* findAll = objectClass->Get<UnityResolve::Method>(
            "FindObjectsOfTypeAll", {"System.Type"});
        void* type = klass->GetType();
        if (findAll != nullptr && type != nullptr) {
            if (auto* array =
                    findAll->Invoke<UnityResolve::UnityType::Array<void*>*>(
                        type)) {
                return array->ToVector();
            }
        }
    }
    return objects;
}

std::string ManagedStringValue(void* value) noexcept {
    if (value == nullptr) {
        return "-";
    }
    try {
        std::string text =
            reinterpret_cast<UnityResolve::UnityType::String*>(value)->ToString();
        for (char& ch : text) {
            if (static_cast<unsigned char>(ch) < 0x20U) {
                ch = ' ';
            }
        }
        return text.empty() ? "-" : text;
    } catch (...) {
        return "-";
    }
}

std::string ManagedObjectClassName(void* object) noexcept {
    if (object == nullptr) {
        return "-";
    }
    void* klass = UnityResolve::Invoke<void*>("il2cpp_object_get_class", object);
    if (klass == nullptr) {
        return "-";
    }
    const char* name = UnityResolve::Invoke<const char*>(
        "il2cpp_class_get_name", klass);
    const char* namespaze = UnityResolve::Invoke<const char*>(
        "il2cpp_class_get_namespace", klass);
    if (name == nullptr || name[0] == '\0') {
        return "-";
    }
    return namespaze != nullptr && namespaze[0] != '\0'
        ? std::string(namespaze) + '.' + name
        : std::string(name);
}

void UnityStereoRenderer::RequestVirtualCameraCensus(
    const char* reason) noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    virtualCameraCensusPending_ = true;
    virtualCameraCensusRunsRemaining_ = 2;
    // First pass after roughly half a second of stable content. The second
    // pass is delayed further below to catch asynchronously instantiated
    // cameras without scanning Resources every frame.
    virtualCameraCensusDelayTicks_ = 30;
    virtualCameraCensusReason_ = reason != nullptr ? reason : "unknown";
    std::ostringstream stream;
    stream << "[VR][vcam] VCAM_CENSUS_REQUEST reason="
           << virtualCameraCensusReason_ << " scene=" << sceneCount_ << '/'
           << loadedSceneCount_ << '/' << activeSceneHandle_;
    Log(stream.str());
}

void UnityStereoRenderer::RunVirtualCameraCensusIfDue() noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled ||
        !sceneIdentityValid_) {
        return;
    }
    const bool identityChanged = !virtualCameraCensusIdentityObserved_ ||
        virtualCameraCensusSceneCount_ != sceneCount_ ||
        virtualCameraCensusLoadedSceneCount_ != loadedSceneCount_ ||
        virtualCameraCensusActiveSceneHandle_ != activeSceneHandle_;
    if (identityChanged) {
        virtualCameraCensusIdentityObserved_ = true;
        virtualCameraCensusSceneCount_ = sceneCount_;
        virtualCameraCensusLoadedSceneCount_ = loadedSceneCount_;
        virtualCameraCensusActiveSceneHandle_ = activeSceneHandle_;
        RequestVirtualCameraCensus("scene-identity");
    }
    if (!virtualCameraCensusPending_ ||
        virtualCameraCensusRunsRemaining_ == 0U || SceneContentUnstable()) {
        return;
    }
    if (virtualCameraCensusDelayTicks_ > 0U) {
        --virtualCameraCensusDelayTicks_;
        return;
    }
    const char* phase = virtualCameraCensusRunsRemaining_ == 2U
        ? "initial"
        : "settled";
    RunVirtualCameraCensus(phase);
    --virtualCameraCensusRunsRemaining_;
    if (virtualCameraCensusRunsRemaining_ == 0U) {
        virtualCameraCensusPending_ = false;
    } else {
        // Approximately three seconds at the normal 60 Hz Tick cadence.
        virtualCameraCensusDelayTicks_ = 180;
    }
}

void UnityStereoRenderer::RunVirtualCameraCensus(const char* phase) noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }

    struct CensusApi {
        UnityResolve::Class* virtualCameraClass = nullptr;
        UnityResolve::Class* switcherClass = nullptr;
        MethodRef resourcesFindAll{};
        MethodRef componentGetGameObject{};
        MethodRef gameObjectGetScene{};
        MethodRef gameObjectGetActiveSelf{};
        MethodRef gameObjectGetActiveInHierarchy{};
        MethodRef behaviourGetEnabled{};
        MethodRef sceneIsValidInternal{};
        MethodRef sceneGetNameInternal{};
        MethodRef sceneGetIsLoadedInternal{};
        MethodRef virtualCameraGetPriority{};
        MethodRef virtualCameraGetIsValid{};
        MethodRef coreGetInstance{};
        MethodRef coreGetBrainCount{};
        MethodRef coreGetActiveBrain{};
        MethodRef coreGetVirtualCameraCount{};
        MethodRef coreGetVirtualCamera{};
        MethodRef brainGetActiveVirtualCamera{};
        MethodRef brainGetOutputCamera{};
        MethodRef switcherGetCameraCount{};
        MethodRef switcherGetCamera{};
        std::int32_t switchableVirtualCameraOffset = -1;
        std::int32_t switchableDirectorOffset = -1;
    };

    static const CensusApi censusApi = [] {
        CensusApi result;
        result.virtualCameraClass = Il2cppUtils::GetClass(
            "Cinemachine.dll", "Cinemachine", "CinemachineVirtualCameraBase");
        result.switcherClass = Il2cppUtils::GetClass(
            "campus-submodule.Runtime.dll", "Campus.Common",
            "CampusCameraSwitcher");
        auto* switchableClass = Il2cppUtils::GetClass(
            "campus-submodule.Runtime.dll", "Campus.Common",
            "CampusSwitchableCamera");

        result.resourcesFindAll = MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Resources",
            "FindObjectsOfTypeAll", true, "UnityEngine.Object[]",
            {"System.Type"}));
        result.componentGetGameObject = MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Component",
            "get_gameObject", false, "UnityEngine.GameObject", {}));
        result.gameObjectGetScene = MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
            "get_scene", false, "UnityEngine.SceneManagement.Scene", {}));
        result.gameObjectGetActiveSelf = MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
            "get_activeSelf", false, "System.Boolean", {}));
        result.gameObjectGetActiveInHierarchy = MethodReference(
            ResolveStrictMethod(
                "UnityEngine.CoreModule.dll", "UnityEngine", "GameObject",
                "get_activeInHierarchy", false, "System.Boolean", {}));
        result.behaviourGetEnabled = MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Behaviour",
            "get_enabled", false, "System.Boolean", {}));
        result.sceneIsValidInternal = MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement",
            "Scene", "IsValidInternal", true, "System.Boolean",
            {"System.Int32"}));
        result.sceneGetNameInternal = MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement",
            "Scene", "GetNameInternal", true, "System.String",
            {"System.Int32"}));
        result.sceneGetIsLoadedInternal = MethodReference(ResolveStrictMethod(
            "UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement",
            "Scene", "GetIsLoadedInternal", true, "System.Boolean",
            {"System.Int32"}));
        result.virtualCameraGetPriority = MethodReference(ResolveStrictMethod(
            "Cinemachine.dll", "Cinemachine", "CinemachineVirtualCameraBase",
            "get_Priority", false, "System.Int32", {}));
        result.virtualCameraGetIsValid = MethodReference(ResolveStrictMethod(
            "Cinemachine.dll", "Cinemachine", "CinemachineVirtualCameraBase",
            "get_IsValid", false, "System.Boolean", {}));
        result.coreGetInstance = MethodReference(ResolveStrictMethod(
            "Cinemachine.dll", "Cinemachine", "CinemachineCore",
            "get_Instance", true, "Cinemachine.CinemachineCore", {}));
        result.coreGetBrainCount = MethodReference(ResolveStrictMethod(
            "Cinemachine.dll", "Cinemachine", "CinemachineCore",
            "get_BrainCount", false, "System.Int32", {}));
        result.coreGetActiveBrain = MethodReference(ResolveStrictMethod(
            "Cinemachine.dll", "Cinemachine", "CinemachineCore",
            "GetActiveBrain", false, "Cinemachine.CinemachineBrain",
            {"System.Int32"}));
        result.coreGetVirtualCameraCount = MethodReference(ResolveStrictMethod(
            "Cinemachine.dll", "Cinemachine", "CinemachineCore",
            "get_VirtualCameraCount", false, "System.Int32", {}));
        result.coreGetVirtualCamera = MethodReference(ResolveStrictMethod(
            "Cinemachine.dll", "Cinemachine", "CinemachineCore",
            "GetVirtualCamera", false,
            "Cinemachine.CinemachineVirtualCameraBase", {"System.Int32"}));
        result.brainGetActiveVirtualCamera = MethodReference(
            ResolveStrictMethod(
                "Cinemachine.dll", "Cinemachine", "CinemachineBrain",
                "get_ActiveVirtualCamera", false,
                "Cinemachine.ICinemachineCamera", {}));
        result.brainGetOutputCamera = MethodReference(ResolveStrictMethod(
            "Cinemachine.dll", "Cinemachine", "CinemachineBrain",
            "get_OutputCamera", false, "UnityEngine.Camera", {}));
        result.switcherGetCameraCount = MethodReference(ResolveStrictMethod(
            "campus-submodule.Runtime.dll", "Campus.Common",
            "CampusCameraSwitcher", "get_CameraCount", false,
            "System.Int32", {}));
        result.switcherGetCamera = MethodReference(ResolveStrictMethod(
            "campus-submodule.Runtime.dll", "Campus.Common",
            "CampusCameraSwitcher", "GetCamera", false,
            "Campus.Common.CampusSwitchableCamera", {"System.Int32"}));
        result.switchableVirtualCameraOffset = FindNamedFieldOffset(
            switchableClass, {"virtualCamera"},
            {"Cinemachine.CinemachineVirtualCamera",
             "CinemachineVirtualCamera"});
        result.switchableDirectorOffset = FindNamedFieldOffset(
            switchableClass, {"director"},
            {"UnityEngine.Playables.PlayableDirector", "PlayableDirector"});
        return result;
    }();

    if (!virtualCameraCensusApiLogged_) {
        virtualCameraCensusApiLogged_ = true;
        std::ostringstream apiLog;
        apiLog << "[VR][vcam] VCAM_CENSUS_API vcamClass="
               << (censusApi.virtualCameraClass != nullptr ? 1 : 0)
               << " findAll=" << (censusApi.resourcesFindAll.Ready() ? 1 : 0)
               << " gameObject="
               << (censusApi.componentGetGameObject.Ready() ? 1 : 0)
               << " scene="
               << (censusApi.gameObjectGetScene.Ready() &&
                           censusApi.sceneIsValidInternal.Ready() &&
                           censusApi.sceneGetNameInternal.Ready() &&
                           censusApi.sceneGetIsLoadedInternal.Ready()
                       ? 1
                       : 0)
               << " state="
               << (censusApi.virtualCameraGetPriority.Ready() &&
                           censusApi.virtualCameraGetIsValid.Ready()
                       ? 1
                       : 0)
               << " core="
               << (censusApi.coreGetInstance.Ready() &&
                           censusApi.coreGetVirtualCameraCount.Ready() &&
                           censusApi.coreGetVirtualCamera.Ready()
                       ? 1
                       : 0)
               << " brain="
               << (censusApi.coreGetBrainCount.Ready() &&
                           censusApi.coreGetActiveBrain.Ready() &&
                           censusApi.brainGetActiveVirtualCamera.Ready()
                       ? 1
                       : 0)
               << " switcher="
               << (censusApi.switcherClass != nullptr &&
                           censusApi.switcherGetCameraCount.Ready() &&
                           censusApi.switcherGetCamera.Ready()
                       ? 1
                       : 0)
               << " switchFields="
               << censusApi.switchableVirtualCameraOffset << '/'
               << censusApi.switchableDirectorOffset;
        Log(apiLog.str());
    }

    if (censusApi.virtualCameraClass == nullptr ||
        !censusApi.resourcesFindAll.HasInfo() ||
        !censusApi.componentGetGameObject.Ready() ||
        !censusApi.gameObjectGetScene.HasInfo() ||
        !censusApi.sceneIsValidInternal.Ready() ||
        !censusApi.sceneGetNameInternal.Ready() ||
        !censusApi.sceneGetIsLoadedInternal.Ready()) {
        Log("[VR][vcam] VCAM_CENSUS_SKIP reason=essential-api-missing");
        return;
    }

    const auto findAll = [](UnityResolve::Class* klass) {
        std::vector<void*> objects;
        if (klass == nullptr) {
            return objects;
        }
        void* type = klass->GetType();
        if (type == nullptr) {
            return objects;
        }
        void* arguments[] = {type};
        void* arrayObject = nullptr;
        if (!RuntimeInvoke(censusApi.resourcesFindAll, nullptr, arguments,
                           &arrayObject, nullptr) ||
            arrayObject == nullptr) {
            return objects;
        }
        try {
            return reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(
                       arrayObject)
                ->ToVector();
        } catch (...) {
            return objects;
        }
    };

    struct SceneInfo {
        int handle = 0;
        bool valid = false;
        bool loaded = false;
        std::string name = "-";
    };
    const auto readScene = [](void* gameObject, SceneInfo* scene) {
        if (gameObject == nullptr || scene == nullptr) {
            return false;
        }
        void* boxedScene = nullptr;
        if (!RuntimeInvoke(censusApi.gameObjectGetScene, gameObject, nullptr,
                           &boxedScene, nullptr) ||
            boxedScene == nullptr) {
            return false;
        }
        void* rawScene = UnboxObject(boxedScene);
        if (rawScene == nullptr ||
            !ReadManagedField(rawScene, 0, &scene->handle)) {
            return false;
        }
        using GetSceneBool = bool (*)(int, void*);
        using GetSceneString = void* (*)(int, void*);
        (void)InvokeManagedResult<bool, GetSceneBool>(
            censusApi.sceneIsValidInternal, &scene->valid, scene->handle);
        (void)InvokeManagedResult<bool, GetSceneBool>(
            censusApi.sceneGetIsLoadedInternal, &scene->loaded, scene->handle);
        void* name = nullptr;
        if (InvokeManagedResult<void*, GetSceneString>(
                censusApi.sceneGetNameInternal, &name, scene->handle)) {
            scene->name = ManagedStringValue(name);
        }
        return true;
    };

    const std::uint64_t serial = ++virtualCameraCensusSerial_;
    std::ostringstream censusStart;
    censusStart << "[VR][vcam] VCAM_CENSUS_BEGIN serial=" << serial
                << " phase=" << (phase != nullptr ? phase : "unknown")
                << " reason=" << virtualCameraCensusReason_
                << " identity=" << sceneCount_ << '/' << loadedSceneCount_
                << '/' << activeSceneHandle_;
    Log(censusStart.str());

    std::vector<void*> coreCameras;
    std::vector<void*> activeBrainCameras;
    void* core = nullptr;
    int coreCount = -1;
    int brainCount = -1;
    using GetStaticObject = void* (*)(void*);
    using GetObject = void* (*)(void*, void*);
    using GetObjectAt = void* (*)(void*, int, void*);
    using GetInt = int (*)(void*, void*);
    using GetBool = bool (*)(void*, void*);
    if (censusApi.coreGetInstance.Ready()) {
        (void)InvokeManagedResult<void*, GetStaticObject>(
            censusApi.coreGetInstance, &core);
    }
    if (core != nullptr && censusApi.coreGetVirtualCameraCount.Ready() &&
        InvokeManagedResult<int, GetInt>(
            censusApi.coreGetVirtualCameraCount, &coreCount, core) &&
        coreCount >= 0 && coreCount <= 4096 &&
        censusApi.coreGetVirtualCamera.Ready()) {
        coreCameras.reserve(static_cast<std::size_t>(coreCount));
        for (int index = 0; index < coreCount; ++index) {
            void* camera = nullptr;
            if (InvokeManagedResult<void*, GetObjectAt>(
                    censusApi.coreGetVirtualCamera, &camera, core, index) &&
                camera != nullptr) {
                coreCameras.push_back(camera);
            }
        }
    }
    if (core != nullptr && censusApi.coreGetBrainCount.Ready() &&
        InvokeManagedResult<int, GetInt>(
            censusApi.coreGetBrainCount, &brainCount, core) &&
        brainCount >= 0 && brainCount <= 64 &&
        censusApi.coreGetActiveBrain.Ready()) {
        for (int index = 0; index < brainCount; ++index) {
            void* brain = nullptr;
            if (!InvokeManagedResult<void*, GetObjectAt>(
                    censusApi.coreGetActiveBrain, &brain, core, index) ||
                !IsUnityManagedObjectAlive(brain)) {
                continue;
            }
            void* activeCamera = nullptr;
            void* outputCamera = nullptr;
            if (censusApi.brainGetActiveVirtualCamera.Ready()) {
                (void)InvokeManagedResult<void*, GetObject>(
                    censusApi.brainGetActiveVirtualCamera, &activeCamera,
                    brain);
            }
            if (censusApi.brainGetOutputCamera.Ready()) {
                (void)InvokeManagedResult<void*, GetObject>(
                    censusApi.brainGetOutputCamera, &outputCamera, brain);
            }
            if (activeCamera != nullptr) {
                activeBrainCameras.push_back(activeCamera);
            }
            std::ostringstream brainLog;
            brainLog << "[VR][vcam] VCAM_CENSUS_BRAIN serial=" << serial
                     << " index=" << index << " brain=" << brain
                     << " name=" << std::quoted(ManagedObjectName(brain))
                     << " active=" << activeCamera
                     << " activeName="
                     << std::quoted(ManagedObjectName(activeCamera))
                     << " activeClass="
                     << ManagedObjectClassName(activeCamera)
                     << " output=" << outputCamera
                     << " outputName="
                     << std::quoted(ManagedObjectName(outputCamera));
            Log(brainLog.str());
        }
    }

    struct CameraRecord {
        void* camera = nullptr;
        void* gameObject = nullptr;
        SceneInfo scene{};
        std::string name = "-";
        std::string className = "-";
        int enabled = -1;
        int activeSelf = -1;
        int activeInHierarchy = -1;
        int cameraValid = -1;
        int priority = 0;
        bool priorityKnown = false;
        bool coreRegistered = false;
        bool brainActive = false;
    };
    const std::vector<void*> allCameras = findAll(censusApi.virtualCameraClass);
    std::vector<CameraRecord> records;
    records.reserve(allCameras.size());
    std::size_t dead = 0;
    std::size_t outsideLoadedScene = 0;
    for (void* camera : allCameras) {
        if (!IsUnityManagedObjectAlive(camera)) {
            ++dead;
            continue;
        }
        CameraRecord record;
        record.camera = camera;
        record.name = ManagedObjectName(camera);
        record.className = ManagedObjectClassName(camera);
        if (!InvokeManagedResult<void*, GetObject>(
                censusApi.componentGetGameObject, &record.gameObject, camera) ||
            !IsUnityManagedObjectAlive(record.gameObject) ||
            !readScene(record.gameObject, &record.scene) ||
            !record.scene.valid || !record.scene.loaded) {
            ++outsideLoadedScene;
            continue;
        }
        bool value = false;
        if (censusApi.behaviourGetEnabled.Ready() &&
            InvokeManagedResult<bool, GetBool>(
                censusApi.behaviourGetEnabled, &value, camera)) {
            record.enabled = value ? 1 : 0;
        }
        if (censusApi.gameObjectGetActiveSelf.Ready() &&
            InvokeManagedResult<bool, GetBool>(
                censusApi.gameObjectGetActiveSelf, &value,
                record.gameObject)) {
            record.activeSelf = value ? 1 : 0;
        }
        if (censusApi.gameObjectGetActiveInHierarchy.Ready() &&
            InvokeManagedResult<bool, GetBool>(
                censusApi.gameObjectGetActiveInHierarchy, &value,
                record.gameObject)) {
            record.activeInHierarchy = value ? 1 : 0;
        }
        if (censusApi.virtualCameraGetIsValid.Ready() &&
            InvokeManagedResult<bool, GetBool>(
                censusApi.virtualCameraGetIsValid, &value, camera)) {
            record.cameraValid = value ? 1 : 0;
        }
        if (censusApi.virtualCameraGetPriority.Ready()) {
            record.priorityKnown = InvokeManagedResult<int, GetInt>(
                censusApi.virtualCameraGetPriority, &record.priority, camera);
        }
        record.coreRegistered = std::find(
            coreCameras.begin(), coreCameras.end(), camera) != coreCameras.end();
        record.brainActive = std::find(
            activeBrainCameras.begin(), activeBrainCameras.end(), camera) !=
            activeBrainCameras.end();
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(), [](const auto& left,
                                                  const auto& right) {
        const auto leftText =
            std::tie(left.scene.handle, left.scene.name, left.name,
                     left.className);
        const auto rightText =
            std::tie(right.scene.handle, right.scene.name, right.name,
                     right.className);
        if (leftText != rightText) {
            return leftText < rightText;
        }
        return reinterpret_cast<std::uintptr_t>(left.camera) <
            reinterpret_cast<std::uintptr_t>(right.camera);
    });

    std::ostringstream counts;
    counts << "[VR][vcam] VCAM_CENSUS_COUNTS serial=" << serial
           << " foundAll=" << allCameras.size()
           << " loaded=" << records.size() << " core=" << coreCount
           << " brains=" << brainCount;
    Log(counts.str());

    std::size_t index = 0;
    while (index < records.size()) {
        const int handle = records[index].scene.handle;
        const std::string& sceneName = records[index].scene.name;
        std::size_t end = index + 1U;
        while (end < records.size() && records[end].scene.handle == handle) {
            ++end;
        }
        std::ostringstream sceneLog;
        sceneLog << "[VR][vcam] VCAM_CENSUS_SCENE serial=" << serial
                 << " handle=" << handle << " name="
                 << std::quoted(sceneName) << " count=" << (end - index);
        Log(sceneLog.str());
        for (; index < end; ++index) {
            const CameraRecord& record = records[index];
            std::ostringstream item;
            item << "[VR][vcam] VCAM_CENSUS_ITEM serial=" << serial
                 << " scene=" << record.scene.handle << " sceneName="
                 << std::quoted(record.scene.name) << " camera="
                 << record.camera << " gameObject=" << record.gameObject
                 << " name=" << std::quoted(record.name)
                 << " class=" << record.className
                 << " enabled=" << record.enabled
                 << " activeSelf=" << record.activeSelf
                 << " activeHierarchy=" << record.activeInHierarchy
                 << " valid=" << record.cameraValid << " priority=";
            if (record.priorityKnown) {
                item << record.priority;
            } else {
                item << '?';
            }
            item << " core=" << (record.coreRegistered ? 1 : 0)
                 << " brainActive=" << (record.brainActive ? 1 : 0);
            Log(item.str());
        }
    }

    std::size_t switcherCount = 0;
    std::size_t selectableCount = 0;
    if (censusApi.switcherClass != nullptr &&
        censusApi.switcherGetCameraCount.Ready() &&
        censusApi.switcherGetCamera.Ready() &&
        censusApi.switchableVirtualCameraOffset >= 0) {
        for (void* switcher : findAll(censusApi.switcherClass)) {
            if (!IsUnityManagedObjectAlive(switcher)) {
                continue;
            }
            void* gameObject = nullptr;
            SceneInfo scene;
            if (!InvokeManagedResult<void*, GetObject>(
                    censusApi.componentGetGameObject, &gameObject, switcher) ||
                !IsUnityManagedObjectAlive(gameObject) ||
                !readScene(gameObject, &scene) || !scene.valid ||
                !scene.loaded) {
                continue;
            }
            int count = -1;
            if (!InvokeManagedResult<int, GetInt>(
                    censusApi.switcherGetCameraCount, &count, switcher) ||
                count < 0 || count > 512) {
                continue;
            }
            ++switcherCount;
            std::ostringstream switcherLog;
            switcherLog << "[VR][vcam] VCAM_SELECTABLE_LIST serial=" << serial
                        << " scene=" << scene.handle << " sceneName="
                        << std::quoted(scene.name) << " switcher=" << switcher
                        << " name=" << std::quoted(ManagedObjectName(switcher))
                        << " count=" << count;
            Log(switcherLog.str());
            for (int cameraIndex = 0; cameraIndex < count; ++cameraIndex) {
                void* entry = nullptr;
                if (!InvokeManagedResult<void*, GetObjectAt>(
                        censusApi.switcherGetCamera, &entry, switcher,
                        cameraIndex) ||
                    !IsUnityManagedObjectAlive(entry)) {
                    continue;
                }
                void* virtualCamera = nullptr;
                void* director = nullptr;
                (void)ReadManagedField(
                    entry, censusApi.switchableVirtualCameraOffset,
                    &virtualCamera);
                if (censusApi.switchableDirectorOffset >= 0) {
                    (void)ReadManagedField(
                        entry, censusApi.switchableDirectorOffset, &director);
                }
                int priority = 0;
                const bool priorityKnown =
                    IsUnityManagedObjectAlive(virtualCamera) &&
                    censusApi.virtualCameraGetPriority.Ready() &&
                    InvokeManagedResult<int, GetInt>(
                        censusApi.virtualCameraGetPriority, &priority,
                        virtualCamera);
                const bool registered = std::find(
                    coreCameras.begin(), coreCameras.end(), virtualCamera) !=
                    coreCameras.end();
                const bool active = std::find(
                    activeBrainCameras.begin(), activeBrainCameras.end(),
                    virtualCamera) != activeBrainCameras.end();
                ++selectableCount;
                std::ostringstream selectable;
                selectable
                    << "[VR][vcam] VCAM_SELECTABLE_ITEM serial=" << serial
                    << " scene=" << scene.handle << " switcher=" << switcher
                    << " index=" << cameraIndex << " entry=" << entry
                    << " entryName=" << std::quoted(ManagedObjectName(entry))
                    << " camera=" << virtualCamera << " cameraName="
                    << std::quoted(ManagedObjectName(virtualCamera))
                    << " cameraClass="
                    << ManagedObjectClassName(virtualCamera)
                    << " cameraAlive="
                    << (IsUnityManagedObjectAlive(virtualCamera) ? 1 : 0)
                    << " director=" << director << " directorName="
                    << std::quoted(ManagedObjectName(director))
                    << " priority=";
                if (priorityKnown) {
                    selectable << priority;
                } else {
                    selectable << '?';
                }
                selectable << " core=" << (registered ? 1 : 0)
                           << " brainActive=" << (active ? 1 : 0);
                Log(selectable.str());
            }
        }
    }

    std::ostringstream end;
    end << "[VR][vcam] VCAM_CENSUS_END serial=" << serial
        << " loaded=" << records.size() << " dead=" << dead
        << " outsideLoadedScene=" << outsideLoadedScene
        << " coreResolved=" << coreCameras.size()
        << " activeBrainsResolved=" << activeBrainCameras.size()
        << " switchers=" << switcherCount
        << " selectable=" << selectableCount;
    Log(end.str());
}

bool ContainsCmovToken(std::string_view text) noexcept {
    if (text.size() < 4U) {
        return false;
    }
    std::string lower;
    lower.reserve(text.size());
    for (unsigned char ch : text) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lower.find("cmov") != std::string::npos;
}

using GetPtrFn = void* (*)(void*, void*);
using FindPassFn = int (*)(void*, void*, void*);
using HasPropertyFn = bool (*)(void*, int, void*);
using GetIntFn = int (*)(void*, void*);
using GetPropertyNameFn = void* (*)(void*, int, void*);
using MaterialVectorFn = void (*)(void*, int, UnityVector4*, void*);

bool MaterialGetVector(
    const UnityStereoRenderer::MethodRef& method,
    bool usesOutParam,
    void* material,
    int nameId,
    UnityVector4* value) noexcept {
    if (!method.HasInfo() || material == nullptr || value == nullptr ||
        nameId == 0) {
        return false;
    }
    if (usesOutParam) {
        void* arguments[] = {&nameId, value};
        void* ignored = nullptr;
        return RuntimeInvoke(method, material, arguments, &ignored, nullptr);
    }
    void* arguments[] = {&nameId};
    void* boxed = nullptr;
    if (!RuntimeInvoke(method, material, arguments, &boxed, nullptr) ||
        boxed == nullptr) {
        return false;
    }
    void* raw = UnboxObject(boxed);
    if (raw == nullptr) {
        return false;
    }
    return ReadVector4Seh(raw, value);
}

bool MaterialSetVector(
    const UnityStereoRenderer::MethodRef& method,
    void* material,
    int nameId,
    const UnityVector4& value) noexcept {
    if (!method.HasInfo() || material == nullptr || nameId == 0) {
        return false;
    }
    UnityVector4 local = value;
    void* arguments[] = {&nameId, &local};
    void* ignored = nullptr;
    return RuntimeInvoke(method, material, arguments, &ignored, nullptr);
}

void UnityStereoRenderer::EnsureSceneManagerApi() noexcept {
    if (sceneManagerApiReady_) {
        return;
    }
    sceneManagerApiReady_ = true;
    auto* klass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine.SceneManagement",
        "SceneManager");
    if (klass == nullptr) {
        klass = Il2cppUtils::GetClass(
            "UnityEngine.dll", "UnityEngine.SceneManagement", "SceneManager");
    }
    std::ostringstream dump;
    dump << "[VR][stereo] SCENE_IDENTITY_API";
    if (klass == nullptr) {
        dump << " class=0 fallback=material-death";
        sceneIdentityValid_ = true;
        Log(dump.str());
        return;
    }
    for (auto* method : klass->methods) {
        if (method == nullptr) {
            continue;
        }
        const auto& name = method->name;
        if (name.find("sceneCount") == std::string::npos &&
            name.find("SceneCount") == std::string::npos &&
            name.find("GetActiveScene") == std::string::npos &&
            name.find("loadedScene") == std::string::npos) {
            continue;
        }
        dump << " | " << MethodSignature(method);
    }
    sceneManagerGetSceneCount_ = MethodReference(
        FindMethodByName(klass, "get_sceneCount", true, 0U));
    sceneManagerGetLoadedSceneCount_ = MethodReference(
        FindMethodByName(klass, "get_loadedSceneCount", true, 0U));
    sceneManagerGetActiveSceneInjected_ = MethodReference(
        FindMethodByName(klass, "GetActiveScene_Injected", true, 1U));
    dump << " sceneCount="
         << (sceneManagerGetSceneCount_.Ready() ? "1" : "0")
         << " loadedCount="
         << (sceneManagerGetLoadedSceneCount_.Ready() ? "1" : "0")
         << " activeInjected="
         << (sceneManagerGetActiveSceneInjected_.Ready() ? "1" : "0");
    if (!sceneManagerGetSceneCount_.Ready() &&
        !sceneManagerGetActiveSceneInjected_.Ready()) {
        dump << " fallback=material-death";
        sceneIdentityValid_ = true;
    }
    Log(dump.str());
}

const char* UnityStereoRenderer::LifetimeWriteCategoryName(
    LifetimeWriteCategory category) noexcept {
    switch (category) {
    case LifetimeWriteCategory::EyeGameObject:
        return "eye-game-object";
    case LifetimeWriteCategory::EyeCamera:
        return "eye-camera";
    case LifetimeWriteCategory::SourceCamera:
        return "source-camera";
    case LifetimeWriteCategory::TargetTexture:
        return "target-texture";
    case LifetimeWriteCategory::MainCameraTag:
        return "main-camera-tag";
    case LifetimeWriteCategory::RenderTarget:
        return "render-target";
    case LifetimeWriteCategory::OutlineMaterial:
        return "outline-material";
    case LifetimeWriteCategory::ProFlare:
        return "pro-flare";
    case LifetimeWriteCategory::UiTextureOverlay:
        return "ui-texture-overlay";
    case LifetimeWriteCategory::LiveCameraOverlay:
        return "live-camera-overlay";
    case LifetimeWriteCategory::CmovParticle:
        return "cmov-particle";
    case LifetimeWriteCategory::Count:
        break;
    }
    return "unknown";
}

void UnityStereoRenderer::RecordLifetimeWrite(
    LifetimeWriteCategory category,
    const char* action,
    void* object,
    void* related,
    std::intptr_t value) noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(category);
    if (index >= lifetimeWrites_.size()) {
        return;
    }
    const SceneReadyDiagnosticState ready = CurrentSceneReadyDiagnosticState();
    LifetimeWriteRecord& record = lifetimeWrites_[index];
    record.serial = ++lifetimeWriteSerial_;
    record.sceneReadyEpoch = ready.epoch;
    record.action = action != nullptr ? action : "unknown";
    record.object = object;
    record.related = related;
    record.value = value;
}

void UnityStereoRenderer::LogLifetimeSnapshot(
    const char* phase,
    const char* where) noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    const std::uint64_t snapshot = ++lifetimeSnapshotSerial_;
    const SceneReadyDiagnosticState ready = CurrentSceneReadyDiagnosticState();
    std::ostringstream header;
    header << "[VR][lifetime] LIFETIME_SNAPSHOT serial=" << snapshot
           << " phase=" << (phase != nullptr ? phase : "unknown")
           << " where=" << (where != nullptr ? where : "unknown")
           << " readyEpoch=" << ready.epoch
           << " revokeSerial=" << ready.revokeSerial
           << " releaseSerial=" << ready.releaseSerial
           << " transition=" << (ready.transitionActive ? 1 : 0)
           << " contentReady=" << (ready.contentReady ? 1 : 0)
           << " loadingKnown=" << (ready.loadingKnown ? 1 : 0)
           << " loading=" << (ready.loadingActive ? 1 : 0)
           << " identity=" << (ready.epochSawIdentity ? 1 : 0)
           << " renderAllowed=" << (ready.renderAllowed ? 1 : 0)
           << " publishAllowed=" << (ready.publishAllowed ? 1 : 0)
           << " scene=" << sceneCount_ << '/' << loadedSceneCount_ << '/'
           << activeSceneHandle_ << " stage=" << StageName(stage_)
           << " armed=" << (stageArmed_ ? 1 : 0)
           << " parked=" << (sceneReadyParked_ ? 1 : 0)
           << " eyeHeld=" << (eyeArmHeld_ ? 1 : 0);
    Log(header.str());

    std::ostringstream state;
    state << "[VR][lifetime] LIFETIME_STATE snapshot=" << snapshot
          << " full=" << fullWidth_ << 'x' << fullHeight_
          << " fullGeneration=" << fullGeneration_
          << " retiredHandles=" << retiredFullTargetHandles_.size()
          << " source=" << latestSourceCamera_
          << " sourceData=" << latestSourceUniversalData_
          << " sourceTarget=" << latestSourceTarget_
          << " suppressedSource=" << suppressedSourceCamera_
          << " sourceSuppressed=" << (sourceCameraSuppressed_ ? 1 : 0)
          << " tinyCamera=" << tinySourceCamera_
          << " tinyTarget=" << tinySourceTarget_
          << " tinySaved=" << tinySavedTarget_
          << " tinyActive=" << (tinyModeActive_ ? 1 : 0)
          << " tagObject=" << eyeMainCameraTaggedObject_
          << " tagApplied=" << (eyeMainCameraTagApplied_ ? 1 : 0)
          << " caches=" << actorOutlineMaterials_.size() << '/'
          << authoredProFlareScales_.size() << '/'
          << uiTextureOverlays_.size() << '/'
          << liveCameraOverlays_.size() << '/' << cmovParticles_.size()
          << " cacheOwned=" << (outlineEyeScaled_ ? 1 : 0) << '/'
          << (proFlareEyeScaled_ ? 1 : 0) << '/'
          << (uiTextureOverlaysHidden_ ? 1 : 0) << '/'
          << (liveCameraOverlaysHidden_ ? 1 : 0) << '/'
          << (cmovParticlesHidden_ ? 1 : 0);
    Log(state.str());

    const auto logObject = [this, snapshot](
                               const char* role,
                               void* managed,
                               Il2CppGCHandle handle) {
        const bool alive = IsUnityManagedObjectAlive(managed);
        void* native = alive ? ReadUnityNativePointer(managed) : nullptr;
        std::ostringstream object;
        object << "[VR][lifetime] LIFETIME_OBJECT snapshot=" << snapshot
               << " role=" << role << " managed=" << managed
               << " native=" << native << " handle=" << handle
               << " alive=" << (alive ? 1 : 0);
        Log(object.str());
    };
    for (std::size_t eye = 0; eye < eyeGameObjects_.size(); ++eye) {
        const char* suffix = eye == 0U ? "left" : "right";
        const std::string gameObjectRole = std::string("eye-game-object-") + suffix;
        const std::string cameraRole = std::string("eye-camera-") + suffix;
        const std::string urpRole = std::string("eye-urp-data-") + suffix;
        const std::string vlRole = std::string("eye-vlsrp-data-") + suffix;
        const std::string admissionRole = std::string("admission-target-") + suffix;
        const std::string fullRole = std::string("full-target-") + suffix;
        logObject(gameObjectRole.c_str(), eyeGameObjects_[eye],
                  eyeGameObjectHandles_[eye]);
        logObject(cameraRole.c_str(), eyeCameras_[eye], eyeCameraHandles_[eye]);
        logObject(urpRole.c_str(), eyeUniversalCameraData_[eye],
                  eyeUniversalCameraDataHandles_[eye]);
        logObject(vlRole.c_str(), eyeVlAdditionalCameraData_[eye],
                  eyeVlAdditionalCameraDataHandles_[eye]);
        logObject(admissionRole.c_str(), admissionTargets_[eye],
                  admissionTargetHandles_[eye]);
        logObject(fullRole.c_str(), fullTargets_[eye], fullTargetHandles_[eye]);
    }
    logObject("latest-source-camera", latestSourceCamera_, nullptr);
    logObject("latest-source-data", latestSourceUniversalData_, nullptr);
    logObject("latest-source-target", latestSourceTarget_, nullptr);
    logObject("suppressed-source-camera", suppressedSourceCamera_, nullptr);
    logObject("tiny-source-camera", tinySourceCamera_, nullptr);
    logObject("tiny-source-target", tinySourceTarget_, tinySourceTargetHandle_);
    logObject("tiny-saved-target", tinySavedTarget_, nullptr);
    logObject("tagged-eye-object", eyeMainCameraTaggedObject_, nullptr);
    logObject("matcap-command-buffer", matcapCompCommandBuffer_, nullptr);

    for (std::size_t index = 0; index < lifetimeWrites_.size(); ++index) {
        const LifetimeWriteRecord& record = lifetimeWrites_[index];
        if (record.serial == 0U) {
            continue;
        }
        const auto category = static_cast<LifetimeWriteCategory>(index);
        std::ostringstream write;
        write << "[VR][lifetime] LIFETIME_LAST_WRITE snapshot=" << snapshot
              << " category=" << LifetimeWriteCategoryName(category)
              << " serial=" << record.serial
              << " readyEpoch=" << record.sceneReadyEpoch
              << " action=" << record.action << " object=" << record.object
              << " related=" << record.related << " value=" << record.value;
        Log(write.str());
    }
}

void UnityStereoRenderer::ObserveLifetimeSceneReadyEdges(
    const char* where) noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        return;
    }
    const SceneReadyDiagnosticState ready = CurrentSceneReadyDiagnosticState();
    if (ready.revokeSerial != observedSceneReadyRevokeSerial_) {
        observedSceneReadyRevokeSerial_ = ready.revokeSerial;
        LogLifetimeSnapshot("ready-revoke", where);
    }
    if (ready.releaseSerial != observedSceneReadyReleaseSerial_) {
        observedSceneReadyReleaseSerial_ = ready.releaseSerial;
        LogLifetimeSnapshot("ready-release", where);
    }
}

bool UnityStereoRenderer::SceneContentUnstable() const noexcept {
    return !SceneReadyAllowsStereoRender() ||
        (EyeArmHeld() && eyeArmAwaitingSettle_);
}

bool UnityStereoRenderer::EyeArmHeld() const noexcept {
    return eyeArmHeld_;
}

void UnityStereoRenderer::HoldEyeArm(const char* reason) noexcept {
    if (reason == nullptr) {
        reason = "unknown";
    }
    const bool identity =
        std::strcmp(reason, "scene-identity") == 0;
    const bool alreadyHeld = eyeArmHeld_;
    eyeArmHeld_ = true;
    if (identity) {
        eyeArmAwaitingSettle_ = true;
    }
    if (alreadyHeld && eyeArmHoldReason_ != nullptr &&
        std::strcmp(eyeArmHoldReason_, reason) == 0) {
        return;
    }
    eyeArmHoldReason_ = reason;
    eyeArmSkipLogged_ = false;
    Log(std::string("[VR][stereo] EYE_ARM_HELD reason=") + reason +
        " settle=" + (eyeArmAwaitingSettle_ ? "1" : "0"));
}

void UnityStereoRenderer::ReleaseEyeArm(const char* reason) noexcept {
    if (!eyeArmHeld_) {
        return;
    }
    eyeArmHeld_ = false;
    eyeArmAwaitingSettle_ = false;
    eyeArmSkipLogged_ = false;
    eyeArmHoldReason_ = nullptr;
    Log(std::string("[VR][stereo] EYE_ARM_RELEASED reason=") +
        (reason != nullptr ? reason : "unknown"));
}

void UnityStereoRenderer::RequestHeavyDiscover() noexcept {
    RequestActorOutlineDiscover("heavy-discover");
    allowFlareDiscover_ = true;
    allowProFlareDiscover_ = true;
}

void UnityStereoRenderer::RequestActorOutlineDiscover(
    const char* reason) noexcept {
    outlineCaptureEnabled_.store(true, std::memory_order_release);
    allowOutlineDiscover_ = true;
    outlineDiscoverAtActorDraw_ = true;
    outlineEmptyDiscoverLogged_ = false;
    Log(std::string("[VR][stereo] EYE_OUTLINE_DISCOVER_QUEUED reason=") +
        (reason != nullptr ? reason : "unknown"));
}

void UnityStereoRenderer::RefreshSceneIdentity(const char* where) noexcept {
    EnsureSceneManagerApi();
    int sceneCount = 0;
    int loadedCount = 0;
    int handle = 0;
    bool gotCount = false;
    bool gotHandle = false;
    if (sceneManagerGetSceneCount_.Ready()) {
        using GetInt = int (*)(void*);
        gotCount = InvokeManagedResult<int, GetInt>(
            sceneManagerGetSceneCount_, &sceneCount);
    }
    if (sceneManagerGetLoadedSceneCount_.Ready()) {
        using GetInt = int (*)(void*);
        int loaded = 0;
        if (InvokeManagedResult<int, GetInt>(
                sceneManagerGetLoadedSceneCount_, &loaded)) {
            loadedCount = loaded;
        }
    }
    if (sceneManagerGetActiveSceneInjected_.Ready()) {
        using Injected = void (*)(void*, void*);
        alignas(8) std::uint8_t sceneBytes[16]{};
        if (InvokeManagedVoid<Injected>(
                sceneManagerGetActiveSceneInjected_, sceneBytes)) {
            handle = *reinterpret_cast<const int*>(sceneBytes);
            gotHandle = true;
        }
    }
    if (!gotCount && !gotHandle) {
        return;
    }
    if (!sceneIdentityValid_) {
        sceneCount_ = sceneCount;
        loadedSceneCount_ = loadedCount;
        activeSceneHandle_ = handle;
        sceneIdentityValid_ = true;
        Log(std::string("[VR][stereo] SCENE_IDENTITY where=") +
            (where != nullptr ? where : "-") +
            " count=" + std::to_string(sceneCount) +
            " loaded=" + std::to_string(loadedCount) +
            " handle=" + std::to_string(handle));
        return;
    }
    if (sceneCount_ == sceneCount && loadedSceneCount_ == loadedCount &&
        activeSceneHandle_ == handle) {
        // Burst of count/handle flips is teardown. The first unchanged
        // sample after that burst is "they finished" — arm on the same Tick.
        if (eyeArmHeld_ && eyeArmAwaitingSettle_ &&
            ReadStereoRenderTargetSpec().enabled) {
            ReleaseEyeArm("identity-settled");
        }
        return;
    }
    const int previousCount = sceneCount_;
    const int previousLoaded = loadedSceneCount_;
    const int previousHandle = activeSceneHandle_;
    const bool sceneAdded = sceneCount > sceneCount_;
    sceneCount_ = sceneCount;
    loadedSceneCount_ = loadedCount;
    activeSceneHandle_ = handle;
    // Snapshot the old resource ownership before the ready gate or cache
    // fences mutate it. This also runs for keep-armed identity flaps.
    LogLifetimeSnapshot("identity-before-gate", where);
    NoteSceneReadyIdentityChanged(where);
    ObserveLifetimeSceneReadyEdges("identity-after-gate");
    // Identity-only flaps (no open ready epoch) are additive UI / idol-path
    // replacements. Holding eyes and queueing disarm is what made .164 hitch
    // the whole session after stereo was already up. Contest still keeps
    // Game3DManager + eligibility while actor materials die (.287: four
    // keep-armed 3→1 flaps, then WriteOutlineMaterialsForEyes SetVector'd
    // the stale 55-material cache). A count decrease is a real unload:
    // drop pointers even when eyes stay armed. Do not FindObjects here —
    // the constructor hook recaptures destination materials. Count-up /
    // handle-only keep-armed flaps keep the live cache.
    const bool keepArmed = SceneReadyAllowsStereoRender();
    const bool sceneRemoved = sceneCount < previousCount;
    std::ostringstream stream;
    stream << "[VR][stereo] SCENE_IDENTITY_CHANGED where="
           << (where != nullptr ? where : "-") << " count=" << previousCount
           << "->" << sceneCount << " loaded=" << previousLoaded << "->"
           << loadedCount << " handle=" << previousHandle << "->"
           << handle << " action=" << (keepArmed ? "keep-armed" : "hold");
    Log(stream.str());
    if (!keepArmed || sceneRemoved) {
        DropOutlineMaterialCache("scene-identity");
    }
    if (keepArmed) {
        return;
    }
    DropUiTextureOverlayCache("scene-identity");
    DropLiveCameraOverlayCache("scene-identity");
    DropCmovParticleCache("scene-identity");
    // .129 enter and .174 exit share an AssetGarbageCollectorHelper
    // null-class fault at GameAssembly+0x8d6f5a, with no VERSION frame.
    // Keep this scene-add cache drop as a conservative stale-write fence,
    // not as a claimed root-cause fix. 3<->2 flickers do not add a scene.
    if (sceneAdded) {
        DropProFlareCache("scene-add");
    }
    HoldEyeArm("scene-identity");
    if (stageArmed_ && !decisionPending_) {
        QueueStageDecision(false, "scene-ineligible");
    }
}

void UnityStereoRenderer::DropOutlineMaterialCache(const char* reason) noexcept {
    const std::string_view dropReason = reason != nullptr ? reason : "unknown";
    const bool resumeEventCapture =
        dropReason == "scene-ready-park" ||
        dropReason == "scene-identity" || dropReason == "material-died";
    const std::size_t cached = actorOutlineMaterials_.size();
    actorOutlineMaterials_.clear();
    outlineCaptureEnabled_.store(false, std::memory_order_release);
    std::size_t pending = 0U;
    {
        std::lock_guard<std::mutex> lock(pendingOutlineMaterialMutex_);
        pending = pendingOutlineMaterials_.size();
        pendingOutlineMaterials_.clear();
        outlineCapturePending_.store(false, std::memory_order_release);
    }
    // A transition drop must not let the next actor draw immediately repopulate
    // the cache from an old-scene fallback scan. The MaterialInfo constructor
    // hook can still queue destination materials, and content-ready explicitly
    // re-arms the one-shot compatibility scan.
    allowOutlineDiscover_ = false;
    outlineDiscoverAtActorDraw_ = false;
    outlineEyeScaled_ = false;
    outlineMaterialLogged_ = false;
    outlineGripRestoreLogged_ = false;
    outlineGripLoggedScale_ = -1.0F;
    lastOutlineWidthScale_ = -1.0F;
    loggedOutlineWidthScale_ = -1.0F;
    outlineMaterialDumpLogged_ = false;
    outlineMaterialDiscoverSerial_ = 0;
    outlineEmptyDiscoverLogged_ = false;
    // Scene identities are generation fences: anything queued before the fence
    // was cleared above, while constructors after it belong to the newest
    // material population. Keep that zero-scan event path open during loading;
    // FindObjects remains disabled until the explicit content-ready request.
    outlineCaptureEnabled_.store(resumeEventCapture, std::memory_order_release);
    if (cached == 0U && pending == 0U) {
        return;
    }
    Log(std::string("[VR][stereo] OUTLINE_CACHE_DROPPED count=") +
        std::to_string(cached) + " pending=" + std::to_string(pending) +
        " reason=" +
        std::string(dropReason));
}

void UnityStereoRenderer::EnsureOutlineMaterialApi() noexcept {
    if (outlineParamId_ == 0) {
        auto* campusPass = Il2cppUtils::GetClass(
            "campus-submodule.Runtime.dll", "Campus.Rendering",
            "CampusActorParameterPass");
        outlineParamId_ = ReadStaticIntField(campusPass, "_OutlineParam");
        if (outlineParamId_ == 0) {
            auto* shaderIds = Il2cppUtils::GetClass(
                "vl-unity.Runtime.dll", "VL", "ShaderIDs");
            outlineParamId_ = ReadStaticIntField(shaderIds, "_OutlineParam");
        }
        if (outlineParamId_ == 0) {
            auto* campusPassAlt = Il2cppUtils::GetClass(
                "Assembly-CSharp.dll", "Campus.Rendering",
                "CampusActorParameterPass");
            outlineParamId_ = ReadStaticIntField(campusPassAlt, "_OutlineParam");
        }
        if (outlineParamId_ == 0) {
            auto* shaderClass = Il2cppUtils::GetClass(
                "UnityEngine.CoreModule.dll", "UnityEngine", "Shader");
            UnityResolve::Method* propertyToID = nullptr;
            if (shaderClass != nullptr) {
                for (auto* method : shaderClass->methods) {
                    if (method != nullptr && method->function != nullptr &&
                        method->static_function &&
                        method->name == "PropertyToID" &&
                        method->args.size() == 1U) {
                        propertyToID = method;
                        break;
                    }
                }
            }
            if (propertyToID != nullptr) {
                void* name = UnityResolve::Invoke<void*, const char*>(
                    "il2cpp_string_new", "_OutlineParam");
                int id = 0;
                if (name != nullptr &&
                    InvokeManagedResult<int, int (*)(void*, void*)>(
                        MethodReference(propertyToID), &id, name) &&
                    id != 0) {
                    outlineParamId_ = id;
                }
            }
        }
    }
    if (outlineMaterialApiReady_) {
        return;
    }
    outlineMaterialApiReady_ = true;

    auto* rendererClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer");
    auto* materialClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Material");
    auto* shaderClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Shader");

    api_.rendererGetSharedMaterials = MethodReference(
        FindMethodByName(rendererClass, "get_sharedMaterials", false, 0U));
    api_.materialGetShader = MethodReference(
        FindMethodByName(materialClass, "get_shader", false, 0U));
    api_.materialFindPass = MethodReference(
        FindMethodByName(materialClass, "FindPass", false, 1U));
    UnityResolve::Method* hasProperty = FindInstanceIntIdMethod(
        materialClass, {"HasProperty"}, 1U);
    UnityResolve::Method* getVector = FindInstanceIntIdMethod(
        materialClass,
        {"GetColorImpl_Injected", "GetVectorImpl_Injected"},
        2U);
    bool getUsesOut = getVector != nullptr;
    if (getVector == nullptr) {
        getVector = FindInstanceIntIdMethod(
            materialClass, {"GetVector", "GetColor"}, 1U);
        getUsesOut = false;
    }
    UnityResolve::Method* setVector = FindInstanceIntIdMethod(
        materialClass,
        {"SetColorImpl_Injected", "SetVectorImpl_Injected", "SetVector",
         "SetColor"},
        2U);
    api_.materialHasProperty = MethodReference(hasProperty);
    api_.materialGetVectorInjected = MethodReference(getVector);
    api_.materialSetVectorInjected = MethodReference(setVector);
    materialGetVectorUsesOutParam_ = getUsesOut;
    outlineGetVectorName_ =
        getVector != nullptr ? getVector->name.c_str() : "-";
    outlineSetVectorName_ =
        setVector != nullptr ? setVector->name.c_str() : "-";
    api_.shaderGetPropertyCount = MethodReference(
        FindMethodByName(shaderClass, "GetPropertyCount", false, 0U));
    api_.shaderGetPropertyName = MethodReference(
        FindMethodByName(shaderClass, "GetPropertyName", false, 1U));

    outlineFindPassForwardName_ = InternIl2CppString("Forward (Outline)");
    outlineFindPassGBufferName_ = InternIl2CppString("GBuffer (Outline)");

    Log(std::string("[VR][stereo] EYE_OUTLINE_MAT_API id=") +
        std::to_string(outlineParamId_) +
        " sharedMats=" + (api_.rendererGetSharedMaterials.Ready() ? "1" : "0") +
        " getShader=" + (api_.materialGetShader.Ready() ? "1" : "0") +
        " findPass=" + (api_.materialFindPass.Ready() ? "1" : "0") +
        " hasProp=" + (api_.materialHasProperty.HasInfo() ? "1" : "0") +
        " getVec=" + (api_.materialGetVectorInjected.HasInfo() ? "1" : "0") +
        " setVec=" + (api_.materialSetVectorInjected.HasInfo() ? "1" : "0") +
        " getName=" + outlineGetVectorName_ +
        " setName=" + outlineSetVectorName_ +
        " getOut=" + (materialGetVectorUsesOutParam_ ? "1" : "0"));
}

bool MaterialLooksLikeOutline(
    void* material,
    const UnityStereoRenderer::MethodRef& findPass,
    const UnityStereoRenderer::MethodRef& getShader,
    void* forwardName,
    void* gbufferName) noexcept {
    if (material == nullptr) {
        return false;
    }
    int forwardPass = -1;
    int gbufferPass = -1;
    if (findPass.Ready() && forwardName != nullptr) {
        (void)InvokeManagedResult<int, FindPassFn>(
            findPass, &forwardPass, material, forwardName);
    }
    if (findPass.Ready() && gbufferName != nullptr) {
        (void)InvokeManagedResult<int, FindPassFn>(
            findPass, &gbufferPass, material, gbufferName);
    }
    if (forwardPass >= 0 || gbufferPass >= 0) {
        return true;
    }
    if (!getShader.Ready()) {
        return false;
    }
    void* shader = nullptr;
    if (!InvokeManagedResult<void*, GetPtrFn>(getShader, &shader, material) ||
        shader == nullptr) {
        return false;
    }
    const std::string shaderName = ManagedObjectName(shader);
    return shaderName.find("Actor") != std::string::npos ||
        shaderName.find("actor") != std::string::npos;
}

void UnityStereoRenderer::DiscoverActorOutlineMaterials() noexcept {
    EnsureOutlineMaterialApi();
    if (!api_.rendererGetSharedMaterials.Ready() || SceneContentUnstable()) {
        return;
    }

    actorOutlineMaterials_.erase(
        std::remove_if(
            actorOutlineMaterials_.begin(), actorOutlineMaterials_.end(),
            [](const AuthoredOutlineMaterial& entry) {
                return !IsUnityManagedObjectAlive(entry.material);
            }),
        actorOutlineMaterials_.end());

    // One-shot after identity settle / source-boundary. Do not poll
    // empty Renderer lists, and do not scan inactive objects -- that
    // is the .122 Tick hang. material-died drops the cache but does
    // not re-arm this scan.
    constexpr std::size_t kMaxMaterials = 2048U;
    auto* rendererClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "Renderer");
    if (rendererClass == nullptr || !actorOutlineMaterials_.empty() ||
        !allowOutlineDiscover_) {
        return;
    }
    allowOutlineDiscover_ = false;
    const auto renderers = rendererClass->FindObjectsByType<void*>();
    Log(std::string("[VR][stereo] HEAVY_DISCOVER kind=outline via=FindObjectsOfType count=") +
        std::to_string(renderers.size()));
    std::uint32_t added = 0;
    for (void* renderer : renderers) {
        if (renderer == nullptr || !IsUnityManagedObjectAlive(renderer) ||
            actorOutlineMaterials_.size() >= kMaxMaterials) {
            continue;
        }
        void* arrayObject = nullptr;
        if (!InvokeManagedResult<void*, GetPtrFn>(
                api_.rendererGetSharedMaterials, &arrayObject, renderer) ||
            arrayObject == nullptr) {
            continue;
        }
        auto* array =
            reinterpret_cast<UnityResolve::UnityType::Array<void*>*>(
                arrayObject);
        std::vector<void*> materials;
        try {
            materials = array->ToVector();
        } catch (...) {
            continue;
        }
        for (void* material : materials) {
            if (material == nullptr || !IsUnityManagedObjectAlive(material) ||
                actorOutlineMaterials_.size() >= kMaxMaterials) {
                continue;
            }
            const bool known = std::any_of(
                actorOutlineMaterials_.begin(), actorOutlineMaterials_.end(),
                [material](const AuthoredOutlineMaterial& entry) {
                    return entry.material == material;
                });
            if (known) {
                continue;
            }
            if (!MaterialLooksLikeOutline(
                    material, api_.materialFindPass, api_.materialGetShader,
                    outlineFindPassForwardName_, outlineFindPassGBufferName_)) {
                continue;
            }
            actorOutlineMaterials_.push_back({material});
            ++added;
        }
    }
    if (added > 0U || !outlineEmptyDiscoverLogged_) {
        outlineEmptyDiscoverLogged_ = true;
        Log(std::string("[VR][stereo] EYE_OUTLINE_MAT_DISCOVER renderers=") +
            std::to_string(renderers.size()) +
            " added=" + std::to_string(added) +
            " total=" + std::to_string(actorOutlineMaterials_.size()));
    }
}

void UnityStereoRenderer::QueueActorOutlineMaterial(void* material) noexcept {
    if (material == nullptr ||
        !outlineCaptureEnabled_.load(std::memory_order_acquire)) {
        return;
    }
    constexpr std::size_t kMaxPendingMaterials = 2048U;
    std::lock_guard<std::mutex> lock(pendingOutlineMaterialMutex_);
    if (!outlineCaptureEnabled_.load(std::memory_order_acquire) ||
        pendingOutlineMaterials_.size() >= kMaxPendingMaterials ||
        std::find(pendingOutlineMaterials_.begin(),
                  pendingOutlineMaterials_.end(), material) !=
            pendingOutlineMaterials_.end()) {
        return;
    }
    pendingOutlineMaterials_.push_back(material);
    outlineCapturePending_.store(true, std::memory_order_release);
}

void UnityStereoRenderer::CaptureQueuedActorOutlineMaterials() noexcept {
    if (!outlineCapturePending_.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<void*> candidates;
    {
        std::lock_guard<std::mutex> lock(pendingOutlineMaterialMutex_);
        candidates.swap(pendingOutlineMaterials_);
        outlineCapturePending_.store(false, std::memory_order_release);
    }
    if (candidates.empty()) {
        return;
    }
    EnsureOutlineMaterialApi();
    constexpr std::size_t kMaxMaterials = 2048U;
    std::uint32_t added = 0;
    for (void* material : candidates) {
        if (material == nullptr || !IsUnityManagedObjectAlive(material) ||
            actorOutlineMaterials_.size() >= kMaxMaterials) {
            continue;
        }
        const bool known = std::any_of(
            actorOutlineMaterials_.begin(), actorOutlineMaterials_.end(),
            [material](const AuthoredOutlineMaterial& entry) {
                return entry.material == material;
            });
        if (known || !MaterialLooksLikeOutline(
                         material, api_.materialFindPass,
                         api_.materialGetShader, outlineFindPassForwardName_,
                         outlineFindPassGBufferName_)) {
            continue;
        }
        actorOutlineMaterials_.push_back({material});
        ++added;
    }
    Log(std::string("[VR][stereo] EYE_OUTLINE_MAT_CAPTURE candidates=") +
        std::to_string(candidates.size()) +
        " added=" + std::to_string(added) +
        " total=" + std::to_string(actorOutlineMaterials_.size()));
}

void UnityStereoRenderer::PrepareActorOutlineMaterialsForCurrentDraw() noexcept {
    const bool captured =
        outlineCapturePending_.load(std::memory_order_acquire);
    if (!IsOwnerThread() || (!outlineDiscoverAtActorDraw_ && !captured) ||
        currentCamera_ == nullptr || SceneContentUnstable() ||
        !SceneReadyAllowsStereoRender()) {
        return;
    }
    const bool leftEye = currentCamera_ == eyeCameras_[0];
    if (!leftEye) {
        return;
    }
    CaptureQueuedActorOutlineMaterials();
    outlineDiscoverAtActorDraw_ = false;
    if (actorOutlineMaterials_.empty()) {
        // Compatibility fallback for a hook miss or an actor that predates
        // hook installation. It is still one-shot and tied to a proven actor
        // draw, never a Tick timer or periodic rediscovery.
        DiscoverActorOutlineMaterials();
    } else {
        allowOutlineDiscover_ = false;
    }
    // OnBeginCamera ran before this deferred discovery. Apply now so the
    // immediately following actor GBuffer draw sees the eye width.
    WriteOutlineMaterialsForEyes();
}

void UnityStereoRenderer::EnsureOutlineOfficialApi() noexcept {
    if (outlineOfficialApiAttempted_) {
        return;
    }
    outlineOfficialApiAttempted_ = true;
    EnsureOutlineMaterialApi();
    auto* curveClass = Il2cppUtils::GetClass(
        "UnityEngine.CoreModule.dll", "UnityEngine", "AnimationCurve");
    if (curveClass != nullptr) {
        for (auto* method : curveClass->methods) {
            if (method != nullptr && method->name == "Evaluate" &&
                HasExactSignature(
                    method, false, "System.Single", {"System.Single"}) &&
                method->function != nullptr && method->address != nullptr) {
                outlineCurveEvaluate_ = MethodReference(method);
                break;
            }
        }
    }
    auto* campusPass = Il2cppUtils::GetClass(
        "campus-submodule.Runtime.dll", "Campus.Rendering",
        "CampusActorParameterPass");
    auto* campusSettings = Il2cppUtils::GetClass(
        "campus-submodule.Runtime.dll", "Campus.Rendering",
        "CampusActorParameterSettings");
    outlineOfficialSettingsOffset_ =
        FindNamedFieldOffset(campusPass, {"_settings"}, {});
    outlineOfficialFieldOffsets_[0] = FindNamedFieldOffset(
        campusSettings, {"outlineRangeMinSize"}, {"System.Single"});
    outlineOfficialFieldOffsets_[1] = FindNamedFieldOffset(
        campusSettings, {"outlineRangeMaxSize"}, {"System.Single"});
    outlineOfficialFieldOffsets_[2] = FindNamedFieldOffset(
        campusSettings, {"outlineRangeDistance"}, {"System.Single"});
    outlineOfficialFieldOffsets_[3] = FindNamedFieldOffset(
        campusSettings, {"outlineFocalLengthScale"},
        {"UnityEngine.AnimationCurve"});
    std::ostringstream summary;
    summary << "[VR][outline] OUTLINE_OFFICIAL_API curveEvaluate="
            << (outlineCurveEvaluate_.Ready() ? 1 : 0)
            << " settingsOffset=0x" << std::hex
            << outlineOfficialSettingsOffset_ << " fieldOffsets=(0x"
            << outlineOfficialFieldOffsets_[0] << ",0x"
            << outlineOfficialFieldOffsets_[1] << ",0x"
            << outlineOfficialFieldOffsets_[2] << ",0x"
            << outlineOfficialFieldOffsets_[3] << ')' << std::dec;
    Log(summary.str());
}

void UnityStereoRenderer::CaptureOfficialOutlineVector(
    void* passInstance) noexcept {
    if (!IsOwnerThread() || passInstance == nullptr) {
        return;
    }
    void* camera = currentCamera_;
    if (camera == nullptr) {
        return;
    }
    EnsureOutlineOfficialApi();
    if (!outlineCurveEvaluate_.Ready() || outlineOfficialSettingsOffset_ < 0 ||
        outlineOfficialFieldOffsets_[0] < 0 ||
        outlineOfficialFieldOffsets_[1] < 0 ||
        outlineOfficialFieldOffsets_[2] < 0 ||
        outlineOfficialFieldOffsets_[3] < 0 ||
        !api_.cameraGetFieldOfView.Ready() ||
        !api_.cameraGetUsePhysicalProperties.Ready()) {
        return;
    }
    using GetFloat = float (*)(void*, void*);
    using GetBool = bool (*)(void*, void*);
    using GetNativeFloat = float (*)(void*, void*);
    using GetNativeVector2 = void (*)(void*, UnityVector2*, void*);
    float fov = 0.0F;
    if (!InvokeManagedResult<float, GetFloat>(
            api_.cameraGetFieldOfView, &fov, camera) ||
        !std::isfinite(fov) || fov <= 0.0F || fov >= 179.0F) {
        return;
    }
    bool physical = false;
    (void)InvokeManagedResult<bool, GetBool>(
        api_.cameraGetUsePhysicalProperties, &physical, camera);
    void* nativeCamera = ReadUnityNativePointer(camera);
    float focalLength = 0.0F;
    UnityVector2 sensor{};
    bool focalValid = false;
    bool sensorValid = false;
    if (nativeCamera != nullptr) {
        focalValid = api_.cameraGetFocalLengthInjected.Ready() &&
            InvokeManagedResult<float, GetNativeFloat>(
                api_.cameraGetFocalLengthInjected, &focalLength,
                nativeCamera) &&
            std::isfinite(focalLength) && focalLength > 0.0F;
        sensorValid = api_.cameraGetSensorSizeInjected.Ready() &&
            InvokeManagedVoid<GetNativeVector2>(
                api_.cameraGetSensorSizeInjected, nativeCamera, &sensor) &&
            std::isfinite(sensor.y) && sensor.y > 0.0F;
    }
    // The official writer consumes the physical focal length; non-physical
    // cameras (our eyes) map FOV to the equivalent focal through the sensor
    // gate. `.176` hardware: eye fov=100.24, sensor=(22.21,24) -> focal
    // 10.0257 -> curve 3.643458, matching the writer-published global.
    float curveInput = 0.0F;
    if (physical && focalValid) {
        curveInput = focalLength;
    } else if (sensorValid) {
        constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;
        const float tangent = std::tan(fov * 0.5F * kDegToRad);
        if (std::isfinite(tangent) && tangent > 0.0F) {
            curveInput = sensor.y * 0.5F / tangent;
        }
    }
    if (!std::isfinite(curveInput) || curveInput <= 0.0F) {
        return;
    }
    void* settings = nullptr;
    if (!ReadManagedField(
            passInstance, outlineOfficialSettingsOffset_, &settings) ||
        settings == nullptr) {
        return;
    }
    float rangeMin = 0.0F;
    float rangeMax = 0.0F;
    float rangeDistance = 0.0F;
    if (!ReadManagedField(
            settings, outlineOfficialFieldOffsets_[0], &rangeMin) ||
        !ReadManagedField(
            settings, outlineOfficialFieldOffsets_[1], &rangeMax) ||
        !ReadManagedField(
            settings, outlineOfficialFieldOffsets_[2], &rangeDistance) ||
        !std::isfinite(rangeMin) || !std::isfinite(rangeMax) ||
        !std::isfinite(rangeDistance) ||
        std::fabs(rangeDistance) <= 0.000001F) {
        return;
    }
    void* curve = nullptr;
    if (!ReadManagedField(
            settings, outlineOfficialFieldOffsets_[3], &curve) ||
        curve == nullptr) {
        return;
    }
    using Evaluate = float (*)(void*, float, void*);
    float curveValue = 0.0F;
    if (!InvokeManagedResult<float, Evaluate>(
            outlineCurveEvaluate_, &curveValue, curve, curveInput) ||
        !std::isfinite(curveValue)) {
        return;
    }
    const bool eye = camera == eyeCameras_[0] || camera == eyeCameras_[1];
    const std::size_t slot = eye ? 1U : 0U;
    OfficialOutlineCapture& target = officialOutline_[slot];
    target.valid = true;
    target.fovDegrees = fov;
    target.value = {rangeMin, rangeMax, 1.0F / rangeDistance, curveValue};
    const auto now = std::chrono::steady_clock::now();
    const std::array<float, 4>& logged = officialOutlineLoggedValue_[slot];
    const bool changed = !officialOutlineLogged_[slot] ||
        std::fabs(target.value[0] - logged[0]) > 0.01F ||
        std::fabs(target.value[1] - logged[1]) > 0.01F ||
        std::fabs(target.value[2] - logged[2]) > 0.001F ||
        std::fabs(target.value[3] - logged[3]) > 0.05F;
    const bool spaced = !officialOutlineLogged_[slot] ||
        now - officialOutlineLoggedAt_[slot] >= std::chrono::seconds(2);
    if (changed && spaced) {
        officialOutlineLogged_[slot] = true;
        officialOutlineLoggedValue_[slot] = target.value;
        officialOutlineLoggedAt_[slot] = now;
        Log(std::string("[VR][outline] OUTLINE_OFFICIAL_CAPTURED role=") +
            (eye ? "eye" : "source") +
            " camera=" + ClassifyCamera(camera) +
            " fov=" + std::to_string(fov) +
            " physical=" + (physical ? "1" : "0") +
            " curveInput=" + std::to_string(curveInput) +
            " value=" + std::to_string(target.value[0]) + "," +
            std::to_string(target.value[1]) + "," +
            std::to_string(target.value[2]) + "," +
            std::to_string(target.value[3]));
    }
}

void UnityStereoRenderer::DumpActorOutlineMaterials() noexcept {
    if (outlineMaterialDumpLogged_ || actorOutlineMaterials_.empty()) {
        return;
    }
    outlineMaterialDumpLogged_ = true;
    EnsureOutlineMaterialApi();
    const std::size_t dumpCount =
        std::min<std::size_t>(actorOutlineMaterials_.size(), 6U);
    for (std::size_t index = 0; index < dumpCount; ++index) {
        void* material = actorOutlineMaterials_[index].material;
        if (material == nullptr || !IsUnityManagedObjectAlive(material)) {
            continue;
        }
        void* shader = nullptr;
        if (api_.materialGetShader.Ready()) {
            (void)InvokeManagedResult<void*, GetPtrFn>(
                api_.materialGetShader, &shader, material);
        }
        int hasOutlineParam = -1;
        if (api_.materialHasProperty.Ready() && outlineParamId_ != 0) {
            bool has = false;
            if (InvokeManagedResult<bool, HasPropertyFn>(
                    api_.materialHasProperty, &has, material,
                    outlineParamId_)) {
                hasOutlineParam = has ? 1 : 0;
            }
        }
        UnityVector4 local{};
        int gotLocal = 0;
        if (MaterialGetVector(
                api_.materialGetVectorInjected, materialGetVectorUsesOutParam_,
                material, outlineParamId_, &local)) {
            gotLocal = 1;
        }
        int forwardPass = -1;
        if (api_.materialFindPass.Ready() &&
            outlineFindPassForwardName_ != nullptr) {
            (void)InvokeManagedResult<int, FindPassFn>(
                api_.materialFindPass, &forwardPass, material,
                outlineFindPassForwardName_);
        }
        std::string outlineProps;
        int propertyCount = 0;
        if (shader != nullptr && api_.shaderGetPropertyCount.Ready() &&
            api_.shaderGetPropertyName.Ready() &&
            InvokeManagedResult<int, GetIntFn>(
                api_.shaderGetPropertyCount, &propertyCount, shader)) {
            const int limit = std::min(propertyCount, 64);
            for (int property = 0; property < limit; ++property) {
                void* nameObject = nullptr;
                if (!InvokeManagedResult<void*, GetPropertyNameFn>(
                        api_.shaderGetPropertyName, &nameObject, shader,
                        property) ||
                    nameObject == nullptr) {
                    continue;
                }
                std::string name;
                try {
                    name = reinterpret_cast<UnityResolve::UnityType::String*>(
                               nameObject)
                               ->ToString();
                } catch (...) {
                    continue;
                }
                if (name.find("utline") == std::string::npos &&
                    name.find("Outline") == std::string::npos) {
                    continue;
                }
                if (!outlineProps.empty()) {
                    outlineProps += ",";
                }
                outlineProps += name;
            }
        }
        Log(std::string("[VR][stereo] EYE_OUTLINE_MAT shader=") +
            ManagedObjectName(shader) +
            " hasOutlineParam=" + std::to_string(hasOutlineParam) +
            " gotLocal=" + std::to_string(gotLocal) +
            " local=" + std::to_string(local.x) + "," +
            std::to_string(local.y) + "," + std::to_string(local.z) + "," +
            std::to_string(local.w) +
            " findPass=" + std::to_string(forwardPass) +
            " propCount=" + std::to_string(propertyCount) +
            " outlineProps=" +
            (outlineProps.empty() ? "-" : outlineProps));
    }
}

void UnityStereoRenderer::WriteOutlineMaterialsForEyes() noexcept {
    if (!SceneReadyAllowsStereoRender()) {
        return;
    }
    EnsureOutlineMaterialApi();
    if (outlineParamId_ == 0 || !api_.materialSetVectorInjected.HasInfo()) {
        return;
    }
    bool materialDied = false;
    for (const AuthoredOutlineMaterial& entry : actorOutlineMaterials_) {
        if (entry.material != nullptr &&
            !IsUnityManagedObjectAlive(entry.material)) {
            materialDied = true;
            break;
        }
    }
    if (materialDied) {
        DropOutlineMaterialCache("material-died");
        return;
    }
    if (actorOutlineMaterials_.empty()) {
        return;
    }
    DumpActorOutlineMaterials();
    GakumasLocal::Config::ClampVrEyeAaSettings();
    const float screenScale = GakumasLocal::Config::vrEyeOutlineWidth;
    const float eyeFov = bloomEyeFovDegrees_ > 1.0F
        ? bloomEyeFovDegrees_
        : 100.24F;
    std::uint32_t written = 0;
    void* lastWritten = nullptr;
    UnityVector4 firstAuthored{};
    UnityVector4 firstWritten{};
    for (AuthoredOutlineMaterial& entry : actorOutlineMaterials_) {
        if (entry.material == nullptr ||
            !IsUnityManagedObjectAlive(entry.material)) {
            continue;
        }
        if (!entry.authoredValid) {
            // Do not GetVector here: the first read is 0,0,0,0, and
            // later reads are our own scaled override. .78 recaptured
            // 0.0149 as "authored", so slider 1 wrote the Grip look.
            entry.authoredX = kOfficialOutlineParam.x;
            entry.authoredY = kOfficialOutlineParam.y;
            entry.authoredZ = kOfficialOutlineParam.z;
            entry.authoredW = kOfficialOutlineParam.w;
            entry.authoredValid = true;
        }
        UnityVector4 authored{
            entry.authoredX, entry.authoredY, entry.authoredZ,
            entry.authoredW};
        // `.179`: base the eye write on the official vector the game's own
        // writer computes for the eye camera (curve-driven w=3.6435 at
        // fov 100.24 in the `.176` run) instead of the eye-era fixed
        // w=4.987752, which `.176` proved accelerates the shader's distance
        // interpolation (`t=saturate(distance*z*w)`). x/y keep the eyes-only
        // menu scale.
        UnityVector4 scaled = authored;
        if (officialOutline_[1].valid) {
            scaled = UnityVector4{
                officialOutline_[1].value[0], officialOutline_[1].value[1],
                officialOutline_[1].value[2], officialOutline_[1].value[3]};
        }
        ScaleOutlineParamWidth(&scaled, screenScale);
        if (!MaterialSetVector(
                api_.materialSetVectorInjected, entry.material,
                outlineParamId_, scaled)) {
            continue;
        }
        if (written == 0U) {
            firstAuthored = authored;
            firstWritten = scaled;
        }
        lastWritten = entry.material;
        ++written;
    }
    if (lastWritten != nullptr) {
        RecordLifetimeWrite(
            LifetimeWriteCategory::OutlineMaterial, "eye-scale", lastWritten,
            nullptr, static_cast<std::intptr_t>(written));
    }
    outlineEyeScaled_ = written > 0U;
    if (written > 0U) {
        lastOutlineWidthScale_ = screenScale;
    }
    const bool scaleChanged = outlineMaterialLogged_ &&
        std::fabs(loggedOutlineWidthScale_ - screenScale) > 0.0005F;
    const bool baseChanged = outlineMaterialLogged_ &&
        std::fabs(outlineEyeLoggedW_ - firstWritten.w) > 0.01F;
    if (written > 0U &&
        (!outlineMaterialLogged_ || scaleChanged || baseChanged)) {
        outlineMaterialLogged_ = true;
        loggedOutlineWidthScale_ = screenScale;
        outlineEyeLoggedW_ = firstWritten.w;
        Log(std::string("[VR][stereo] EYE_OUTLINE_MAT_SCALED written=") +
            std::to_string(written) +
            " total=" + std::to_string(actorOutlineMaterials_.size()) +
            " official=" + (officialOutline_[1].valid ? "1" : "0") +
            " modeFov=" + std::to_string(kModeBodyVerticalFovDegrees) +
            " eyeFov=" + std::to_string(eyeFov) +
            " screenScale=" + std::to_string(screenScale) +
            " before=" + std::to_string(firstAuthored.x) + "," +
            std::to_string(firstAuthored.y) + "," +
            std::to_string(firstAuthored.z) + "," +
            std::to_string(firstAuthored.w) +
            " after=" + std::to_string(firstWritten.x) + "," +
            std::to_string(firstWritten.y) + "," +
            std::to_string(firstWritten.z) + "," +
            std::to_string(firstWritten.w));
    }
}

void UnityStereoRenderer::WriteOutlineMaterialsForGrip() noexcept {
    if (!outlineEyeScaled_ || outlineParamId_ == 0 ||
        !api_.materialSetVectorInjected.HasInfo()) {
        return;
    }
    // Shared materials keep a local _OutlineParam after the eye write, and
    // this Unity build has no Material.RemoveProperty, so the local override
    // cannot be dropped — it must be restored to the value the next actor
    // camera officially expects. `.176` proved the earlier synthetic restore
    // (x/y scaled by a tangent ratio, w left at the eye-era fixed 4.987752)
    // is exactly the Grip thick-outline root cause: official w at Home is
    // 1.5384, so the fixed w made common actor distances read 1.11-1.44x
    // wider. `.179`: put back the complete official vector captured for the
    // latest non-eye actor camera at the CampusActorParameterPass.Execute
    // boundary (ranges + live curve). Legacy synthetic scaling stays only as
    // a fallback while no official capture exists yet.
    const bool official = officialOutline_[0].valid;
    float gripScale = 1.0F;
    float liveFov = sourceLiveFovDegrees_;
    UnityVector4 grip = kOfficialOutlineParam;
    if (official) {
        grip = UnityVector4{
            officialOutline_[0].value[0], officialOutline_[0].value[1],
            officialOutline_[0].value[2], officialOutline_[0].value[3]};
        liveFov = officialOutline_[0].fovDegrees;
    } else {
        const float eyeFov = bloomEyeFovDegrees_ > 1.0F
            ? bloomEyeFovDegrees_
            : 100.24F;
        gripScale = ModeBodyScreenScale(eyeFov);
        if (std::isfinite(liveFov) && liveFov > 1.0F && liveFov < 170.0F) {
            constexpr float kDegreesToRadians =
                3.14159265358979323846F / 180.0F;
            const float modeTan = std::tan(
                kModeBodyVerticalFovDegrees * 0.5F * kDegreesToRadians);
            const float liveTan =
                std::tan(liveFov * 0.5F * kDegreesToRadians);
            if (modeTan > 0.0F && std::isfinite(liveTan) && liveTan > 0.0F) {
                const float follow =
                    std::min(2.0F, std::max(0.1F, liveTan / modeTan));
                gripScale = std::min(1.0F, gripScale * follow);
            }
        }
        ScaleOutlineParamWidth(&grip, gripScale);
    }
    std::uint32_t written = 0;
    void* lastWritten = nullptr;
    for (const AuthoredOutlineMaterial& entry : actorOutlineMaterials_) {
        if (entry.material == nullptr ||
            !IsUnityManagedObjectAlive(entry.material)) {
            continue;
        }
        if (!MaterialSetVector(
                api_.materialSetVectorInjected, entry.material, outlineParamId_,
                grip)) {
            continue;
        }
        lastWritten = entry.material;
        ++written;
    }
    if (lastWritten != nullptr) {
        RecordLifetimeWrite(
            LifetimeWriteCategory::OutlineMaterial, "grip-restore", lastWritten,
            nullptr, static_cast<std::intptr_t>(written));
    }
    outlineEyeScaled_ = false;
    const auto now = std::chrono::steady_clock::now();
    // In the official path the vector varies with the live shot; gate the
    // log on the curve output (w) instead of the legacy synthetic scale.
    const float logKey = official ? grip.w : gripScale;
    const bool scaleMoved =
        std::fabs(logKey - outlineGripLoggedScale_) > 0.05F;
    if (written > 0U &&
        (!outlineGripRestoreLogged_ ||
         (scaleMoved && now - outlineGripLogAt_ > std::chrono::seconds(1)))) {
        outlineGripRestoreLogged_ = true;
        outlineGripLoggedScale_ = logKey;
        outlineGripLogAt_ = now;
        Log(std::string("[VR][stereo] GRIP_OUTLINE_MAT_RESTORED written=") +
            std::to_string(written) +
            " official=" + (official ? "1" : "0") +
            " gripScale=" + std::to_string(gripScale) +
            " liveFov=" + std::to_string(liveFov) +
            " value=" + std::to_string(grip.x) + "," +
            std::to_string(grip.y) + "," +
            std::to_string(grip.z) + "," +
            std::to_string(grip.w));
    }
}

void UnityStereoRenderer::RestoreOutlineMaterials() noexcept {
    // Teardown must not SetVector. Contest / Idol-path entry keeps
    // Game3DManager + portrait eligibility while actor materials die;
    // the .58 ProFlare lesson applies: drop the cache, do not write.
    DropOutlineMaterialCache("restore");
}

namespace {

bool ReadInstanceFloatSeh(
    void* instance, std::int32_t offset, float* out) noexcept {
    if (instance == nullptr || offset < 0 || out == nullptr) {
        return false;
    }
    __try {
        *out = *reinterpret_cast<float*>(
            reinterpret_cast<std::uintptr_t>(instance) +
            static_cast<std::uintptr_t>(offset));
        return std::isfinite(*out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadInstancePointerSeh(
    void* instance,
    std::int32_t offset,
    void** value) noexcept {
    if (instance == nullptr || offset < 0 || value == nullptr) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(instance) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadInstanceBoolSeh(
    void* instance,
    std::int32_t offset,
    bool* value) noexcept {
    if (instance == nullptr || offset < 0 || value == nullptr) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<bool*>(
            reinterpret_cast<std::uint8_t*>(instance) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadInstanceIntSeh(
    void* instance,
    std::int32_t offset,
    int* value) noexcept {
    if (instance == nullptr || offset < 0 || value == nullptr) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<int*>(
            reinterpret_cast<std::uint8_t*>(instance) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadInstanceVector4Seh(
    void* instance,
    std::int32_t offset,
    UnityVector4* value) noexcept {
    if (instance == nullptr || offset < 0 || value == nullptr) {
        return false;
    }
    __try {
        *value = *reinterpret_cast<UnityVector4*>(
            reinterpret_cast<std::uint8_t*>(instance) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteInstanceFloatSeh(
    void* instance, std::int32_t offset, float value) noexcept {
    if (instance == nullptr || offset < 0 || !std::isfinite(value)) {
        return false;
    }
    __try {
        *reinterpret_cast<float*>(
            reinterpret_cast<std::uintptr_t>(instance) +
            static_cast<std::uintptr_t>(offset)) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void UnityStereoRenderer::DiscoverUiTextureOverlays() noexcept {
    auto* klass = reinterpret_cast<UnityResolve::Class*>(
        api_.uiTextureOverlayClass);
    if (klass == nullptr || !api_.componentGetGameObject.Ready() ||
        SceneContentUnstable()) {
        return;
    }
    EnsureOutlineMaterialApi();
    if (uiTextureOverlayColorId_ == 0) {
        uiTextureOverlayColorId_ = ResolveShaderPropertyId("_OverlayColor");
    }
    uiTextureOverlays_.erase(
        std::remove_if(
            uiTextureOverlays_.begin(), uiTextureOverlays_.end(),
            [](const UiTextureOverlayEntry& entry) {
                return !IsUnityManagedObjectAlive(entry.component) ||
                    !IsUnityManagedObjectAlive(entry.gameObject);
            }),
        uiTextureOverlays_.end());
    const auto now = std::chrono::steady_clock::now();
    const bool empty = uiTextureOverlays_.empty();
    if (empty && now < uiTextureOverlayNextDiscoverAt_) {
        return;
    }
    constexpr std::uint64_t kRediscoverEvery = 120U;
    const bool rediscover =
        empty || (uiTextureOverlayDiscoverSerial_ % kRediscoverEvery) == 0U;
    ++uiTextureOverlayDiscoverSerial_;
    if (!rediscover) {
        return;
    }
    if (empty) {
        uiTextureOverlayNextDiscoverAt_ = now + std::chrono::seconds(8);
    }
    const auto objects = FindManagedObjectsOfType(klass);
    using GetObject = void* (*)(void*, void*);
    using GetBool = bool (*)(void*, void*);
    using GetInt = int (*)(void*, void*);
    std::uint32_t added = 0;
    for (void* component : objects) {
        if (!IsUnityManagedObjectAlive(component) ||
            uiTextureOverlays_.size() >= 128U) {
            continue;
        }
        const bool known = std::any_of(
            uiTextureOverlays_.begin(), uiTextureOverlays_.end(),
            [component](const UiTextureOverlayEntry& entry) {
                return entry.component == component;
            });
        if (known) {
            continue;
        }
        void* gameObject = nullptr;
        if (!InvokeManagedResult<void*, GetObject>(
                api_.componentGetGameObject, &gameObject, component) ||
            !IsUnityManagedObjectAlive(gameObject)) {
            continue;
        }
        uiTextureOverlays_.push_back({component, gameObject});
        ++added;

        bool enabled = false;
        int layer = -1;
        if (api_.behaviourGetEnabled.Ready()) {
            (void)InvokeManagedResult<bool, GetBool>(
                api_.behaviourGetEnabled, &enabled, component);
        }
        if (api_.gameObjectGetLayer.Ready()) {
            (void)InvokeManagedResult<int, GetInt>(
                api_.gameObjectGetLayer, &layer, gameObject);
        }
        void* declaredShader = nullptr;
        void* overlayTexture = nullptr;
        void* overlaySprite = nullptr;
        void* material = nullptr;
        (void)ReadInstancePointerSeh(
            component, api_.uiTextureOverlayShaderOffset, &declaredShader);
        (void)ReadInstancePointerSeh(
            component, api_.uiTextureOverlayTextureOffset, &overlayTexture);
        (void)ReadInstancePointerSeh(
            component, api_.uiTextureOverlaySpriteOffset, &overlaySprite);
        (void)ReadInstancePointerSeh(
            component, api_.uiTextureOverlayMaterialOffset, &material);
        bool clampUv = false;
        bool alphaMask = false;
        int mode = -1;
        (void)ReadInstanceBoolSeh(
            component, api_.uiTextureOverlayClampUvOffset, &clampUv);
        (void)ReadInstanceBoolSeh(
            component, api_.uiTextureOverlayAlphaMaskOffset, &alphaMask);
        (void)ReadInstanceIntSeh(
            component, api_.uiTextureOverlayModeOffset, &mode);
        UnityVector4 fieldColor{};
        const bool fieldColorOk = ReadInstanceVector4Seh(
            component, api_.uiTextureOverlayColorOffset, &fieldColor);
        void* materialShader = nullptr;
        if (IsUnityManagedObjectAlive(material) &&
            api_.materialGetShader.Ready()) {
            (void)InvokeManagedResult<void*, GetPtrFn>(
                api_.materialGetShader, &materialShader, material);
        }
        UnityVector4 materialColor{};
        const bool materialColorOk = IsUnityManagedObjectAlive(material) &&
            MaterialGetVector(
                api_.materialGetVectorInjected,
                materialGetVectorUsesOutParam_, material,
                uiTextureOverlayColorId_, &materialColor);

        std::string hierarchy = ManagedObjectName(gameObject);
        if (api_.componentGetTransform.Ready() &&
            api_.transformGetParent.Ready()) {
            void* transform = nullptr;
            if (InvokeManagedResult<void*, GetObject>(
                    api_.componentGetTransform, &transform, component)) {
                for (std::uint32_t depth = 0;
                     depth < 8U && IsUnityManagedObjectAlive(transform);
                     ++depth) {
                    void* parent = nullptr;
                    if (!InvokeManagedResult<void*, GetObject>(
                            api_.transformGetParent, &parent, transform) ||
                        !IsUnityManagedObjectAlive(parent)) {
                        break;
                    }
                    void* parentObject = nullptr;
                    if (InvokeManagedResult<void*, GetObject>(
                            api_.componentGetGameObject, &parentObject,
                            parent) &&
                        IsUnityManagedObjectAlive(parentObject)) {
                        hierarchy = ManagedObjectName(parentObject) + "/" +
                            hierarchy;
                    }
                    transform = parent;
                }
            }
        }
        std::ostringstream entry;
        entry << "[VR][stereo] UI_TEXTURE_OVERLAY_FOUND component="
              << component << " gameObject=" << gameObject
              << " name=\"" << ManagedObjectName(gameObject) << "\""
              << " path=\"" << hierarchy << "\" enabled="
              << (enabled ? 1 : 0) << " layer=" << layer
              << " texture=\"" << ManagedObjectName(overlayTexture) << "\""
              << " sprite=\"" << ManagedObjectName(overlaySprite) << "\""
              << " declaredShader=\"" << ManagedObjectName(declaredShader)
              << "\" material=\"" << ManagedObjectName(material)
              << "\" materialShader=\"" << ManagedObjectName(materialShader)
              << "\" mode=" << mode << " alphaMask=" << (alphaMask ? 1 : 0)
              << " clampUv=" << (clampUv ? 1 : 0) << " fieldColor=";
        if (fieldColorOk) {
            entry << fieldColor.x << ',' << fieldColor.y << ','
                  << fieldColor.z << ',' << fieldColor.w;
        } else {
            entry << '-';
        }
        entry << " materialColor=";
        if (materialColorOk) {
            entry << materialColor.x << ',' << materialColor.y << ','
                  << materialColor.z << ',' << materialColor.w;
        } else {
            entry << '-';
        }
        Log(entry.str());
    }
    if (!uiTextureOverlayDiscoverLogged_ || added > 0U) {
        uiTextureOverlayDiscoverLogged_ = true;
        Log("[VR][stereo] UI_TEXTURE_OVERLAY_DISCOVER found=" +
            std::to_string(objects.size()) + " added=" +
            std::to_string(added) + " cached=" +
            std::to_string(uiTextureOverlays_.size()) + " colorId=" +
            std::to_string(uiTextureOverlayColorId_));
    }
}

void UnityStereoRenderer::HideUiTextureOverlaysForEyes() noexcept {
    if (!GakumasLocal::Config::vrHideUiTextureOverlay) {
        RestoreUiTextureOverlays("toggle-off");
        return;
    }
    if (uiTextureOverlaysHidden_) {
        return;
    }
    EnsureOutlineMaterialApi();
    if (uiTextureOverlayColorId_ == 0) {
        uiTextureOverlayColorId_ = ResolveShaderPropertyId("_OverlayColor");
    }
    if (uiTextureOverlayColorId_ == 0 ||
        !api_.materialHasProperty.HasInfo() ||
        !api_.materialGetVectorInjected.HasInfo() ||
        !api_.materialSetVectorInjected.HasInfo()) {
        if (!uiTextureOverlaySkipLogged_) {
            uiTextureOverlaySkipLogged_ = true;
            Log("[VR][stereo] UI_TEXTURE_OVERLAY_HIDE_SKIPPED reason=material-api");
        }
        return;
    }
    std::vector<void*> writtenMaterials;
    std::uint32_t hidden = 0;
    std::uint32_t dead = 0;
    std::uint32_t noMaterial = 0;
    std::uint32_t noProperty = 0;
    UnityVector4 firstAuthored{};
    for (UiTextureOverlayEntry& entry : uiTextureOverlays_) {
        entry.hiddenByUs = false;
        entry.hiddenMaterial = nullptr;
        if (!IsUnityManagedObjectAlive(entry.component) ||
            !IsUnityManagedObjectAlive(entry.gameObject)) {
            ++dead;
            continue;
        }
        void* material = nullptr;
        if (!ReadInstancePointerSeh(
                entry.component, api_.uiTextureOverlayMaterialOffset,
                &material) ||
            !IsUnityManagedObjectAlive(material)) {
            ++noMaterial;
            continue;
        }
        if (std::find(
                writtenMaterials.begin(), writtenMaterials.end(), material) !=
            writtenMaterials.end()) {
            continue;
        }
        bool hasColor = false;
        if (!InvokeManagedResult<bool, HasPropertyFn>(
                api_.materialHasProperty, &hasColor, material,
                uiTextureOverlayColorId_) ||
            !hasColor) {
            ++noProperty;
            continue;
        }
        UnityVector4 authored{};
        if (!MaterialGetVector(
                api_.materialGetVectorInjected,
                materialGetVectorUsesOutParam_, material,
                uiTextureOverlayColorId_, &authored)) {
            ++noProperty;
            continue;
        }
        UnityVector4 transparent = authored;
        transparent.w = 0.0F;
        if (!MaterialSetVector(
                api_.materialSetVectorInjected, material,
                uiTextureOverlayColorId_, transparent)) {
            continue;
        }
        entry.hiddenMaterial = material;
        entry.authoredColorX = authored.x;
        entry.authoredColorY = authored.y;
        entry.authoredColorZ = authored.z;
        entry.authoredColorW = authored.w;
        entry.hiddenByUs = true;
        writtenMaterials.push_back(material);
        if (hidden == 0U) {
            firstAuthored = authored;
        }
        ++hidden;
    }
    uiTextureOverlaysHidden_ = hidden > 0U;
    if (!writtenMaterials.empty()) {
        RecordLifetimeWrite(
            LifetimeWriteCategory::UiTextureOverlay, "hide-material",
            writtenMaterials.back(), nullptr, static_cast<std::intptr_t>(hidden));
    }
    if (hidden > 0U && !uiTextureOverlayHideLogged_) {
        uiTextureOverlayHideLogged_ = true;
        Log("[VR][stereo] UI_TEXTURE_OVERLAY_HIDDEN count=" +
            std::to_string(hidden) + " cached=" +
            std::to_string(uiTextureOverlays_.size()) + " dead=" +
            std::to_string(dead) + " noMaterial=" +
            std::to_string(noMaterial) + " noProperty=" +
            std::to_string(noProperty) + " firstBefore=" +
            std::to_string(firstAuthored.x) + "," +
            std::to_string(firstAuthored.y) + "," +
            std::to_string(firstAuthored.z) + "," +
            std::to_string(firstAuthored.w) + " afterAlpha=0 policy=eyes-only");
    } else if (hidden == 0U && !uiTextureOverlaySkipLogged_) {
        uiTextureOverlaySkipLogged_ = true;
        Log("[VR][stereo] UI_TEXTURE_OVERLAY_HIDE_SKIPPED reason=no-writable-material cached=" +
            std::to_string(uiTextureOverlays_.size()) + " dead=" +
            std::to_string(dead) + " noMaterial=" +
            std::to_string(noMaterial) + " noProperty=" +
            std::to_string(noProperty));
    }
}

void UnityStereoRenderer::RestoreUiTextureOverlays(
    const char* reason) noexcept {
    if (!uiTextureOverlaysHidden_) {
        return;
    }
    std::uint32_t restored = 0;
    std::uint32_t dead = 0;
    void* lastRestored = nullptr;
    for (UiTextureOverlayEntry& entry : uiTextureOverlays_) {
        if (!entry.hiddenByUs) {
            continue;
        }
        entry.hiddenByUs = false;
        void* material = entry.hiddenMaterial;
        entry.hiddenMaterial = nullptr;
        if (!IsUnityManagedObjectAlive(material)) {
            ++dead;
            continue;
        }
        const UnityVector4 authored{
            entry.authoredColorX, entry.authoredColorY,
            entry.authoredColorZ, entry.authoredColorW};
        if (MaterialSetVector(
                api_.materialSetVectorInjected, material,
                uiTextureOverlayColorId_, authored)) {
            lastRestored = material;
            ++restored;
        }
    }
    if (lastRestored != nullptr) {
        RecordLifetimeWrite(
            LifetimeWriteCategory::UiTextureOverlay, "restore-material",
            lastRestored, nullptr, static_cast<std::intptr_t>(restored));
    }
    uiTextureOverlaysHidden_ = false;
    if (!uiTextureOverlayRestoreLogged_) {
        uiTextureOverlayRestoreLogged_ = true;
        Log("[VR][stereo] UI_TEXTURE_OVERLAY_RESTORED count=" +
            std::to_string(restored) + " dead=" + std::to_string(dead) +
            " reason=" + (reason != nullptr ? reason : "unknown"));
    }
}

void UnityStereoRenderer::DropUiTextureOverlayCache(
    const char* reason) noexcept {
    RestoreUiTextureOverlays(reason);
    const std::size_t cached = uiTextureOverlays_.size();
    uiTextureOverlays_.clear();
    uiTextureOverlayDiscoverSerial_ = 0;
    uiTextureOverlayNextDiscoverAt_ = {};
    uiTextureOverlayDiscoverLogged_ = false;
    uiTextureOverlayHideLogged_ = false;
    uiTextureOverlaySkipLogged_ = false;
    uiTextureOverlayRestoreLogged_ = false;
    if (cached > 0U) {
        Log("[VR][stereo] UI_TEXTURE_OVERLAY_CACHE_DROPPED count=" +
            std::to_string(cached) + " reason=" +
            (reason != nullptr ? reason : "unknown"));
    }
}

void UnityStereoRenderer::DiscoverLiveCameraOverlays() noexcept {
    auto* klass = reinterpret_cast<UnityResolve::Class*>(
        api_.liveCameraOverlayClass);
    if (klass == nullptr || !api_.componentGetGameObject.Ready() ||
        SceneContentUnstable()) {
        return;
    }
    liveCameraOverlays_.erase(
        std::remove_if(
            liveCameraOverlays_.begin(), liveCameraOverlays_.end(),
            [](const LiveCameraOverlayEntry& entry) {
                return !IsUnityManagedObjectAlive(entry.component) ||
                    !IsUnityManagedObjectAlive(entry.gameObject);
            }),
        liveCameraOverlays_.end());
    const auto now = std::chrono::steady_clock::now();
    const bool empty = liveCameraOverlays_.empty();
    if (empty && now < liveCameraOverlayNextDiscoverAt_) {
        return;
    }
    constexpr std::uint64_t kRediscoverEvery = 120U;
    const bool rediscover =
        empty || (liveCameraOverlayDiscoverSerial_ % kRediscoverEvery) == 0U;
    ++liveCameraOverlayDiscoverSerial_;
    if (!rediscover) {
        return;
    }
    // .140 used an 8 s empty cooldown and missed the Live intro after a
    // lobby found=0. Rediscover empty caches every 250 ms while stereo
    // is eligible so a late-spawned near-plane card is visible in-log
    // before the rectangle finishes growing.
    if (empty) {
        liveCameraOverlayNextDiscoverAt_ =
            now + std::chrono::milliseconds(250);
    }
    const auto objects = FindManagedObjectsOfType(klass);
    using GetObject = void* (*)(void*, void*);
    using GetBool = bool (*)(void*, void*);
    using GetInt = int (*)(void*, void*);
    std::uint32_t added = 0;
    for (void* component : objects) {
        if (!IsUnityManagedObjectAlive(component) ||
            liveCameraOverlays_.size() >= 64U) {
            continue;
        }
        const bool known = std::any_of(
            liveCameraOverlays_.begin(), liveCameraOverlays_.end(),
            [component](const LiveCameraOverlayEntry& entry) {
                return entry.component == component;
            });
        if (known) {
            continue;
        }
        void* gameObject = nullptr;
        if (!InvokeManagedResult<void*, GetObject>(
                api_.componentGetGameObject, &gameObject, component) ||
            !IsUnityManagedObjectAlive(gameObject)) {
            continue;
        }
        liveCameraOverlays_.push_back({component, gameObject, false});
        ++added;

        bool activeSelf = false;
        int layer = -1;
        if (api_.gameObjectGetActiveSelf.Ready()) {
            (void)InvokeManagedResult<bool, GetBool>(
                api_.gameObjectGetActiveSelf, &activeSelf, gameObject);
        }
        if (api_.gameObjectGetLayer.Ready()) {
            (void)InvokeManagedResult<int, GetInt>(
                api_.gameObjectGetLayer, &layer, gameObject);
        }
        float offset = 0.0F;
        float scaler = 0.0F;
        const bool offsetOk = ReadInstanceFloatSeh(
            component, api_.liveCameraOverlayOffsetFieldOffset, &offset);
        const bool scalerOk = ReadInstanceFloatSeh(
            component, api_.liveCameraOverlayScalerFieldOffset, &scaler);

        std::string hierarchy = ManagedObjectName(gameObject);
        if (api_.componentGetTransform.Ready() &&
            api_.transformGetParent.Ready()) {
            void* transform = nullptr;
            if (InvokeManagedResult<void*, GetObject>(
                    api_.componentGetTransform, &transform, component)) {
                for (std::uint32_t depth = 0;
                     depth < 6U && IsUnityManagedObjectAlive(transform);
                     ++depth) {
                    void* parent = nullptr;
                    if (!InvokeManagedResult<void*, GetObject>(
                            api_.transformGetParent, &parent, transform) ||
                        !IsUnityManagedObjectAlive(parent)) {
                        break;
                    }
                    void* parentObject = nullptr;
                    if (InvokeManagedResult<void*, GetObject>(
                            api_.componentGetGameObject, &parentObject,
                            parent) &&
                        IsUnityManagedObjectAlive(parentObject)) {
                        hierarchy = ManagedObjectName(parentObject) + "/" +
                            hierarchy;
                    }
                    transform = parent;
                }
            }
        }
        std::ostringstream entry;
        entry << "[VR][stereo] LIVE_CAMERA_OVERLAY_FOUND component="
              << component << " gameObject=" << gameObject
              << " name=\"" << ManagedObjectName(gameObject) << "\""
              << " path=\"" << hierarchy << "\" activeSelf="
              << (activeSelf ? 1 : 0) << " layer=" << layer
              << " offset=";
        if (offsetOk) {
            entry << offset;
        } else {
            entry << '-';
        }
        entry << " scaler=";
        if (scalerOk) {
            entry << scaler;
        } else {
            entry << '-';
        }
        Log(entry.str());
    }
    if (added > 0U || !liveCameraOverlayDiscoverLogged_ ||
        (empty && now >= liveCameraOverlayNextEmptyLogAt_)) {
        liveCameraOverlayDiscoverLogged_ = true;
        if (empty) {
            liveCameraOverlayNextEmptyLogAt_ = now + std::chrono::seconds(1);
        }
        Log("[VR][stereo] LIVE_CAMERA_OVERLAY_DISCOVER found=" +
            std::to_string(objects.size()) + " added=" +
            std::to_string(added) + " cached=" +
            std::to_string(liveCameraOverlays_.size()));
    }
}

void UnityStereoRenderer::HideLiveCameraOverlaysForEyes() noexcept {
    if (!GakumasLocal::Config::vrHideUiTextureOverlay) {
        RestoreLiveCameraOverlays("toggle-off");
        return;
    }
    if (!api_.gameObjectSetActive.Ready() ||
        !api_.gameObjectGetActiveSelf.Ready()) {
        return;
    }
    using GetBool = bool (*)(void*, void*);
    using GetObject = void* (*)(void*, void*);
    using SetActive = void (*)(void*, bool, void*);
    std::uint32_t hidden = 0;
    std::uint32_t dead = 0;
    std::uint32_t cameraObjectSkips = 0;
    void* lastChanged = nullptr;
    void* sourceGameObject = nullptr;
    if (IsUnityManagedObjectAlive(latestSourceCamera_) &&
        api_.componentGetGameObject.Ready()) {
        (void)InvokeManagedResult<void*, GetObject>(
            api_.componentGetGameObject, &sourceGameObject,
            latestSourceCamera_);
    }
    for (LiveCameraOverlayEntry& entry : liveCameraOverlays_) {
        if (!IsUnityManagedObjectAlive(entry.component) ||
            !IsUnityManagedObjectAlive(entry.gameObject)) {
            entry.hiddenByUs = false;
            ++dead;
            continue;
        }
        // Never deactivate a camera GameObject. If the component lives on
        // the source/eye itself, the census has found the wrong suppression
        // boundary and the log must prove that before another mechanism is
        // attempted. Deactivating it here would starve Tick/eye callbacks.
        if (entry.gameObject == sourceGameObject ||
            entry.gameObject == eyeGameObjects_[0] ||
            entry.gameObject == eyeGameObjects_[1]) {
            ++cameraObjectSkips;
            continue;
        }
        if (entry.hiddenByUs) {
            continue;
        }
        bool activeSelf = false;
        if (!InvokeManagedResult<bool, GetBool>(
                api_.gameObjectGetActiveSelf, &activeSelf,
                entry.gameObject) ||
            !activeSelf) {
            continue;
        }
        if (InvokeManagedVoid<SetActive>(
                api_.gameObjectSetActive, entry.gameObject, false)) {
            entry.hiddenByUs = true;
            lastChanged = entry.gameObject;
            ++hidden;
        }
    }
    if (lastChanged != nullptr) {
        RecordLifetimeWrite(
            LifetimeWriteCategory::LiveCameraOverlay, "set-inactive",
            lastChanged, nullptr, static_cast<std::intptr_t>(hidden));
    }
    if (hidden > 0U) {
        liveCameraOverlaysHidden_ = true;
    }
    if (hidden > 0U && !liveCameraOverlayHideLogged_) {
        liveCameraOverlayHideLogged_ = true;
        Log("[VR][stereo] LIVE_CAMERA_OVERLAY_HIDDEN count=" +
            std::to_string(hidden) + " dead=" + std::to_string(dead) +
            " cameraObjectSkips=" + std::to_string(cameraObjectSkips) +
            " policy=eyes-only");
    } else if (cameraObjectSkips > 0U && !liveCameraOverlayHideLogged_) {
        liveCameraOverlayHideLogged_ = true;
        Log("[VR][stereo] LIVE_CAMERA_OVERLAY_HIDE_SKIPPED reason=camera-object count=" +
            std::to_string(cameraObjectSkips));
    }
}

void UnityStereoRenderer::RestoreLiveCameraOverlays(
    const char* reason) noexcept {
    if (!liveCameraOverlaysHidden_) {
        return;
    }
    using SetActive = void (*)(void*, bool, void*);
    std::uint32_t restored = 0;
    std::uint32_t dead = 0;
    void* lastChanged = nullptr;
    for (LiveCameraOverlayEntry& entry : liveCameraOverlays_) {
        if (!entry.hiddenByUs) {
            continue;
        }
        entry.hiddenByUs = false;
        if (!IsUnityManagedObjectAlive(entry.component) ||
            !IsUnityManagedObjectAlive(entry.gameObject)) {
            ++dead;
            continue;
        }
        if (InvokeManagedVoid<SetActive>(
                api_.gameObjectSetActive, entry.gameObject, true)) {
            lastChanged = entry.gameObject;
            ++restored;
        }
    }
    if (lastChanged != nullptr) {
        RecordLifetimeWrite(
            LifetimeWriteCategory::LiveCameraOverlay, "set-active",
            lastChanged, nullptr, static_cast<std::intptr_t>(restored));
    }
    liveCameraOverlaysHidden_ = false;
    if (!liveCameraOverlayRestoreLogged_) {
        liveCameraOverlayRestoreLogged_ = true;
        Log("[VR][stereo] LIVE_CAMERA_OVERLAY_RESTORED count=" +
            std::to_string(restored) + " dead=" + std::to_string(dead) +
            " reason=" + (reason != nullptr ? reason : "unknown"));
    }
}

void UnityStereoRenderer::DropLiveCameraOverlayCache(
    const char* reason) noexcept {
    RestoreLiveCameraOverlays(reason);
    const std::size_t cached = liveCameraOverlays_.size();
    liveCameraOverlays_.clear();
    liveCameraOverlayDiscoverSerial_ = 0;
    liveCameraOverlayNextDiscoverAt_ = {};
    liveCameraOverlayNextEmptyLogAt_ = {};
    liveCameraOverlayDiscoverLogged_ = false;
    liveCameraOverlayHideLogged_ = false;
    liveCameraOverlayRestoreLogged_ = false;
    if (cached > 0U) {
        Log("[VR][stereo] LIVE_CAMERA_OVERLAY_CACHE_DROPPED count=" +
            std::to_string(cached) + " reason=" +
            (reason != nullptr ? reason : "unknown"));
    }
}

void UnityStereoRenderer::DiscoverCmovParticles() noexcept {
    auto* klass = reinterpret_cast<UnityResolve::Class*>(
        api_.particleSystemClass);
    if (klass == nullptr || !api_.componentGetGameObject.Ready() ||
        SceneContentUnstable()) {
        return;
    }
    cmovParticles_.erase(
        std::remove_if(
            cmovParticles_.begin(), cmovParticles_.end(),
            [](const CmovParticleEntry& entry) {
                return !IsUnityManagedObjectAlive(entry.component) ||
                    !IsUnityManagedObjectAlive(entry.gameObject);
            }),
        cmovParticles_.end());
    const auto now = std::chrono::steady_clock::now();
    const bool empty = cmovParticles_.empty();
    if (empty && now < cmovParticleNextDiscoverAt_) {
        return;
    }
    constexpr std::uint64_t kRediscoverEvery = 120U;
    const bool rediscover =
        empty || (cmovParticleDiscoverSerial_ % kRediscoverEvery) == 0U;
    ++cmovParticleDiscoverSerial_;
    if (!rediscover) {
        return;
    }
    if (empty) {
        cmovParticleNextDiscoverAt_ = now + std::chrono::milliseconds(250);
    }
    const auto objects = FindManagedObjectsOfType(klass);
    using GetObject = void* (*)(void*, void*);
    using GetBool = bool (*)(void*, void*);
    using GetInt = int (*)(void*, void*);
    const auto buildPath = [this](void* component, void* gameObject) {
        std::string hierarchy = ManagedObjectName(gameObject);
        if (!api_.componentGetTransform.Ready() ||
            !api_.transformGetParent.Ready()) {
            return hierarchy;
        }
        using GetObjectFn = void* (*)(void*, void*);
        void* transform = nullptr;
        if (!InvokeManagedResult<void*, GetObjectFn>(
                api_.componentGetTransform, &transform, component)) {
            return hierarchy;
        }
        for (std::uint32_t depth = 0;
             depth < 6U && IsUnityManagedObjectAlive(transform);
             ++depth) {
            void* parent = nullptr;
            if (!InvokeManagedResult<void*, GetObjectFn>(
                    api_.transformGetParent, &parent, transform) ||
                !IsUnityManagedObjectAlive(parent)) {
                break;
            }
            void* parentObject = nullptr;
            if (InvokeManagedResult<void*, GetObjectFn>(
                    api_.componentGetGameObject, &parentObject, parent) &&
                IsUnityManagedObjectAlive(parentObject)) {
                hierarchy = ManagedObjectName(parentObject) + "/" + hierarchy;
            }
            transform = parent;
        }
        return hierarchy;
    };
    std::uint32_t added = 0;
    std::uint32_t cmovSeen = 0;
    for (void* component : objects) {
        if (!IsUnityManagedObjectAlive(component)) {
            continue;
        }
        void* gameObject = nullptr;
        if (!InvokeManagedResult<void*, GetObject>(
                api_.componentGetGameObject, &gameObject, component) ||
            !IsUnityManagedObjectAlive(gameObject)) {
            continue;
        }
        const std::string name = ManagedObjectName(gameObject);
        const std::string path = buildPath(component, gameObject);
        if (!ContainsCmovToken(name) && !ContainsCmovToken(path)) {
            continue;
        }
        ++cmovSeen;
        if (cmovParticles_.size() >= 128U) {
            continue;
        }
        const bool known = std::any_of(
            cmovParticles_.begin(), cmovParticles_.end(),
            [component](const CmovParticleEntry& entry) {
                return entry.component == component;
            });
        if (known) {
            continue;
        }
        cmovParticles_.push_back({component, gameObject, false});
        ++added;
        bool activeSelf = false;
        int layer = -1;
        if (api_.gameObjectGetActiveSelf.Ready()) {
            (void)InvokeManagedResult<bool, GetBool>(
                api_.gameObjectGetActiveSelf, &activeSelf, gameObject);
        }
        if (api_.gameObjectGetLayer.Ready()) {
            (void)InvokeManagedResult<int, GetInt>(
                api_.gameObjectGetLayer, &layer, gameObject);
        }
        std::ostringstream entry;
        entry << "[VR][stereo] LIVE_CMOV_PS_FOUND component=" << component
              << " gameObject=" << gameObject << " name=\"" << name
              << "\" path=\"" << path << "\" activeSelf="
              << (activeSelf ? 1 : 0) << " layer=" << layer;
        Log(entry.str());
    }
    if (added > 0U || !cmovParticleDiscoverLogged_ ||
        (empty && now >= cmovParticleNextEmptyLogAt_)) {
        cmovParticleDiscoverLogged_ = true;
        if (empty) {
            cmovParticleNextEmptyLogAt_ = now + std::chrono::seconds(1);
        }
        Log("[VR][stereo] LIVE_CMOV_PS_DISCOVER scanned=" +
            std::to_string(objects.size()) + " cmov=" +
            std::to_string(cmovSeen) + " added=" + std::to_string(added) +
            " cached=" + std::to_string(cmovParticles_.size()));
    }
}

void UnityStereoRenderer::HideCmovParticlesForEyes() noexcept {
    if (!GakumasLocal::Config::vrHideUiTextureOverlay) {
        RestoreCmovParticles("toggle-off");
        return;
    }
    if (!api_.gameObjectSetActive.Ready() ||
        !api_.gameObjectGetActiveSelf.Ready()) {
        return;
    }
    using GetBool = bool (*)(void*, void*);
    using GetObject = void* (*)(void*, void*);
    using SetActive = void (*)(void*, bool, void*);
    std::uint32_t hidden = 0;
    std::uint32_t dead = 0;
    std::uint32_t cameraObjectSkips = 0;
    std::uint32_t refought = 0;
    void* lastChanged = nullptr;
    void* sourceGameObject = nullptr;
    if (IsUnityManagedObjectAlive(latestSourceCamera_) &&
        api_.componentGetGameObject.Ready()) {
        (void)InvokeManagedResult<void*, GetObject>(
            api_.componentGetGameObject, &sourceGameObject,
            latestSourceCamera_);
    }
    for (CmovParticleEntry& entry : cmovParticles_) {
        if (!IsUnityManagedObjectAlive(entry.component) ||
            !IsUnityManagedObjectAlive(entry.gameObject)) {
            entry.hiddenByUs = false;
            ++dead;
            continue;
        }
        if (entry.gameObject == sourceGameObject ||
            entry.gameObject == eyeGameObjects_[0] ||
            entry.gameObject == eyeGameObjects_[1]) {
            ++cameraObjectSkips;
            continue;
        }
        bool activeSelf = false;
        if (!InvokeManagedResult<bool, GetBool>(
                api_.gameObjectGetActiveSelf, &activeSelf,
                entry.gameObject) ||
            !activeSelf) {
            continue;
        }
        if (InvokeManagedVoid<SetActive>(
                api_.gameObjectSetActive, entry.gameObject, false)) {
            if (entry.hiddenByUs) {
                ++refought;
            }
            entry.hiddenByUs = true;
            lastChanged = entry.gameObject;
            ++hidden;
        }
    }
    if (lastChanged != nullptr) {
        RecordLifetimeWrite(
            LifetimeWriteCategory::CmovParticle, "set-inactive", lastChanged,
            nullptr, static_cast<std::intptr_t>(hidden));
    }
    if (hidden > 0U) {
        cmovParticlesHidden_ = true;
    }
    if (hidden > 0U && !cmovParticleHideLogged_) {
        cmovParticleHideLogged_ = true;
        Log("[VR][stereo] LIVE_CMOV_PS_HIDDEN count=" +
            std::to_string(hidden) + " dead=" + std::to_string(dead) +
            " refought=" + std::to_string(refought) +
            " cameraObjectSkips=" + std::to_string(cameraObjectSkips) +
            " policy=eyes-only token=cmov");
    } else if (cameraObjectSkips > 0U && !cmovParticleHideLogged_) {
        cmovParticleHideLogged_ = true;
        Log("[VR][stereo] LIVE_CMOV_PS_HIDE_SKIPPED reason=camera-object count=" +
            std::to_string(cameraObjectSkips));
    }
}

void UnityStereoRenderer::RestoreCmovParticles(
    const char* reason) noexcept {
    if (!cmovParticlesHidden_) {
        return;
    }
    using SetActive = void (*)(void*, bool, void*);
    std::uint32_t restored = 0;
    std::uint32_t dead = 0;
    void* lastChanged = nullptr;
    for (CmovParticleEntry& entry : cmovParticles_) {
        if (!entry.hiddenByUs) {
            continue;
        }
        entry.hiddenByUs = false;
        if (!IsUnityManagedObjectAlive(entry.component) ||
            !IsUnityManagedObjectAlive(entry.gameObject)) {
            ++dead;
            continue;
        }
        if (InvokeManagedVoid<SetActive>(
                api_.gameObjectSetActive, entry.gameObject, true)) {
            lastChanged = entry.gameObject;
            ++restored;
        }
    }
    if (lastChanged != nullptr) {
        RecordLifetimeWrite(
            LifetimeWriteCategory::CmovParticle, "set-active", lastChanged,
            nullptr, static_cast<std::intptr_t>(restored));
    }
    cmovParticlesHidden_ = false;
    if (!cmovParticleRestoreLogged_) {
        cmovParticleRestoreLogged_ = true;
        Log("[VR][stereo] LIVE_CMOV_PS_RESTORED count=" +
            std::to_string(restored) + " dead=" + std::to_string(dead) +
            " reason=" + (reason != nullptr ? reason : "unknown"));
    }
}

void UnityStereoRenderer::DropCmovParticleCache(
    const char* reason) noexcept {
    RestoreCmovParticles(reason);
    const std::size_t cached = cmovParticles_.size();
    cmovParticles_.clear();
    cmovParticleDiscoverSerial_ = 0;
    cmovParticleNextDiscoverAt_ = {};
    cmovParticleNextEmptyLogAt_ = {};
    cmovParticleDiscoverLogged_ = false;
    cmovParticleHideLogged_ = false;
    cmovParticleRestoreLogged_ = false;
    if (cached > 0U) {
        Log("[VR][stereo] LIVE_CMOV_PS_CACHE_DROPPED count=" +
            std::to_string(cached) + " reason=" +
            (reason != nullptr ? reason : "unknown"));
    }
}

void UnityStereoRenderer::ApplyModeBodyLensFlareScales() noexcept {
    // Probe .54: scale scene LensFlareComponentSRP scale AND intensity
    // by mode-body screenScale (~29.9/eyeFov). Goal is black-floor
    // falloff under ~100° eyes; bloom stays off. Empty scenes stay
    // empty -- do not fall back to an inactive-object scan.
    auto* flareClass =
        reinterpret_cast<UnityResolve::Class*>(api_.lensFlareClass);
    if (flareClass == nullptr || api_.lensFlareScaleOffset < 0 ||
        bloomEyeFovDegrees_ <= 1.0F || SceneContentUnstable() ||
        !SceneReadyAllowsStereoRender()) {
        return;
    }
    authoredFlareScales_.erase(
        std::remove_if(
            authoredFlareScales_.begin(), authoredFlareScales_.end(),
            [](const AuthoredFlareScale& entry) {
                return entry.component == nullptr ||
                    !IsUnityManagedObjectAlive(entry.component);
            }),
        authoredFlareScales_.end());
    const float screenScale = ModeBodyScreenScale(bloomEyeFovDegrees_);
    std::uint32_t discovered = 0;
    const char* discoverVia = "none";
    if (authoredFlareScales_.empty() && allowFlareDiscover_) {
        allowFlareDiscover_ = false;
        auto objects = flareClass->FindObjectsByType<void*>();
        discoverVia = "FindObjectsOfType";
        discovered = static_cast<std::uint32_t>(objects.size());
        Log(std::string("[VR][stereo] HEAVY_DISCOVER kind=flare via=FindObjectsOfType count=") +
            std::to_string(discovered));
        for (void* component : objects) {
            if (component == nullptr ||
                !IsUnityManagedObjectAlive(component) ||
                authoredFlareScales_.size() >= 256U) {
                continue;
            }
            const bool known = std::any_of(
                authoredFlareScales_.begin(), authoredFlareScales_.end(),
                [component](const AuthoredFlareScale& entry) {
                    return entry.component == component;
                });
            if (known) {
                continue;
            }
            float currentScale = 0.0F;
            float currentIntensity = 0.0F;
            if (!ReadInstanceFloatSeh(
                    component, api_.lensFlareScaleOffset, &currentScale) ||
                currentScale <= 0.0F) {
                continue;
            }
            if (api_.lensFlareIntensityOffset >= 0) {
                (void)ReadInstanceFloatSeh(
                    component, api_.lensFlareIntensityOffset,
                    &currentIntensity);
            }
            authoredFlareScales_.push_back(
                {component, currentScale, currentIntensity});
        }
    }
    std::uint32_t scaled = 0;
    float firstAuthoredScale = 0.0F;
    float firstWrittenScale = 0.0F;
    float firstAuthoredIntensity = 0.0F;
    float firstWrittenIntensity = 0.0F;
    for (AuthoredFlareScale& entry : authoredFlareScales_) {
        if (entry.component == nullptr ||
            !IsUnityManagedObjectAlive(entry.component) ||
            entry.scale <= 0.0F) {
            continue;
        }
        const float writtenScale = entry.scale * screenScale;
        if (!WriteInstanceFloatSeh(
                entry.component, api_.lensFlareScaleOffset, writtenScale)) {
            continue;
        }
        float writtenIntensity = entry.intensity;
        if (api_.lensFlareIntensityOffset >= 0 && entry.intensity > 0.0F) {
            writtenIntensity = entry.intensity * screenScale;
            (void)WriteInstanceFloatSeh(
                entry.component, api_.lensFlareIntensityOffset,
                writtenIntensity);
        }
        if (scaled == 0U) {
            firstAuthoredScale = entry.scale;
            firstWrittenScale = writtenScale;
            firstAuthoredIntensity = entry.intensity;
            firstWrittenIntensity = writtenIntensity;
        }
        ++scaled;
    }
    if (!lensFlareScaleLogged_) {
        lensFlareScaleLogged_ = true;
        Log(std::string("[VR][stereo] LENS_FLARE_SCALE_APPLIED count=") +
            std::to_string(scaled) +
            " discovered=" + std::to_string(discovered) +
            " via=" + discoverVia +
            " modeFov=" + std::to_string(kModeBodyVerticalFovDegrees) +
            " eyeFov=" + std::to_string(bloomEyeFovDegrees_) +
            " screenScale=" + std::to_string(screenScale) +
            " scaleAuthored=" + std::to_string(firstAuthoredScale) +
            " scaleWritten=" + std::to_string(firstWrittenScale) +
            " intensityAuthored=" +
                std::to_string(firstAuthoredIntensity) +
            " intensityWritten=" +
                std::to_string(firstWrittenIntensity));
    }
}

void UnityStereoRenderer::RestoreLensFlareScales() noexcept {
    if (authoredFlareScales_.empty() || api_.lensFlareScaleOffset < 0) {
        authoredFlareScales_.clear();
        lensFlareScaleLogged_ = false;
        lensFlareDiscoverSerial_ = 0;
        return;
    }
    std::uint32_t restored = 0;
    for (const AuthoredFlareScale& entry : authoredFlareScales_) {
        if (entry.component == nullptr || entry.scale <= 0.0F) {
            continue;
        }
        bool ok = WriteInstanceFloatSeh(
            entry.component, api_.lensFlareScaleOffset, entry.scale);
        if (api_.lensFlareIntensityOffset >= 0 && entry.intensity > 0.0F) {
            ok = WriteInstanceFloatSeh(
                     entry.component, api_.lensFlareIntensityOffset,
                     entry.intensity) &&
                ok;
        }
        if (ok) {
            ++restored;
        }
    }
    if (restored > 0U) {
        Log(std::string("[VR][stereo] LENS_FLARE_SCALE_RESTORED count=") +
            std::to_string(restored));
    }
    authoredFlareScales_.clear();
    lensFlareScaleLogged_ = false;
    lensFlareDiscoverSerial_ = 0;
}

void UnityStereoRenderer::DiscoverProFlares() noexcept {
    auto* proClass =
        reinterpret_cast<UnityResolve::Class*>(api_.proFlareClass);
    if (proClass == nullptr || api_.proFlareGlobalScaleOffset < 0 ||
        SceneContentUnstable()) {
        return;
    }
    constexpr std::size_t kMaxProFlares = 2048U;
    // Drop destroyed Live instances so lobby cameras never rewrite them.
    authoredProFlareScales_.erase(
        std::remove_if(
            authoredProFlareScales_.begin(), authoredProFlareScales_.end(),
            [](const AuthoredProFlareScale& entry) {
                return !IsUnityManagedObjectAlive(entry.component);
            }),
        authoredProFlareScales_.end());
    if (!authoredProFlareScales_.empty() || !allowProFlareDiscover_) {
        return;
    }
    allowProFlareDiscover_ = false;
    auto objects = proClass->FindObjectsByType<void*>();
    const char* discoverVia = "FindObjectsOfType";
    Log(std::string("[VR][stereo] HEAVY_DISCOVER kind=proflare via=FindObjectsOfType count=") +
        std::to_string(objects.size()));
    std::uint32_t added = 0;
    for (void* component : objects) {
        if (component == nullptr ||
            !IsUnityManagedObjectAlive(component) ||
            authoredProFlareScales_.size() >= kMaxProFlares) {
            continue;
        }
        const bool known = std::any_of(
            authoredProFlareScales_.begin(), authoredProFlareScales_.end(),
            [component](const AuthoredProFlareScale& entry) {
                return entry.component == component;
            });
        if (known) {
            continue;
        }
        float currentScale = 0.0F;
        float currentBrightness = 0.0F;
        float edgeBoost = 0.0F;
        float centerBoost = 0.0F;
        if (!ReadInstanceFloatSeh(
                component, api_.proFlareGlobalScaleOffset, &currentScale) ||
            currentScale <= 0.0F) {
            continue;
        }
        if (api_.proFlareGlobalBrightnessOffset >= 0) {
            (void)ReadInstanceFloatSeh(
                component, api_.proFlareGlobalBrightnessOffset,
                &currentBrightness);
        }
        if (api_.proFlareDynamicEdgeBoostOffset >= 0) {
            (void)ReadInstanceFloatSeh(
                component, api_.proFlareDynamicEdgeBoostOffset, &edgeBoost);
        }
        if (api_.proFlareDynamicCenterBoostOffset >= 0) {
            (void)ReadInstanceFloatSeh(
                component, api_.proFlareDynamicCenterBoostOffset,
                &centerBoost);
        }
        authoredProFlareScales_.push_back(
            {component, currentScale, currentBrightness, edgeBoost,
             centerBoost});
        ++added;
    }
    if (!proFlareScaleLogged_ &&
        (added > 0U || !authoredProFlareScales_.empty())) {
        // Defer APPLIED log to WriteProFlareScalesForEyes.
        (void)discoverVia;
    }
}

void UnityStereoRenderer::WriteProFlareScalesForGrip() noexcept {
    // Shared scene ProFlare components — write authored so Grip /
    // Monitor / source keep the 30° look while eyes use a shrink.
    DiscoverProFlares();
    if (authoredProFlareScales_.empty() ||
        api_.proFlareGlobalScaleOffset < 0) {
        return;
    }
    std::uint32_t written = 0;
    std::uint32_t skippedDead = 0;
    void* lastWritten = nullptr;
    for (const AuthoredProFlareScale& entry : authoredProFlareScales_) {
        if (entry.component == nullptr || entry.globalScale <= 0.0F) {
            continue;
        }
        if (!IsUnityManagedObjectAlive(entry.component)) {
            ++skippedDead;
            continue;
        }
        if (!WriteInstanceFloatSeh(
                entry.component, api_.proFlareGlobalScaleOffset,
                entry.globalScale)) {
            continue;
        }
        if (api_.proFlareGlobalBrightnessOffset >= 0 &&
            entry.globalBrightness > 0.0F) {
            (void)WriteInstanceFloatSeh(
                entry.component, api_.proFlareGlobalBrightnessOffset,
                entry.globalBrightness);
        }
        if (api_.proFlareDynamicEdgeBoostOffset >= 0) {
            (void)WriteInstanceFloatSeh(
                entry.component, api_.proFlareDynamicEdgeBoostOffset,
                entry.dynamicEdgeBoost);
        }
        if (api_.proFlareDynamicCenterBoostOffset >= 0) {
            (void)WriteInstanceFloatSeh(
                entry.component, api_.proFlareDynamicCenterBoostOffset,
                entry.dynamicCenterBoost);
        }
        lastWritten = entry.component;
        ++written;
    }
    if (lastWritten != nullptr) {
        RecordLifetimeWrite(
            LifetimeWriteCategory::ProFlare, "grip-authored", lastWritten,
            nullptr, static_cast<std::intptr_t>(written));
    }
    proFlareEyeScaled_ = false;
    if (!proFlareGripWriteLogged_ && (written > 0U || skippedDead > 0U)) {
        proFlareGripWriteLogged_ = true;
        Log(std::string("[VR][stereo] PRO_FLARE_GRIP_RESTORED count=") +
            std::to_string(written) +
            " skippedDead=" + std::to_string(skippedDead) +
            " cached=" + std::to_string(authoredProFlareScales_.size()));
    }
}

void UnityStereoRenderer::WriteProFlareScalesForEyes() noexcept {
    // Projection-equivalent ProFlare is applied later, at the exact
    // per-camera ScheduleFlares element-buffer boundary. Scene components are
    // shared by source/Grip/both eyes, so no fallback writes are allowed here:
    // an unavailable replacement intentionally leaves the authored flare
    // large and therefore makes this hardware probe's failure unambiguous.
    if (SceneContentUnstable() || !SceneReadyAllowsStereoRender()) {
        return;
    }
    DiscoverProFlares();
    proFlareEyeScaled_ = false;
    if (ProjectionEquivalentProFlareReady()) {
        // Both live detours are installed. Projection-equivalent sizing is
        // applied later to the eye-owned ScheduleFlares element buffer; shared
        // source/Grip values remain authored.
        if (!proFlareScaleLogged_) {
            proFlareScaleLogged_ = true;
            Log(std::string(
                    "[VR][fov] PRO_FLARE_REPLACEMENT_ACTIVE cached=") +
                std::to_string(authoredProFlareScales_.size()) +
                " globalScaleWrites=0 brightnessWrites=0 boostWrites=0" +
                " perCameraElementSize=1");
        }
        return;
    }
    if (!proFlareScaleLogged_) {
        proFlareScaleLogged_ = true;
        Log(std::string(
                "[VR][fov] PRO_FLARE_REPLACEMENT_UNAVAILABLE cached=") +
            std::to_string(authoredProFlareScales_.size()) +
            " globalScaleWrites=0 brightnessWrites=0 boostWrites=0" +
            " sharedAuthored=1 failureVisible=1");
    }
}

void UnityStereoRenderer::DropProFlareCache(const char* reason) noexcept {
    const std::size_t cached = authoredProFlareScales_.size();
    authoredProFlareScales_.clear();
    proFlareScaleLogged_ = false;
    proFlareGripWriteLogged_ = false;
    proFlareDiscoverSerial_ = 0;
    proFlareEyeScaled_ = false;
    if (cached > 0U) {
        Log(std::string("[VR][stereo] PRO_FLARE_CACHE_DROPPED count=") +
            std::to_string(cached) + " reason=" +
            (reason != nullptr ? reason : "unknown") +
            " sharedStateAuthored=1");
    }
}

void UnityStereoRenderer::RestoreProFlareScales() noexcept {
    // Best-effort authored write for still-alive instances, then drop
    // so destroyed Live pointers are never touched again.
    WriteProFlareScalesForGrip();
    DropProFlareCache("restore");
}

void UnityStereoRenderer::DeactivateBoundEyeDepthOfField() noexcept {
    DeactivateVolumeComponent(eyeDofComponents_[0], "left-vl");
    NeutralizeDepthOfFieldParameters(eyeDofComponents_[0], "left-vl");
    DeactivateVolumeComponent(eyeDofComponents_[1], "right-vl");
    NeutralizeDepthOfFieldParameters(eyeDofComponents_[1], "right-vl");
    DeactivateVolumeComponent(eyeUrpDofComponents_[0], "left-urp");
    NeutralizeDepthOfFieldParameters(eyeUrpDofComponents_[0], "left-urp");
    DeactivateVolumeComponent(eyeUrpDofComponents_[1], "right-urp");
    NeutralizeDepthOfFieldParameters(eyeUrpDofComponents_[1], "right-urp");
    if (!eyeDofParamsLogged_ &&
        (eyeDofComponents_[0] != nullptr ||
         eyeDofComponents_[1] != nullptr ||
         eyeUrpDofComponents_[0] != nullptr ||
         eyeUrpDofComponents_[1] != nullptr)) {
        eyeDofParamsLogged_ = true;
    }
    if ((eyeDofComponents_[0] != nullptr ||
         eyeDofComponents_[1] != nullptr ||
         eyeUrpDofComponents_[0] != nullptr ||
         eyeUrpDofComponents_[1] != nullptr) &&
        api_.volumeComponentActiveOffset >= 0) {
        eyeDofDeactivatedLogged_ = true;
    }
}

void* UnityStereoRenderer::CurrentCamera() const noexcept {
    return currentCamera_;
}

void UnityStereoRenderer::SyncEyeAsMainCameraTag() noexcept {
    // .114 forensics (pid=38020): the tag write itself worked — native
    // Camera.main returned the left eye the same millisecond. The HMD
    // then went black because the mod starved itself: the camera tracker
    // selected the eye as unityCameraDiagnosticMain and Tick (gated on that
    // selection at the Cinemachine PushState hook) stopped, so eyes never
    // re-armed and restore was unreachable. Fixes: the tracker now
    // refuses eye cameras (Hook.cpp), the source keeps its own tag (a
    // disabled camera already loses native FindMainCamera, so the enabled
    // tagged eye wins without stealing anything), and RestoreSourceCamera
    // untags the eye before re-enabling the source so two enabled
    // MainCamera-tagged cameras never coexist.
    const bool want = GakumasLocal::Config::vrEyeAsMainCamera &&
        sourceCameraSuppressed_ &&
        eyeGameObjects_[0] != nullptr &&
        IsUnityManagedObjectAlive(eyeGameObjects_[0]);
    if (want == eyeMainCameraTagApplied_ &&
        (!want || eyeMainCameraTaggedObject_ == eyeGameObjects_[0])) {
        return;
    }
    if (mainCameraTagString_ == nullptr) {
        mainCameraTagString_ = InternIl2CppString("MainCamera");
        untaggedTagString_ = InternIl2CppString("Untagged");
    }
    if (!api_.gameObjectSetTag.Ready() || mainCameraTagString_ == nullptr ||
        untaggedTagString_ == nullptr) {
        if (!eyeMainCameraTagApiLogged_) {
            eyeMainCameraTagApiLogged_ = true;
            Log("[VR][stereo] EYE_MAIN_CAMERA_TAG ready=0");
        }
        return;
    }
    using SetTag = void (*)(void*, void*, void*);
    if (want) {
        // Write-ahead log: if set_tag ever faults, the last line names it.
        Log("[VR][stereo] EYE_MAIN_CAMERA_TAG write=eye value=MainCamera");
        if (!InvokeManagedVoid<SetTag>(
                api_.gameObjectSetTag, eyeGameObjects_[0],
                mainCameraTagString_)) {
            Log("[VR][stereo] EYE_MAIN_CAMERA_TAG failed=eye-write");
            return;
        }
        eyeMainCameraTagApplied_ = true;
        eyeMainCameraTaggedObject_ = eyeGameObjects_[0];
        RecordLifetimeWrite(
            LifetimeWriteCategory::MainCameraTag, "set-main-camera",
            eyeGameObjects_[0], mainCameraTagString_, 1);
        Log("[VR][stereo] EYE_MAIN_CAMERA_TAG applied=1 eye=left sourceTagKept=1");
        return;
    }
    if (eyeMainCameraTaggedObject_ != nullptr &&
        IsUnityManagedObjectAlive(eyeMainCameraTaggedObject_)) {
        Log("[VR][stereo] EYE_MAIN_CAMERA_TAG write=eye value=Untagged");
        if (!InvokeManagedVoid<SetTag>(
                api_.gameObjectSetTag, eyeMainCameraTaggedObject_,
                untaggedTagString_)) {
            Log("[VR][stereo] EYE_MAIN_CAMERA_TAG failed=eye-clear");
        } else {
            RecordLifetimeWrite(
                LifetimeWriteCategory::MainCameraTag, "set-untagged",
                eyeMainCameraTaggedObject_, untaggedTagString_, 0);
        }
    }
    eyeMainCameraTagApplied_ = false;
    eyeMainCameraTaggedObject_ = nullptr;
    Log("[VR][stereo] EYE_MAIN_CAMERA_TAG applied=0");
}

void* UnityStereoRenderer::MainCameraOverride() const noexcept {
    if (!GakumasLocal::Config::vrEyeAsMainCamera || !sourceCameraSuppressed_) {
        return nullptr;
    }
    // TAA / object motion vectors need one stable Camera.main for the
    // whole frame. Swapping to the right eye mid-frame would refresh
    // previous transforms twice (left then right) and zero the right
    // eye's object MVs.
    return eyeCameras_[0];
}

const char* UnityStereoRenderer::ClassifyCamera(void* camera) const noexcept {
    if (camera == nullptr) {
        return "none";
    }
    if (camera == eyeCameras_[0]) {
        return "left";
    }
    if (camera == eyeCameras_[1]) {
        return "right";
    }
    if (camera == latestSourceCamera_) {
        return "source";
    }
    return "other";
}

bool UnityStereoRenderer::BeginActorShadowSourceAnchor(
    const char* passName) noexcept {
    // Pass-activity census, ahead of every gate below. The .89 run showed
    // the campus pass only executes per-frame in some scenes, so anything
    // keyed on its call count can silently starve; this prints which passes
    // ran on which cameras every ~10 s so the log shows the actual pattern.
    if (passName != nullptr && IsOwnerThread()) {
        std::size_t passIndex = 3;
        if (std::strcmp(passName, "campus-actor-param") == 0) {
            passIndex = 0;
        } else if (std::strcmp(passName, "vl-actor-param") == 0) {
            passIndex = 1;
        } else if (std::strcmp(passName, "actor-shadow") == 0 ||
                   std::strcmp(passName, "actor-shadow-add") == 0) {
            passIndex = 2;
        }
        const char* cameraClass = ClassifyCamera(currentCamera_);
        std::size_t cameraIndex = 3;
        if (std::strcmp(cameraClass, "source") == 0) {
            cameraIndex = 0;
        } else if (std::strcmp(cameraClass, "left") == 0) {
            cameraIndex = 1;
        } else if (std::strcmp(cameraClass, "right") == 0) {
            cameraIndex = 2;
        }
        ++passActivityCounts_[passIndex][cameraIndex];
        const auto now = std::chrono::steady_clock::now();
        if (passActivityPrintAt_ == std::chrono::steady_clock::time_point{}) {
            passActivityPrintAt_ = now + std::chrono::seconds(10);
        } else if (now >= passActivityPrintAt_) {
            passActivityPrintAt_ = now + std::chrono::seconds(10);
            std::ostringstream stream;
            stream << "[VR][shadow] PASS_ACTIVITY window=10s";
            static constexpr const char* kPassNames[4] = {
                "campus", "vl", "shadow", "other"};
            for (std::size_t pass = 0; pass < 4; ++pass) {
                stream << ' ' << kPassNames[pass] << "=s:"
                       << passActivityCounts_[pass][0] << ",l:"
                       << passActivityCounts_[pass][1] << ",r:"
                       << passActivityCounts_[pass][2] << ",o:"
                       << passActivityCounts_[pass][3];
                passActivityCounts_[pass] = {};
            }
            Log(stream.str());
        }
        // Render-side diagnostic host: fires on either MatCap parameter pass while
        // it renders the source camera, rate-limited by wall clock instead of
        // the .87 "every 30th call" rule that starved all session on .89.
        if ((passIndex == 0 || passIndex == 1) && cameraIndex == 0 &&
            currentCamera_ != nullptr) {
            ++actorLightDiagnosticCalls_;
            if (now >= actorLightDiagnosticNextAt_) {
                actorLightDiagnosticNextAt_ =
                    now + std::chrono::milliseconds(1500);
                RecordActorLightDiagnostics(ReadCurrentCameraPoseForDiagnostics());
            }
        }
    }
    if (actorShadowAnchorActive_ ||
        !GakumasLocal::Config::vrActorShadowSourceAnchor || !IsOwnerThread() ||
        !actorShadowAnchorPoseValid_) {
        return false;
    }
    void* camera = currentCamera_;
    if (camera == nullptr ||
        (camera != eyeCameras_[0] && camera != eyeCameras_[1] &&
         camera != latestSourceCamera_)) {
        return false;
    }
    if (!api_.componentGetTransform.Ready() ||
        !api_.transformSetPositionAndRotationInjected.Ready() ||
        !api_.transformGetPositionInjected.Ready() ||
        !api_.transformGetRotationInjected.Ready()) {
        if (!actorShadowAnchorSkipLogged_) {
            actorShadowAnchorSkipLogged_ = true;
            Log("[VR][shadow] ACTOR_SHADOW_ANCHOR_UNAVAILABLE reason=transform-api");
        }
        return false;
    }
    if (!IsUnityManagedObjectAlive(camera)) {
        return false;
    }
    using GetTransform = void* (*)(void*, void*);
    using GetVector3 = void (*)(void*, UnityVector3*, void*);
    using GetQuaternion = void (*)(void*, UnityQuaternion*, void*);
    using SetPositionAndRotation = void (*)(
        void*, const UnityVector3*, const UnityQuaternion*, void*);
    void* transform = nullptr;
    if (!InvokeManagedResult<void*, GetTransform>(
            api_.componentGetTransform, &transform, camera) ||
        transform == nullptr) {
        return false;
    }
    void* nativeTransform = nullptr;
    if (api_.transformUsesNativeSelf ||
        api_.transformPositionGetterUsesNativeSelf ||
        api_.transformRotationGetterUsesNativeSelf) {
        nativeTransform = ReadUnityNativePointer(transform);
        if (nativeTransform == nullptr) {
            return false;
        }
    }
    void* writeSelf =
        api_.transformUsesNativeSelf ? nativeTransform : transform;
    void* positionSelf =
        api_.transformPositionGetterUsesNativeSelf ? nativeTransform : transform;
    void* rotationSelf =
        api_.transformRotationGetterUsesNativeSelf ? nativeTransform : transform;
    UnityVector3 savedPosition{};
    UnityQuaternion savedRotation{};
    if (!InvokeManagedVoid<GetVector3>(
            api_.transformGetPositionInjected, positionSelf, &savedPosition) ||
        !InvokeManagedVoid<GetQuaternion>(
            api_.transformGetRotationInjected, rotationSelf, &savedRotation)) {
        return false;
    }
    const pose::Pose saved{
        {savedPosition.x, savedPosition.y, savedPosition.z},
        {savedRotation.x, savedRotation.y, savedRotation.z, savedRotation.w},
    };
    if (!IsFinitePose(saved)) {
        return false;
    }
    const UnityVector3 anchorPosition(
        actorShadowAnchorPose_.position.x,
        actorShadowAnchorPose_.position.y,
        actorShadowAnchorPose_.position.z);
    const UnityQuaternion anchorRotation(
        actorShadowAnchorPose_.orientation.x,
        actorShadowAnchorPose_.orientation.y,
        actorShadowAnchorPose_.orientation.z,
        actorShadowAnchorPose_.orientation.w);
    if (!InvokeManagedVoid<SetPositionAndRotation>(
            api_.transformSetPositionAndRotationInjected, writeSelf,
            &anchorPosition, &anchorRotation)) {
        return false;
    }
    actorShadowSavedTransformSelf_ = writeSelf;
    actorShadowSavedPose_ = saved;
    actorShadowAnchorActive_ = true;
    ++actorShadowAnchorSamples_;
    const char* cameraClass = ClassifyCamera(camera);
    const std::string key =
        std::string(passName != nullptr ? passName : "?") + '/' + cameraClass;
    if (std::find(actorShadowAnchorLoggedKeys_.begin(),
                  actorShadowAnchorLoggedKeys_.end(),
                  key) == actorShadowAnchorLoggedKeys_.end()) {
        actorShadowAnchorLoggedKeys_.push_back(key);
        // Forward vectors, not positions: the complaint is about direction,
        // and a near-origin lobby shot makes the positions look identical.
        const pose::Vector3 headForward =
            pose::Rotate(saved.orientation, pose::Vector3{0.0F, 0.0F, 1.0F});
        const pose::Vector3 shotForward = pose::Rotate(
            actorShadowAnchorPose_.orientation, pose::Vector3{0.0F, 0.0F, 1.0F});
        std::ostringstream stream;
        stream << "[VR][shadow] ACTOR_SHADOW_ANCHOR_APPLIED pass=" << key
               << std::fixed << std::setprecision(3) << " headFwd=("
               << headForward.x << ',' << headForward.y << ','
               << headForward.z << ") shotFwd=(" << shotForward.x << ','
               << shotForward.y << ',' << shotForward.z << ')';
        Log(stream.str());
    }
    return true;
}

void UnityStereoRenderer::EndActorShadowSourceAnchor() noexcept {
    if (!actorShadowAnchorActive_) {
        return;
    }
    actorShadowAnchorActive_ = false;
    void* self = actorShadowSavedTransformSelf_;
    actorShadowSavedTransformSelf_ = nullptr;
    if (self == nullptr ||
        !api_.transformSetPositionAndRotationInjected.Ready()) {
        return;
    }
    using SetPositionAndRotation = void (*)(
        void*, const UnityVector3*, const UnityQuaternion*, void*);
    const UnityVector3 position(
        actorShadowSavedPose_.position.x,
        actorShadowSavedPose_.position.y,
        actorShadowSavedPose_.position.z);
    const UnityQuaternion rotation(
        actorShadowSavedPose_.orientation.x,
        actorShadowSavedPose_.orientation.y,
        actorShadowSavedPose_.orientation.z,
        actorShadowSavedPose_.orientation.w);
    InvokeManagedVoid<SetPositionAndRotation>(
        api_.transformSetPositionAndRotationInjected, self, &position,
        &rotation);
}

bool UnityStereoRenderer::EnsureVolumeTriggerProxy() noexcept {
    if (volumeTriggerGameObject_ != nullptr &&
        IsUnityManagedObjectAlive(volumeTriggerGameObject_) &&
        volumeTriggerTransform_ != nullptr) {
        return true;
    }
    if (volumeTriggerGameObject_ != nullptr) {
        // The proxy died despite DontDestroyOnLoad (forced teardown). Retire
        // the rooted handle, drop every pointer that referenced the shell,
        // and rebuild. Never rebind the dead shell.
        if (volumeTriggerGameObjectHandle_ != nullptr) {
            retiredVolumeTriggerHandles_.push_back(
                volumeTriggerGameObjectHandle_);
        }
        volumeTriggerGameObjectHandle_ = nullptr;
        volumeTriggerGameObject_ = nullptr;
        volumeTriggerTransform_ = nullptr;
        volumeAnchorAppliedData_ = nullptr;
        volumeAnchorSourceCamera_ = nullptr;
        volumeAnchorSavedTrigger_ = nullptr;
        volumeAnchorActive_ = false;
        Log("[VR][stereo] VOLUME_ANCHOR proxy=dead action=recreate");
    }
    if (api_.gameObjectClass == nullptr ||
        !api_.internalCreateGameObject.Ready() ||
        !api_.gameObjectGetTransform.Ready() ||
        !api_.dontDestroyOnLoad.Ready() ||
        !api_.transformSetPositionAndRotationInjected.Ready()) {
        if (!volumeAnchorProxyFailedLogged_) {
            volumeAnchorProxyFailedLogged_ = true;
            Log("[VR][stereo] VOLUME_ANCHOR proxy=unavailable reason=api");
        }
        return false;
    }
    void* gameObject = NewIl2CppObject(api_.gameObjectClass);
    auto* managedName =
        UnityResolve::UnityType::String::New("__GakumasVrVolumeTrigger");
    if (gameObject == nullptr || managedName == nullptr) {
        if (!volumeAnchorProxyFailedLogged_) {
            volumeAnchorProxyFailedLogged_ = true;
            Log("[VR][stereo] VOLUME_ANCHOR proxy=unavailable reason=allocate");
        }
        return false;
    }
    using CreateGameObject = void (*)(void*, void*, void*);
    using DontDestroyOnLoad = void (*)(void*, void*);
    using GetTransform = void* (*)(void*, void*);
    void* transform = nullptr;
    if (!InvokeManagedVoid<CreateGameObject>(
            api_.internalCreateGameObject, gameObject, managedName) ||
        !InvokeManagedResult<void*, GetTransform>(
            api_.gameObjectGetTransform, &transform, gameObject) ||
        transform == nullptr ||
        !InvokeManagedVoid<DontDestroyOnLoad>(
            api_.dontDestroyOnLoad, gameObject)) {
        if (!volumeAnchorProxyFailedLogged_) {
            volumeAnchorProxyFailedLogged_ = true;
            Log("[VR][stereo] VOLUME_ANCHOR proxy=unavailable reason=create");
        }
        return false;
    }
    volumeTriggerGameObjectHandle_ = CreateGcHandle(gameObject);
    if (volumeTriggerGameObjectHandle_ == nullptr) {
        if (!volumeAnchorProxyFailedLogged_) {
            volumeAnchorProxyFailedLogged_ = true;
            Log("[VR][stereo] VOLUME_ANCHOR proxy=unavailable reason=gchandle");
        }
        return false;
    }
    volumeTriggerGameObject_ = gameObject;
    volumeTriggerTransform_ = transform;
    volumeAnchorProxyFailedLogged_ = false;
    std::ostringstream line;
    line << "[VR][stereo] VOLUME_ANCHOR proxy=created gameObject=0x"
         << std::hex << reinterpret_cast<std::uintptr_t>(gameObject)
         << " transform=0x"
         << reinterpret_cast<std::uintptr_t>(transform);
    Log(line.str());
    return true;
}

void UnityStereoRenderer::RestoreVolumeTriggerAnchor(
    const char* reason) noexcept {
    if (!volumeAnchorActive_) {
        return;
    }
    void* data = volumeAnchorAppliedData_;
    void* saved = volumeAnchorSavedTrigger_;
    volumeAnchorAppliedData_ = nullptr;
    volumeAnchorSourceCamera_ = nullptr;
    volumeAnchorSavedTrigger_ = nullptr;
    volumeAnchorActive_ = false;
    const char* reasonText = reason != nullptr ? reason : "?";
    if (data == nullptr || !IsUnityManagedObjectAlive(data) ||
        !api_.universalSetVolumeTrigger.Ready()) {
        Log(std::string("[VR][stereo] VOLUME_ANCHOR restore=skip reason=") +
            reasonText + " dataAlive=0");
        return;
    }
    if (saved != nullptr && !IsUnityManagedObjectAlive(saved)) {
        // The game's own trigger died with its scene; null restores the URP
        // default (camera transform). Never bind a dead shell.
        saved = nullptr;
    }
    using SetObject = void (*)(void*, void*, void*);
    const bool restored = InvokeManagedVoid<SetObject>(
        api_.universalSetVolumeTrigger, data, saved);
    Log(std::string("[VR][stereo] VOLUME_ANCHOR restore=") +
        (restored ? "1" : "0") + " reason=" + reasonText);
}

void UnityStereoRenderer::UpdateVolumeTriggerAnchor(
    void* sourceCamera, const UnityStereoCameraFrame& frame) noexcept {
    if (!GakumasLocal::Config::vrVolumeSourceAnchor) {
        RestoreVolumeTriggerAnchor("config-off");
        return;
    }
    if (sourceCamera == nullptr || !frame.cinematicPoseValid ||
        !IsFinitePose(frame.cinematicPose)) {
        // Keep the last binding: the proxy still holds the last authored
        // pose, and identity changes are handled on the next valid Tick.
        return;
    }
    if (!EnsureVolumeTriggerProxy()) {
        return;
    }
    void* transformSelf = api_.transformUsesNativeSelf
        ? ReadUnityNativePointer(volumeTriggerTransform_)
        : volumeTriggerTransform_;
    if (transformSelf == nullptr) {
        return;
    }
    using SetPositionAndRotation = void (*)(
        void*, const UnityVector3*, const UnityQuaternion*, void*);
    const UnityVector3 position(
        frame.cinematicPose.position.x,
        frame.cinematicPose.position.y,
        frame.cinematicPose.position.z);
    const UnityQuaternion rotation(
        frame.cinematicPose.orientation.x,
        frame.cinematicPose.orientation.y,
        frame.cinematicPose.orientation.z,
        frame.cinematicPose.orientation.w);
    if (!InvokeManagedVoid<SetPositionAndRotation>(
            api_.transformSetPositionAndRotationInjected, transformSelf,
            &position, &rotation)) {
        return;
    }
    if (volumeAnchorActive_ && volumeAnchorSourceCamera_ == sourceCamera &&
        volumeAnchorAppliedData_ != nullptr &&
        IsUnityManagedObjectAlive(volumeAnchorAppliedData_)) {
        // Steady state: only the proxy pose needed refreshing.
        return;
    }
    if (volumeAnchorActive_ && volumeAnchorSourceCamera_ != sourceCamera) {
        RestoreVolumeTriggerAnchor("source-changed");
    } else if (volumeAnchorActive_) {
        // The applied camera data died with its scene; the saved trigger
        // died with it, so there is nothing to write back.
        volumeAnchorAppliedData_ = nullptr;
        volumeAnchorSourceCamera_ = nullptr;
        volumeAnchorSavedTrigger_ = nullptr;
        volumeAnchorActive_ = false;
        Log("[VR][stereo] VOLUME_ANCHOR rebind=1 reason=data-dead");
    }
    if (!IsUnityManagedObjectAlive(sourceCamera) ||
        !api_.componentGetComponent.Ready() ||
        !api_.universalGetVolumeTrigger.Ready() ||
        !api_.universalSetVolumeTrigger.Ready()) {
        return;
    }
    using GetComponent = void* (*)(void*, void*, void*);
    using GetObject = void* (*)(void*, void*);
    using SetObject = void (*)(void*, void*, void*);
    void* data = nullptr;
    if (!InvokeManagedResult<void*, GetComponent>(
            api_.componentGetComponent, &data, sourceCamera,
            api_.universalCameraDataReflectionType) ||
        data == nullptr) {
        return;
    }
    void* saved = nullptr;
    if (!InvokeManagedResult<void*, GetObject>(
            api_.universalGetVolumeTrigger, &saved, data)) {
        return;
    }
    if (saved == volumeTriggerTransform_) {
        // Leftover from a previous bind on the same data: the true game
        // value was null, never persist our own write as the saved target.
        saved = nullptr;
    }
    if (!InvokeManagedVoid<SetObject>(
            api_.universalSetVolumeTrigger, data, volumeTriggerTransform_)) {
        return;
    }
    volumeAnchorAppliedData_ = data;
    volumeAnchorSourceCamera_ = sourceCamera;
    volumeAnchorSavedTrigger_ = saved;
    volumeAnchorActive_ = true;
    std::ostringstream line;
    line << "[VR][stereo] VOLUME_ANCHOR applied=1 source=0x" << std::hex
         << reinterpret_cast<std::uintptr_t>(sourceCamera)
         << " data=0x" << reinterpret_cast<std::uintptr_t>(data)
         << " trigger=0x"
         << reinterpret_cast<std::uintptr_t>(volumeTriggerTransform_)
         << " saved=0x" << reinterpret_cast<std::uintptr_t>(saved)
         << std::dec << std::fixed << std::setprecision(3)
         << " anchorPos=" << frame.cinematicPose.position.x << ','
         << frame.cinematicPose.position.y << ','
         << frame.cinematicPose.position.z;
    Log(line.str());
}

bool UnityStereoRenderer::ReadShaderGlobal(
    const MethodRef& getter,
    std::int32_t nameId,
    float* out,
    std::size_t count) noexcept {
    if (nameId == 0 || out == nullptr || !getter.Ready()) {
        return false;
    }
    std::int32_t localId = nameId;
    void* arguments[] = {&localId};
    void* boxed = nullptr;
    if (!RuntimeInvoke(getter, nullptr, arguments, &boxed, nullptr) ||
        boxed == nullptr) {
        return false;
    }
    void* data = UnboxObject(boxed);
    if (data == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!ReadManagedField(
                data, static_cast<std::int32_t>(i * sizeof(float)), &out[i])) {
            return false;
        }
    }
    return true;
}

pose::Pose UnityStereoRenderer::ReadCurrentCameraPoseForDiagnostics() noexcept {
    // Zero orientation marks "unreadable"; a valid quaternion can never be
    // all zeros.
    pose::Pose failed{};
    failed.orientation = {0.0F, 0.0F, 0.0F, 0.0F};
    void* camera = currentCamera_;
    if (camera == nullptr || !api_.componentGetTransform.Ready() ||
        !api_.transformGetPositionInjected.Ready() ||
        !api_.transformGetRotationInjected.Ready() ||
        !IsUnityManagedObjectAlive(camera)) {
        return failed;
    }
    using GetTransform = void* (*)(void*, void*);
    using GetVector3 = void (*)(void*, UnityVector3*, void*);
    using GetQuaternion = void (*)(void*, UnityQuaternion*, void*);
    void* transform = nullptr;
    if (!InvokeManagedResult<void*, GetTransform>(
            api_.componentGetTransform, &transform, camera) ||
        transform == nullptr) {
        return failed;
    }
    void* nativeTransform = nullptr;
    if (api_.transformPositionGetterUsesNativeSelf ||
        api_.transformRotationGetterUsesNativeSelf) {
        nativeTransform = ReadUnityNativePointer(transform);
        if (nativeTransform == nullptr) {
            return failed;
        }
    }
    void* positionSelf = api_.transformPositionGetterUsesNativeSelf
        ? nativeTransform
        : transform;
    void* rotationSelf = api_.transformRotationGetterUsesNativeSelf
        ? nativeTransform
        : transform;
    UnityVector3 position{};
    UnityQuaternion rotation{};
    if (!InvokeManagedVoid<GetVector3>(
            api_.transformGetPositionInjected, positionSelf, &position) ||
        !InvokeManagedVoid<GetQuaternion>(
            api_.transformGetRotationInjected, rotationSelf, &rotation)) {
        return failed;
    }
    const pose::Pose result{
        {position.x, position.y, position.z},
        {rotation.x, rotation.y, rotation.z, rotation.w},
    };
    return IsFinitePose(result) ? result : failed;
}

bool UnityStereoRenderer::EnsureActorLightShaderIds() noexcept {
    const bool allKnown = actorLightMatcapMainId_ != 0 && actorLightMatcapRimId_ != 0 &&
        actorLightLitActorMainId_ != 0 && actorLightVlSpecColorId_ != 0 &&
        actorLightGlobalLightParameterId_ != 0 && actorLightMatcapLightColorId_ != 0 &&
        actorLightDirectionId_ != 0 && actorLightWorldToActorShadowId_ != 0;
    if (allKnown && actorLightDiagnosticResolved_) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < actorLightIdRetryAt_) {
        return actorLightMatcapMainId_ != 0;
    }
    actorLightIdRetryAt_ = now + std::chrono::seconds(1);
    if (!actorLightDiagnosticResolved_) {
        actorLightDiagnosticResolved_ = true;
        auto* shaderClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Shader");
        if (shaderClass != nullptr) {
            for (auto* method : shaderClass->methods) {
                if (method == nullptr || method->function == nullptr ||
                    method->address == nullptr || !method->static_function ||
                    method->args.size() != 1U || method->args[0] == nullptr ||
                    method->args[0]->pType == nullptr ||
                    method->args[0]->pType->name != "System.Int32" ||
                    method->return_type == nullptr) {
                    continue;
                }
                if (method->name == "GetGlobalVector" &&
                    method->return_type->name == "UnityEngine.Vector4") {
                    shaderGetGlobalVector_ = MethodReference(method);
                } else if (method->name == "GetGlobalMatrix" &&
                           method->return_type->name ==
                               "UnityEngine.Matrix4x4") {
                    shaderGetGlobalMatrix_ = MethodReference(method);
                }
            }
        }
    }
    // The passes cache their Shader.PropertyToID results in static int
    // fields, but those are filled by class constructors that may not have
    // run yet (ActorShadowPass came back 0 on the .85 run). Retry any zero
    // ID until it materializes.
    const std::int32_t before[8] = {
        actorLightMatcapMainId_, actorLightMatcapRimId_, actorLightLitActorMainId_,
        actorLightVlSpecColorId_, actorLightGlobalLightParameterId_,
        actorLightMatcapLightColorId_, actorLightDirectionId_,
        actorLightWorldToActorShadowId_};
    auto* campusPass = Il2cppUtils::GetClass(
        "campus-submodule.Runtime.dll", "Campus.Rendering",
        "CampusActorParameterPass");
    if (actorLightMatcapMainId_ == 0) {
        actorLightMatcapMainId_ =
            ReadStaticIntField(campusPass, "_MatCapMainLight");
    }
    if (actorLightMatcapRimId_ == 0) {
        actorLightMatcapRimId_ =
            ReadStaticIntField(campusPass, "_MatCapRimLight");
    }
    if (actorLightLitActorMainId_ == 0) {
        actorLightLitActorMainId_ =
            ReadStaticIntField(campusPass, "_LitActorMainLight");
    }
    if (actorLightLitActorRimId_ == 0) {
        actorLightLitActorRimId_ =
            ReadStaticIntField(campusPass, "_LitActorRimLight");
    }
    if (actorLightVlSpecColorId_ == 0) {
        actorLightVlSpecColorId_ =
            ReadStaticIntField(campusPass, "_VLSpecColor");
    }
    if (actorLightGlobalLightParameterId_ == 0) {
        actorLightGlobalLightParameterId_ =
            ReadStaticIntField(campusPass, "_GlobalLightParameter");
    }
    if (actorLightMatcapLightColorId_ == 0) {
        actorLightMatcapLightColorId_ =
            ReadStaticIntField(campusPass, "_MatCapLightColor");
    }
    if (actorLightMatcapRimColorId_ == 0) {
        actorLightMatcapRimColorId_ =
            ReadStaticIntField(campusPass, "_MatCapRimColor");
    }
    if (actorLightMatcapParamId_ == 0) {
        actorLightMatcapParamId_ = ReadStaticIntField(campusPass, "_MatCapParam");
    }
    auto* shadowPass = Il2cppUtils::GetClass(
        "vl-unity.Runtime.dll", "VL.Rendering", "ActorShadowPass");
    if (actorLightDirectionId_ == 0) {
        actorLightDirectionId_ =
            ReadStaticIntField(shadowPass, "_LightDirection");
    }
    if (actorLightWorldToActorShadowId_ == 0) {
        actorLightWorldToActorShadowId_ =
            ReadStaticIntField(shadowPass, "_WorldToActorShadow");
    }
    // PropertyToID fallback: the static fields above are only filled once
    // the owning class constructor runs. The .91 Live session retried
    // ActorShadowPass IDs for ~150 s without that ctor. Shader property
    // names equal the field names, so resolve any still-zero ID directly.
    if (actorLightDirectionId_ == 0 || actorLightWorldToActorShadowId_ == 0 ||
        actorLightMatcapMainId_ == 0 || actorLightMatcapRimId_ == 0 ||
        actorLightLitActorMainId_ == 0 || actorLightLitActorRimId_ == 0 ||
        actorLightVlSpecColorId_ == 0 || actorLightGlobalLightParameterId_ == 0 ||
        actorLightMatcapLightColorId_ == 0 ||
        actorLightMatcapParamId_ == 0 ||
        actorLightWorldSpaceCameraPosId_ == 0) {
        auto* shaderClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Shader");
        UnityResolve::Method* propertyToID = nullptr;
        if (shaderClass != nullptr) {
            for (auto* method : shaderClass->methods) {
                if (method != nullptr && method->function != nullptr &&
                    method->static_function &&
                    method->name == "PropertyToID" &&
                    method->args.size() == 1U) {
                    propertyToID = method;
                    break;
                }
            }
        }
        const auto resolveId =
            [&propertyToID, this](const char* name) noexcept -> std::int32_t {
            if (propertyToID == nullptr) {
                return 0;
            }
            void* managedName = UnityResolve::Invoke<void*, const char*>(
                "il2cpp_string_new", name);
            int id = 0;
            if (managedName != nullptr &&
                InvokeManagedResult<int, int (*)(void*, void*)>(
                    MethodReference(propertyToID), &id, managedName)) {
                return id;
            }
            return 0;
        };
        if (actorLightMatcapMainId_ == 0) {
            actorLightMatcapMainId_ = resolveId("_MatCapMainLight");
        }
        if (actorLightMatcapRimId_ == 0) {
            actorLightMatcapRimId_ = resolveId("_MatCapRimLight");
        }
        if (actorLightLitActorMainId_ == 0) {
            actorLightLitActorMainId_ = resolveId("_LitActorMainLight");
        }
        if (actorLightLitActorRimId_ == 0) {
            actorLightLitActorRimId_ = resolveId("_LitActorRimLight");
        }
        if (actorLightVlSpecColorId_ == 0) {
            actorLightVlSpecColorId_ = resolveId("_VLSpecColor");
        }
        if (actorLightGlobalLightParameterId_ == 0) {
            actorLightGlobalLightParameterId_ =
                resolveId("_GlobalLightParameter");
        }
        if (actorLightMatcapLightColorId_ == 0) {
            actorLightMatcapLightColorId_ = resolveId("_MatCapLightColor");
        }
        if (actorLightMatcapRimColorId_ == 0) {
            actorLightMatcapRimColorId_ = resolveId("_MatCapRimColor");
        }
        if (actorLightMatcapParamId_ == 0) {
            actorLightMatcapParamId_ = resolveId("_MatCapParam");
        }
        if (actorLightWorldSpaceCameraPosId_ == 0) {
            actorLightWorldSpaceCameraPosId_ = resolveId("_WorldSpaceCameraPos");
        }
        if (actorLightWorldToCameraId_ == 0) {
            actorLightWorldToCameraId_ = resolveId("unity_WorldToCamera");
        }
        if (actorLightCameraToWorldId_ == 0) {
            actorLightCameraToWorldId_ = resolveId("unity_CameraToWorld");
        }
        if (actorLightDirectionId_ == 0) {
            actorLightDirectionId_ = resolveId("_LightDirection");
        }
        if (actorLightWorldToActorShadowId_ == 0) {
            actorLightWorldToActorShadowId_ = resolveId("_WorldToActorShadow");
        }
    }
    const bool changed = before[0] != actorLightMatcapMainId_ ||
        before[1] != actorLightMatcapRimId_ ||
        before[2] != actorLightLitActorMainId_ ||
        before[3] != actorLightVlSpecColorId_ ||
        before[4] != actorLightGlobalLightParameterId_ ||
        before[5] != actorLightMatcapLightColorId_ ||
        before[6] != actorLightDirectionId_ ||
        before[7] != actorLightWorldToActorShadowId_;
    if (changed) {
        std::ostringstream stream;
        stream << "[VR][shadow] ACTOR_LIGHT_DIAGNOSTIC_READY matcapMain="
               << actorLightMatcapMainId_ << " matcapRim=" << actorLightMatcapRimId_
               << " litMain=" << actorLightLitActorMainId_
               << " litRim=" << actorLightLitActorRimId_
               << " specColor=" << actorLightVlSpecColorId_
               << " glp=" << actorLightGlobalLightParameterId_
               << " lightColor=" << actorLightMatcapLightColorId_
               << " camPos=" << actorLightWorldSpaceCameraPosId_
               << " lightDir=" << actorLightDirectionId_
               << " worldToShadow=" << actorLightWorldToActorShadowId_
               << " getVector=" << (shaderGetGlobalVector_.Ready() ? 1 : 0)
               << " getMatrix="
               << (shaderGetGlobalMatrix_.Ready() ? 1 : 0);
        Log(stream.str());
    }
    return actorLightMatcapMainId_ != 0 || actorLightLitActorMainId_ != 0;
}

void UnityStereoRenderer::RecordActorLightDiagnostics(
    const pose::Pose& headPose) noexcept {
    // One-shot entry trace, before any resolve work: if a run ends with this
    // line but no ACTOR_LIGHT_DIAGNOSTIC sample after it, the crash is inside the
    // diagnostic resolve/read path.
    if (!actorLightDiagnosticEnterLogged_) {
        actorLightDiagnosticEnterLogged_ = true;
        Log("[VR][shadow] ACTOR_LIGHT_DIAGNOSTIC_ENTER first call");
    }
    (void)EnsureActorLightShaderIds();
    const auto readGlobal =
        [this](const MethodRef& getter, std::int32_t id, float* out,
               std::size_t count) -> bool {
        return ReadShaderGlobal(getter, id, out, count);
    };
    const auto appendVector =
        [](std::ostringstream& stream, const char* label, const float* value,
           bool ok) {
            stream << ' ' << label << '=';
            if (!ok) {
                stream << '-';
                return;
            }
            stream << '(' << value[0] << ',' << value[1] << ',' << value[2]
                   << ',' << value[3] << ')';
        };
    float matcapMain[4]{};
    float matcapRim[4]{};
    float litMain[4]{};
    float specColor[4]{};
    float globalLight[4]{};
    float lightColor[4]{};
    float lightDir[4]{};
    float camPos[4]{};
    float shadowMatrix[16]{};
    const bool matcapMainOk = readGlobal(
        shaderGetGlobalVector_, actorLightMatcapMainId_, matcapMain, 4U);
    const bool matcapRimOk =
        readGlobal(shaderGetGlobalVector_, actorLightMatcapRimId_, matcapRim, 4U);
    const bool litMainOk =
        readGlobal(shaderGetGlobalVector_, actorLightLitActorMainId_, litMain, 4U);
    const bool specColorOk = readGlobal(
        shaderGetGlobalVector_, actorLightVlSpecColorId_, specColor, 4U);
    const bool globalLightOk = readGlobal(
        shaderGetGlobalVector_, actorLightGlobalLightParameterId_, globalLight,
        4U);
    const bool lightColorOk = readGlobal(
        shaderGetGlobalVector_, actorLightMatcapLightColorId_, lightColor, 4U);
    const bool lightDirOk = readGlobal(
        shaderGetGlobalVector_, actorLightDirectionId_, lightDir, 4U);
    const bool camPosOk = readGlobal(
        shaderGetGlobalVector_, actorLightWorldSpaceCameraPosId_, camPos, 4U);
    const bool shadowOk = readGlobal(
        shaderGetGlobalMatrix_, actorLightWorldToActorShadowId_, shadowMatrix, 16U);
    const bool headPoseOk = headPose.orientation.x != 0.0F ||
        headPose.orientation.y != 0.0F || headPose.orientation.z != 0.0F ||
        headPose.orientation.w != 0.0F;
    const pose::Vector3 headForward = pose::Rotate(
        headPose.orientation, pose::Vector3{0.0F, 0.0F, 1.0F});
    const pose::Vector3 shotForward = pose::Rotate(
        actorShadowAnchorPose_.orientation, pose::Vector3{0.0F, 0.0F, 1.0F});
    std::ostringstream stream;
    stream << "[VR][shadow] ACTOR_LIGHT_DIAGNOSTIC" << std::fixed
           << std::setprecision(3);
    appendVector(stream, "matcapMain", matcapMain, matcapMainOk);
    appendVector(stream, "matcapRim", matcapRim, matcapRimOk);
    appendVector(stream, "litMain", litMain, litMainOk);
    appendVector(stream, "specColor", specColor, specColorOk);
    appendVector(stream, "glp", globalLight, globalLightOk);
    appendVector(stream, "lightColor", lightColor, lightColorOk);
    appendVector(stream, "lightDir", lightDir, lightDirOk);
    appendVector(stream, "camPos", camPos, camPosOk);
    stream << " shadowM0=";
    if (shadowOk) {
        float shadowSum = 0.0F;
        for (float value : shadowMatrix) {
            shadowSum += std::abs(value);
        }
        // Matrix4x4 is column-major in memory; elements [2],[6],[10] form
        // row 2 of ShadowUV<-World — the light-view z row, i.e. the actual
        // projection axis. Row 0 (the [0],[4],[8] samples inside shadowM0's
        // columns) can roll with the head without the shadow moving, so the
        // verdict on "does the shadow follow the head" must read this row.
        stream << '(' << shadowMatrix[0] << ',' << shadowMatrix[1] << ','
               << shadowMatrix[2] << ',' << shadowMatrix[3]
               << ") shadowRow2=(" << shadowMatrix[2] << ','
               << shadowMatrix[6] << ',' << shadowMatrix[10]
               << ") shadowSum=" << shadowSum;
    } else {
        stream << '-';
    }
    stream << " headFwd=";
    if (headPoseOk) {
        stream << '(' << headForward.x << ',' << headForward.y << ','
               << headForward.z << ')';
    } else {
        stream << '-';
    }
    // VLActorShadow volume readback: directionalType decides how the
    // projected actor shadow derives its virtual light (0=Fixed 1=Matcap
    // 2=ShadowLight). Matcap derives from the rendering camera's rotation,
    // which would match the user's "projection follows head angle, not
    // position" report exactly.
    std::int32_t shadowDirType = -1;
    float shadowLightDir[3]{};
    bool shadowLightDirOk = false;
    if (!actorShadowVolumeResolved_) {
        actorShadowVolumeResolved_ = true;
        auto* actorShadowClass = Il2cppUtils::GetClass(
            "vl-unity.Runtime.dll", "VL.Rendering", "VLActorShadow");
        if (actorShadowClass != nullptr) {
            vlActorShadowType_ = actorShadowClass->GetType();
            actorShadowDirectionalTypeOffset_ =
                FindClassFieldOffset(actorShadowClass, {"directionalType"});
            actorShadowLightDirectionalOffset_ =
                FindClassFieldOffset(actorShadowClass, {"lightDirectional"});
        }
    }
    if (vlActorShadowType_ != nullptr && volumeManagerGetInstance_.Ready() &&
        volumeManagerGetStack_.Ready()) {
        using GetStatic = void* (*)(void*);
        using GetObject = void* (*)(void*, void*);
        void* volumeManager = nullptr;
        void* stack = nullptr;
        if (InvokeManagedResult<void*, GetStatic>(
                volumeManagerGetInstance_, &volumeManager) &&
            volumeManager != nullptr &&
            InvokeManagedResult<void*, GetObject>(
                volumeManagerGetStack_, &stack, volumeManager) &&
            stack != nullptr) {
            void* component = GetVolumeComponent(stack, vlActorShadowType_);
            if (component != nullptr) {
                if (actorShadowDirectionalTypeOffset_ >= 0) {
                    void* typeParam = ReadVolumeParameter(
                        component, actorShadowDirectionalTypeOffset_);
                    if (typeParam != nullptr) {
                        if (volumeParameterEnumValueOffset_ < 0) {
                            volumeParameterEnumValueOffset_ =
                                ResolveVolumeField(
                                    typeParam, {"m_Value", "value"}, {});
                        }
                        if (volumeParameterEnumValueOffset_ >= 0) {
                            (void)ReadManagedField(
                                typeParam, volumeParameterEnumValueOffset_,
                                &shadowDirType);
                        }
                    }
                }
                if (actorShadowLightDirectionalOffset_ >= 0) {
                    void* dirParam = ReadVolumeParameter(
                        component, actorShadowLightDirectionalOffset_);
                    if (dirParam != nullptr) {
                        if (volumeParameterVector3ValueOffset_ < 0) {
                            volumeParameterVector3ValueOffset_ =
                                ResolveVolumeField(
                                    dirParam, {"m_Value", "value"},
                                    {"UnityEngine.Vector3", "Vector3"});
                        }
                        if (volumeParameterVector3ValueOffset_ >= 0) {
                            shadowLightDirOk =
                                ReadManagedField(
                                    dirParam, volumeParameterVector3ValueOffset_,
                                    &shadowLightDir[0]) &&
                                ReadManagedField(
                                    dirParam,
                                    volumeParameterVector3ValueOffset_ +
                                        static_cast<std::int32_t>(sizeof(float)),
                                    &shadowLightDir[1]) &&
                                ReadManagedField(
                                    dirParam,
                                    volumeParameterVector3ValueOffset_ +
                                        static_cast<std::int32_t>(
                                            2U * sizeof(float)),
                                    &shadowLightDir[2]);
                        }
                    }
                }
            }
        }
    }
    stream << " shadowType=";
    if (shadowDirType >= 0) {
        stream << shadowDirType;
    } else {
        stream << '-';
    }
    stream << " lightAuth=";
    if (shadowLightDirOk) {
        stream << '(' << shadowLightDir[0] << ',' << shadowLightDir[1] << ','
               << shadowLightDir[2] << ')';
    } else {
        stream << '-';
    }
    stream << " shotFwd=(" << shotForward.x << ',' << shotForward.y << ','
           << shotForward.z << ") anchorValid="
           << (actorShadowAnchorPoseValid_ ? 1 : 0)
           << " tick=" << actorLightDiagnosticTicks_;
    Log(stream.str());
}

void UnityStereoRenderer::ApplyActorMatcapCompensation(
    void* renderContext, void* renderingData) noexcept {
    // After CampusActorParameterPass.Execute. Rim-only compensate
    // (accepted `.130`) needs the projected-shadow bracket's saved
    // head pose. Authored rim (`.146`) runs when the toon-frame lock
    // is eligible, even if that shadow bracket is off. The matcap
    // frame lock itself lives on VLDeferredPass.RenderActor.
    if (renderContext == nullptr || !IsOwnerThread()) {
        return;
    }
    // Resolve the shader IDs here instead of leaning on the probe: the .89
    // run proved the probe trigger can starve for a whole session, which
    // left this gate stuck on matcapMainId=0 and the compensation inert.
    // Called unconditionally (not just while matcapMainId==0): the .90 run
    // showed the ActorShadowPass IDs (_LightDirection/_WorldToActorShadow)
    // stayed 0 all session because this short-circuited once the MatCap ID
    // was known; Ensure itself is O(1) when everything is resolved.
    (void)EnsureActorLightShaderIds();
    // One-shot gate trace: the .88 crash left zero compensation logs behind,
    // which contradicted every static reading of this function. Record the
    // gate values the first time the bracket is active so the next hardware
    // log settles where execution actually stops.
    if (!matcapCompGateLogged_) {
        matcapCompGateLogged_ = true;
        Log(std::string("[VR][shadow] MATCAP_COMP_GATE context=1 matcapMainId=") +
            std::to_string(actorLightMatcapMainId_) +
            " litMainId=" + std::to_string(actorLightLitActorMainId_) +
            " resolved=" + (matcapCompResolved_ ? "1" : "0"));
    }
    if (!matcapCompResolved_) {
        matcapCompResolved_ = true;
        auto* volumeManagerClass = Il2cppUtils::GetClass(
            "Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering",
            "VolumeManager");
        volumeManagerGetInstance_ = MethodReference(
            FindMethodByName(volumeManagerClass, "get_instance", true, 0U));
        volumeManagerGetStack_ = MethodReference(
            FindMethodByName(volumeManagerClass, "get_stack", false, 0U));
        auto* actorParamClass = Il2cppUtils::GetClass(
            "Unity.RenderPipelines.Universal.Runtime.dll", "VL.Rendering",
            "VLActorParameter");
        if (actorParamClass != nullptr) {
            vlActorParameterType_ = actorParamClass->GetType();
            actorParamMainAngleOffset_ =
                FindClassFieldOffset(actorParamClass, {"mainLightAngle"});
            actorParamMainSpaceOffset_ =
                FindClassFieldOffset(actorParamClass, {"mainLightSpace"});
            actorParamRimAngleOffset_ =
                FindClassFieldOffset(actorParamClass, {"rimAngle"});
            actorParamRimPowerOffset_ =
                FindClassFieldOffset(actorParamClass, {"rimPower"});
            actorParamRimColorOffset_ =
                FindClassFieldOffset(actorParamClass, {"rimColor"});
            // Dump-proven pack (UpdateActorCommand 0x39b): component
            // +0x50 / +0x58 / +0x70. Resolve by candidate names first;
            // fall back to the dumped raw offsets on this build.
            actorParamMatcapOffsetOffset_ = FindClassFieldOffset(
                actorParamClass,
                {"matcapOffset", "matCapOffset", "shadeOffset"});
            if (actorParamMatcapOffsetOffset_ < 0) {
                actorParamMatcapOffsetOffset_ = 0x50;
            }
            actorParamMatcapSmoothOffset_ = FindClassFieldOffset(
                actorParamClass,
                {"matcapSmoothScale", "matCapSmoothScale", "smoothScale"});
            if (actorParamMatcapSmoothOffset_ < 0) {
                actorParamMatcapSmoothOffset_ = 0x58;
            }
            actorParamShadeApplyOffset_ = FindClassFieldOffset(
                actorParamClass,
                {"shadeApplyRatio", "matcapApplyRatio", "applyRatio"});
            if (actorParamShadeApplyOffset_ < 0) {
                actorParamShadeApplyOffset_ = 0x70;
            }
            if (!actorParamFieldsLogged_) {
                actorParamFieldsLogged_ = true;
                std::ostringstream fieldsStream;
                fieldsStream << "[VR][shadow] ACTOR_PARAM_FIELDS";
                for (auto* field : actorParamClass->fields) {
                    if (field != nullptr) {
                        fieldsStream << ' ' << field->name << '='
                                     << field->offset;
                    }
                }
                Log(fieldsStream.str());
            }
            actorParamGiScaleOffset_ =
                FindClassFieldOffset(actorParamClass, {"giScale"});
            actorParamAddLightOffset_ =
                FindClassFieldOffset(actorParamClass, {"additiveLightScale"});
            actorParamAddSpecOffset_ = FindClassFieldOffset(
                actorParamClass, {"additiveLightSpecularScale"});
        }
        auto* litActorClass = Il2cppUtils::GetClass(
            "campus-submodule.Runtime.dll", "Campus.Rendering",
            "CampusLitActor");
        if (litActorClass != nullptr) {
            campusLitActorType_ = litActorClass->GetType();
            litActorMainAngleOffset_ =
                FindClassFieldOffset(litActorClass, {"mainLightAngle"});
            litActorMainSpaceOffset_ =
                FindClassFieldOffset(litActorClass, {"mainLightSpace"});
            litActorRimAngleOffset_ =
                FindClassFieldOffset(litActorClass, {"rimAngle"});
            litActorRimPowerOffset_ =
                FindClassFieldOffset(litActorClass, {"rimPower"});
        }
        auto* commandBufferClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
            "CommandBuffer");
        UnityResolve::Method* setGlobalVector = nullptr;
        if (commandBufferClass != nullptr) {
            for (auto* method : commandBufferClass->methods) {
                if (method == nullptr || method->function == nullptr ||
                    method->static_function ||
                    method->name != "SetGlobalVector" ||
                    method->args.size() != 2U || method->args[0] == nullptr ||
                    method->args[0]->pType == nullptr ||
                    method->args[0]->pType->name != "System.Int32") {
                    continue;
                }
                setGlobalVector = method;
                break;
            }
        }
        commandBufferSetGlobalVector_ = MethodReference(setGlobalVector);
        UnityResolve::Method* setGlobalMatrix = nullptr;
        if (commandBufferClass != nullptr) {
            for (auto* method : commandBufferClass->methods) {
                if (method == nullptr || method->function == nullptr ||
                    method->static_function ||
                    method->name != "SetGlobalMatrix" ||
                    method->args.size() != 2U || method->args[0] == nullptr ||
                    method->args[0]->pType == nullptr ||
                    method->args[0]->pType->name != "System.Int32") {
                    continue;
                }
                setGlobalMatrix = method;
                break;
            }
        }
        commandBufferSetGlobalMatrix_ = MethodReference(setGlobalMatrix);
        commandBufferClear_ = MethodReference(
            FindMethodByName(commandBufferClass, "Clear", false, 0U));
        UnityResolve::Method* setupCameraProperties = nullptr;
        UnityResolve::Method* setViewProjection = nullptr;
        if (commandBufferClass != nullptr) {
            for (auto* method : commandBufferClass->methods) {
                if (method == nullptr || method->function == nullptr ||
                    method->static_function) {
                    continue;
                }
                if (method->name == "SetupCameraProperties") {
                    if (method->args.empty() || method->args[0] == nullptr ||
                        method->args[0]->pType == nullptr) {
                        continue;
                    }
                    const std::string& first = method->args[0]->pType->name;
                    if (first != "UnityEngine.Camera" && first != "Camera") {
                        continue;
                    }
                    bool extraOk = true;
                    for (std::size_t i = 1; i < method->args.size(); ++i) {
                        if (method->args[i] == nullptr ||
                            method->args[i]->pType == nullptr) {
                            extraOk = false;
                            break;
                        }
                        const std::string& extra =
                            method->args[i]->pType->name;
                        if (extra != "System.Boolean" && extra != "Boolean") {
                            extraOk = false;
                            break;
                        }
                    }
                    if (!extraOk) {
                        continue;
                    }
                    // Prefer Camera+bool so stereoSetup can be forced off.
                    if (setupCameraProperties == nullptr ||
                        method->args.size() >
                            setupCameraProperties->args.size()) {
                        setupCameraProperties = method;
                    }
                } else if (
                    method->name == "SetViewProjectionMatrices" ||
                    method->name == "SetViewProjectionMatrices_Injected") {
                    if (method->args.size() != 2U ||
                        method->args[0] == nullptr ||
                        method->args[0]->pType == nullptr ||
                        method->args[1] == nullptr ||
                        method->args[1]->pType == nullptr) {
                        continue;
                    }
                    const std::string& a0 = method->args[0]->pType->name;
                    const std::string& a1 = method->args[1]->pType->name;
                    if (a0.find("Matrix4x4") == std::string::npos ||
                        a1.find("Matrix4x4") == std::string::npos) {
                        continue;
                    }
                    const bool injected =
                        method->name.find("Injected") != std::string::npos;
                    if (setViewProjection == nullptr || injected) {
                        setViewProjection = method;
                        commandBufferSetViewProjectionIsInjected_ = injected;
                    }
                }
            }
        }
        commandBufferSetupCameraProperties_ =
            MethodReference(setupCameraProperties);
        commandBufferSetupCameraPropertiesArgs_ =
            setupCameraProperties != nullptr ? setupCameraProperties->args.size()
                                             : 0U;
        commandBufferSetViewProjectionMatrices_ =
            MethodReference(setViewProjection);
        auto* contextClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine.Rendering",
            "ScriptableRenderContext");
        DumpSetupCameraCandidates(commandBufferClass, "CommandBuffer");
        DumpSetupCameraCandidates(contextClass, "ScriptableRenderContext");
        bool contextSetupStatic = false;
        bool contextSetupStereo = false;
        bool contextSetupEye = false;
        auto* contextSetup = PickSetupCameraProperties(
            contextClass, &contextSetupStatic, &contextSetupStereo,
            &contextSetupEye);
        contextSetupCameraProperties_ = MethodReference(contextSetup);
        contextSetupCameraPropertiesArgs_ =
            contextSetup != nullptr ? contextSetup->args.size() : 0U;
        contextSetupCameraPropertiesIsStatic_ = contextSetupStatic;
        contextSetupCameraPropertiesHasStereo_ = contextSetupStereo;
        contextSetupCameraPropertiesHasEye_ = contextSetupEye;
        contextExecuteCommandBuffer_ = MethodReference(FindMethodByName(
            contextClass, "ExecuteCommandBuffer", false, 1U));
        auto* rendererClass = Il2cppUtils::GetClass(
            "Unity.RenderPipelines.Universal.Runtime.dll",
            "UnityEngine.Rendering.Universal", "ScriptableRenderer");
        UnityResolve::Method* setCameraMatrices = nullptr;
        if (rendererClass != nullptr) {
            for (auto* method : rendererClass->methods) {
                if (method == nullptr || method->function == nullptr ||
                    !method->static_function ||
                    method->name != "SetCameraMatrices" ||
                    method->args.size() != 3U ||
                    method->args[0] == nullptr ||
                    method->args[0]->pType == nullptr ||
                    method->args[2] == nullptr ||
                    method->args[2]->pType == nullptr) {
                    continue;
                }
                const std::string& first = method->args[0]->pType->name;
                const std::string& third = method->args[2]->pType->name;
                if (first.find("CommandBuffer") == std::string::npos) {
                    continue;
                }
                if (method->args[1] == nullptr ||
                    method->args[1]->pType == nullptr ||
                    method->args[1]->pType->name.find("CameraData") ==
                        std::string::npos) {
                    continue;
                }
                if (third != "System.Boolean" && third != "Boolean") {
                    continue;
                }
                setCameraMatrices = method;
                break;
            }
        }
        scriptableRendererSetCameraMatrices_ =
            MethodReference(setCameraMatrices);
        if (commandBufferClass != nullptr &&
            commandBufferSetGlobalVector_.Ready() &&
            commandBufferClear_.Ready() &&
            contextExecuteCommandBuffer_.Ready()) {
            auto* ctor =
                FindMethodByName(commandBufferClass, ".ctor", false, 0U);
            void* buffer = NewIl2CppObject(commandBufferClass->address);
            if (buffer != nullptr && ctor != nullptr) {
                using Ctor = void (*)(void*, void*);
                if (InvokeManagedVoid<Ctor>(MethodReference(ctor), buffer)) {
                    (void)CreateGcHandle(buffer);
                    matcapCompCommandBuffer_ = buffer;
                }
            }
        }
        Log(std::string("[VR][shadow] MATCAP_COMP_API stack=") +
            (volumeManagerGetInstance_.Ready() &&
                     volumeManagerGetStack_.Ready()
                 ? "1"
                 : "0") +
            " paramType=" + (vlActorParameterType_ != nullptr ? "1" : "0") +
            " litType=" + (campusLitActorType_ != nullptr ? "1" : "0") +
            " mainAngleOff=" + std::to_string(actorParamMainAngleOffset_) +
            " spaceOff=" + std::to_string(actorParamMainSpaceOffset_) +
            " rimAngleOff=" + std::to_string(actorParamRimAngleOffset_) +
            " rimPowerOff=" + std::to_string(actorParamRimPowerOffset_) +
            " rimColorOff=" + std::to_string(actorParamRimColorOffset_) +
            " matcapOff=" + std::to_string(actorParamMatcapOffsetOffset_) +
            " smoothOff=" + std::to_string(actorParamMatcapSmoothOffset_) +
            " applyOff=" + std::to_string(actorParamShadeApplyOffset_) +
            " giOff=" + std::to_string(actorParamGiScaleOffset_) +
            " addOff=" + std::to_string(actorParamAddLightOffset_) +
            " addSpecOff=" + std::to_string(actorParamAddSpecOffset_) +
            " litAngleOff=" + std::to_string(litActorMainAngleOffset_) +
            " litSpaceOff=" + std::to_string(litActorMainSpaceOffset_) +
            " litRimOff=" + std::to_string(litActorRimAngleOffset_) +
            " camPosId=" + std::to_string(actorLightWorldSpaceCameraPosId_) +
            " setupCam=" +
            (commandBufferSetupCameraProperties_.Ready() ? "1" : "0") +
            " setupArgs=" +
            std::to_string(commandBufferSetupCameraPropertiesArgs_) +
            " ctxSetup=" +
            (contextSetupCameraProperties_.HasInfo() ? "1" : "0") +
            " ctxArgs=" +
            std::to_string(contextSetupCameraPropertiesArgs_) +
            (contextSetupCameraPropertiesHasStereo_ ? "/st" : "") +
            (contextSetupCameraPropertiesHasEye_ ? "/eye" : "") +
            (contextSetupCameraPropertiesIsStatic_ ? "/static" : "") +
            " setVP=" +
            (commandBufferSetViewProjectionMatrices_.Ready() ? "1" : "0") +
            (commandBufferSetViewProjectionIsInjected_ ? "/inj" : "") +
            " setMats=" +
            (scriptableRendererSetCameraMatrices_.Ready() ? "1" : "0") +
            " cmdBuffer=" +
            (matcapCompCommandBuffer_ != nullptr ? "1" : "0"));
    }
    if (matcapCompCommandBuffer_ == nullptr ||
        !commandBufferSetGlobalVector_.Ready() ||
        !commandBufferClear_.Ready() ||
        !contextExecuteCommandBuffer_.Ready()) {
        return;
    }
    struct UnityVector4Value {
        float x;
        float y;
        float z;
        float w;
    };
    const auto setGlobal = [this](std::int32_t nameId,
                                  const UnityVector4Value& value) -> bool {
        if (nameId == 0) {
            return false;
        }
        std::int32_t localId = nameId;
        UnityVector4Value localValue = value;
        void* arguments[] = {&localId, &localValue};
        void* ignored = nullptr;
        return RuntimeInvoke(
            commandBufferSetGlobalVector_, matcapCompCommandBuffer_,
            arguments, &ignored, nullptr);
    };
    using NoArgumentVoid = void (*)(void*, void*);
    if (!InvokeManagedVoid<NoArgumentVoid>(
            commandBufferClear_, matcapCompCommandBuffer_)) {
        return;
    }

    constexpr float kDegToRad = 0.01745329252F;
    const auto authoredVector = [](float pitchDeg,
                                   float yawDeg) noexcept -> pose::Vector3 {
        const float pitch = pitchDeg * kDegToRad;
        const float yaw = yawDeg * kDegToRad;
        return pose::Vector3{
            std::sin(yaw) * std::cos(pitch),
            std::sin(pitch),
            std::cos(yaw) * std::cos(pitch)};
    };

    pose::Vector3 authoredMain{};
    pose::Vector3 mainOut{};
    bool haveMatcap = false;
    bool wrote = false;
    const char* rimWrite = "-";

    const auto compensateOne = [&](void* component,
                                   std::int32_t mainAngleOffset,
                                   std::int32_t spaceOffset,
                                   std::int32_t rimAngleOffset,
                                   std::int32_t rimPowerOffset,
                                   std::int32_t mainId,
                                   std::int32_t rimId,
                                   pose::Vector3* authoredOut,
                                   pose::Vector3* compensatedOut,
                                   bool calibrate,
                                   bool invert,
                                   bool writeMain) -> bool {
        if (component == nullptr || mainAngleOffset < 0 || mainId == 0) {
            return false;
        }
        const bool toonFrameLocked = IsMatcapFrameVpSwapEligible();
        if (!actorShadowAnchorActive_ && !toonFrameLocked) {
            return false;
        }
        void* mainAngleParam =
            ReadVolumeParameter(component, mainAngleOffset);
        if (mainAngleParam == nullptr) {
            return false;
        }
        if (volumeParameterVector2ValueOffset_ < 0) {
            volumeParameterVector2ValueOffset_ = ResolveVolumeField(
                mainAngleParam, {"m_Value", "value"},
                {"UnityEngine.Vector2", "Vector2"});
            if (volumeParameterVector2ValueOffset_ < 0) {
                volumeParameterVector2ValueOffset_ = ResolveVolumeField(
                    mainAngleParam, {"m_Value", "value"}, {});
            }
        }
        if (volumeParameterVector2ValueOffset_ < 0) {
            return false;
        }
        float mainAngle[2]{};
        if (!ReadManagedField(
                mainAngleParam, volumeParameterVector2ValueOffset_,
                &mainAngle[0]) ||
            !ReadManagedField(
                mainAngleParam,
                volumeParameterVector2ValueOffset_ +
                    static_cast<std::int32_t>(sizeof(float)),
                &mainAngle[1]) ||
            !std::isfinite(mainAngle[0]) || !std::isfinite(mainAngle[1])) {
            return false;
        }
        std::int32_t spaceValue = 0;
        if (spaceOffset >= 0) {
            void* spaceParam = ReadVolumeParameter(component, spaceOffset);
            if (spaceParam != nullptr) {
                if (volumeParameterEnumValueOffset_ < 0) {
                    volumeParameterEnumValueOffset_ = ResolveVolumeField(
                        spaceParam, {"m_Value", "value"}, {});
                }
                if (volumeParameterEnumValueOffset_ >= 0) {
                    (void)ReadManagedField(
                        spaceParam, volumeParameterEnumValueOffset_,
                        &spaceValue);
                }
            }
        }
        if (calibrate && matcapCompAngleOrder_ < 0) {
            float live[4]{};
            if (!ReadShaderGlobal(
                    shaderGetGlobalVector_, mainId, live, 4U)) {
                return false;
            }
            const pose::Vector3 candidate0 =
                authoredVector(mainAngle[0], mainAngle[1]);
            const pose::Vector3 candidate1 =
                authoredVector(mainAngle[1], mainAngle[0]);
            const auto dot = [&](const pose::Vector3& value) noexcept {
                return value.x * live[0] + value.y * live[1] +
                    value.z * live[2];
            };
            const float dot0 = dot(candidate0);
            const float dot1 = dot(candidate1);
            constexpr float kMatchThreshold = 0.9995F;
            if (dot0 >= kMatchThreshold && dot0 >= dot1) {
                matcapCompAngleOrder_ = 0;
            } else if (dot1 >= kMatchThreshold) {
                matcapCompAngleOrder_ = 1;
            } else {
                if (!matcapCompFailLogged_) {
                    matcapCompFailLogged_ = true;
                    std::ostringstream stream;
                    stream << "[VR][shadow] MATCAP_COMP_CALIBRATION_PENDING"
                           << std::fixed << std::setprecision(3)
                           << " angle=(" << mainAngle[0] << ','
                           << mainAngle[1] << ") live=(" << live[0] << ','
                           << live[1] << ',' << live[2] << ") dot0="
                           << dot0 << " dot1=" << dot1;
                    Log(stream.str());
                }
                return false;
            }
            matcapCompSpaceValue_ = spaceValue;
            std::ostringstream stream;
            stream << "[VR][shadow] MATCAP_COMP_CALIBRATED order="
                   << matcapCompAngleOrder_ << " space=" << spaceValue
                   << std::fixed << std::setprecision(3) << " angle=("
                   << mainAngle[0] << ',' << mainAngle[1] << ") dot0="
                   << dot0 << " dot1=" << dot1;
            Log(stream.str());
        }
        if (matcapCompAngleOrder_ < 0) {
            return false;
        }
        if (spaceValue != matcapCompSpaceValue_) {
            if (!matcapCompSpaceSkipLogged_) {
                matcapCompSpaceSkipLogged_ = true;
                Log("[VR][shadow] MATCAP_COMP_SPACE_SKIP calibrated=" +
                    std::to_string(matcapCompSpaceValue_) +
                    " current=" + std::to_string(spaceValue));
            }
            return false;
        }
        const pose::Vector3 authored = matcapCompAngleOrder_ == 0
            ? authoredVector(mainAngle[0], mainAngle[1])
            : authoredVector(mainAngle[1], mainAngle[0]);
        toonAuthoredPitchDeg_ =
            matcapCompAngleOrder_ == 0 ? mainAngle[0] : mainAngle[1];
        toonAuthoredPitchValid_ = std::isfinite(toonAuthoredPitchDeg_);
        // Current (.88–.127): R_head⁻¹·R_shot. Invert: R_head·R_shot⁻¹,
        // which is the view-space transport N_eye·L_eye = N_shot·L_shot
        // if saved=head and anchor=shot (ignoring the S z-flip). Needs
        // the projected-shadow bracket's saved head pose.
        pose::Vector3 compensated = authored;
        pose::Quaternion normalizedDelta{};
        bool haveCompensate = false;
        if (actorShadowAnchorActive_) {
            pose::Quaternion delta = invert
                ? pose::Multiply(
                    actorShadowSavedPose_.orientation,
                    pose::Conjugate(actorShadowAnchorPose_.orientation))
                : pose::Multiply(
                    pose::Conjugate(actorShadowSavedPose_.orientation),
                    actorShadowAnchorPose_.orientation);
            if (!pose::TryNormalize(delta, normalizedDelta)) {
                return false;
            }
            haveCompensate = true;
            const auto compensate =
                [&normalizedDelta](pose::Vector3 value) noexcept -> pose::Vector3 {
                value.z = -value.z;
                value = pose::Rotate(normalizedDelta, value);
                value.z = -value.z;
                return value;
            };
            compensated = compensate(authored);
            if (writeMain && !toonFrameLocked &&
                !setGlobal(
                    mainId,
                    {compensated.x, compensated.y, compensated.z, 0.0F})) {
                return false;
            }
        } else if (writeMain && !toonFrameLocked) {
            return false;
        }
        if (toonFrameLocked) {
            const pose::Vector3 write =
                toonLightViewConstructed_ ? toonLightView_ : authored;
            if (!setGlobal(
                    mainId, {write.x, write.y, write.z, 0.0F})) {
                return false;
            }
        }
        if (rimId != 0 && rimAngleOffset >= 0 && rimPowerOffset >= 0) {
            void* rimAngleParam =
                ReadVolumeParameter(component, rimAngleOffset);
            void* rimPowerParam =
                ReadVolumeParameter(component, rimPowerOffset);
            float rimAngle[2]{};
            float rimPower = 0.0F;
            if (rimAngleParam != nullptr && rimPowerParam != nullptr &&
                ReadManagedField(
                    rimAngleParam, volumeParameterVector2ValueOffset_,
                    &rimAngle[0]) &&
                ReadManagedField(
                    rimAngleParam,
                    volumeParameterVector2ValueOffset_ +
                        static_cast<std::int32_t>(sizeof(float)),
                    &rimAngle[1]) &&
                std::isfinite(rimAngle[0]) &&
                std::isfinite(rimAngle[1])) {
                if (api_.volumeParameterFloatValueOffset >= 0) {
                    (void)ReadManagedField(
                        rimPowerParam,
                        api_.volumeParameterFloatValueOffset, &rimPower);
                }
                const pose::Vector3 authoredRim =
                    matcapCompAngleOrder_ == 0
                    ? authoredVector(rimAngle[0], rimAngle[1])
                    : authoredVector(rimAngle[1], rimAngle[0]);
                // `.146`: when the eye actor GBuffer runs with the shot
                // view matrix (unity_MatrixV swap), the authored rim is
                // already correct in that frame; compensating on top would
                // counter-rotate it and the `.130` lock would follow the
                // head again, inverted.
                const char* rimCameraRole = ClassifyCamera(currentCamera_);
                const bool rimEyeFrameLocked = rimCameraRole != nullptr &&
                    (std::strcmp(rimCameraRole, "left") == 0 ||
                     std::strcmp(rimCameraRole, "right") == 0) &&
                    toonFrameLocked;
                if (rimEyeFrameLocked) {
                    (void)setGlobal(
                        rimId,
                        {authoredRim.x, authoredRim.y, authoredRim.z,
                         rimPower});
                    rimWrite = "authored";
                } else if (haveCompensate) {
                    pose::Vector3 rimCompensated = authoredRim;
                    rimCompensated.z = -rimCompensated.z;
                    rimCompensated =
                        pose::Rotate(normalizedDelta, rimCompensated);
                    rimCompensated.z = -rimCompensated.z;
                    (void)setGlobal(
                        rimId,
                        {rimCompensated.x, rimCompensated.y, rimCompensated.z,
                         rimPower});
                    rimWrite = "compensate";
                }
            }
        }
        if (authoredOut != nullptr) {
            *authoredOut = authored;
        }
        if (compensatedOut != nullptr) {
            *compensatedOut = compensated;
        }
        return true;
    };

    const auto loadStack = [&]() -> void* {
        if (!volumeManagerGetInstance_.Ready() ||
            !volumeManagerGetStack_.Ready()) {
            return nullptr;
        }
        using GetStatic = void* (*)(void*);
        using GetObject = void* (*)(void*, void*);
        void* volumeManager = nullptr;
        void* stack = nullptr;
        if (!InvokeManagedResult<void*, GetStatic>(
                volumeManagerGetInstance_, &volumeManager) ||
            volumeManager == nullptr ||
            !InvokeManagedResult<void*, GetObject>(
                volumeManagerGetStack_, &stack, volumeManager) ||
            stack == nullptr) {
            return nullptr;
        }
        return stack;
    };

    void* stack = loadStack();
    void* component = stack != nullptr
        ? GetVolumeComponent(stack, vlActorParameterType_)
        : nullptr;
    if ((actorShadowAnchorActive_ || IsMatcapFrameVpSwapEligible()) &&
        actorLightMatcapMainId_ != 0) {
        haveMatcap = compensateOne(
            component, actorParamMainAngleOffset_,
            actorParamMainSpaceOffset_, actorParamRimAngleOffset_,
            actorParamRimPowerOffset_, actorLightMatcapMainId_,
            actorLightMatcapRimId_, &authoredMain, &mainOut, true,
            false,
            false);
        wrote = haveMatcap;
    }
    const char* cameraRole = ClassifyCamera(currentCamera_);
    const bool isEyeCamera = cameraRole != nullptr &&
        (std::strcmp(cameraRole, "left") == 0 ||
         std::strcmp(cameraRole, "right") == 0);
    // Eyes-only toon band threshold bias (.138, menu restored in .185).
    // `_MatCapParam` = (offset, smooth, applyRatio, 0) from the volume
    // at +0x50/+0x58/+0x70; GBuffer threshold is `.x`. Source / Grip
    // keep the authored global.
    const float bandBias = GakumasLocal::Config::vrEyeShadeBandBias;
    float matcapParamAuthored[3]{};
    bool bandBiasWritten = false;
    if (isEyeCamera && std::fabs(bandBias) > 0.001F && component != nullptr &&
        actorLightMatcapParamId_ != 0) {
        const auto readFloatParam =
            [&](std::int32_t fieldOffset, float* out) -> bool {
            if (fieldOffset < 0 || out == nullptr) {
                return false;
            }
            void* param = ReadVolumeParameter(component, fieldOffset);
            if (param == nullptr) {
                return false;
            }
            if (api_.volumeParameterFloatValueOffset < 0) {
                api_.volumeParameterFloatValueOffset = ResolveVolumeField(
                    param, {"m_Value", "value"},
                    {"System.Single", "Single"});
            }
            return api_.volumeParameterFloatValueOffset >= 0 &&
                ReadManagedField(
                    param, api_.volumeParameterFloatValueOffset, out) &&
                std::isfinite(*out);
        };
        if (readFloatParam(
                actorParamMatcapOffsetOffset_, &matcapParamAuthored[0]) &&
            readFloatParam(
                actorParamMatcapSmoothOffset_, &matcapParamAuthored[1]) &&
            readFloatParam(
                actorParamShadeApplyOffset_, &matcapParamAuthored[2])) {
            bandBiasWritten = setGlobal(
                actorLightMatcapParamId_,
                {matcapParamAuthored[0] + bandBias, matcapParamAuthored[1],
                 matcapParamAuthored[2], 0.0F});
            wrote = wrote || bandBiasWritten;
        }
    }
    bool customExecuted = false;
    if (wrote) {
        // ScriptableRenderContext is a single-pointer struct passed by
        // value, so the hook's `context` argument *is* the m_Ptr handle.
        // The icall takes a ScriptableRenderContext* self — passing the
        // raw handle crashes at UnityPlayer+0xe6547 (the .88 三连崩).
        void* contextStructSelf = renderContext;
        if (!matcapCompExecLogged_) {
            matcapCompExecLogged_ = true;
            Log("[VR][shadow] MATCAP_COMP_EXEC_FIRST context=1 cmd=1");
        }
        using ExecuteBuffer = void (*)(void*, void*, void*);
        customExecuted = InvokeManagedVoid<ExecuteBuffer>(
            contextExecuteCommandBuffer_, &contextStructSelf,
            matcapCompCommandBuffer_);
    }
    if (!customExecuted) {
        return;
    }
    ++matcapCompApplied_;
    // Time-throttled sample (2 s): the .90 run lost every %900 modulo sample
    // between count=1 and count=10800, so the sampling no longer depends on
    // hitting an exact counter value.
    const auto logNow = std::chrono::steady_clock::now();
    if (matcapCompApplied_ == 1U || logNow >= matcapCompLogAt_) {
        matcapCompLogAt_ = logNow + std::chrono::seconds(2);
        const pose::Vector3 headForward = pose::Rotate(
            actorShadowSavedPose_.orientation, pose::Vector3{0.0F, 0.0F, 1.0F});
        const bool frameEligible = IsMatcapFrameVpSwapEligible();
        const auto formulaName = [this, frameEligible]() noexcept -> const char* {
            if (!frameEligible) {
                return "rim-only";
            }
            switch (toonLightingReference_) {
            case ToonLightingReference::LeftEye:
                return "toon-left-eye";
            case ToonLightingReference::SourcePose:
                return "toon-source";
            case ToonLightingReference::PlayerLookAt:
                return "toon-player-lookat";
            case ToonLightingReference::HeadsetLookAt:
                return "toon-headset-lookat";
            case ToonLightingReference::CinematicLookAt:
                return "toon-cinematic-lookat";
            case ToonLightingReference::Unavailable:
            default:
                return "toon-unavailable";
            }
        };
        std::ostringstream stream;
        stream << "[VR][shadow] MATCAP_COMP_APPLIED count="
               << matcapCompApplied_
               << " formula=" << formulaName()
               << " toonLock="
               << (GakumasLocal::Config::vrActorToonSourceAnchor ? 1 : 0)
               << " followRef=" << GakumasLocal::Config::vrToonFollowRef
               << " constructedL=" << (toonLightViewConstructed_ ? 1 : 0)
               << " camera=" << cameraRole
               << std::fixed << std::setprecision(3);
        if (haveMatcap) {
            stream << " authored=(" << authoredMain.x << ','
                   << authoredMain.y << ',' << authoredMain.z << ") out=("
                   << mainOut.x << ',' << mainOut.y << ',' << mainOut.z
                   << ')';
        }
        stream << " rimWrite=" << rimWrite;
        if (bandBiasWritten) {
            stream << " bandBias=" << bandBias << " matcapParamAuthored=("
                   << matcapParamAuthored[0] << ',' << matcapParamAuthored[1]
                   << ',' << matcapParamAuthored[2] << ')';
        }
        stream << " headFwd=(" << headForward.x << ',' << headForward.y
               << ',' << headForward.z << ')';
        Log(stream.str());
    }
}

void UnityStereoRenderer::LogShadowHeartbeat() noexcept {
    // Deliberately no gates: this runs on whatever thread the campus pass
    // hook runs on, and reports the raw state the gated paths depend on.
    // deltas are "deadline minus now" in ms; negative means overdue (should
    // have fired), "unset" means the zero-initialized time_point.
    const auto now = std::chrono::steady_clock::now();
    const auto deadlineText =
        [&now](std::chrono::steady_clock::time_point at) -> std::string {
        if (at == std::chrono::steady_clock::time_point{}) {
            return "unset";
        }
        return std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(at - now)
                .count());
    };
    std::uint64_t censusTotal = 0;
    for (const auto& perPass : passActivityCounts_) {
        for (const std::uint32_t value : perPass) {
            censusTotal += value;
        }
    }
    std::ostringstream stream;
    stream << "[VR][shadow] SHADOW_HB self=" << static_cast<const void*>(this)
           << " tid=" << GetCurrentThreadId() << " owner=" << ownerThreadId_
           << " cam=" << ClassifyCamera(currentCamera_)
           << " anchorActive=" << (actorShadowAnchorActive_ ? 1 : 0)
           << " anchorValid=" << (actorShadowAnchorPoseValid_ ? 1 : 0)
           << " applied=" << matcapCompApplied_
           << " gbufferLock=" << matcapGbufferLockApplied_
           << " lock="
           << (GakumasLocal::Config::vrActorShadowSourceAnchor ? 1 : 0)
           << " toonLock="
           << (GakumasLocal::Config::vrActorToonSourceAnchor ? 1 : 0)
           << " toonShared=1"
           << " toonPose=" << (toonLightingPoseValid_ ? 1 : 0)
           << " followRef=" << GakumasLocal::Config::vrToonFollowRef
           << " diagnosticCalls=" << actorLightDiagnosticCalls_
           << " census=" << censusTotal
           << " campusS=" << passActivityCounts_[0][0]
           << " printIn=" << deadlineText(passActivityPrintAt_)
           << " diagnosticIn=" << deadlineText(actorLightDiagnosticNextAt_)
           << " ids=" << actorLightMatcapMainId_ << '/' << actorLightMatcapRimId_ << '/'
           << actorLightLitActorMainId_ << '/' << actorLightVlSpecColorId_ << '/'
           << actorLightGlobalLightParameterId_ << '/' << actorLightMatcapLightColorId_
           << '/' << actorLightWorldSpaceCameraPosId_ << '/'
           << actorLightDirectionId_ << '/'
           << actorLightWorldToActorShadowId_;
    Log(stream.str());
}

bool UnityStereoRenderer::EnsureCameraDataFieldOffsets() noexcept {
    if (actorShadowViewPatchResolved_) {
        return renderingDataCameraDataOffset_ >= 0 &&
            cameraDataViewMatrixOffset_ >= 0;
    }
    actorShadowViewPatchResolved_ = true;
    constexpr std::int32_t kBoxedHeader = 0x10;
    auto* renderingDataClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "RenderingData");
    auto* cameraDataClass = Il2cppUtils::GetClass(
        "Unity.RenderPipelines.Universal.Runtime.dll",
        "UnityEngine.Rendering.Universal", "CameraData");
    const std::int32_t cameraDataOffset =
        FindClassFieldOffset(renderingDataClass, {"cameraData"});
    const std::int32_t viewOffset = FindClassFieldOffset(
        cameraDataClass, {"m_ViewMatrix", "viewMatrix"});
    const std::int32_t projOffset = FindClassFieldOffset(
        cameraDataClass, {"m_ProjectionMatrix", "projectionMatrix"});
    const std::int32_t posOffset = FindClassFieldOffset(
        cameraDataClass, {"worldSpaceCameraPos"});
    // il2cpp reports field offsets against the boxed layout; the hook
    // receives a raw struct pointer, so strip the 0x10 object header.
    if (cameraDataOffset >= kBoxedHeader) {
        renderingDataCameraDataOffset_ = cameraDataOffset - kBoxedHeader;
    }
    if (viewOffset >= kBoxedHeader) {
        cameraDataViewMatrixOffset_ = viewOffset - kBoxedHeader;
    }
    if (projOffset >= kBoxedHeader) {
        cameraDataProjectionOffset_ = projOffset - kBoxedHeader;
    }
    if (posOffset >= kBoxedHeader) {
        cameraDataWorldPosOffset_ = posOffset - kBoxedHeader;
    }
    Log(std::string("[VR][shadow] ACTOR_SHADOW_VIEW_PATCH_API rdCamOff=") +
        std::to_string(renderingDataCameraDataOffset_) +
        " viewOff=" + std::to_string(cameraDataViewMatrixOffset_) +
        " projOff=" + std::to_string(cameraDataProjectionOffset_) +
        " posOff=" + std::to_string(cameraDataWorldPosOffset_));
    return renderingDataCameraDataOffset_ >= 0 &&
        cameraDataViewMatrixOffset_ >= 0;
}

bool UnityStereoRenderer::InvokeContextSetupCameraProperties(
    void* renderContext) noexcept {
    if (renderContext == nullptr || currentCamera_ == nullptr ||
        !contextSetupCameraProperties_.HasInfo()) {
        return false;
    }
    void* contextStructSelf = renderContext;
    void* instance = contextSetupCameraPropertiesIsStatic_
        ? nullptr
        : &contextStructSelf;
    if (contextSetupCameraProperties_.Ready() &&
        !contextSetupCameraPropertiesIsStatic_) {
        if (contextSetupCameraPropertiesHasStereo_ &&
            contextSetupCameraPropertiesHasEye_) {
            using Setup3 = void (*)(void*, void*, bool, int, void*);
            return InvokeManagedVoid<Setup3>(
                contextSetupCameraProperties_, &contextStructSelf,
                currentCamera_, false, 0);
        }
        if (contextSetupCameraPropertiesHasStereo_) {
            using Setup2 = void (*)(void*, void*, bool, void*);
            return InvokeManagedVoid<Setup2>(
                contextSetupCameraProperties_, &contextStructSelf,
                currentCamera_, false);
        }
        using Setup1 = void (*)(void*, void*, void*);
        return InvokeManagedVoid<Setup1>(
            contextSetupCameraProperties_, &contextStructSelf, currentCamera_);
    }
    bool stereo = false;
    int eye = 0;
    if (contextSetupCameraPropertiesIsStatic_) {
        if (contextSetupCameraPropertiesHasStereo_ &&
            contextSetupCameraPropertiesHasEye_) {
            void* arguments[] = {renderContext, currentCamera_, &stereo, &eye};
            void* ignored = nullptr;
            return RuntimeInvoke(
                contextSetupCameraProperties_, nullptr, arguments, &ignored,
                nullptr);
        }
        if (contextSetupCameraPropertiesHasStereo_) {
            void* arguments[] = {renderContext, currentCamera_, &stereo};
            void* ignored = nullptr;
            return RuntimeInvoke(
                contextSetupCameraProperties_, nullptr, arguments, &ignored,
                nullptr);
        }
        void* arguments[] = {renderContext, currentCamera_};
        void* ignored = nullptr;
        return RuntimeInvoke(
            contextSetupCameraProperties_, nullptr, arguments, &ignored,
            nullptr);
    }
    if (contextSetupCameraPropertiesHasStereo_ &&
        contextSetupCameraPropertiesHasEye_) {
        void* arguments[] = {currentCamera_, &stereo, &eye};
        void* ignored = nullptr;
        return RuntimeInvoke(
            contextSetupCameraProperties_, instance, arguments, &ignored,
            nullptr);
    }
    if (contextSetupCameraPropertiesHasStereo_) {
        void* arguments[] = {currentCamera_, &stereo};
        void* ignored = nullptr;
        return RuntimeInvoke(
            contextSetupCameraProperties_, instance, arguments, &ignored,
            nullptr);
    }
    void* arguments[] = {currentCamera_};
    void* ignored = nullptr;
    return RuntimeInvoke(
        contextSetupCameraProperties_, instance, arguments, &ignored, nullptr);
}

bool UnityStereoRenderer::BeginActorMatcapCamPosLock(
    void* renderContext, void* renderingData) noexcept {
    if (renderContext == nullptr || renderingData == nullptr ||
        !IsOwnerThread()) {
        return false;
    }
    (void)EnsureActorLightShaderIds();
    pose::Vector3 shotCamPos{};
    float eyeCamPos[3]{};
    bool haveEyeCamPos = false;
    bool mats = false;
    bool vpSwapped = false;
    const char* skip = nullptr;
    const bool locked = UploadMatcapCamPos(
        renderContext, renderingData, /*useShotPosition=*/true, &shotCamPos,
        eyeCamPos, &haveEyeCamPos, &mats, &vpSwapped, &skip);
    if (locked) {
        ++matcapGbufferLockApplied_;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool firstEnter = !matcapGbufferEnterLogged_;
    if (firstEnter) {
        matcapGbufferEnterLogged_ = true;
    }
    if (firstEnter || matcapGbufferLockApplied_ == 1U ||
        now >= matcapGbufferLogAt_) {
        matcapGbufferLogAt_ = now + std::chrono::seconds(2);
        std::ostringstream stream;
        stream << "[VR][shadow] MATCAP_GBUFFER_LOCK"
               << " camera=" << ClassifyCamera(currentCamera_)
               << " lock=" << (locked ? "1" : "0")
               << " count=" << matcapGbufferLockApplied_
               << " via=setglobal mats=" << (mats ? "1" : "0")
               << " vp=" << (vpSwapped ? "1" : "0")
               << std::fixed << std::setprecision(3);
        if (skip != nullptr) {
            stream << " skip=" << skip;
        }
        stream << " shotCamPos=(" << actorShadowAnchorPose_.position.x << ','
               << actorShadowAnchorPose_.position.y << ','
               << actorShadowAnchorPose_.position.z << ')'
               << " lightingCamPos=(" << shotCamPos.x << ',' << shotCamPos.y
               << ',' << shotCamPos.z << ')'
               << " constructedL=" << (toonLightViewConstructed_ ? 1 : 0);
        if (haveEyeCamPos) {
            stream << " eyeCamPos=(" << eyeCamPos[0] << ',' << eyeCamPos[1]
                   << ',' << eyeCamPos[2] << ')';
        }
        Log(stream.str());
    }
    return locked;
}

void UnityStereoRenderer::EndActorMatcapCamPosLock(
    void* renderContext, void* renderingData) noexcept {
    if (renderContext == nullptr || renderingData == nullptr ||
        !IsOwnerThread()) {
        return;
    }
    const char* skip = nullptr;
    if (!UploadMatcapCamPos(
            renderContext, renderingData, /*useShotPosition=*/false, nullptr,
            nullptr, nullptr, nullptr, nullptr, &skip) &&
        !matcapGbufferRestoreFailLogged_) {
        // A failed restore leaves the shot cam pos in the global sheet for
        // the rest of this eye's frame; later passes would shift. Loud, once.
        matcapGbufferRestoreFailLogged_ = true;
        std::ostringstream stream;
        stream << "[VR][shadow] MATCAP_GBUFFER_RESTORE_FAIL skip="
               << (skip != nullptr ? skip : "?");
        Log(stream.str());
    }
}

namespace {
// Unity view matrix from a world pose: V = diag(1,1,-1,1) · [Rᵀ | −Rᵀ·t],
// stored column-major (element(row, col) at [col * 4 + row]). Same math as
// BeginActorShadowViewPatch.
void BuildUnityViewMatrixFromPose(
    const pose::Quaternion& q, const pose::Vector3& t, float out[16]) noexcept {
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    const float r00 = 1.0F - 2.0F * (yy + zz);
    const float r01 = 2.0F * (xy - wz);
    const float r02 = 2.0F * (xz + wy);
    const float r10 = 2.0F * (xy + wz);
    const float r11 = 1.0F - 2.0F * (xx + zz);
    const float r12 = 2.0F * (yz - wx);
    const float r20 = 2.0F * (xz - wy);
    const float r21 = 2.0F * (yz + wx);
    const float r22 = 1.0F - 2.0F * (xx + yy);
    const float tx = -(r00 * t.x + r10 * t.y + r20 * t.z);
    const float ty = -(r01 * t.x + r11 * t.y + r21 * t.z);
    const float tz = -(r02 * t.x + r12 * t.y + r22 * t.z);
    for (int i = 0; i < 16; ++i) {
        out[i] = 0.0F;
    }
    out[0] = r00;
    out[4] = r10;
    out[8] = r20;
    out[12] = tx;
    out[1] = r01;
    out[5] = r11;
    out[9] = r21;
    out[13] = ty;
    out[2] = -r02;
    out[6] = -r12;
    out[10] = -r22;
    out[14] = -tz;
    out[15] = 1.0F;
}

// out = a · b, all column-major (element(row, col) at [col * 4 + row]).
void MultiplyColumnMajor(
    const float a[16], const float b[16], float out[16]) noexcept {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0F;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

// Inverse of a rigid view matrix (orthogonal upper 3x3, even with the -z
// flip): rotation part transposes, translation is −Mᵀ·d.
void InvertRigidViewMatrix(const float in[16], float out[16]) noexcept {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[col * 4 + row] = in[row * 4 + col];
        }
        out[row * 4 + 3] = 0.0F;
    }
    for (int row = 0; row < 3; ++row) {
        out[12 + row] =
            -(out[0 * 4 + row] * in[12] + out[1 * 4 + row] * in[13] +
              out[2 * 4 + row] * in[14]);
    }
    out[15] = 1.0F;
}
}  // namespace

void UnityStereoRenderer::NoteToonFollowIndex(int index) noexcept {
    toonFollowIndex_ = index;
}

void UnityStereoRenderer::NoteToonActorSample(
    int index,
    const pose::Vector3& position,
    const pose::Vector3& forward) noexcept {
    if (index < 0 || !pose::IsFinite(position) || !pose::IsFinite(forward)) {
        return;
    }
    const std::int64_t now = pose::MonotonicNowNanoseconds();
    ToonActorSample* stalest = &toonActors_[0];
    for (auto& slot : toonActors_) {
        if (slot.index == index) {
            slot.position = position;
            slot.forward = forward;
            slot.lastSeenNanoseconds = now;
            return;
        }
        if (slot.lastSeenNanoseconds < stalest->lastSeenNanoseconds) {
            stalest = &slot;
        }
    }
    stalest->index = index;
    stalest->position = position;
    stalest->forward = forward;
    stalest->lastSeenNanoseconds = now;
}

bool UnityStereoRenderer::TrySelectToonActor(
    const pose::Vector3& reference,
    bool firstPerson,
    pose::Vector3& position,
    pose::Vector3& forward,
    bool& frontOnly) const noexcept {
    frontOnly = false;
    const std::int64_t now = pose::MonotonicNowNanoseconds();
    constexpr std::int64_t kStaleNanoseconds = 1'000'000'000LL;
    const auto horizontalSq = [](const pose::Vector3& a,
                                 const pose::Vector3& b) noexcept {
        const float dx = a.x - b.x;
        const float dz = a.z - b.z;
        return dx * dx + dz * dz;
    };
    const ToonActorSample* nearest = nullptr;
    const ToonActorSample* nearestOther = nullptr;
    const ToonActorSample* self = nullptr;
    float nearestSq = 0.0F;
    float nearestOtherSq = 0.0F;
    for (const auto& slot : toonActors_) {
        if (slot.index < 0 ||
            now - slot.lastSeenNanoseconds >= kStaleNanoseconds ||
            !pose::IsFinite(slot.position)) {
            continue;
        }
        const float distSq = horizontalSq(reference, slot.position);
        if (slot.index == toonFollowIndex_) {
            self = &slot;
        } else if (nearestOther == nullptr || distSq < nearestOtherSq) {
            nearestOther = &slot;
            nearestOtherSq = distSq;
        }
        if (nearest == nullptr || distSq < nearestSq) {
            nearest = &slot;
            nearestSq = distSq;
        }
    }
    const ToonActorSample* picked = nullptr;
    if (firstPerson) {
        if (nearestOther != nullptr) {
            picked = nearestOther;
        } else if (self != nullptr) {
            picked = self;
            frontOnly = true;
        }
    } else {
        picked = nearest;
        if (picked == nullptr && self != nullptr) {
            picked = self;
        }
    }
    if (picked == nullptr) {
        return false;
    }
    position = picked->position;
    forward = picked->forward;
    return pose::IsFinite(position);
}

void UnityStereoRenderer::RefreshToonLightingState() noexcept {
    const bool freeCam = camera::IsVrFreeCameraLocomotionActive();
    toonLightingReference_ = ToonLightingReference::Unavailable;
    // Stereo coherence is below the setting: lock off follows one live left-eye
    // frame, never two per-eye frames. Lock on retains the independent target
    // behavior (source rig, or explicit player/headset look-at).
    if (!GakumasLocal::Config::vrActorToonSourceAnchor) {
        toonLightingReference_ = ToonLightingReference::LeftEye;
        toonPauseFrozen_ = false;
        toonLastLookYawValid_ = false;
        toonWasFreeCam_ = false;
        toonFrozenPitchValid_ = false;
        toonLightViewConstructed_ = false;
        if (leftEyePoseValid_ && IsFinitePose(leftEyePose_)) {
            toonLightingPose_ = leftEyePose_;
            toonLightingPoseValid_ = true;
        } else {
            toonLightingPoseValid_ = false;
        }
        return;
    }
    // Lock target 0 = sourcePose / free-camera rig. 1/2 = look-at from
    // player/headset, free-camera only. Both eyes consume the same result.
    const bool lookAt =
        freeCam &&
        GakumasLocal::Config::vrToonFollowRef !=
            GakumasLocal::Config::kVrToonFollowRefSource;
    if (lookAt && !toonWasFreeCam_) {
        toonFrozenPitchDeg_ =
            toonAuthoredPitchValid_ ? toonAuthoredPitchDeg_ : 0.0F;
        toonFrozenPitchValid_ = true;
    }
    if (!lookAt) {
        toonFrozenPitchValid_ = false;
        toonLastLookYawValid_ = false;
        toonLightViewConstructed_ = false;
    }
    toonWasFreeCam_ = lookAt;

    if (!lookAt) {
        toonLightingReference_ = ToonLightingReference::SourcePose;
        if (playerPoseValid_ && IsFinitePose(playerPose_)) {
            toonLightingPose_ = playerPose_;
            toonLightingPoseValid_ = true;
            toonLightViewConstructed_ = false;
        } else {
            toonLightingPoseValid_ = false;
        }
        return;
    }

    pose::Vector3 ref{};
    bool haveRef = false;
    if (GakumasLocal::Config::vrToonFollowRef ==
            GakumasLocal::Config::kVrToonFollowRefHeadset &&
        headsetPoseValid_) {
        ref = headsetPose_.position;
        haveRef = pose::IsFinite(ref);
        if (haveRef) {
            toonLightingReference_ = ToonLightingReference::HeadsetLookAt;
        }
    }
    if (!haveRef && playerPoseValid_) {
        ref = playerPose_.position;
        haveRef = pose::IsFinite(ref);
        if (haveRef) {
            toonLightingReference_ = ToonLightingReference::PlayerLookAt;
        }
    }
    if (!haveRef && cinematicPoseValid_) {
        ref = cinematicPose_.position;
        haveRef = pose::IsFinite(ref);
        if (haveRef) {
            toonLightingReference_ = ToonLightingReference::CinematicLookAt;
        }
    }
    if (!haveRef) {
        toonLightingPoseValid_ = false;
        return;
    }

    constexpr float kDegToRad = 0.01745329252F;
    constexpr float kDegenerateHorizontal = 0.05F;
    const float pitchRad =
        (toonFrozenPitchValid_ ? toonFrozenPitchDeg_ : 0.0F) * kDegToRad;
    pose::Vector3 actorPos{};
    pose::Vector3 actorFwd{};
    bool frontOnly = false;
    const bool haveActor = TrySelectToonActor(
        ref,
        camera::IsVrFreeCameraFirstPerson(),
        actorPos,
        actorFwd,
        frontOnly);
    float yawRad = 0.0F;
    bool haveYaw = false;
    if (haveActor) {
        float hx = 0.0F;
        float hz = 0.0F;
        if (frontOnly) {
            hx = actorFwd.x;
            hz = actorFwd.z;
        } else {
            hx = actorPos.x - ref.x;
            hz = actorPos.z - ref.z;
        }
        const float horizontal = std::sqrt(hx * hx + hz * hz);
        if (std::isfinite(horizontal) && horizontal >= kDegenerateHorizontal) {
            yawRad = std::atan2(hx, hz);
            haveYaw = std::isfinite(yawRad);
        }
    }
    if (!haveYaw && toonLastLookYawValid_) {
        yawRad = toonLastLookYawRad_;
        haveYaw = true;
    }
    pose::Quaternion orient{};
    if (haveYaw && pose::TryYawPitchOrientation(yawRad, pitchRad, orient)) {
        toonLastLookYawRad_ = yawRad;
        toonLastLookYawValid_ = true;
        toonLightingPose_.position = ref;
        toonLightingPose_.orientation = orient;
        toonLightingPoseValid_ = IsFinitePose(toonLightingPose_);
        toonLightView_ = {0.0F, 0.0F, 1.0F};
        toonLightViewConstructed_ = true;
        return;
    }
    if (toonLightingPoseValid_) {
        toonLightingPose_.position = ref;
        toonLightViewConstructed_ = true;
        return;
    }
    if (playerPoseValid_) {
        toonLightingPose_ = playerPose_;
        toonLightingPose_.position = ref;
        toonLightingPoseValid_ = IsFinitePose(toonLightingPose_);
        toonLightView_ = {0.0F, 0.0F, 1.0F};
        toonLightViewConstructed_ = true;
        return;
    }
    toonLightingPoseValid_ = false;
}

// `.144`: upload `_WorldSpaceCameraPos` with a plain recorded
// SetGlobalVector, exactly the way URP's own SetCameraMatrices publishes
// it. `.145` extends the same bracket with SetGlobalMatrix on
// unity_WorldToCamera / unity_CameraToWorld: `.144` hardware killed the
// distance follow but head *pitch* still moved the band — the matcap frame
// rows (cb0[65–67]) live in the same SetGlobal-published $Globals cb and
// were still the eye view. The native unity_MatrixV/VP used by the vertex
// path is a separate cbuffer and stays untouched, so stereo is safe. `.143` proved the transform-poke + SetupCameraProperties pair is a
// no-op here: ScriptableRenderContext calls are deferred until Submit, so
// the queued Setup reads the already-restored eye transform, and
// SetCameraMatrices(setInverseMatrices=true) re-sets the global from
// cameraData.worldSpaceCameraPos (eye) on top. A command buffer records
// its *values* at call time and the context queue preserves call order,
// so a SetGlobalVector executed at RenderActor time lands after the
// official setupCamera and right before the actor GBuffer draws. `.139`
// failed with the same write only because it recorded at BeforeRendering,
// ahead of the official overwrite.
// Full eligibility for the `.146` unity_MatrixV swap (camera role is the
// caller's check). Shared with the rim-compensation gate so the parameter
// pass and the GBuffer bracket agree on whether the eye actor draws run in
// the shot frame.
bool UnityStereoRenderer::TryCurrentToonLightingPose(
    pose::Pose& out) noexcept {
    RefreshToonLightingState();
    const char* cameraRole = ClassifyCamera(currentCamera_);
    if (cameraRole == nullptr ||
        (std::strcmp(cameraRole, "left") != 0 &&
         std::strcmp(cameraRole, "right") != 0)) {
        return false;
    }
    if (!toonLightingPoseValid_ || !IsFinitePose(toonLightingPose_)) {
        return false;
    }
    out = toonLightingPose_;
    return true;
}

bool UnityStereoRenderer::IsMatcapFrameVpSwapEligible() noexcept {
    RefreshToonLightingState();
    if (!toonLightingPoseValid_ ||
        actorLightWorldSpaceCameraPosId_ == 0) {
        return false;
    }
    if (matcapCompCommandBuffer_ == nullptr ||
        !commandBufferSetGlobalVector_.Ready() ||
        !commandBufferClear_.Ready() ||
        !contextExecuteCommandBuffer_.Ready() ||
        !commandBufferSetViewProjectionMatrices_.Ready()) {
        return false;
    }
    if (!EnsureCameraDataFieldOffsets() ||
        renderingDataCameraDataOffset_ < 0 || cameraDataWorldPosOffset_ < 0 ||
        cameraDataViewMatrixOffset_ < 0 || cameraDataProjectionOffset_ < 0) {
        return false;
    }
    const pose::Vector3& shot = toonLightingPose_.position;
    return std::isfinite(shot.x) && std::isfinite(shot.y) &&
        std::isfinite(shot.z);
}

bool UnityStereoRenderer::UploadMatcapCamPos(
    void* renderContext,
    void* renderingData,
    bool useShotPosition,
    pose::Vector3* shotOut,
    float* eyeCamPosOut,
    bool* haveEyeCamPosOut,
    bool* matsOut,
    bool* vpOut,
    const char** skipOut) noexcept {
    if (skipOut != nullptr) {
        *skipOut = nullptr;
    }
    if (haveEyeCamPosOut != nullptr) {
        *haveEyeCamPosOut = false;
    }
    if (matsOut != nullptr) {
        *matsOut = false;
    }
    if (vpOut != nullptr) {
        *vpOut = false;
    }
    const auto fail = [skipOut](const char* reason) noexcept {
        if (skipOut != nullptr) {
            *skipOut = reason;
        }
        return false;
    };
    const char* cameraRole = ClassifyCamera(currentCamera_);
    const bool isEye = cameraRole != nullptr &&
        (std::strcmp(cameraRole, "left") == 0 ||
         std::strcmp(cameraRole, "right") == 0);
    if (!isEye) {
        return fail("not-eye");
    }
    // The lighting pose is resolved per frame (left eye / player / headset).
    // Do not require actorShadowAnchorActive_.
    if (actorLightWorldSpaceCameraPosId_ == 0) {
        return fail("id-or-pose");
    }
    if (matcapCompCommandBuffer_ == nullptr ||
        !commandBufferSetGlobalVector_.Ready() ||
        !commandBufferClear_.Ready() ||
        !contextExecuteCommandBuffer_.Ready()) {
        return fail("no-cmd-api");
    }
    pose::Pose lightingPose{};
    if (useShotPosition &&
        !TryCurrentToonLightingPose(lightingPose)) {
        return fail("bad-lighting");
    }
    const pose::Vector3 shot = useShotPosition
        ? lightingPose.position
        : actorShadowAnchorPose_.position;
    if (useShotPosition &&
        (!std::isfinite(shot.x) || !std::isfinite(shot.y) ||
         !std::isfinite(shot.z))) {
        return fail("bad-shot");
    }
    if (shotOut != nullptr) {
        *shotOut = lightingPose.position;
    }
    // The eye value for the post-draw restore comes from the frame's own
    // cameraData (worldSpaceCameraPos, `.142` posOff=452), not from a
    // GetGlobalVector readback — the sheet lags the deferred queue.
    if (!EnsureCameraDataFieldOffsets() ||
        renderingDataCameraDataOffset_ < 0 || cameraDataWorldPosOffset_ < 0) {
        return fail("no-camdata");
    }
    auto* cameraData = static_cast<std::uint8_t*>(renderingData) +
        renderingDataCameraDataOffset_;
    float eyePos[3]{};
    std::memcpy(
        eyePos, cameraData + cameraDataWorldPosOffset_, sizeof(eyePos));
    if (!std::isfinite(eyePos[0]) || !std::isfinite(eyePos[1]) ||
        !std::isfinite(eyePos[2])) {
        return fail("bad-eye-pos");
    }
    if (eyeCamPosOut != nullptr) {
        std::memcpy(eyeCamPosOut, eyePos, sizeof(eyePos));
        if (haveEyeCamPosOut != nullptr) {
            *haveEyeCamPosOut = true;
        }
    }

    using NoArgumentVoid = void (*)(void*, void*);
    if (!InvokeManagedVoid<NoArgumentVoid>(
            commandBufferClear_, matcapCompCommandBuffer_)) {
        return fail("clear-fail");
    }
    struct UnityVector4Value {
        float x;
        float y;
        float z;
        float w;
    };
    UnityVector4Value value{};
    if (useShotPosition) {
        value = UnityVector4Value{
            lightingPose.position.x, lightingPose.position.y,
            lightingPose.position.z, 0.0F};
    } else {
        value = UnityVector4Value{eyePos[0], eyePos[1], eyePos[2], 0.0F};
    }
    std::int32_t nameId =
        static_cast<std::int32_t>(actorLightWorldSpaceCameraPosId_);
    void* arguments[] = {&nameId, &value};
    void* ignored = nullptr;
    if (!RuntimeInvoke(
            commandBufferSetGlobalVector_, matcapCompCommandBuffer_,
            arguments, &ignored, nullptr)) {
        return fail("set-fail");
    }
    // `.145`: matcap frame rotation via unity_WorldToCamera — hardware
    // showed the band's pitch follow survived it, and `.144` proved this
    // same bracket does reach the draws: the frame rows cb0[65–67] are the
    // *native* unity_MatrixV, which only SetViewProjectionMatrices writes.
    // Kept so unity_WorldToCamera stays consistent with the `.146` swap.
    if (commandBufferSetGlobalMatrix_.Ready() && actorLightWorldToCameraId_ != 0 &&
        actorLightCameraToWorldId_ != 0 && cameraDataViewMatrixOffset_ >= 0) {
        float view[16]{};
        if (useShotPosition) {
            BuildUnityViewMatrixFromPose(
                lightingPose.orientation, lightingPose.position, view);
        } else {
            std::memcpy(
                view, cameraData + cameraDataViewMatrixOffset_, sizeof(view));
        }
        if (std::isfinite(view[0]) && std::isfinite(view[15]) &&
            std::fabs(view[15] - 1.0F) <= 0.01F) {
            float inverse[16]{};
            InvertRigidViewMatrix(view, inverse);
            const auto setMatrix = [this](std::int32_t matrixId,
                                          float (&matrix)[16]) -> bool {
                std::int32_t localId = matrixId;
                void* matrixArguments[] = {&localId, matrix};
                void* matrixIgnored = nullptr;
                return RuntimeInvoke(
                    commandBufferSetGlobalMatrix_, matcapCompCommandBuffer_,
                    matrixArguments, &matrixIgnored, nullptr);
            };
            const bool matsWritten = setMatrix(actorLightWorldToCameraId_, view) &&
                setMatrix(actorLightCameraToWorldId_, inverse);
            if (matsOut != nullptr) {
                *matsOut = matsWritten;
            }
        }
    }
    // `.146` / `.266`: swap native unity_MatrixV for the actor draws
    // while keeping the rasterized VP numerically identical.
    // Both eyes share one lighting pose (IPD = 0). P' =
    // eyeProj·eyeView·lightingView⁻¹ so GPU(P')·lightingView =
    // GPU(eyeProj)·eyeView — vertices stay on the official stereo path.
    // Restore writes (eyeView, eyeProj) back.
    if (IsMatcapFrameVpSwapEligible()) {
        float eyeView[16]{};
        float eyeProj[16]{};
        std::memcpy(
            eyeView, cameraData + cameraDataViewMatrixOffset_,
            sizeof(eyeView));
        std::memcpy(
            eyeProj, cameraData + cameraDataProjectionOffset_,
            sizeof(eyeProj));
        const bool eyeOk = std::isfinite(eyeView[0]) &&
            std::isfinite(eyeView[15]) &&
            std::fabs(eyeView[15] - 1.0F) <= 0.01F &&
            std::isfinite(eyeProj[0]) && std::isfinite(eyeProj[5]) &&
            std::fabs(eyeProj[0]) >= 1.0e-6F;
        if (eyeOk) {
            const auto recordSetVp = [this](float (&viewM)[16],
                                            float (&projM)[16]) -> bool {
                if (commandBufferSetViewProjectionIsInjected_) {
                    using SetVP = void (*)(void*, void*, void*, void*);
                    return InvokeManagedVoid<SetVP>(
                        commandBufferSetViewProjectionMatrices_,
                        matcapCompCommandBuffer_, viewM, projM);
                }
                void* vpArgs[] = {viewM, projM};
                void* vpIgnored = nullptr;
                return RuntimeInvoke(
                    commandBufferSetViewProjectionMatrices_,
                    matcapCompCommandBuffer_, vpArgs, &vpIgnored, nullptr);
            };
            bool vpWritten = false;
            if (useShotPosition) {
                float lightingView[16]{};
                BuildUnityViewMatrixFromPose(
                    lightingPose.orientation, lightingPose.position,
                    lightingView);
                float lightingViewInverse[16]{};
                InvertRigidViewMatrix(lightingView, lightingViewInverse);
                float projView[16]{};
                MultiplyColumnMajor(eyeProj, eyeView, projView);
                float projPrime[16]{};
                MultiplyColumnMajor(projView, lightingViewInverse, projPrime);
                vpWritten = recordSetVp(lightingView, projPrime);
            } else {
                vpWritten = recordSetVp(eyeView, eyeProj);
            }
            if (vpOut != nullptr) {
                *vpOut = vpWritten;
            }
        }
    }
    void* contextStructSelf = renderContext;
    using ExecuteBuffer = void (*)(void*, void*, void*);
    if (!InvokeManagedVoid<ExecuteBuffer>(
            contextExecuteCommandBuffer_, &contextStructSelf,
            matcapCompCommandBuffer_)) {
        return fail("exec-fail");
    }
    return true;
}

bool UnityStereoRenderer::BeginActorShadowViewPatch(
    void* renderingData) noexcept {
    // Augments an active anchor bracket only: same toggle, same shot pose.
    if (renderingData == nullptr || !actorShadowAnchorActive_ ||
        !IsOwnerThread() || actorShadowViewPatchActive_) {
        return false;
    }
    if (!EnsureCameraDataFieldOffsets() ||
        renderingDataCameraDataOffset_ < 0 || cameraDataViewMatrixOffset_ < 0) {
        return false;
    }
    // Unity view matrix from the anchored shot pose:
    // V = diag(1,1,-1,1) · [Rᵀ | −Rᵀ·t], stored column-major.
    const pose::Quaternion& q = actorShadowAnchorPose_.orientation;
    const pose::Vector3& t = actorShadowAnchorPose_.position;
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    // camera→world rotation R(row, col)
    const float r00 = 1.0F - 2.0F * (yy + zz);
    const float r01 = 2.0F * (xy - wz);
    const float r02 = 2.0F * (xz + wy);
    const float r10 = 2.0F * (xy + wz);
    const float r11 = 1.0F - 2.0F * (xx + zz);
    const float r12 = 2.0F * (yz - wx);
    const float r20 = 2.0F * (xz - wy);
    const float r21 = 2.0F * (yz + wx);
    const float r22 = 1.0F - 2.0F * (xx + yy);
    // world→camera: rows of Rᵀ; translation −Rᵀ·t.
    const float tx = -(r00 * t.x + r10 * t.y + r20 * t.z);
    const float ty = -(r01 * t.x + r11 * t.y + r21 * t.z);
    const float tz = -(r02 * t.x + r12 * t.y + r22 * t.z);
    float shotView[16]{};
    // element(row, col) lives at [col * 4 + row]
    shotView[0] = r00;
    shotView[4] = r10;
    shotView[8] = r20;
    shotView[12] = tx;
    shotView[1] = r01;
    shotView[5] = r11;
    shotView[9] = r21;
    shotView[13] = ty;
    shotView[2] = -r02;
    shotView[6] = -r12;
    shotView[10] = -r22;
    shotView[14] = -tz;
    shotView[15] = 1.0F;
    auto* cameraData = static_cast<std::uint8_t*>(renderingData) +
        renderingDataCameraDataOffset_;
    float* view =
        reinterpret_cast<float*>(cameraData + cameraDataViewMatrixOffset_);
    std::memcpy(actorShadowViewPatchSavedView_, view, sizeof(float) * 16U);
    std::memcpy(view, shotView, sizeof(float) * 16U);
    if (cameraDataWorldPosOffset_ >= 0) {
        float* worldPos =
            reinterpret_cast<float*>(cameraData + cameraDataWorldPosOffset_);
        std::memcpy(
            actorShadowViewPatchSavedPos_, worldPos, sizeof(float) * 3U);
        worldPos[0] = t.x;
        worldPos[1] = t.y;
        worldPos[2] = t.z;
    }
    actorShadowViewPatchActive_ = true;
    const auto now = std::chrono::steady_clock::now();
    if (now >= actorShadowViewPatchLogAt_) {
        actorShadowViewPatchLogAt_ = now + std::chrono::seconds(2);
        std::ostringstream stream;
        stream << "[VR][shadow] ACTOR_SHADOW_VIEW_PATCH" << std::fixed
               << std::setprecision(3) << " headRow2=("
               << actorShadowViewPatchSavedView_[2] << ','
               << actorShadowViewPatchSavedView_[6] << ','
               << actorShadowViewPatchSavedView_[10] << ") shotRow2=("
               << shotView[2] << ',' << shotView[6] << ',' << shotView[10]
               << ')';
        Log(stream.str());
    }
    return true;
}

void UnityStereoRenderer::EndActorShadowViewPatch(
    void* renderingData) noexcept {
    if (!actorShadowViewPatchActive_) {
        return;
    }
    actorShadowViewPatchActive_ = false;
    if (renderingData == nullptr || renderingDataCameraDataOffset_ < 0 ||
        cameraDataViewMatrixOffset_ < 0) {
        return;
    }
    auto* cameraData = static_cast<std::uint8_t*>(renderingData) +
        renderingDataCameraDataOffset_;
    float* view =
        reinterpret_cast<float*>(cameraData + cameraDataViewMatrixOffset_);
    std::memcpy(view, actorShadowViewPatchSavedView_, sizeof(float) * 16U);
    if (cameraDataWorldPosOffset_ >= 0) {
        float* worldPos =
            reinterpret_cast<float*>(cameraData + cameraDataWorldPosOffset_);
        std::memcpy(
            worldPos, actorShadowViewPatchSavedPos_, sizeof(float) * 3U);
    }
}

bool UnityStereoRenderer::ShouldSuppressDepthOfField(
    void* volumeComponent,
    bool originalActive) noexcept {
    const bool sameThread =
        ownerThreadId_ == 0U || ownerThreadId_ == GetCurrentThreadId();
    const bool byInstance = volumeComponent != nullptr &&
        (volumeComponent == eyeDofComponents_[0] ||
         volumeComponent == eyeDofComponents_[1] ||
         volumeComponent == eyeUrpDofComponents_[0] ||
         volumeComponent == eyeUrpDofComponents_[1]);
    const bool byCamera = sameThread && currentCamera_ != nullptr &&
        (currentCamera_ == eyeCameras_[0] || currentCamera_ == eyeCameras_[1]);
    const bool afterSource = sameThread && stage_ == LadderStage::StereoFull &&
        (expectOwnedEyeDof_ || sourceCameraSuppressed_);
    const bool suppress = byInstance || byCamera || afterSource;
    ++vlDofIsActiveQueries_;
    if (vlDofIsActiveQueries_ == 1U ||
        vlDofIsActiveQueries_ % 300U == 0U) {
        Log("[VR][stereo] VLDOF_IS_ACTIVE_QUERY count=" +
            std::to_string(vlDofIsActiveQueries_) +
            " originalActive=" + (originalActive ? "1" : "0") +
            " byInstance=" + (byInstance ? "1" : "0") +
            " byCamera=" + (byCamera ? "1" : "0") +
            " afterSource=" + (afterSource ? "1" : "0") +
            " self=" +
            std::to_string(reinterpret_cast<std::uintptr_t>(volumeComponent)) +
            " source=" +
            std::to_string(reinterpret_cast<std::uintptr_t>(sourceDofComponent_)) +
            " currentIsEye=" + (byCamera ? "1" : "0") +
            " suppress=" + (suppress ? "1" : "0"));
    }
    if (!suppress) {
        return false;
    }
    ++eyeDofSuppressQueries_;
    if (originalActive && !eyeDofActiveSuppressedLogged_) {
        eyeDofActiveSuppressedLogged_ = true;
        Log("[VR][stereo] EYE_VLDOF_SUPPRESSED_ACTIVE byInstance=" +
            std::string(byInstance ? "1" : "0") +
            " byCamera=" + (byCamera ? "1" : "0") +
            " afterSource=" + (afterSource ? "1" : "0"));
    }
    if (eyeDofSuppressQueries_ == 1U ||
        eyeDofSuppressQueries_ % 300U == 0U) {
        Log("[VR][stereo] EYE_VLDOF_SUPPRESSED count=" +
            std::to_string(eyeDofSuppressQueries_) +
            " originalActive=" + (originalActive ? "1" : "0") +
            " byInstance=" + (byInstance ? "1" : "0") +
            " byCamera=" + (byCamera ? "1" : "0") +
            " afterSource=" + (afterSource ? "1" : "0"));
    }
    return true;
}

bool UnityStereoRenderer::ConfigureCamera(
    std::size_t eye,
    void* sourceCamera,
    void* target,
    bool sceneEnabled,
    const UnityStereoCameraFrame& frame,
    float sourceDepth) noexcept {
    using CopyFrom = void (*)(void*, void*, void*);
    using SetTarget = void (*)(void*, void*, void*);
    using SetMask = void (*)(void*, int, void*);
    using SetDepth = void (*)(void*, float, void*);
    using GetClip = float (*)(void*, void*);
    using SetClip = void (*)(void*, float, void*);
    const std::string eyeName = eye == 0U ? "left" : "right";
    Log("[VR][stereo] CALL_BEGIN stage=configure.copy eye=" + eyeName);
    // A disabled source is no longer a valid temporal donor.
    // Camera.CopyFrom copies previousViewProjection / non-jittered
    // projection / explicit worldToCamera from that frozen camera onto
    // the eyes, so TAA jitters (outlines get fat) but reprojects against
    // the last cinematic VP and smears moving characters.
    if (sourceCameraSuppressed_) {
        using GetMask = int (*)(void*, void*);
        int sourceMask = 0;
        if (!InvokeManagedResult<int, GetMask>(
                api_.cameraGetCullingMask, &sourceMask, sourceCamera) ||
            !InvokeManagedVoid<SetMask>(
                api_.cameraSetCullingMask, eyeCameras_[eye], sourceMask)) {
            return FailStage("configure.copy-culling", eye);
        }
        if (api_.cameraResetWorldToCameraMatrix.Ready()) {
            using Reset = void (*)(void*, void*);
            if (!InvokeManagedVoid<Reset>(
                    api_.cameraResetWorldToCameraMatrix, eyeCameras_[eye])) {
                return FailStage("configure.reset-world-to-camera", eye);
            }
        }
        if (!copyFromSkippedLogged_) {
            copyFromSkippedLogged_ = true;
            Log("[VR][stereo] COPY_FROM_SKIPPED reason=source-suppressed");
        }
        Log("[VR][stereo] CALL_OK stage=configure.copy eye=" + eyeName +
            " mode=skip-source-suppressed");
    } else if (!InvokeManagedVoid<CopyFrom>(api_.cameraCopyFrom,
                                           eyeCameras_[eye], sourceCamera)) {
        return FailStage("configure.copy", eye);
    } else {
        Log("[VR][stereo] CALL_OK stage=configure.copy eye=" + eyeName);
    }
    Log("[VR][stereo] CALL_BEGIN stage=configure.target eye=" + eyeName);
    if (!InvokeManagedVoid<SetTarget>(api_.cameraSetTargetTexture,
                                     eyeCameras_[eye], target)) {
        return FailStage("configure.target", eye);
    }
    RecordLifetimeWrite(
        LifetimeWriteCategory::TargetTexture, "eye-bind", eyeCameras_[eye],
        target, static_cast<std::intptr_t>(eye));
    Log("[VR][stereo] CALL_OK stage=configure.target eye=" + eyeName);
    if (sceneEnabled) {
        if (eye < frame.trackingSample.eyes.size() &&
            IsFiniteFov(frame.trackingSample.eyes[eye].fov)) {
            camera::ProjectionIntrinsics eyeIntrinsics{};
            const auto& fov = frame.trackingSample.eyes[eye].fov;
            if (camera::TryDeriveProjectionIntrinsics(
                    fov.angleLeft, fov.angleRight, fov.angleDown,
                    fov.angleUp, eyeIntrinsics)) {
                bloomEyeFovDegrees_ = eyeIntrinsics.verticalFieldOfViewDegrees;
            }
        }
        CaptureSourceLens(sourceCamera);
    }
    if (sceneEnabled && !ConfigureCameraData(eye, sourceCamera, target)) {
        return false;
    }
    if (!sceneEnabled) {
        Log("[VR][stereo] CALL_BEGIN stage=configure.mask eye=" + eyeName +
            " value=0");
        if (!InvokeManagedVoid<SetMask>(api_.cameraSetCullingMask,
                                       eyeCameras_[eye], 0)) {
            return FailStage("configure.mask", eye);
        }
        Log("[VR][stereo] CALL_OK stage=configure.mask eye=" + eyeName +
            " value=0");
    }
    const float eyeDepth = sourceDepth + (eye == 0U ? 0.25F : 0.5F);
    Log("[VR][stereo] CALL_BEGIN stage=configure.depth eye=" + eyeName +
        " value=" + std::to_string(eyeDepth));
    if (!InvokeManagedVoid<SetDepth>(api_.cameraSetDepth,
                                    eyeCameras_[eye], eyeDepth)) {
        return FailStage("configure.depth", eye);
    }
    Log("[VR][stereo] CALL_OK stage=configure.depth eye=" + eyeName);
    if (sceneEnabled) {
        float nearClip = 0.0F;
        float sourceFarClip = 0.0F;
        Log("[VR][stereo] CALL_BEGIN stage=configure.near-clip eye=" + eyeName);
        if (!InvokeManagedResult<float, GetClip>(api_.cameraGetNearClipPlane,
                &nearClip, sourceCamera)) {
            return FailStage("configure.near-clip", eye);
        }
        Log("[VR][stereo] CALL_OK stage=configure.near-clip eye=" + eyeName +
            " value=" + std::to_string(nearClip));
        Log("[VR][stereo] CALL_BEGIN stage=configure.far-clip eye=" + eyeName);
        if (!InvokeManagedResult<float, GetClip>(api_.cameraGetFarClipPlane,
                &sourceFarClip, sourceCamera)) {
            return FailStage("configure.far-clip", eye);
        }
        Log("[VR][stereo] CALL_OK stage=configure.far-clip eye=" + eyeName +
            " value=" + std::to_string(sourceFarClip));
        if (!std::isfinite(sourceFarClip) ||
            sourceFarClip <= nearClip + 0.01F) {
            return FailStage("configure.far-clip-input", eye);
        }
        const float eyeFarClip =
            std::max(sourceFarClip, kMinimumEyeFarClip);
        Log("[VR][stereo] CALL_BEGIN stage=configure.eye-far-clip eye=" +
            eyeName + " source=" + std::to_string(sourceFarClip) +
            " effective=" + std::to_string(eyeFarClip));
        if (!InvokeManagedVoid<SetClip>(api_.cameraSetFarClipPlane,
                                       eyeCameras_[eye], eyeFarClip)) {
            return FailStage("configure.eye-far-clip", eye);
        }
        float appliedEyeFarClip = 0.0F;
        if (!InvokeManagedResult<float, GetClip>(
                api_.cameraGetFarClipPlane, &appliedEyeFarClip,
                eyeCameras_[eye])) {
            return FailStage("configure.eye-far-clip-readback", eye);
        }
        if (!std::isfinite(appliedEyeFarClip) ||
            std::abs(appliedEyeFarClip - eyeFarClip) > 0.01F) {
            Log("[VR][stereo] EYE_FAR_CLIP_MISMATCH eye=" + eyeName +
                " requested=" + std::to_string(eyeFarClip) +
                " readback=" + std::to_string(appliedEyeFarClip));
            return FailStage("configure.eye-far-clip-mismatch", eye);
        }
        if (verboseFrameLog_) {
            Log("[VR][stereo] EYE_FAR_CLIP_APPLIED eye=" + eyeName +
                " source=" + std::to_string(sourceFarClip) +
                " effective=" + std::to_string(appliedEyeFarClip) +
                " minimum=" + std::to_string(kMinimumEyeFarClip) +
                " extended=" +
                std::string(eyeFarClip > sourceFarClip ? "1" : "0"));
        }
        const float eyeNearClip = std::min(nearClip, kMaximumEyeNearClip);
        Log("[VR][stereo] CALL_BEGIN stage=configure.eye-near-clip eye=" +
            eyeName + " source=" + std::to_string(nearClip) +
            " effective=" + std::to_string(eyeNearClip));
        if (!InvokeManagedVoid<SetClip>(api_.cameraSetNearClipPlane,
                                       eyeCameras_[eye], eyeNearClip)) {
            return FailStage("configure.eye-near-clip", eye);
        }
        float appliedEyeNearClip = 0.0F;
        if (!InvokeManagedResult<float, GetClip>(
                api_.cameraGetNearClipPlane, &appliedEyeNearClip,
                eyeCameras_[eye])) {
            return FailStage("configure.eye-near-clip-readback", eye);
        }
        if (!std::isfinite(appliedEyeNearClip) ||
            std::abs(appliedEyeNearClip - eyeNearClip) > 0.001F) {
            Log("[VR][stereo] EYE_NEAR_CLIP_MISMATCH eye=" + eyeName +
                " requested=" + std::to_string(eyeNearClip) +
                " readback=" + std::to_string(appliedEyeNearClip));
            return FailStage("configure.eye-near-clip-mismatch", eye);
        }
        if (verboseFrameLog_) {
            Log("[VR][stereo] EYE_NEAR_CLIP_APPLIED eye=" + eyeName +
                " source=" + std::to_string(nearClip) +
                " effective=" + std::to_string(appliedEyeNearClip) +
                " maximum=" + std::to_string(kMaximumEyeNearClip) +
                " clamped=" +
                std::string(eyeNearClip < nearClip ? "1" : "0"));
        }
        if (!ApplyEyePoseAndProjection(
                eye, frame, appliedEyeNearClip, appliedEyeFarClip)) {
            return false;
        }
    }
    return SetCameraEnabled(eye, true);
}

bool UnityStereoRenderer::ArmCurrentStage(
    void* sourceCamera,
    const UnityStereoCameraFrame& frame,
    const TargetSpecSnapshot& targetSpec) noexcept {
    if (stage_ == LadderStage::Complete || stage_ == LadderStage::Failed ||
        stageArmed_ || sourceCamera == nullptr || EyeArmHeld() ||
        !frame.trackingSample.valid || frame.trackingSample.viewCount != 2U ||
        frame.trackingSample.revision == 0U) {
        return false;
    }
    const std::size_t count = ExpectedCameraCount();
    const bool full = stage_ == LadderStage::StereoFull;
    verboseFrameLog_ = !full || !continuousStereo_ || publishedFrames_ < 2U ||
        (publishedFrames_ + 1U) % 300U == 0U;
    if (full && (targetSpec.width == 0U || targetSpec.height == 0U ||
                 targetSpec.generation == 0U)) {
        return false;
    }
    for (std::size_t eye = 0; eye < count; ++eye) {
        if (!EnsureCamera(eye) ||
            (!full && !EnsureAdmissionTarget(eye)) ||
            (full && !EnsureFullTarget(eye, targetSpec.width, targetSpec.height,
                                       targetSpec.generation))) {
            return false;
        }
    }
    GakumasLocal::Config::ClampVrEyeAaSettings();
    const bool smaaT2xRequested =
        full && GakumasLocal::Config::vrEyeAaMode == 4;
    const bool tscmaaRequested =
        full && GakumasLocal::Config::vrEyeAaMode == 5;
    const int nativeTemporalAaMode = smaaT2xRequested ? 4 :
        (tscmaaRequested ? 5 : 0);
    const int smaaT2xQuality = GakumasLocal::Config::vrEyeSmaaQuality;
    if (nativeTemporalAaMode != lastNativeTemporalAaMode_) {
        const char* reason = "aa-mode-leave-native-temporal";
        if (lastNativeTemporalAaMode_ == 0 && nativeTemporalAaMode == 4) {
            reason = "aa-mode-enter-smaa-t2x";
        } else if (lastNativeTemporalAaMode_ == 4 && nativeTemporalAaMode == 0) {
            reason = "aa-mode-leave-smaa-t2x";
        } else if (lastNativeTemporalAaMode_ == 0 && nativeTemporalAaMode == 5) {
            reason = "aa-mode-enter-tscmaa";
        } else if (lastNativeTemporalAaMode_ == 5 && nativeTemporalAaMode == 0) {
            reason = "aa-mode-leave-tscmaa";
        } else if (lastNativeTemporalAaMode_ == 4 && nativeTemporalAaMode == 5) {
            reason = "aa-mode-switch-smaa-t2x-to-tscmaa";
        } else if (lastNativeTemporalAaMode_ == 5 && nativeTemporalAaMode == 4) {
            reason = "aa-mode-switch-tscmaa-to-smaa-t2x";
        }
        ResetTemporalHistories(reason);
        smaaT2xReady_ = false;
        tscmaaReady_ = false;
        smaaT2xCaptureCount_.fill(0);
        smaaT2xCaptureFaultCount_.fill(0);
        smaaT2xWarmupLogCount_ = 0;
        smaaT2xResolvedCount_ = 0;
        tscmaaWarmupLogCount_ = 0;
        tscmaaResolvedCount_ = 0;
        if (!smaaT2xRequested) {
            smaaT2xPass_.Reset();
        }
        if (!tscmaaRequested) {
            tscmaaPass_.Reset();
        }
        lastNativeTemporalAaMode_ = nativeTemporalAaMode;
    }
    if (nativeTemporalAaMode != 0 && lastSmaaT2xQuality_ != smaaT2xQuality) {
        ResetTemporalHistories(tscmaaRequested
            ? "tscmaa-quality-change"
            : "smaa-quality-change");
        lastSmaaT2xQuality_ = smaaT2xQuality;
    }
    if (nativeTemporalAaMode != 0 && lastSmaaT2xPoseEpoch_ != 0U &&
        lastSmaaT2xPoseEpoch_ != frame.trackingSample.poseEpoch) {
        ResetTemporalHistories("pose-epoch-change");
    }
    if (nativeTemporalAaMode != 0) {
        lastSmaaT2xPoseEpoch_ = frame.trackingSample.poseEpoch;
    }
    if (full) {
        // A copy queued by an unpublished/parked prior pair must never cross
        // the next pair token or a target/scene rebuild.
        smaaT2xMotionCopyTokens_.fill(0);
        smaaT2xOrderedMotionReadyForPair_ = false;
    }
    smaaT2xRequestedForPair_ = smaaT2xRequested;
    smaaT2xActiveForPair_ = smaaT2xRequested && smaaT2xReady_;
    tscmaaRequestedForPair_ = tscmaaRequested;
    tscmaaActiveForPair_ = tscmaaRequested && tscmaaReady_;
    smaaT2xPairPhase_ = smaaT2xPhase_;
    if (full) {
        ++smaaT2xPairToken_;
        if (smaaT2xPairToken_ == 0U) {
            ++smaaT2xPairToken_;
        }
    }
    const std::uint64_t nextResolvedCount = smaaT2xResolvedCount_ + 1U;
    smaaT2xComprehensiveProbeForPair_ =
        GakumasLocal::Config::vrDiagnosticsStartupEnabled &&
        smaaT2xActiveForPair_ && smaaT2xPass_.HasHistory() &&
        (nextResolvedCount <= 2U || nextResolvedCount % 30U == 0U);
    const std::uint64_t nextTscmaaResolvedCount = tscmaaResolvedCount_ + 1U;
    tscmaaComprehensiveProbeForPair_ =
        GakumasLocal::Config::vrDiagnosticsStartupEnabled &&
        tscmaaActiveForPair_ && tscmaaPass_.HasHistory() &&
        (nextTscmaaResolvedCount <= 2U ||
         nextTscmaaResolvedCount % 30U == 0U);
    smaaT2xExpectedProjectionValid_.fill(false);
    smaaT2xMotionHistoryCorrectedForPair_.fill(false);
    using GetDepth = float (*)(void*, void*);
    float sourceDepth = 0.0F;
    Log("[VR][stereo] CALL_BEGIN stage=configure.source-depth");
    if (!InvokeManagedResult<float, GetDepth>(api_.cameraGetDepth,
                                             &sourceDepth, sourceCamera)) {
        return FailStage("configure.source-depth");
    }
    Log("[VR][stereo] CALL_OK stage=configure.source-depth value=" +
        std::to_string(sourceDepth));
    std::ostringstream begin;
    begin << "[VR][stereo] STAGE_BEGIN stage=" << StageName(stage_)
          << " afterContext=" << completedContextEpoch_
          << " eyes=" << count << " expectedMask=0x" << std::hex
          << static_cast<unsigned int>(ExpectedMask()) << std::dec
          << " size=" << (full ? targetSpec.width : kAdmissionTargetWidth) << 'x'
          << (full ? targetSpec.height : kAdmissionTargetHeight)
           << " cullingMask=" << (full ? "scene" : "0")
           << " poseRevision=" << frame.trackingSample.revision;
    if (smaaT2xRequested) {
        begin << " smaaT2x=" << (smaaT2xActiveForPair_ ? "active" : "warmup")
              << " phase=" << smaaT2xPairPhase_
              << " token=" << smaaT2xPairToken_;
    } else if (tscmaaRequested) {
        begin << " tscmaa=" << (tscmaaActiveForPair_ ? "active" : "warmup")
              << " jitter=off"
              << " token=" << smaaT2xPairToken_;
    }
    Log(begin.str());
    for (std::size_t eye = 0; eye < count; ++eye) {
        void* target = full ? fullTargets_[eye] : admissionTargets_[eye];
        if (!ConfigureCamera(eye, sourceCamera, target, full, frame,
                             sourceDepth)) {
            Log("[VR][stereo] STAGE_CONFIGURATION_ROLLBACK_BEGIN stage=" +
                std::string(StageName(stage_)) + " failedEye=" +
                (eye == 0U ? "left" : "right"));
            for (std::size_t rollbackEye = 0U;
                 rollbackEye <= eye && rollbackEye < cameraEnabled_.size();
                 ++rollbackEye) {
                if (cameraEnabled_[rollbackEye] &&
                    !SetCameraEnabled(rollbackEye, false)) {
                    Log("[VR][stereo] STAGE_CONFIGURATION_ROLLBACK_FAILED eye=" +
                        std::string(rollbackEye == 0U ? "left" : "right"));
                }
            }
            if (stage_ != LadderStage::Failed) {
                FailStage("arm.configure", eye);
            }
            return false;
        }
    }
    stageContextEpoch_ = 0;
    observedMask_ = 0;
    armedPoseRevision_ = frame.trackingSample.revision;
    armedFrame_ = frame;
    stageArmed_ = true;
    for (std::size_t index = count; index > 0U; --index) {
        const std::size_t activationEye = index - 1U;
        if (gameObjectActive_[activationEye]) {
            continue;
        }
        if (!SetGameObjectActive(activationEye, true)) {
            const std::size_t failedEye = index - 1U;
            Log("[VR][stereo] STAGE_ACTIVATION_ROLLBACK_BEGIN stage=" +
                std::string(StageName(stage_)) + " failedEye=" +
                (failedEye == 0U ? "left" : "right"));
            for (std::size_t eye = 0; eye < gameObjectActive_.size(); ++eye) {
                if (gameObjectActive_[eye] &&
                    !SetGameObjectActive(eye, false)) {
                    Log("[VR][stereo] STAGE_ACTIVATION_ROLLBACK_FAILED eye=" +
                        std::string(eye == 0U ? "left" : "right"));
                }
            }
            stageArmed_ = false;
            return FailStage("arm.activate", failedEye);
        }
    }
    Log("[VR][stereo] STAGE_ARMED stage=" +
        std::string(StageName(stage_)) + " poseRevision=" +
        std::to_string(armedPoseRevision_));
    return true;
}

void UnityStereoRenderer::Tick(
    void* sourceCamera,
    const UnityStereoCameraFrame& frame,
    bool pipelineIdle) noexcept {
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) {
        EnsureLivenessCrashProbe();
    }
    while (IsOwnerThread() && ConsumeLivePauseToggle()) {
        ToggleLivePause();
    }
    if (IsOwnerThread()) {
        // Captured before the eligibility gates below: the actor-shadow
        // anchor also serves the Grip path, where the eye ladder never
        // arms. `.268` restores `.266`: projected shadows follow
        // `sourcePose` (shot, or free-cam rig). Pinning this to
        // `cinematicPose` made free-cam distance swing the map.
        actorShadowAnchorPose_ = frame.sourcePose;
        actorShadowAnchorPoseValid_ = frame.sourcePoseValid &&
            IsFinitePose(frame.sourcePose);
        cinematicPose_ = frame.cinematicPose;
        cinematicPoseValid_ = frame.cinematicPoseValid &&
            IsFinitePose(frame.cinematicPose);
        playerPose_ = frame.sourcePose;
        playerPoseValid_ = frame.sourcePoseValid &&
            IsFinitePose(frame.sourcePose);
        headsetPose_ = frame.composedPose.center;
        headsetPoseValid_ = IsFinitePose(headsetPose_);
        leftEyePose_ = frame.composedPose.eyes[0];
        leftEyePoseValid_ = IsFinitePose(leftEyePose_);
        // Volume-trigger source anchor (`.229`): keep volume-stack
        // evaluation at the authored shot pose so the head-driven source
        // camera stops hard-swapping local blend=0 lighting volumes
        // (lobby boundary row, `.228` census).
        UpdateVolumeTriggerAnchor(sourceCamera, frame);
        TickHandGlowSticks(
            headsetPose_, headsetPoseValid_, frame.trackingSample);
        // Diagnostic counter only: the .85 run showed Tick goes quiet inside
        // Live (one probe sample in 16 s, none after pause), so the sampling
        // itself moved to the render-side anchor bracket, which the .84 run
        // proved fires every frame. Diagnostics print this counter so the two
        // cadences can be compared offline.
        ++actorLightDiagnosticTicks_;
    }
    if (!pipelineIdle || !IsOwnerThread()) {
        return;
    }
    const auto spec = ReadStereoRenderTargetSpec();
    const bool sourcePresent = spec.enabled && sourceCamera != nullptr &&
        IsUnityManagedObjectAlive(sourceCamera) &&
        frame.trackingSample.valid && frame.trackingSample.viewCount == 2U &&
        frame.trackingSample.revision != 0U;
    // Sample loading before identity so a same-Tick loading edge can
    // start an epoch and still HoldEyeArm on this identity change.
    SampleSceneReady("tick", spec.enabled, sourcePresent);
    ObserveLifetimeSceneReadyEdges("tick-after-sample");
    const bool sceneContentReadyEdge = SceneReadyConsumeContentReadyEdge();
    if (sceneContentReadyEdge) {
        RequestActorOutlineDiscover("content-ready");
    }
    RefreshSceneIdentity("tick");
    if (sceneContentReadyEdge) {
        RequestVirtualCameraCensus("content-ready");
    }
    RunVirtualCameraCensusIfDue();
    if (!beginLogged_) {
        beginLogged_ = true;
        Log("[VR][stereo] QUEUE_LADDER_BEGIN stages=A/B/C crashPolicy=preserve-first-fault");
    }
    if (releaseRequested_.load(std::memory_order_acquire)) {
        RetireSmaaT2xMotionCopyTargets("release-requested");
        smaaT2xOrderedMotionReadyForPair_ = false;
        if (smaaT2xPass_.IsPrepared()) {
            smaaT2xPass_.Reset();
            smaaT2xReady_ = false;
            Log("[VR][smaa-t2x] SMAA_T2X_HISTORY_RESET reason=runtime-release resources=retired");
        }
        const bool tscmaaWasPrepared = tscmaaPass_.IsPrepared();
        tscmaaPass_.Reset();
        tscmaaReady_ = false;
        tscmaaActiveForPair_ = false;
        if (tscmaaWasPrepared) {
            Log("[VR][tscmaa] TSCMAA_HISTORY_RESET reason=runtime-release resources=retired");
        }
        if (stageArmed_ && !decisionPending_) {
            QueueStageDecision(false, "release-requested");
        }
        RestoreSourceCamera("release-requested");
        RestoreProFlareScales();
        RestoreOutlineMaterials();
        DropUiTextureOverlayCache("release-requested");
        DropLiveCameraOverlayCache("release-requested");
        DropCmovParticleCache("release-requested");
        return;
    }
    if (!sourcePresent) {
        ApplySceneIneligible();
        return;
    }
    // A ready epoch invalidates the mailbox and OpenXR holds the portrait
    // loading panel. Do not keep rendering two full-resolution hidden eyes
    // while the official content-complete state is false. Restore the source
    // heartbeat and park Stage C once; content-ready later re-arms it once for
    // the source/left/right proof frame.
    if (stage_ == LadderStage::StereoFull &&
        !SceneReadyAllowsStereoRender()) {
        if (stageArmed_ && !decisionPending_) {
            QueueStageDecision(false, "scene-ready-park");
        }
        RestoreSourceCamera("scene-ready-park");
        if (!sceneReadyParked_) {
            sceneReadyParked_ = true;
            // LoadingManager rose before SceneManager identity moved in the
            // .167 Idol-path crashes. Retire every material/component pointer
            // at this first signal so no BeginCamera write can race scene
            // unload while we wait for the later identity sample.
            DropOutlineMaterialCache("scene-ready-park");
            DropProFlareCache("scene-ready-park");
            DropLiveCameraOverlayCache("scene-ready-park");
            DropCmovParticleCache("scene-ready-park");
            Log("[VR][scene-ready] SCENE_READY_PARK requested=1");
        }
        return;
    }
    if (sceneReadyParked_) {
        sceneReadyParked_ = false;
        Log("[VR][scene-ready] SCENE_READY_RESUME reason=content-ready");
    }
    // Portrait can flip eligible while scenes are still swapping. Stay
    // held only while identity is still moving; the first stable sample
    // releases in RefreshSceneIdentity and we arm on this Tick.
    if (EyeArmHeld()) {
        if (!eyeArmAwaitingSettle_) {
            ReleaseEyeArm("layout-ready");
        } else {
            if (stageArmed_ && !decisionPending_) {
                QueueStageDecision(false, "scene-ineligible");
            }
            if (!eyeArmSkipLogged_) {
                eyeArmSkipLogged_ = true;
                Log(std::string("[VR][stereo] EYE_ARM_SKIPPED reason=") +
                    (eyeArmHoldReason_ != nullptr ? eyeArmHoldReason_
                                                  : "unknown"));
            }
            return;
        }
    }
    if (stage_ == LadderStage::Failed || stage_ == LadderStage::Complete ||
        releaseRequested_.load(std::memory_order_acquire)) {
        RestoreSourceCamera("stage-terminal");
        RestoreUiTextureOverlays("stage-terminal");
        return;
    }
    failureStage_ = nullptr;
    if (!EnsureManagedApi()) {
        return;
    }
    // `.148` hardware excluded Campus.Common.UITextureOverlay: every live
    // instance belonged to menu Noti/ImageEffect, and hiding those materials
    // did not affect the camera-following wash. Keep the implementation only
    // as persisted forensics; do not mutate unrelated UI while the eye-pass
    // dispatcher trace identifies the actual render stage.
    using GetTarget = void* (*)(void*, void*);
    void* sourceTarget = nullptr;
    if (!InvokeManagedResult<void*, GetTarget>(
            api_.cameraGetTargetTexture, &sourceTarget, sourceCamera)) {
        FailStage("source.target-read");
        return;
    }
    // Tiny mode owns the source targetTexture; the boundary check must see
    // the game's own target or apply/lift would flap STEREO_FRAME_INVALIDATED.
    if (tinyModeActive_ && sourceCamera == tinySourceCamera_ &&
        sourceTarget == tinySourceTarget_) {
        sourceTarget = tinySavedTarget_;
    }
    const bool sourceBoundary =
        (latestSourceCamera_ != nullptr && latestSourceCamera_ != sourceCamera) ||
        (latestSourceTarget_ != nullptr && latestSourceTarget_ != sourceTarget) ||
        (lastEligibleGeneration_ != 0U &&
         lastEligibleGeneration_ != spec.generation);
    if (sourceBoundary && latestSourceCamera_ != nullptr &&
        latestSourceCamera_ != sourceCamera) {
        RestoreSourceCamera("source-boundary");
        NoteSceneReadySourceCameraChanged();
        ObserveLifetimeSceneReadyEdges("source-boundary");
    }
    if (stereoInvalidatedForIneligibility_ || sourceBoundary) {
        ResetTemporalHistories(sourceBoundary
            ? "source-camera-boundary"
            : "scene-resumed");
        if (publishedFrames_ != 0U) {
            std::ostringstream boundary;
            boundary << "[VR][stereo] STEREO_FRAME_INVALIDATED reason="
                     << (stereoInvalidatedForIneligibility_
                             ? "scene-resumed"
                             : "source-boundary")
                     << " oldCamera=" << latestSourceCamera_
                     << " newCamera=" << sourceCamera
                     << " oldTarget=" << latestSourceTarget_
                     << " newTarget=" << sourceTarget
                     << " oldGeneration=" << lastEligibleGeneration_
                     << " newGeneration=" << spec.generation;
            Log(boundary.str());
        }
        InvalidateUnityStereoFrame();
        continuousStereo_ = false;
        sourceFingerprintValid_.fill(false);
        stereoInvalidatedForIneligibility_ = false;
        latestSourceVolumeStack_ = nullptr;
        sourceCutSignatureValid_ = false;
        sourceHistoryResetLatched_ = false;
        RestoreLensFlareScales();
        DropOutlineMaterialCache("source-boundary");
        RequestHeavyDiscover();
        // Do not RestoreProFlareScales here — source-boundary flaps
        // during Live kept undoing .55 shrinks.
    }
    latestSourceCamera_ = sourceCamera;
    latestSourceTarget_ = sourceTarget;
    lastEligibleGeneration_ = spec.generation;
    latestFrame_ = frame;
    latestTargetSpec_ = {spec.eyeWidth, spec.eyeHeight, spec.generation};
    SyncSourceCameraSuppression(sourceCamera);
    DiscoverLiveCameraOverlays();
    HideLiveCameraOverlaysForEyes();
    DiscoverCmovParticles();
    HideCmovParticlesForEyes();
    if (stageArmed_ || decisionPending_ || publishPending_) {
        return;
    }
    ArmCurrentStage(sourceCamera, frame, latestTargetSpec_);
    if (!portraitArmedLogged_ && !spec.landscape && stageArmed_) {
        MaybeLogPortraitArmed(sourceCamera);
    }
    SyncSourceCameraSuppression(sourceCamera);
}

void UnityStereoRenderer::OnBeginContext() noexcept {
    if (!IsOwnerThread() || contextActive_) {
        return;
    }
    expectOwnedEyeDof_ = sourceCameraSuppressed_;
    contextActive_ = true;
    ++activeContextEpoch_;
    if (stageArmed_ && stageContextEpoch_ == 0U) {
        stageContextEpoch_ = activeContextEpoch_;
        Log("[VR][stereo] CONTEXT_BEGIN stage=" +
            std::string(StageName(stage_)) + " epoch=" +
            std::to_string(stageContextEpoch_));
    }
}

void UnityStereoRenderer::OnBeginCamera(void* camera) noexcept {
    if (ownerThreadId_ != 0U && ownerThreadId_ != GetCurrentThreadId()) {
        return;
    }
    currentCamera_ = camera;
    EnsureSkyRenderHooks(*this);
    EnsureOutlineMaterialApi();
    if (camera != nullptr && !SceneReadyAllowsStereoPublish()) {
        NoteSceneReadyCameraBoundary(ClassifyCamera(camera), true);
        ObserveLifetimeSceneReadyEdges("begin-camera");
    }
    if (camera != nullptr &&
        (camera == eyeCameras_[0] || camera == eyeCameras_[1])) {
        expectOwnedEyeDof_ = true;
        DeactivateBoundEyeDepthOfField();
        const std::size_t eye = camera == eyeCameras_[0] ? 0U : 1U;
        ApplyModeBodyBloom(eye);
        if (eye == 0U) {
            ApplyModeBodyLensFlareScales();
            WriteProFlareScalesForEyes();
            WriteOutlineMaterialsForEyes();
        }
        NoteBeginEyeSky(camera);
        if (!eyeBeginCameraLogged_) {
            eyeBeginCameraLogged_ = true;
            Log("[VR][stereo] EYE_BEGIN_CAMERA eye=" +
                std::string(camera == eyeCameras_[0] ? "left" : "right"));
        }
    } else if (camera != nullptr) {
        RestoreUiTextureOverlays("non-eye-begin");
        // Any non-eye camera (Game3DManager / Monitor / Grip path)
        // must see authored ProFlare — components are scene-shared.
        // Only flip back while eyes left them scaled; after Live exit
        // the cache is dropped and this becomes a no-op.
        expectOwnedEyeDof_ = false;
        if (proFlareEyeScaled_) {
            WriteProFlareScalesForGrip();
        }
        if (outlineEyeScaled_) {
            WriteOutlineMaterialsForGrip();
        }
    }
}

bool UnityStereoRenderer::OnEndCamera(void* camera) noexcept {
    if (ownerThreadId_ == 0U || ownerThreadId_ == GetCurrentThreadId()) {
        if (camera != nullptr && !SceneReadyAllowsStereoPublish()) {
            NoteSceneReadyCameraBoundary(ClassifyCamera(camera), false);
            ObserveLifetimeSceneReadyEdges("end-camera");
        }
        if (currentCamera_ == camera) {
            currentCamera_ = nullptr;
        }
        if (stage_ == LadderStage::StereoFull &&
            camera != nullptr && camera == latestSourceCamera_) {
            expectOwnedEyeDof_ = true;
            if (!sourceCameraEndedLogged_) {
                sourceCameraEndedLogged_ = true;
                Log("[VR][stereo] SOURCE_CAMERA_ENDED expectEyeDof=1");
            }
            LogTaaForensics(
                camera, latestSourceUniversalData_, "source",
                sourceCameraSuppressed_);
        }
    }
    std::size_t eye = eyeCameras_.size();
    for (std::size_t index = 0; index < eyeCameras_.size(); ++index) {
        if (camera != nullptr && camera == eyeCameras_[index]) {
            eye = index;
            break;
        }
    }
    if (eye == eyeCameras_.size()) {
        return false;
    }
    LogTaaForensics(
        camera, eyeUniversalCameraData_[eye],
        eye == 0U ? "left" : "right", sourceCameraSuppressed_);
    if (eye == 1U) {
        RestoreUiTextureOverlays("right-eye-end");
    }
    if (!IsOwnerThread() || !stageArmed_ || !contextActive_ ||
        stageContextEpoch_ == 0U || activeContextEpoch_ != stageContextEpoch_) {
        if (!ownedEyeOutsideContextLogged_) {
            ownedEyeOutsideContextLogged_ = true;
            std::ostringstream outside;
            outside << "[VR][stereo] EYE_CALLBACK_OUTSIDE_CONTEXT eye="
                    << (eye == 0U ? "left" : "right")
                    << " tid=" << GetCurrentThreadId()
                    << " ownerTid=" << ownerThreadId_
                    << " armed=" << stageArmed_
                    << " contextActive=" << contextActive_
                    << " activeEpoch=" << activeContextEpoch_
                    << " stageEpoch=" << stageContextEpoch_;
            Log(outside.str());
        }
        return true;
    }
    observedMask_ = static_cast<std::uint8_t>(
        observedMask_ | static_cast<std::uint8_t>(1U << eye));
    std::ostringstream stream;
    stream << "[VR][stereo] EYE_RENDERED stage=" << StageName(stage_)
           << " eye=" << (eye == 0U ? "left" : "right")
           << " epoch=" << activeContextEpoch_ << " observedMask=0x"
           << std::hex << static_cast<unsigned int>(observedMask_);
    Log(stream.str());
    return true;
}

void UnityStereoRenderer::OnEndContext(bool pipelineIdle) noexcept {
    if (ownerThreadId_ == 0U || ownerThreadId_ == GetCurrentThreadId()) {
        RestoreUiTextureOverlays("context-end");
    }
    if (!pipelineIdle || !IsOwnerThread() || !contextActive_) {
        return;
    }
    currentCamera_ = nullptr;
    expectOwnedEyeDof_ = false;
    completedContextEpoch_ = activeContextEpoch_;
    contextActive_ = false;
    // Observation only: source restore is managed mutation and must wait
    // for OnRenderLoopCompleted. Queue the resumable abort here so
    // teardown still happens when Cinemachine Tick has already gone quiet.
    if (!ReadStereoRenderTargetSpec().enabled) {
        if (stageArmed_ && !decisionPending_) {
            QueueStageDecision(false, "scene-ineligible");
        }
        return;
    }
    if (!stageArmed_ || decisionPending_ || stageContextEpoch_ == 0U ||
        completedContextEpoch_ != stageContextEpoch_) {
        return;
    }
    // Content-incomplete transitions park the two full-resolution eye cameras
    // at the render-loop safe point. Once content is complete, one fresh
    // context is allowed through to collect source/left/right boundaries.
    if (stage_ == LadderStage::StereoFull &&
        !SceneReadyAllowsStereoPublish()) {
        if (!SceneReadyAllowsStereoRender()) {
            QueueStageDecision(false, "scene-ready-park");
            return;
        }
        stageContextEpoch_ = 0;
        if (SceneReadyConsumePublishHoldLog()) {
            Log("[VR][scene-ready] SCENE_READY_HOLD reason=readiness-frame");
        }
        return;
    }
    const bool passed = observedMask_ == ExpectedMask();
    std::ostringstream stream;
    stream << "[VR][stereo] CONTEXT_COMPLETE stage=" << StageName(stage_)
           << " epoch=" << completedContextEpoch_ << " observedMask=0x"
           << std::hex << static_cast<unsigned int>(observedMask_)
           << " expectedMask=0x" << static_cast<unsigned int>(ExpectedMask())
           << std::dec << " decision=" << (passed ? "pass" : "miss");
    Log(stream.str());
    QueueStageDecision(passed, passed ? "expected-eyes-rendered"
                                      : "expected-eye-missing");
}

void UnityStereoRenderer::QueueStageDecision(
    bool passed,
    const char* reason) noexcept {
    if (decisionPending_) {
        return;
    }
    decisionPending_ = true;
    decisionPassed_ = passed;
    decisionReason_ = reason != nullptr ? reason : "unknown";
}

void UnityStereoRenderer::ApplySceneIneligible() noexcept {
    RestoreSourceCamera("scene-ineligible");
    DropUiTextureOverlayCache("scene-ineligible");
    DropLiveCameraOverlayCache("scene-ineligible");
    DropCmovParticleCache("scene-ineligible");
    if (!stereoInvalidatedForIneligibility_) {
        stereoInvalidatedForIneligibility_ = true;
        ResetTemporalHistories("scene-ineligible");
        InvalidateUnityStereoFrame();
        continuousStereo_ = false;
        sourceFingerprintValid_.fill(false);
        RestoreLensFlareScales();
        // .58/.59 kept the ProFlare cache across exit so lobby /
        // Monitor BeginCamera kept writing ~900 destroyed Live
        // instances → GameAssembly 0xc0000005. Drop without a
        // mass write during teardown; alive checks guard the rest.
        DropProFlareCache("scene-ineligible");
        RestoreOutlineMaterials();
        portraitArmedLogged_ = false;
        HoldEyeArm("scene-ineligible");
        Log("[VR][stereo] STEREO_FRAME_INVALIDATED reason=scene-ineligible");
    }
    if (stageArmed_ && !decisionPending_) {
        QueueStageDecision(false, "scene-ineligible");
    }
}

void UnityStereoRenderer::MaybeLogPortraitArmed(void* sourceCamera) noexcept {
    if (portraitArmedLogged_) {
        return;
    }
    float fov = 0.0F;
    if (api_.cameraGetFieldOfView.Ready() && sourceCamera != nullptr &&
        IsUnityManagedObjectAlive(sourceCamera)) {
        using GetFov = float (*)(void*, void*);
        if (!InvokeManagedResult<float, GetFov>(
                api_.cameraGetFieldOfView, &fov, sourceCamera)) {
            fov = 0.0F;
        }
    }
    std::uint32_t uiCameras = 0;
    if (auto* cameraClass = Il2cppUtils::GetClass(
            "UnityEngine.CoreModule.dll", "UnityEngine", "Camera")) {
        for (void* camera : FindManagedObjectsOfType(cameraClass)) {
            const std::string name = ManagedObjectName(camera);
            if (name.find("UICamera") != std::string::npos ||
                name.find("UiCamera") != std::string::npos) {
                ++uiCameras;
            }
        }
    }
    std::ostringstream stream;
    stream << "[VR][stereo] STEREO_PORTRAIT_ARMED source="
           << ManagedObjectName(sourceCamera) << " fov=" << std::fixed
           << std::setprecision(2) << fov << " uiCameras=" << uiCameras;
    Log(stream.str());
    portraitArmedLogged_ = true;
}

bool UnityStereoRenderer::QueueSmaaT2xMotionVectorCopy(
    void* camera,
    void* renderContext,
    void* renderTexture,
    int renderPassEvent) noexcept {
    constexpr int kVerifiedPostProcessEvent = 550;
    const bool nativeTemporalAaRequested =
        smaaT2xRequestedForPair_ || tscmaaRequestedForPair_;
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled ||
        !nativeTemporalAaRequested || !stageArmed_ ||
        renderPassEvent != kVerifiedPostProcessEvent || camera == nullptr ||
        camera != currentCamera_ || renderContext == nullptr ||
        renderTexture == nullptr) {
        return false;
    }
    std::size_t eye = eyeCameras_.size();
    for (std::size_t index = 0; index < eyeCameras_.size(); ++index) {
        if (camera == eyeCameras_[index]) {
            eye = index;
            break;
        }
    }
    if (eye >= eyeCameras_.size()) {
        return false;
    }
    if (smaaT2xActiveForPair_ &&
        !smaaT2xMotionHistoryCorrectedForPair_[eye]) {
        const std::uint64_t count = ++smaaT2xMotionHistoryFaultCount_[eye];
        if (count <= 2U || count % 300U == 0U) {
            Log("[VR][smaa-t2x] SMAA_T2X_FAULT mv-history-not-corrected eye=" +
                std::string(eye == 0U ? "left" : "right") + " token=" +
                std::to_string(smaaT2xPairToken_) + " count=" +
                std::to_string(count));
        }
        return false;
    }
    if (smaaT2xComprehensiveProbeForPair_) {
        LogSmaaT2xProjectionReadback(eye, "post-process");
    }

    // `.200`-`.202` proved every CPU-timed immediate-context D3D copy
    // races Unity's own GPU submission: the shared RTHandle read back either
    // the other eye's field or recycled full-range garbage. Record the copy
    // through Unity's own CommandBuffer on this pass's ScriptableRenderContext
    // instead, so it executes exactly after this eye's verified post-process
    // commands and before any later pass can recycle the shared allocation.
    const auto queueFault = [this, eye](const char* stage,
                                        const std::string& detail) {
        const auto faultCount = ++smaaT2xCaptureFaultCount_[eye];
        if (faultCount <= 2U || faultCount % 300U == 0U) {
            const std::string aaTag = tscmaaRequestedForPair_
                ? "[VR][tscmaa] TSCMAA_FAULT "
                : "[VR][smaa-t2x] SMAA_T2X_FAULT ";
            Log(aaTag + std::string(stage) +
                " eye=" + (eye == 0U ? "left" : "right") + " token=" +
                std::to_string(smaaT2xPairToken_) + " count=" +
                std::to_string(faultCount) +
                (detail.empty() ? std::string() : " detail=\"" + detail + '"'));
        }
        return false;
    };
    if (!api_.commandBufferClear.Ready() ||
        !api_.commandBufferCopyTexture.Ready() ||
        !api_.renderTargetIdentifierConstructor.Ready() ||
        !api_.scriptableRenderContextExecuteCommandBuffer.Ready() ||
        api_.commandBufferClass == nullptr) {
        return queueFault("mv-copy-api", "");
    }
    if (!EnsureSmaaT2xMotionCopyTarget(eye, renderTexture) ||
        !EnsureSmaaT2xMotionCopyCommandBuffer()) {
        return false;
    }
    using NoArgumentVoid = void (*)(void*, void*);
    if (!InvokeManagedVoid<NoArgumentVoid>(
            api_.commandBufferClear, smaaT2xMotionCopyCommandBuffer_)) {
        return queueFault("mv-copy-clear", "");
    }
    // dump.cs layout of UnityEngine.Rendering.RenderTargetIdentifier:
    // m_Type 0x0, m_NameID 0x4, m_InstanceID 0x8, m_BufferPointer 0x10,
    // m_MipLevel 0x18, m_CubeFace 0x1C, m_DepthSlice 0x20 (size 0x28).
    struct RenderTargetIdentifierValue {
        std::int32_t type;
        std::int32_t nameId;
        std::int32_t instanceId;
        std::int32_t padding0;
        void* bufferPointer;
        std::int32_t mipLevel;
        std::int32_t cubeFace;
        std::int32_t depthSlice;
        std::int32_t padding1;
    };
    static_assert(sizeof(RenderTargetIdentifierValue) == 0x28);
    static_assert(offsetof(RenderTargetIdentifierValue, bufferPointer) == 0x10);
    static_assert(offsetof(RenderTargetIdentifierValue, depthSlice) == 0x20);
    RenderTargetIdentifierValue source{};
    RenderTargetIdentifierValue destination{};
    using IdentifierCtor = void (*)(void*, void*, void*);
    if (!InvokeManagedVoid<IdentifierCtor>(
            api_.renderTargetIdentifierConstructor, &source, renderTexture) ||
        !InvokeManagedVoid<IdentifierCtor>(
            api_.renderTargetIdentifierConstructor, &destination,
            smaaT2xMotionCopyTargets_[eye])) {
        return queueFault("mv-copy-identifier", "");
    }
    void* copyArguments[]{&source, &destination};
    void* ignored = nullptr;
    std::string exception;
    if (!RuntimeInvoke(api_.commandBufferCopyTexture,
                       smaaT2xMotionCopyCommandBuffer_, copyArguments,
                       &ignored, &exception)) {
        return queueFault("mv-copy-record", exception);
    }
    // ScriptableRenderContext is a single-pointer struct passed by value, so
    // the hook's `context` argument *is* the m_Ptr handle. The icall takes a
    // ScriptableRenderContext* self (proven by the matcap-compensation path).
    void* contextStructSelf = renderContext;
    using ExecuteBuffer = void (*)(void*, void*, void*);
    if (!InvokeManagedVoid<ExecuteBuffer>(
            api_.scriptableRenderContextExecuteCommandBuffer,
            &contextStructSelf, smaaT2xMotionCopyCommandBuffer_)) {
        return queueFault("mv-copy-execute", "");
    }
    smaaT2xMotionCopyTokens_[eye] = smaaT2xPairToken_;
    smaaT2xMotionCopyEvents_[eye] = renderPassEvent;
    const std::uint64_t count = ++smaaT2xMotionBoundaryCount_;
    if (count <= 4U || count % 600U == 0U ||
        smaaT2xComprehensiveProbeForPair_ ||
        tscmaaComprehensiveProbeForPair_) {
        const std::string aaTag = tscmaaRequestedForPair_
            ? "[VR][tscmaa] TSCMAA_MV_BOUNDARY source="
            : "[VR][smaa-t2x] SMAA_T2X_MV_BOUNDARY source=";
        Log(aaTag +
            std::string(eye == 0U ? "left-post" : "right-post") +
            " action=queue-copy target=" +
            (eye == 0U ? "left" : "right") + " token=" +
            std::to_string(smaaT2xPairToken_) + " count=" +
            std::to_string(count));
    }
    return true;
}

bool UnityStereoRenderer::EnsureSmaaT2xMotionCopyTarget(
    std::size_t eye,
    void* source) noexcept {
    if (eye >= smaaT2xMotionCopyTargets_.size() || source == nullptr) {
        return false;
    }
    UnityRenderTextureDescriptorValue descriptor{};
    std::string exception;
    if (!ReadRenderTextureDescriptor(
            api_.renderTextureGetDescriptor, source,
            api_.renderTextureDescriptorValueSize, &descriptor, &exception)) {
        const auto faultCount = ++smaaT2xCaptureFaultCount_[eye];
        if (faultCount <= 2U || faultCount % 300U == 0U) {
            const std::string aaTag = tscmaaRequestedForPair_
                ? "[VR][tscmaa] TSCMAA_FAULT"
                : "[VR][smaa-t2x] SMAA_T2X_FAULT";
            Log(aaTag + " mv-copy-descriptor eye=" +
                std::string(eye == 0U ? "left" : "right") + " detail=\"" +
                exception + '"');
        }
        return false;
    }
    const auto width = static_cast<std::uint32_t>(descriptor.width);
    const auto height = static_cast<std::uint32_t>(descriptor.height);
    if (width == 0U || height == 0U) {
        return false;
    }
    if (smaaT2xMotionCopyTargets_[eye] != nullptr) {
        const bool alive =
            IsUnityManagedObjectAlive(smaaT2xMotionCopyTargets_[eye]);
        if (!alive || width != smaaT2xMotionCopyWidth_ ||
            height != smaaT2xMotionCopyHeight_) {
            RetireSmaaT2xMotionCopyTargets(alive ? "size-change"
                                                 : "dead-object");
        }
    }
    if (smaaT2xMotionCopyTargets_[eye] != nullptr) {
        return true;
    }
    if (!CreateRenderTarget(eye, width, height, "smaa-t2x-mv-copy", source,
                            &smaaT2xMotionCopyTargets_[eye],
                            &smaaT2xMotionCopyTargetHandles_[eye],
                            /*fatal=*/false)) {
        return false;
    }
    smaaT2xMotionCopyWidth_ = width;
    smaaT2xMotionCopyHeight_ = height;
    return true;
}

bool UnityStereoRenderer::EnsureSmaaT2xMotionCopyCommandBuffer() noexcept {
    const std::string aaFaultTag = tscmaaRequestedForPair_
        ? "[VR][tscmaa] TSCMAA_FAULT "
        : "[VR][smaa-t2x] SMAA_T2X_FAULT ";
    if (smaaT2xMotionCopyCommandBuffer_ != nullptr &&
        !IsUnityManagedObjectAlive(smaaT2xMotionCopyCommandBuffer_)) {
        if (smaaT2xMotionCopyCommandBufferHandle_ != nullptr) {
            retiredSmaaT2xMotionCopyTargetHandles_.push_back(
                smaaT2xMotionCopyCommandBufferHandle_);
        }
        smaaT2xMotionCopyCommandBuffer_ = nullptr;
        smaaT2xMotionCopyCommandBufferHandle_ = nullptr;
    }
    if (smaaT2xMotionCopyCommandBuffer_ != nullptr) {
        return true;
    }
    void* buffer = NewIl2CppObject(api_.commandBufferClass);
    using Ctor = void (*)(void*, void*);
    if (buffer == nullptr ||
        !InvokeManagedVoid<Ctor>(api_.commandBufferConstructor, buffer)) {
        Log(aaFaultTag + "mv-copy-buffer-create");
        return false;
    }
    Il2CppGCHandle handle = CreateGcHandle(buffer);
    if (handle == nullptr) {
        Log(aaFaultTag + "mv-copy-buffer-root");
        return false;
    }
    smaaT2xMotionCopyCommandBuffer_ = buffer;
    smaaT2xMotionCopyCommandBufferHandle_ = handle;
    if (!smaaT2xMotionCopyBufferLogged_) {
        smaaT2xMotionCopyBufferLogged_ = true;
        Log(tscmaaRequestedForPair_
            ? "[VR][tscmaa] TSCMAA_MV_COPY_BUFFER created=1"
            : "[VR][smaa-t2x] SMAA_T2X_MV_COPY_BUFFER created=1");
    }
    return true;
}

bool UnityStereoRenderer::CaptureSmaaT2xMotionVectorTexture(
    std::size_t eye,
    ID3D11Texture2D* texture,
    int renderPassEvent,
    const char* boundary) noexcept {
    if (eye >= eyeCameras_.size() || texture == nullptr ||
        smaaT2xPairToken_ == 0U) {
        return false;
    }
    const std::string boundaryName = boundary != nullptr ? boundary : "unknown";
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    texture->GetDevice(&device);
    if (device != nullptr) {
        device->GetImmediateContext(&context);
    }
    const bool captured = context != nullptr &&
        smaaT2xPass_.CaptureMotionVector(
            context, eye, texture, smaaT2xPairToken_);
    d3d11::SmaaT2xPass::MotionVectorValueDiagnostics values{};
    const std::uint64_t nextCaptureCount = smaaT2xCaptureCount_[eye] + 1U;
    const bool shouldReadValues =
        GakumasLocal::Config::vrDiagnosticsStartupEnabled && captured &&
        (smaaT2xComprehensiveProbeForPair_ || nextCaptureCount <= 2U ||
         nextCaptureCount % 120U == 0U);
    const bool valuesReady = shouldReadValues && context != nullptr &&
        smaaT2xPass_.ReadMotionVectorValues(context, eye, values);
    if (context != nullptr) {
        context->Release();
    }
    if (device != nullptr) {
        device->Release();
    }
    if (!captured) {
        const auto faultCount = ++smaaT2xCaptureFaultCount_[eye];
        if (faultCount <= 2U || faultCount % 300U == 0U) {
            Log("[VR][smaa-t2x] SMAA_T2X_FAULT mv-capture eye=" +
                std::string(eye == 0U ? "left" : "right") + " token=" +
                std::to_string(smaaT2xPairToken_) + " count=" +
                std::to_string(faultCount) + " boundary=" + boundaryName +
                " status=" +
                d3d11::SmaaT2xPass::StatusName(smaaT2xPass_.LastStatus()) +
                " hr=" + std::to_string(static_cast<long>(
                    smaaT2xPass_.LastHresult())));
            const auto diagnostics =
                smaaT2xPass_.LastMotionVectorDiagnostics();
            const auto& description = diagnostics.source;
            std::ostringstream details;
            details << "[VR][smaa-t2x] SMAA_T2X_MV_DESC eye="
                    << (eye == 0U ? "left" : "right")
                    << " token=" << smaaT2xPairToken_
                    << " count=" << faultCount
                    << " boundary=" << boundaryName
                    << " mismatch=0x" << std::hex << diagnostics.mismatchMask
                    << std::dec
                    << " actual=" << description.Width << 'x'
                    << description.Height
                    << " mips=" << description.MipLevels
                    << " array=" << description.ArraySize
                    << " format=" << static_cast<unsigned int>(description.Format)
                    << " samples=" << description.SampleDesc.Count
                    << " sampleQuality=" << description.SampleDesc.Quality
                    << " usage=" << static_cast<unsigned int>(description.Usage)
                    << " bind=0x" << std::hex << description.BindFlags
                    << " cpu=0x" << description.CPUAccessFlags
                    << " misc=0x" << description.MiscFlags << std::dec
                    << " expected=" << diagnostics.expectedWidth << 'x'
                    << diagnostics.expectedHeight
                    << " mips=1 array=1 formats="
                    << static_cast<unsigned int>(DXGI_FORMAT_R16G16_FLOAT)
                    << '|'
                    << static_cast<unsigned int>(DXGI_FORMAT_R16G16B16A16_TYPELESS)
                    << " samples=1 floatView="
                    << static_cast<unsigned int>(diagnostics.viewFormat)
                    << " bytesPerPixel=" << diagnostics.bytesPerPixel;
            Log(details.str());
        }
        return false;
    }
    ++smaaT2xCaptureCount_[eye];
    if (shouldReadValues) {
        std::ostringstream valueLine;
        valueLine << std::fixed << std::setprecision(7)
                  << "[VR][smaa-t2x] SMAA_T2X_MV_VALUES eye="
                  << (eye == 0U ? "left" : "right")
                  << " token=" << smaaT2xPairToken_
                  << " count=" << smaaT2xCaptureCount_[eye]
                  << " boundary=" << boundaryName
                  << " ready=" << (valuesReady ? 1 : 0)
                  << " samples=" << values.sampleCount
                  << " finite=" << values.finiteSampleCount
                  << " nonfinite=" << values.nonFiniteSampleCount
                  << " x=" << values.minimumX << ',' << values.maximumX
                  << " y=" << values.minimumY << ',' << values.maximumY
                  << " magnitudeMean=" << values.meanMagnitude
                  << " magnitudeMax=" << values.maximumMagnitude
                  << " z=" << values.minimumZ << ',' << values.maximumZ
                  << " w=" << values.minimumW << ',' << values.maximumW;
        Log(valueLine.str());
    }
    if (smaaT2xCaptureCount_[eye] <= 2U ||
        smaaT2xCaptureCount_[eye] % 300U == 0U) {
        const auto diagnostics = smaaT2xPass_.LastMotionVectorDiagnostics();
        Log("[VR][smaa-t2x] SMAA_T2X_MV_CAPTURE eye=" +
            std::string(eye == 0U ? "left" : "right") + " token=" +
            std::to_string(smaaT2xPairToken_) + " count=" +
            std::to_string(smaaT2xCaptureCount_[eye]) + " boundary=" +
            boundaryName + " event=" +
            std::to_string(renderPassEvent) + " resourceFormat=" +
            std::to_string(static_cast<unsigned int>(diagnostics.source.Format)) +
            " floatView=" + std::to_string(static_cast<unsigned int>(
                diagnostics.viewFormat)) + " bytesPerPixel=" +
            std::to_string(diagnostics.bytesPerPixel));
    }
    return true;
}

bool UnityStereoRenderer::CaptureTscmaaMotionVectorTexture(
    std::size_t eye,
    ID3D11Texture2D* texture,
    int renderPassEvent,
    const char* boundary) noexcept {
    if (eye >= eyeCameras_.size() || texture == nullptr ||
        smaaT2xPairToken_ == 0U) {
        return false;
    }
    const std::string boundaryName = boundary != nullptr ? boundary : "unknown";
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    texture->GetDevice(&device);
    if (device != nullptr) {
        device->GetImmediateContext(&context);
    }
    const bool captured = context != nullptr &&
        tscmaaPass_.CaptureMotionVector(
            context, eye, texture, smaaT2xPairToken_);
    d3d11::TscmaaPass::MotionVectorValueDiagnostics values{};
    const std::uint64_t nextCaptureCount = smaaT2xCaptureCount_[eye] + 1U;
    const bool shouldReadValues =
        GakumasLocal::Config::vrDiagnosticsStartupEnabled && captured &&
        (tscmaaComprehensiveProbeForPair_ || nextCaptureCount <= 2U ||
         nextCaptureCount % 120U == 0U);
    const bool valuesReady = shouldReadValues && context != nullptr &&
        tscmaaPass_.ReadMotionVectorValues(context, eye, values);
    if (context != nullptr) {
        context->Release();
    }
    if (device != nullptr) {
        device->Release();
    }
    if (!captured) {
        const auto faultCount = ++smaaT2xCaptureFaultCount_[eye];
        if (faultCount <= 2U || faultCount % 300U == 0U) {
            Log("[VR][tscmaa] TSCMAA_FAULT mv-capture eye=" +
                std::string(eye == 0U ? "left" : "right") + " token=" +
                std::to_string(smaaT2xPairToken_) + " count=" +
                std::to_string(faultCount) + " boundary=" + boundaryName +
                " status=" +
                d3d11::TscmaaPass::StatusName(tscmaaPass_.LastStatus()) +
                " hr=" + std::to_string(static_cast<long>(
                    tscmaaPass_.LastHresult())));
            const auto diagnostics =
                tscmaaPass_.LastMotionVectorDiagnostics();
            const auto& description = diagnostics.source;
            std::ostringstream details;
            details << "[VR][tscmaa] TSCMAA_MV_DESC eye="
                    << (eye == 0U ? "left" : "right")
                    << " token=" << smaaT2xPairToken_
                    << " count=" << faultCount
                    << " boundary=" << boundaryName
                    << " mismatch=0x" << std::hex << diagnostics.mismatchMask
                    << std::dec
                    << " actual=" << description.Width << 'x'
                    << description.Height
                    << " mips=" << description.MipLevels
                    << " array=" << description.ArraySize
                    << " format=" << static_cast<unsigned int>(description.Format)
                    << " samples=" << description.SampleDesc.Count
                    << " sampleQuality=" << description.SampleDesc.Quality
                    << " usage=" << static_cast<unsigned int>(description.Usage)
                    << " bind=0x" << std::hex << description.BindFlags
                    << " cpu=0x" << description.CPUAccessFlags
                    << " misc=0x" << description.MiscFlags << std::dec
                    << " expected=" << diagnostics.expectedWidth << 'x'
                    << diagnostics.expectedHeight
                    << " mips=1 array=1 formats="
                    << static_cast<unsigned int>(DXGI_FORMAT_R16G16_FLOAT)
                    << '|'
                    << static_cast<unsigned int>(DXGI_FORMAT_R16G16B16A16_TYPELESS)
                    << " samples=1 floatView="
                    << static_cast<unsigned int>(diagnostics.viewFormat)
                    << " bytesPerPixel=" << diagnostics.bytesPerPixel;
            Log(details.str());
        }
        return false;
    }
    ++smaaT2xCaptureCount_[eye];
    if (shouldReadValues) {
        std::ostringstream valueLine;
        valueLine << std::fixed << std::setprecision(7)
                  << "[VR][tscmaa] TSCMAA_MV_VALUES eye="
                  << (eye == 0U ? "left" : "right")
                  << " token=" << smaaT2xPairToken_
                  << " count=" << smaaT2xCaptureCount_[eye]
                  << " boundary=" << boundaryName
                  << " ready=" << (valuesReady ? 1 : 0)
                  << " samples=" << values.sampleCount
                  << " finite=" << values.finiteSampleCount
                  << " nonfinite=" << values.nonFiniteSampleCount
                  << " x=" << values.minimumX << ',' << values.maximumX
                  << " y=" << values.minimumY << ',' << values.maximumY
                  << " magnitudeMean=" << values.meanMagnitude
                  << " magnitudeMax=" << values.maximumMagnitude
                  << " z=" << values.minimumZ << ',' << values.maximumZ
                  << " w=" << values.minimumW << ',' << values.maximumW;
        Log(valueLine.str());
    }
    if (smaaT2xCaptureCount_[eye] <= 2U ||
        smaaT2xCaptureCount_[eye] % 300U == 0U) {
        const auto diagnostics = tscmaaPass_.LastMotionVectorDiagnostics();
        Log("[VR][tscmaa] TSCMAA_MV_CAPTURE eye=" +
            std::string(eye == 0U ? "left" : "right") + " token=" +
            std::to_string(smaaT2xPairToken_) + " count=" +
            std::to_string(smaaT2xCaptureCount_[eye]) + " boundary=" +
            boundaryName + " event=" +
            std::to_string(renderPassEvent) + " resourceFormat=" +
            std::to_string(static_cast<unsigned int>(diagnostics.source.Format)) +
            " floatView=" + std::to_string(static_cast<unsigned int>(
                diagnostics.viewFormat)) + " bytesPerPixel=" +
            std::to_string(diagnostics.bytesPerPixel));
    }
    return true;
}

bool UnityStereoRenderer::CaptureQueuedSmaaT2xMotionVectors() noexcept {
    if ((!smaaT2xRequestedForPair_ && !tscmaaRequestedForPair_) || !stageArmed_ ||
        smaaT2xPairToken_ == 0U) {
        return false;
    }
    const std::string aaFaultTag = tscmaaRequestedForPair_
        ? "[VR][tscmaa] TSCMAA_FAULT "
        : "[VR][smaa-t2x] SMAA_T2X_FAULT ";
    for (std::size_t eye = 0; eye < smaaT2xMotionCopyTokens_.size(); ++eye) {
        if (smaaT2xMotionCopyTokens_[eye] != smaaT2xPairToken_) {
            const auto count = ++smaaT2xCaptureFaultCount_[eye];
            if (count <= 2U || count % 300U == 0U) {
                Log(aaFaultTag + "mv-copy-not-queued eye=" +
                    std::string(eye == 0U ? "left" : "right") +
                    " copyToken=" +
                    std::to_string(smaaT2xMotionCopyTokens_[eye]) +
                    " pairToken=" + std::to_string(smaaT2xPairToken_) +
                    " count=" + std::to_string(count));
            }
            return false;
        }
    }
    // The complete SRP render loop has returned, so Unity has submitted the
    // context that contains both queued copies. The per-eye copy targets are
    // mod-owned and only written by those ordered copies, so the immutable
    // snapshot can never observe another pass recycling the shared RTHandle.
    for (std::size_t eye = 0; eye < smaaT2xMotionCopyTargets_.size(); ++eye) {
        void* target = smaaT2xMotionCopyTargets_[eye];
        if (target == nullptr || !IsUnityManagedObjectAlive(target)) {
            const auto count = ++smaaT2xCaptureFaultCount_[eye];
            if (count <= 2U || count % 300U == 0U) {
                Log(aaFaultTag + "mv-copy-target-dead eye=" +
                    std::string(eye == 0U ? "left" : "right") + " token=" +
                    std::to_string(smaaT2xPairToken_) + " count=" +
                    std::to_string(count));
            }
            RetireSmaaT2xMotionCopyTargets("dead-object");
            return false;
        }
        using GetNativeTexturePtr = void* (*)(void*, void*);
        void* nativePointer = nullptr;
        if (!InvokeManagedResult<void*, GetNativeTexturePtr>(
                api_.textureGetNativeTexturePtr, &nativePointer, target) ||
            nativePointer == nullptr) {
            const auto count = ++smaaT2xCaptureFaultCount_[eye];
            if (count <= 2U || count % 300U == 0U) {
                Log(aaFaultTag + "mv-copy-native eye=" +
                    std::string(eye == 0U ? "left" : "right") + " token=" +
                    std::to_string(smaaT2xPairToken_) + " count=" +
                    std::to_string(count));
            }
            return false;
        }
        ID3D11Texture2D* texture = nullptr;
        if (!QueryD3D11Texture(nativePointer, &texture) ||
            texture == nullptr) {
            const auto count = ++smaaT2xCaptureFaultCount_[eye];
            if (count <= 2U || count % 300U == 0U) {
                Log(aaFaultTag + "mv-copy-query eye=" +
                    std::string(eye == 0U ? "left" : "right") + " token=" +
                    std::to_string(smaaT2xPairToken_) + " count=" +
                    std::to_string(count));
            }
            return false;
        }
        const bool captured = tscmaaRequestedForPair_
            ? CaptureTscmaaMotionVectorTexture(
                  eye, texture, smaaT2xMotionCopyEvents_[eye],
                  eye == 0U ? "loop-copy-left" : "loop-copy-right")
            : CaptureSmaaT2xMotionVectorTexture(
                  eye, texture, smaaT2xMotionCopyEvents_[eye],
                  eye == 0U ? "loop-copy-left" : "loop-copy-right");
        texture->Release();
        if (!captured) {
            return false;
        }
        const std::uint64_t count = ++smaaT2xMotionBoundaryCount_;
        if (count <= 4U || count % 600U == 0U ||
            smaaT2xComprehensiveProbeForPair_ ||
            tscmaaComprehensiveProbeForPair_) {
            const std::string aaTag = tscmaaRequestedForPair_
                ? "[VR][tscmaa] TSCMAA_MV_BOUNDARY source=render-loop "
                : "[VR][smaa-t2x] SMAA_T2X_MV_BOUNDARY source=render-loop ";
            Log(aaTag + "action=capture target=" +
                std::string(eye == 0U ? "left" : "right") + " token=" +
                std::to_string(smaaT2xPairToken_) + " count=" +
                std::to_string(count));
        }
    }
    return true;
}

void UnityStereoRenderer::RetireSmaaT2xMotionCopyTargets(
    const char* reason) noexcept {
    const bool tscmaa = tscmaaRequestedForPair_ ||
        GakumasLocal::Config::vrEyeAaMode == 5 ||
        lastNativeTemporalAaMode_ == 5;
    bool retired = false;
    for (std::size_t eye = 0; eye < smaaT2xMotionCopyTargets_.size(); ++eye) {
        if (smaaT2xMotionCopyTargets_[eye] == nullptr) {
            continue;
        }
        retired = true;
        if (IsUnityManagedObjectAlive(smaaT2xMotionCopyTargets_[eye])) {
            std::string exception;
            if (!RuntimeInvoke(api_.renderTextureRelease,
                               smaaT2xMotionCopyTargets_[eye], nullptr,
                               nullptr, &exception)) {
                const std::string aaTag = tscmaa
                    ? "[VR][tscmaa] TSCMAA_FAULT"
                    : "[VR][smaa-t2x] SMAA_T2X_FAULT";
                Log(aaTag + " mv-copy-release eye=" +
                    std::string(eye == 0U ? "left" : "right") + " detail=\"" +
                    exception + '"');
            }
        }
        if (smaaT2xMotionCopyTargetHandles_[eye] != nullptr) {
            retiredSmaaT2xMotionCopyTargetHandles_.push_back(
                smaaT2xMotionCopyTargetHandles_[eye]);
        }
        smaaT2xMotionCopyTargets_[eye] = nullptr;
        smaaT2xMotionCopyTargetHandles_[eye] = nullptr;
    }
    smaaT2xMotionCopyTokens_.fill(0);
    smaaT2xMotionCopyEvents_.fill(0);
    smaaT2xMotionCopyWidth_ = 0;
    smaaT2xMotionCopyHeight_ = 0;
    if (retired) {
        const std::string aaTag = tscmaa
            ? "[VR][tscmaa] TSCMAA_MV_COPY_RETIRED reason="
            : "[VR][smaa-t2x] SMAA_T2X_MV_COPY_RETIRED reason=";
        Log(aaTag +
            std::string(reason != nullptr ? reason : "unknown") +
            " retiredHandles=" +
            std::to_string(retiredSmaaT2xMotionCopyTargetHandles_.size()));
    }
}

void UnityStereoRenderer::ConsumeStageDecisionAtSafePoint() noexcept {
    if (!decisionPending_) {
        return;
    }
    if (decisionPassed_ && stage_ == LadderStage::StereoFull &&
        !SceneReadyAllowsStereoPublish()) {
        if (!SceneReadyAllowsStereoRender()) {
            decisionPassed_ = false;
            decisionReason_ = "scene-ready-park";
        } else {
            decisionPending_ = false;
            stageContextEpoch_ = 0;
            if (SceneReadyConsumePublishHoldLog()) {
                Log("[VR][scene-ready] SCENE_READY_HOLD reason=keep-armed");
            }
            return;
        }
    }
    const bool persistentVlsrpLifecycle = stage_ == LadderStage::StereoFull;
    for (std::size_t eye = 0; eye < eyeGameObjects_.size(); ++eye) {
        if (persistentVlsrpLifecycle) {
            if (cameraEnabled_[eye] && !SetCameraEnabled(eye, false)) {
                FailStage("safe-point.disable-camera", eye);
                return;
            }
        } else if (gameObjectActive_[eye] &&
                   !SetGameObjectActive(eye, false)) {
            FailStage("safe-point.deactivate", eye);
            return;
        }
    }
    stageArmed_ = false;
    decisionPending_ = false;
    Log("[VR][stereo] STAGE_DISARMED stage=" +
        std::string(StageName(stage_)) +
        " cameraMask=0 gameObjects=" +
        (persistentVlsrpLifecycle ? "persistent" : "inactive"));
    if (!decisionPassed_) {
        const std::string_view reason =
            decisionReason_ != nullptr ? decisionReason_ : "unknown";
        const bool sceneReadyPark = reason == "scene-ready-park" ||
            (stage_ == LadderStage::StereoFull &&
             !SceneReadyAllowsStereoRender());
        const bool resumable = reason == "scene-ineligible" || sceneReadyPark;
        Log("[VR][stereo] " +
            std::string(resumable ? "STAGE_SUSPENDED" : "STAGE_ABORT") +
            " stage=" + StageName(stage_) + " reason=" +
            std::string(reason));
        InvalidateUnityStereoFrame();
        if (sceneReadyPark) {
            const bool firstPark = !sceneReadyParked_;
            sceneReadyParked_ = true;
            if (firstPark) {
                DropOutlineMaterialCache("scene-ready-park");
                DropProFlareCache("scene-ready-park");
                DropLiveCameraOverlayCache("scene-ready-park");
                DropCmovParticleCache("scene-ready-park");
                if (SceneReadyAllowsStereoRender()) {
                    // A loading fall may have completed before this queued
                    // park reached the safe point. Re-arm after the drop so
                    // the content-ready fallback edge is not lost.
                    RequestActorOutlineDiscover("content-ready-after-park");
                }
            }
            RestoreSourceCamera("scene-ready-park");
            Log("[VR][scene-ready] SCENE_READY_PARK eyes=off source=restored");
        }
        if (!resumable) {
            stage_ = LadderStage::Failed;
        }
        return;
    }
    Log("[VR][stereo] STAGE_PASS stage=" + std::string(StageName(stage_)));
    if (stage_ == LadderStage::StereoFull) {
        publishPending_ = true;
        publishAfterLoopSerial_ = renderLoopSerial_ + 1U;
        return;
    }
    AdvanceStage(true);
    // Re-arm only from the next post-Cinemachine Tick. It supplies a fresh
    // source Camera, pose and confirmed target generation after scene changes.
}

void UnityStereoRenderer::ResetTemporalHistories(const char* reason) noexcept {
    const std::string tag = reason != nullptr ? reason : "unknown";
    taaHistoryResetPending_.fill(true);
    ResetSkyTaaSamples();
    smaaT2xPass_.ResetHistory();
    tscmaaPass_.ResetHistory();
    smaaT2xPhase_ = 0;
    smaaT2xPairPhase_ = 0;
    smaaT2xResolvedCount_ = 0;
    tscmaaResolvedCount_ = 0;
    smaaT2xComprehensiveProbeForPair_ = false;
    tscmaaComprehensiveProbeForPair_ = false;
    smaaT2xExpectedProjectionValid_.fill(false);
    smaaT2xMotionCopyTokens_.fill(0);
    smaaT2xOrderedMotionReadyForPair_ = false;
    ++temporalHistoryResetCount_;
    if (lastTemporalHistoryResetReason_ != tag ||
        temporalHistoryResetCount_ <= 2U ||
        temporalHistoryResetCount_ % 300U == 0U) {
        Log("[VR][smaa-t2x] SMAA_T2X_HISTORY_RESET reason=" + tag +
            " count=" + std::to_string(temporalHistoryResetCount_));
        if (tscmaaRequestedForPair_ ||
            GakumasLocal::Config::vrEyeAaMode == 5 ||
            lastNativeTemporalAaMode_ == 5) {
            Log("[VR][tscmaa] TSCMAA_HISTORY_RESET reason=" + tag +
                " count=" + std::to_string(temporalHistoryResetCount_));
        }
        lastTemporalHistoryResetReason_ = tag;
    }
}

void UnityStereoRenderer::FallBackSmaaT2x(const char* reason) noexcept {
    const std::string tag = reason != nullptr ? reason : "unknown";
    ResetTemporalHistories(tag.c_str());
    smaaT2xReady_ = false;
    smaaT2xActiveForPair_ = false;
    Log("[VR][smaa-t2x] SMAA_T2X_FALLBACK reason=" + tag +
        " action=withhold-jittered-pair next=urp-smaa-warmup");
}

void UnityStereoRenderer::FallBackTscmaa(const char* reason) noexcept {
    const std::string tag = reason != nullptr ? reason : "unknown";
    ResetTemporalHistories(tag.c_str());
    tscmaaReady_ = false;
    tscmaaActiveForPair_ = false;
    Log("[VR][tscmaa] TSCMAA_FALLBACK reason=" + tag +
        " action=withhold-temporal-pair next=urp-smaa-warmup");
}

void UnityStereoRenderer::TryPublishStereoAtSafePoint() noexcept {
    if (!publishPending_ || stage_ != LadderStage::StereoFull ||
        renderLoopSerial_ < publishAfterLoopSerial_) {
        return;
    }
    if (!SceneReadyAllowsStereoPublish()) {
        publishPending_ = false;
        smaaT2xMotionCopyTokens_.fill(0);
        smaaT2xOrderedMotionReadyForPair_ = false;
        if (SceneReadyConsumePublishHoldLog()) {
            Log("[VR][scene-ready] SCENE_READY_HOLD reason=withhold-mailbox");
        }
        return;
    }
    // The diagnostic boundary is one-shot. Any clean failure below moves the
    // ladder to Failed and must not repeat native calls on later render loops.
    publishPending_ = false;
    using GetNativeTexturePtr = void* (*)(void*, void*);
    std::array<void*, 2> nativePointers{};
    std::array<ID3D11Texture2D*, 2> textures{};
    for (std::size_t eye = 0; eye < 2U; ++eye) {
        Log("[VR][stereo] CALL_BEGIN stage=native.get-texture eye=" +
            std::string(eye == 0U ? "left" : "right"));
        if (!InvokeManagedResult<void*, GetNativeTexturePtr>(
                api_.textureGetNativeTexturePtr, &nativePointers[eye],
                fullTargets_[eye]) || nativePointers[eye] == nullptr) {
            for (auto* texture : textures) {
                if (texture != nullptr) texture->Release();
            }
            FailStage("native.get-texture", eye);
            return;
        }
        Log("[VR][stereo] CALL_OK stage=native.get-texture eye=" +
            std::string(eye == 0U ? "left" : "right") + " ptr=" +
            std::to_string(reinterpret_cast<std::uintptr_t>(nativePointers[eye])));
        Log("[VR][stereo] CALL_BEGIN stage=native.query-d3d11 eye=" +
            std::string(eye == 0U ? "left" : "right"));
        if (!QueryD3D11Texture(nativePointers[eye], &textures[eye])) {
            for (auto* texture : textures) {
                if (texture != nullptr) texture->Release();
            }
            FailStage("native.query-d3d11", eye);
            return;
        }
        Log("[VR][stereo] CALL_OK stage=native.query-d3d11 eye=" +
            std::string(eye == 0U ? "left" : "right"));
        D3D11_TEXTURE2D_DESC description{};
        Log("[VR][stereo] CALL_BEGIN stage=native.get-desc eye=" +
            std::string(eye == 0U ? "left" : "right"));
        textures[eye]->GetDesc(&description);
        Log("[VR][stereo] CALL_OK stage=native.get-desc eye=" +
            std::string(eye == 0U ? "left" : "right"));
        std::ostringstream ready;
        ready << "[VR][stereo] NATIVE_POINTER_READY eye="
              << (eye == 0U ? "left" : "right") << " ptr=" << textures[eye]
              << " size=" << description.Width << 'x' << description.Height
              << " format=" << static_cast<unsigned int>(description.Format)
              << " samples=" << description.SampleDesc.Count;
        Log(ready.str());
    }
    std::array<ID3D11Texture2D*, 2> publishTextures = textures;
    if (smaaT2xRequestedForPair_) {
        const bool orderedMotionReady = smaaT2xOrderedMotionReadyForPair_;
        using GetBool = bool (*)(void*, void*);
        std::array<bool, 2> srgb{};
        bool srgbReady = true;
        for (std::size_t eye = 0; eye < srgb.size(); ++eye) {
            if (!InvokeManagedResult<bool, GetBool>(
                    api_.renderTextureGetSrgb, &srgb[eye],
                    fullTargets_[eye])) {
                srgbReady = false;
                break;
            }
        }
        if (srgbReady && srgb[0] != srgb[1]) {
            srgbReady = false;
        }

        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        textures[0]->GetDevice(&device);
        if (device != nullptr) {
            device->GetImmediateContext(&context);
        }
        D3D11_TEXTURE2D_DESC colorDescription{};
        textures[0]->GetDesc(&colorDescription);
        const bool capturedBeforePrepare = orderedMotionReady &&
            smaaT2xPass_.HasFreshMotionVectors(smaaT2xPairToken_);
        d3d11::SmaaT2xPass::PrepareDiagnostics prepare{};
        const bool prepared = srgbReady && device != nullptr &&
            smaaT2xPass_.Prepare(
                device, colorDescription, srgb[0], &prepare);
        const bool freshAfterPrepare = prepared &&
            smaaT2xPass_.HasFreshMotionVectors(smaaT2xPairToken_);

        if (smaaT2xActiveForPair_) {
            std::array<ID3D11Texture2D*, 2> resolved{};
            const bool collectReprojectionAlignment =
                smaaT2xComprehensiveProbeForPair_;
            const bool resolvedReady = prepared && capturedBeforePrepare &&
                freshAfterPrepare && context != nullptr &&
                smaaT2xPass_.ResolveStereo(
                    context, textures, srgb[0],
                    GakumasLocal::Config::vrEyeSmaaQuality,
                    smaaT2xPairPhase_, smaaT2xPairToken_,
                    collectReprojectionAlignment, resolved);
            if (!resolvedReady) {
                if (context != nullptr) context->Release();
                if (device != nullptr) device->Release();
                for (auto* texture : textures) {
                    if (texture != nullptr) texture->Release();
                }
                const std::string failure = !srgbReady
                    ? "srgb-mismatch"
                    : !prepared
                        ? std::string("prepare-") +
                            d3d11::SmaaT2xPass::StatusName(
                                smaaT2xPass_.LastStatus())
                        : !capturedBeforePrepare || !freshAfterPrepare
                            ? "motion-vectors-missing-or-stale"
                            : std::string("resolve-") +
                                d3d11::SmaaT2xPass::StatusName(
                                    smaaT2xPass_.LastStatus());
                FallBackSmaaT2x(failure.c_str());
                return;
            }
            publishTextures = resolved;
            ++smaaT2xResolvedCount_;
            if (collectReprojectionAlignment) {
                d3d11::SmaaT2xPass::ReprojectionAlignmentDiagnostics
                    alignment{};
                const bool alignmentReady =
                    smaaT2xPass_.ReadReprojectionAlignment(context, alignment);
                const std::array<const char*,
                    d3d11::SmaaT2xPass::ProbeGroupCount> candidateLabels{
                    "own-current,other-current,average-current,zero",
                    "own-current,own-previous,other-previous,average-previous",
                    "own,expected-jitter,inverse-jitter,jitter-only",
                    "current-alpha,history-alpha,history-weight,color-error",
                    "resolved-current-error,current-luma,resolved-luma,alpha-delta",
                };
                for (std::size_t group = 0;
                     group < d3d11::SmaaT2xPass::ProbeGroupCount; ++group) {
                    std::ostringstream line;
                    line << std::fixed << std::setprecision(7)
                         << "[VR][smaa-t2x] SMAA_T2X_PROBE token="
                         << smaaT2xPairToken_ << " count="
                         << smaaT2xResolvedCount_ << " ready="
                         << (alignmentReady ? 1 : 0) << " group="
                         << d3d11::SmaaT2xPass::ProbeGroupName(
                                static_cast<d3d11::SmaaT2xPass::ProbeGroup>(group))
                         << " candidates=" << candidateLabels[group];
                    for (std::size_t eye = 0; eye < 2U; ++eye) {
                        const auto& values = alignment.groups[group][eye];
                        const char* prefix = eye == 0U ? "left" : "right";
                        const auto append = [&](const char* suffix, const auto& list) {
                            line << ' ' << prefix << suffix << '=';
                            for (std::size_t candidate = 0;
                                 candidate < list.size(); ++candidate) {
                                if (candidate != 0U) line << ',';
                                line << list[candidate];
                            }
                        };
                        append("Mean", values.meanValue);
                        append("Min", values.minimumValue);
                        append("Max", values.maximumValue);
                        append("Valid", values.validSampleCount);
                    }
                    Log(line.str());
                }
                const auto resources = smaaT2xPass_.GetResourceDiagnostics();
                std::ostringstream resourceLine;
                resourceLine << "[VR][smaa-t2x] SMAA_T2X_RESOURCES token="
                             << smaaT2xPairToken_ << " count="
                             << smaaT2xResolvedCount_ << " phase="
                             << smaaT2xPairPhase_ << " poseRevision="
                             << armedFrame_.trackingSample.revision
                             << " poseEpoch="
                             << armedFrame_.trackingSample.poseEpoch;
                const auto& currentPhase =
                    d3d11::kSmaaT2xPhases[smaaT2xPairPhase_];
                const auto& previousPhase =
                    d3d11::kSmaaT2xPhases[smaaT2xPairPhase_ ^ 1U];
                resourceLine << std::fixed << std::setprecision(9)
                             << " jitterDeltaUv="
                             << (previousPhase.jitterX - currentPhase.jitterX) /
                                    static_cast<float>(colorDescription.Width)
                             << ','
                             << (previousPhase.jitterY - currentPhase.jitterY) /
                                    static_cast<float>(colorDescription.Height);
                for (std::size_t eye = 0; eye < resources.eyes.size(); ++eye) {
                    const auto& value = resources.eyes[eye];
                    resourceLine << ' ' << (eye == 0U ? "left=" : "right=")
                                 << std::hex << "input:0x"
                                 << reinterpret_cast<std::uintptr_t>(textures[eye])
                                 << ",native:0x"
                                 << reinterpret_cast<std::uintptr_t>(nativePointers[eye])
                                 << ",mv:0x" << value.motion
                                 << ",currentMv:0x" << value.currentMotionProbe
                                 << ",prevMv:0x" << value.previousMotionProbe
                                 << ",current:0x" << value.currentSpatial
                                 << ",previous:0x" << value.previousSpatial
                                 << ",resolved:0x" << value.resolved << std::dec
                                 << ",mvToken:" << value.motionPairToken
                                 << ",currentMvToken:"
                                 << value.currentMotionProbePairToken
                                 << ",prevMvToken:"
                                 << value.previousMotionProbePairToken
                                 << ",nextSpatial:" << value.nextSpatialWriteIndex
                                 << ",history:" << (value.historyValid ? 1 : 0);
                }
                Log(resourceLine.str());
            }
            Log("[VR][smaa-t2x] SMAA_T2X_FRAME resolved=1 token=" +
                std::to_string(smaaT2xPairToken_) + " phase=" +
                std::to_string(smaaT2xPairPhase_) + " history=" +
                std::string(smaaT2xPass_.HasHistory() ? "valid" : "cold"));
        } else if (prepared && capturedBeforePrepare) {
            // Prepare may rebuild size-dependent resources and intentionally
            // retire the warmup MV copies. Readiness begins on the next pair,
            // whose captures will target the final immutable snapshots.
            smaaT2xReady_ = true;
            smaaT2xPhase_ = 0;
            Log("[VR][smaa-t2x] SMAA_T2X_READY size=" +
                std::to_string(prepare.width) + "x" +
                std::to_string(prepare.height) + " format=" +
                std::to_string(static_cast<unsigned int>(
                    prepare.resourceFormat)) + " viewFormat=" +
                std::to_string(static_cast<unsigned int>(prepare.viewFormat)) +
                " allocationMiB=" + std::to_string(
                    static_cast<double>(prepare.allocationBytes) /
                    (1024.0 * 1024.0)) + " allocationMs=" +
                std::to_string(prepare.allocationMilliseconds));
        } else {
            smaaT2xReady_ = false;
            const std::string reason = !srgbReady
                ? "srgb-unavailable"
                : !prepared
                    ? std::string("prepare-") +
                        d3d11::SmaaT2xPass::StatusName(
                            smaaT2xPass_.LastStatus())
                    : "motion-vectors-not-yet-complete";
            const auto warmupCount = ++smaaT2xWarmupLogCount_;
            if (warmupCount <= 2U || warmupCount % 300U == 0U) {
                Log("[VR][smaa-t2x] SMAA_T2X_WARMUP ready=0 count=" +
                    std::to_string(warmupCount) + " reason=" + reason +
                    " publish=urp-smaa");
            }
        }
        if (context != nullptr) context->Release();
        if (device != nullptr) device->Release();
    } else if (tscmaaRequestedForPair_) {
        const bool orderedMotionReady = smaaT2xOrderedMotionReadyForPair_;
        using GetBool = bool (*)(void*, void*);
        std::array<bool, 2> srgb{};
        bool srgbReady = true;
        for (std::size_t eye = 0; eye < srgb.size(); ++eye) {
            if (!InvokeManagedResult<bool, GetBool>(
                    api_.renderTextureGetSrgb, &srgb[eye],
                    fullTargets_[eye])) {
                srgbReady = false;
                break;
            }
        }
        if (srgbReady && srgb[0] != srgb[1]) {
            srgbReady = false;
        }

        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        textures[0]->GetDevice(&device);
        if (device != nullptr) {
            device->GetImmediateContext(&context);
        }
        D3D11_TEXTURE2D_DESC colorDescription{};
        textures[0]->GetDesc(&colorDescription);
        const bool capturedBeforePrepare = orderedMotionReady &&
            tscmaaPass_.HasFreshMotionVectors(smaaT2xPairToken_);
        d3d11::TscmaaPass::PrepareDiagnostics prepare{};
        const bool prepared = srgbReady && device != nullptr &&
            tscmaaPass_.Prepare(
                device, colorDescription, srgb[0], &prepare);
        const bool freshAfterPrepare = prepared &&
            tscmaaPass_.HasFreshMotionVectors(smaaT2xPairToken_);

        if (tscmaaActiveForPair_) {
            std::array<ID3D11Texture2D*, 2> resolved{};
            d3d11::TscmaaPass::TemporalStats temporalStats{};
            const bool resolvedReady = prepared && capturedBeforePrepare &&
                freshAfterPrepare && context != nullptr &&
                tscmaaPass_.ResolveStereo(
                    context, textures, srgb[0],
                    GakumasLocal::Config::vrEyeSmaaQuality,
                    smaaT2xPairToken_, resolved,
                    tscmaaComprehensiveProbeForPair_ ? &temporalStats
                                                     : nullptr);
            if (!resolvedReady) {
                if (context != nullptr) context->Release();
                if (device != nullptr) device->Release();
                for (auto* texture : textures) {
                    if (texture != nullptr) texture->Release();
                }
                const std::string failure = !srgbReady
                    ? "srgb-mismatch"
                    : !prepared
                        ? std::string("prepare-") +
                            d3d11::TscmaaPass::StatusName(
                                tscmaaPass_.LastStatus())
                        : !capturedBeforePrepare || !freshAfterPrepare
                            ? "motion-vectors-missing-or-stale"
                            : std::string("resolve-") +
                                d3d11::TscmaaPass::StatusName(
                                    tscmaaPass_.LastStatus());
                FallBackTscmaa(failure.c_str());
                return;
            }
            publishTextures = resolved;
            ++tscmaaResolvedCount_;
            if (tscmaaComprehensiveProbeForPair_) {
                const auto resources = tscmaaPass_.GetResourceDiagnostics();
                std::ostringstream resourceLine;
                resourceLine << "[VR][tscmaa] TSCMAA_RESOURCES token="
                             << smaaT2xPairToken_ << " count="
                             << tscmaaResolvedCount_
                             << " poseRevision="
                             << armedFrame_.trackingSample.revision
                             << " poseEpoch="
                             << armedFrame_.trackingSample.poseEpoch
                             << " quality="
                             << GakumasLocal::Config::vrEyeSmaaQuality
                             << " workingEdges=0x" << std::hex
                             << resources.workingEdges
                             << " workingShapeCandidates=0x"
                             << resources.workingShapeCandidates
                             << " workingDeferredItems=0x"
                             << resources.workingDeferredItems;
                for (std::size_t eye = 0; eye < resources.eyes.size(); ++eye) {
                    const auto& value = resources.eyes[eye];
                    resourceLine << ' ' << (eye == 0U ? "left=" : "right=")
                                 << "input:0x"
                                 << reinterpret_cast<std::uintptr_t>(textures[eye])
                                 << ",native:0x"
                                 << reinterpret_cast<std::uintptr_t>(nativePointers[eye])
                                 << ",mv:0x" << value.motion
                                 << ",spatial:0x" << value.spatial
                                 << ",history:0x" << value.resolvedHistory
                                 << ",output:0x" << value.resolvedOutput
                                 << std::dec
                                 << ",mvToken:" << value.motionPairToken
                                 << ",nextResolved:"
                                 << value.nextResolvedWriteIndex
                                 << ",historyValid:"
                                 << (value.historyValid ? 1 : 0);
                }
                Log(resourceLine.str());
            }
            if (tscmaaComprehensiveProbeForPair_) {
                std::ostringstream statsLine;
                statsLine << "[VR][tscmaa] TSCMAA_TEMPORAL_STATS token="
                          << smaaT2xPairToken_ << " count="
                          << tscmaaResolvedCount_ << " valid="
                          << (temporalStats.valid ? 1 : 0);
                const double pixelCount =
                    static_cast<double>(colorDescription.Width) *
                    static_cast<double>(colorDescription.Height);
                for (std::size_t eye = 0;
                     eye < temporalStats.eyes.size(); ++eye) {
                    const auto& value = temporalStats.eyes[eye];
                    const double blended =
                        static_cast<double>(value.blendedPixels);
                    statsLine << ' ' << (eye == 0U ? "left=" : "right=")
                              << "edges:" << value.edgeCandidatePixels
                              << ",edgePct:"
                              << (pixelCount > 0.0
                                      ? 100.0 * value.edgeCandidatePixels /
                                            pixelCount
                                      : 0.0)
                              << ",blended:" << value.blendedPixels
                              << ",meanWeight:"
                              << (blended > 0.0
                                      ? value.weightMilliSum /
                                            (1000.0 * blended)
                                      : 0.0)
                              << ",meanDelta:"
                              << (blended > 0.0
                                      ? value.deltaMilliSum /
                                            (1000.0 * blended)
                                      : 0.0);
                }
                Log(statsLine.str());
            }
            Log("[VR][tscmaa] TSCMAA_FRAME resolved=1 token=" +
                std::to_string(smaaT2xPairToken_) + " history=" +
                std::string(tscmaaPass_.HasHistory() ? "valid" : "cold") +
                " jitter=off");
        } else if (prepared && capturedBeforePrepare) {
            // Prepare may rebuild size-dependent resources and retire the
            // warmup MV snapshots. Readiness begins on the next complete pair.
            tscmaaReady_ = true;
            Log("[VR][tscmaa] TSCMAA_READY size=" +
                std::to_string(prepare.width) + "x" +
                std::to_string(prepare.height) + " format=" +
                std::to_string(static_cast<unsigned int>(
                    prepare.resourceFormat)) + " viewFormat=" +
                std::to_string(static_cast<unsigned int>(prepare.viewFormat)) +
                " allocationMiB=" + std::to_string(
                    static_cast<double>(prepare.allocationBytes) /
                    (1024.0 * 1024.0)) + " allocationMs=" +
                std::to_string(prepare.allocationMilliseconds) +
                " jitter=off");
        } else {
            tscmaaReady_ = false;
            const std::string reason = !srgbReady
                ? "srgb-unavailable"
                : !prepared
                    ? std::string("prepare-") +
                        d3d11::TscmaaPass::StatusName(
                            tscmaaPass_.LastStatus())
                    : "motion-vectors-not-yet-complete";
            const auto warmupCount = ++tscmaaWarmupLogCount_;
            if (warmupCount <= 2U || warmupCount % 300U == 0U) {
                Log("[VR][tscmaa] TSCMAA_WARMUP ready=0 count=" +
                    std::to_string(warmupCount) + " reason=" + reason +
                    " publish=urp-smaa jitter=off");
            }
        }
        if (context != nullptr) context->Release();
        if (device != nullptr) device->Release();
    }
    d3d11::StereoRenderMailbox::PublishDiagnostics diagnostics{};
    // GPU readback is useful for admission diagnostics but stalls the shared
    // immediate context. Never repeat it during steady immersive rendering.
    if (verboseFrameLog_ && publishedFrames_ < 2U) {
        std::array<std::uint64_t, 2> fingerprints{};
        std::array<bool, 2> sampled{};
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        textures[0]->GetDevice(&device);
        if (device != nullptr) {
            device->GetImmediateContext(&context);
            device->Release();
        }
        if (context != nullptr) {
            for (std::size_t eye = 0; eye < textures.size(); ++eye) {
                sampled[eye] = d3d11::ComputeTextureFingerprint(
                    context, publishTextures[eye], 0, fingerprints[eye]);
            }
            context->Release();
        }
        std::ostringstream content;
        content << "[VR][stereo] STEREO_SOURCE_FINGERPRINT nextFrame="
                << (publishedFrames_ + 1U);
        for (std::size_t eye = 0; eye < fingerprints.size(); ++eye) {
            content << ' ' << (eye == 0U ? "left=" : "right=");
            if (sampled[eye]) {
                content << "0x" << std::hex << fingerprints[eye] << std::dec
                        << " changed="
                        << (!sourceFingerprintValid_[eye] ||
                            sourceFingerprints_[eye] != fingerprints[eye]);
                sourceFingerprints_[eye] = fingerprints[eye];
                sourceFingerprintValid_[eye] = true;
            } else {
                content << "unavailable";
            }
        }
        Log(content.str());
    }
    Log("[VR][stereo] MAILBOX_PUBLISH_BEGIN poseRevision=" +
        std::to_string(armedFrame_.trackingSample.revision));
    const bool published = PublishUnityStereoFrame(
        publishTextures[0], publishTextures[1], armedFrame_.trackingSample,
        &diagnostics);
    for (auto* texture : textures) {
        if (texture != nullptr) texture->Release();
    }
    if (!published) {
        Log("[VR][stereo] MAILBOX_PUBLISH_REJECTED reason=" +
            std::string(d3d11::StereoRenderMailbox::PublishStatusName(
                diagnostics.status)) + " stagingHr=" +
            std::to_string(static_cast<long>(diagnostics.stagingHresult)));
        FailStage("native.mailbox-publish");
        return;
    }
    continuousStereo_ = true;
    ++publishedFrames_;
    if (smaaT2xActiveForPair_) {
        smaaT2xPhase_ ^= 1U;
    }
    Log("[VR][stereo] MAILBOX_PUBLISHED frame=" +
        std::to_string(publishedFrames_) + " poseRevision=" +
        std::to_string(armedFrame_.trackingSample.revision) + " stagingSlot=" +
        std::to_string(diagnostics.stagingSlot));
}

void UnityStereoRenderer::OnRenderLoopCompleted() noexcept {
    if (!IsOwnerThread()) {
        return;
    }
    if (!renderLoopBoundaryLogged_) {
        renderLoopBoundaryLogged_ = true;
        Log("[VR][stereo] RENDER_LOOP_BOUNDARY_READY tid=" +
            std::to_string(GetCurrentThreadId()) +
            " completedContext=" + std::to_string(completedContextEpoch_));
    }
    ++renderLoopSerial_;
    const bool stereoEligible = ReadStereoRenderTargetSpec().enabled;
    const bool sourcePresent = stereoEligible &&
        !stereoInvalidatedForIneligibility_ &&
        latestSourceCamera_ != nullptr &&
        IsUnityManagedObjectAlive(latestSourceCamera_);
    SampleSceneReady("render-loop", stereoEligible, sourcePresent);
    ObserveLifetimeSceneReadyEdges("render-loop-after-sample");
    if (SceneReadyConsumeContentReadyEdge()) {
        RequestActorOutlineDiscover("content-ready");
    }
    // .165 released the ready epoch while StereoFull stayed bound to a
    // skipped context. Rebind so the next EndContext can queue pass.
    if (SceneReadyAllowsStereoPublish() && stageArmed_ &&
        stage_ == LadderStage::StereoFull && !decisionPending_ &&
        !publishPending_ && publishedFrames_ == 0U) {
        stageContextEpoch_ = 0;
    }
    RefreshSceneIdentity("render-loop");
    if (!stereoEligible) {
        ApplySceneIneligible();
    } else if (EyeArmHeld() && stageArmed_ && !decisionPending_) {
        QueueStageDecision(false, "scene-ineligible");
    }
    // Both eyes queued their MV copies inside the just-submitted context, so
    // the mod-owned per-eye copy targets are now the stable snapshot sources.
    if ((smaaT2xRequestedForPair_ || tscmaaRequestedForPair_) && stageArmed_ &&
        !smaaT2xOrderedMotionReadyForPair_) {
        smaaT2xOrderedMotionReadyForPair_ =
            CaptureQueuedSmaaT2xMotionVectors();
    }
    ConsumeStageDecisionAtSafePoint();
    TryPublishStereoAtSafePoint();
}

void UnityStereoRenderer::AdvanceStage(bool passed) noexcept {
    if (!passed) {
        stage_ = LadderStage::Failed;
        return;
    }
    const LadderStage previous = stage_;
    if (stage_ == LadderStage::AdmissionOne) {
        stage_ = LadderStage::AdmissionTwo;
    } else if (stage_ == LadderStage::AdmissionTwo) {
        stage_ = LadderStage::StereoFull;
    }
    Log("[VR][stereo] STAGE_UPGRADE from=" +
        std::string(StageName(previous)) + " to=" + StageName(stage_));
}

const char* UnityStereoRenderer::StageName(LadderStage stage) noexcept {
    switch (stage) {
    case LadderStage::AdmissionOne: return "A";
    case LadderStage::AdmissionTwo: return "B";
    case LadderStage::StereoFull: return "C";
    case LadderStage::Complete: return "complete";
    case LadderStage::Failed: return "failed";
    }
    return "unknown";
}

std::uint8_t UnityStereoRenderer::ExpectedMask() const noexcept {
    return stage_ == LadderStage::AdmissionOne ? 0x1U : 0x3U;
}

std::size_t UnityStereoRenderer::ExpectedCameraCount() const noexcept {
    return stage_ == LadderStage::AdmissionOne ? 1U : 2U;
}

bool UnityStereoRenderer::IsOwnerThread() noexcept {
    const std::uint32_t current = GetCurrentThreadId();
    if (ownerThreadId_ == 0U) {
        ownerThreadId_ = current;
        Log("[VR][stereo] QUEUE_LADDER_OWNER_THREAD tid=" +
            std::to_string(current));
        return true;
    }
    if (ownerThreadId_ == current) {
        return true;
    }
    if (!ownerThreadMismatchLogged_) {
        ownerThreadMismatchLogged_ = true;
        Log("[VR][stereo] QUEUE_LADDER_THREAD_MISMATCH owner=" +
            std::to_string(ownerThreadId_) + " current=" +
            std::to_string(current));
    }
    return false;
}

void UnityStereoRenderer::Release(bool pipelineIdle) noexcept {
    (void)pipelineIdle;
    releaseRequested_.store(true, std::memory_order_release);
    // Release may be called off the Unity main thread. It only publishes the
    // request; the Unity-thread Tick owns all queue state and managed mutation.
    InvalidateUnityStereoFrame();
}

bool UnityStereoRenderer::FailStage(
    const char* stage,
    std::size_t eye) noexcept {
    failureStage_ = stage != nullptr ? stage : "unknown";
    std::ostringstream stream;
    stream << "[VR][stereo] STAGE_FAILED ladderStage=" << StageName(stage_)
           << " boundary=" << failureStage_;
    if (eye < 2U) {
        stream << " eye=" << (eye == 0U ? "left" : "right");
    }
    stream << " completedContext=" << completedContextEpoch_;
    Log(stream.str());
    publishPending_ = false;
    decisionPending_ = false;
    smaaT2xMotionCopyTokens_.fill(0);
    smaaT2xOrderedMotionReadyForPair_ = false;
    stage_ = LadderStage::Failed;
    RestoreSourceCamera("stage-failed");
    RestoreUiTextureOverlays("stage-failed");
    InvalidateUnityStereoFrame();
    return false;
}

void UnityStereoRenderer::Log(std::string_view message) const noexcept {
    if (stage_ == LadderStage::StereoFull && continuousStereo_ &&
        !verboseFrameLog_) {
        // [VR][shadow] passes the mute: every shadow diagnostic is already
        // wall-clock throttled (>=1.5 s), and this steady-stereo filter is
        // what silently ate ALL shadow telemetry on the .88-.91 runs (the
        // "impossible" pattern of counters rising while their log lines
        // vanished, resuming instantly in scene-ineligible windows).
        const bool alwaysKeep =
            message.find("[VR][shadow]") != std::string_view::npos ||
            message.find("[VR][fov]") != std::string_view::npos ||
            message.find("FAILED") != std::string_view::npos ||
            message.find("REJECTED") != std::string_view::npos ||
            message.find("MISMATCH") != std::string_view::npos ||
            message.find("OUTSIDE_CONTEXT") != std::string_view::npos ||
            message.find("ROLLBACK") != std::string_view::npos ||
            message.find("FAULT") != std::string_view::npos ||
            message.find("EXCEPTION") != std::string_view::npos ||
            message.find("EYE_OUTLINE") != std::string_view::npos ||
            message.find("GRIP_OUTLINE") != std::string_view::npos ||
            message.find("OUTLINE_OFFICIAL") != std::string_view::npos ||
            message.find("SOURCE_CAMERA_SUPPRESSED") != std::string_view::npos ||
            message.find("SOURCE_CAMERA_RESTORED") != std::string_view::npos ||
            message.find("SOURCE_CAMERA_REENABLE") != std::string_view::npos ||
            message.find("SOURCE_TINY") != std::string_view::npos ||
            // Grip transparency apply/lift must stay visible in steady
            // stereo or a dead toggle would be indistinguishable from an
            // inactive scene.
            message.find("GRIP_TRANSPARENCY") != std::string_view::npos ||
            message.find("GRIP_TRACE_") != std::string_view::npos ||
            message.find("GRIP_BLUR_SOURCE_") != std::string_view::npos ||
            message.find("SOURCE_HISTORY_RESET") != std::string_view::npos ||
            message.find("SOURCE_CUT_PULSE") != std::string_view::npos ||
            message.find("TAA_PASS") != std::string_view::npos ||
            message.find("TAA_JITTER") != std::string_view::npos ||
            message.find("TAA_MV_UPDATE") != std::string_view::npos ||
            message.find("SMAA_T2X_") != std::string_view::npos ||
            message.find("TSCMAA_") != std::string_view::npos ||
            // Functional VL post-process actions must remain visible even
            // when steady stereo mutes frame-level diagnostics. Otherwise a
            // dead menu toggle is indistinguishable from an inactive scene.
            message.find("EYE_JUMPFLOOD") != std::string_view::npos ||
            message.find("VL_MOTION_BLUR") != std::string_view::npos ||
            message.find("EYE_VL_FLARE") != std::string_view::npos ||
            message.find("EYE_VL_TEXTURE_BLUR") != std::string_view::npos ||
            message.find("EYE_RENDER_OBJECTS") != std::string_view::npos ||
            message.find("OBJECT_MV") != std::string_view::npos ||
            message.find("DEFERRED_STENCIL") != std::string_view::npos ||
            message.find("SKY_RUNTIME_LAYOUT") != std::string_view::npos ||
            message.find("COMPUTE_VIEW_DIR") != std::string_view::npos ||
            message.find("VLSRP_HISTORY_RESET") != std::string_view::npos ||
            message.find("COPY_FROM_SKIPPED") != std::string_view::npos ||
            message.find("TAA_INPUT") != std::string_view::npos ||
            // .114: the applied=1 line was eaten by this very mute, which
            // cost a night of forensics. Tag traffic always passes.
            message.find("EYE_MAIN_CAMERA_TAG") != std::string_view::npos ||
            message.find("STEREO_PORTRAIT_ARMED") != std::string_view::npos ||
            message.find("STEREO_FRAME_INVALIDATED") != std::string_view::npos ||
            message.find("SCENE_IDENTITY") != std::string_view::npos ||
            // Per-scene virtual-camera diagnostics are low-frequency and
            // explain whether a requested specific-camera selection is even
            // possible. Never let steady stereo hide the only census proof.
            message.find("VCAM_") != std::string_view::npos ||
            message.find("OUTLINE_CACHE_DROPPED") != std::string_view::npos ||
            message.find("PRO_FLARE_CACHE_DROPPED") != std::string_view::npos ||
            message.find("UI_TEXTURE_OVERLAY") != std::string_view::npos ||
            message.find("LIVE_CAMERA_OVERLAY") != std::string_view::npos ||
            message.find("LIVE_CMOV_PS") != std::string_view::npos ||
            message.find("PRO_FLARE_RENDER") != std::string_view::npos ||
            message.find("HEAVY_DISCOVER") != std::string_view::npos ||
            message.find("EYE_ARM_HELD") != std::string_view::npos ||
            message.find("EYE_ARM_SKIPPED") != std::string_view::npos ||
            message.find("EYE_ARM_RELEASED") != std::string_view::npos ||
            message.find("SCENE_READY") != std::string_view::npos ||
            message.find("LIFETIME_") != std::string_view::npos ||
            message.find("PORTRAIT_LATCH") != std::string_view::npos ||
            message.find("FREECAM_HEAD_SKIP") != std::string_view::npos ||
            message.find("HAND_GLOW") != std::string_view::npos;
        if (!alwaysKeep) {
            return;
        }
    }
    WriteVrLog(message);
}

} // namespace gakumas::vr
