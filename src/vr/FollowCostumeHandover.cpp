#include "camera/FollowActorSelection.hpp"
#include "FollowCostumeHandover.hpp"
#include "LivePause.hpp"
#include "VrFreeCamera.hpp"
#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "../GakumasLocalify/Il2cppUtils.hpp"
#include "../GakumasLocalify/camera/camera.hpp"
#include "../deps/UnityResolve/UnityResolve.hpp"
#include "../hooks/HookManager.hpp"
#include <Windows.h>
#include <cstring>
#include <initializer_list>
#include <intrin.h>
#include <sstream>
#include <string>

namespace gakumas::vr {
namespace {
using Class = UnityResolve::Class;
using Method = UnityResolve::Method;
using String = UnityResolve::UnityType::String;
using MethodInfo = Il2cppUtils::MethodInfo;

constexpr int kActorCacheCapacity = 16;

struct ActorIdSlot {
    int index = -1;
    void* token = nullptr;
    std::string id;
};

ActorIdSlot actorIds[kActorCacheCapacity]{};
int lastOfficialIndex = -1;
int lastEmptyOfficial = -2;
int pendingOfficialIndex = -1;
std::string lastOfficialId;
void* sceneIdentity = nullptr;
bool ready = false;
bool loggedShape = false;

Class* actorClass = nullptr;
Class* managerClass = nullptr;
Class* timelineClass = nullptr;
Method* getIndex = nullptr;
Method* getDescriptor = nullptr;
Method* getCharacterId = nullptr;
Method* getGameObjectName = nullptr;
Method* getUnityName = nullptr;
Method* getUnityGameObject = nullptr;
Method* setFocusTarget = nullptr;
Method* setFocusTargetIndex = nullptr;
Method* notifySignal = nullptr;
Method* notifySignalIface = nullptr;
MethodInfo* getIndexInfo = nullptr;
MethodInfo* getDescriptorInfo = nullptr;
MethodInfo* getCharacterIdInfo = nullptr;
MethodInfo* getGameObjectNameInfo = nullptr;
MethodInfo* getUnityNameInfo = nullptr;
MethodInfo* getUnityGameObjectInfo = nullptr;
Il2cppUtils::FieldInfo* managerInstanceField = nullptr;
UnityResolve::Field* focusIndexField = nullptr;

using FocusTargetFn = void (*)(void*, void*);
using FocusIndexFn = void (*)(void*, int, void*);
using NotifyFn = void (*)(void*, int, int, void*);
FocusTargetFn originalFocusTarget = nullptr;
FocusIndexFn originalFocusIndex = nullptr;
NotifyFn originalNotify = nullptr;
NotifyFn originalNotifyIface = nullptr;

void Log(const std::string& text) {
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled)
        WriteVrLog("[VR][camera] " + text);
}

template <class T>
T Api(const char* name) noexcept {
    return reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"), name));
}

bool Copy(const void* from, void* to, size_t bytes) noexcept {
    if (!from) return false;
    __try {
        memcpy(to, from, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Alive(void* object) noexcept {
    void* native = nullptr;
    return object &&
        Copy(static_cast<char*>(object) +
                 offsetof(UnityResolve::UnityType::UnityObject, m_CachedPtr),
             &native, sizeof(native)) &&
        native;
}

bool Invoke(Method* m, void* instance, void** args, void** result) noexcept {
    using Fn = void* (*)(void*, void*, void**, void**);
    static auto fn = Api<Fn>("il2cpp_runtime_invoke");
    if (!fn || !m || !m->address) return false;
    void* exception = nullptr;
    __try {
        *result = fn(m->address, instance, args, &exception);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return !exception;
}

// Shared-thunk campus getters (function=93FA3500 family) need the MethodInfo
// trailing argument. LateUpdate get_index is a dedicated pointer and omits it.
void* CallObject(MethodInfo* info, Method* fallback, void* instance) {
    if (!instance) return nullptr;
    if (info && info->methodPointer) {
        using Fn = void* (*)(void*, void*);
        void* value = nullptr;
        __try {
            value = reinterpret_cast<Fn>(info->methodPointer)(instance, info);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            value = nullptr;
        }
        if (value) return value;
    }
    void* value = nullptr;
    if (fallback && Invoke(fallback, instance, nullptr, &value)) return value;
    return nullptr;
}

Method* Exact(Class* c, const char* name, const char* result, bool isStatic,
              std::initializer_list<const char*> args) {
    if (!c) return nullptr;
    for (auto* m : c->methods) {
        if (!m || m->name != name || m->static_function != isStatic || !m->function ||
            !m->address || !m->return_type || m->return_type->name != result ||
            m->args.size() != args.size()) {
            continue;
        }
        size_t i = 0;
        bool match = true;
        for (auto* type : args) {
            auto* a = m->args[i++];
            if (!a || !a->pType || a->pType->name != type) match = false;
        }
        if (match) return m;
    }
    return nullptr;
}

int UnboxInt(void* boxed) noexcept {
    using Unbox = void* (*)(void*);
    static auto unbox = Api<Unbox>("il2cpp_object_unbox");
    int value = -1;
    if (unbox && boxed) Copy(unbox(boxed), &value, sizeof(value));
    return value;
}

std::string StringText(void* value) {
    return value ? static_cast<String*>(value)->ToString() : std::string();
}

int ReadIndex(void* actor) {
    if (!actor) return -1;
    if (getIndexInfo && getIndexInfo->methodPointer) {
        using Fn = int (*)(void*);
        int value = -1;
        __try {
            value = reinterpret_cast<Fn>(getIndexInfo->methodPointer)(actor);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            value = -1;
        }
        if (value >= 0) return value;
    }
    void* boxed = nullptr;
    if (!getIndex || !Invoke(getIndex, actor, nullptr, &boxed)) return -1;
    return UnboxInt(boxed);
}

std::string ReadUnityObjectName(void* object) {
    void* name = CallObject(getUnityNameInfo, getUnityName, object);
    return StringText(name);
}

std::string ReadGameObjectName(void* component) {
    if (!Alive(component)) return {};
    void* go = CallObject(getUnityGameObjectInfo, getUnityGameObject, component);
    if (Alive(go)) {
        const auto name = ReadUnityObjectName(go);
        if (!name.empty()) return name;
    }
    return ReadUnityObjectName(component);
}

std::string ReadCharacterId(void* actor) {
    if (!Alive(actor)) return {};
    void* descriptor = CallObject(getDescriptorInfo, getDescriptor, actor);
    if (Alive(descriptor) || descriptor) {
        const auto id = CharacterIdFromActorName(
            StringText(CallObject(getCharacterIdInfo, getCharacterId, descriptor)));
        if (!id.empty()) return id;
    }
    const auto fromGetter = CharacterIdFromActorName(
        StringText(CallObject(getGameObjectNameInfo, getGameObjectName, actor)));
    if (!fromGetter.empty()) return fromGetter;
    return CharacterIdFromActorName(ReadGameObjectName(actor));
}

std::string LookupId(int index) {
    if (index < 0) return {};
    for (const auto& slot : actorIds) {
        if (slot.index == index) return slot.id;
    }
    return {};
}

void* LookupToken(int index) {
    if (index < 0) return nullptr;
    for (const auto& slot : actorIds) {
        if (slot.index == index) return slot.token;
    }
    return nullptr;
}

void RememberActor(void* actor, int index, const std::string& id) {
    if (!actor || index < 0) return;
    ActorIdSlot* empty = &actorIds[0];
    for (auto& slot : actorIds) {
        if (slot.index == index) {
            slot.token = actor;
            const bool firstId = slot.id.empty() && !id.empty();
            if (!id.empty()) slot.id = id;
            if (firstId) {
                auto line = std::ostringstream();
                line << "FOLLOW_COSTUME_CACHE index=" << index << " id=" << id;
                Log(line.str());
            }
            return;
        }
        if (slot.index < 0) empty = &slot;
    }
    empty->index = index;
    empty->token = actor;
    empty->id = id;
    if (!id.empty()) {
        auto line = std::ostringstream();
        line << "FOLLOW_COSTUME_CACHE index=" << index << " id=" << id;
        Log(line.str());
    }
}

void ResetScene() {
    for (auto& slot : actorIds) slot = {};
    lastOfficialIndex = -1;
    lastEmptyOfficial = -2;
    pendingOfficialIndex = -1;
    lastOfficialId.clear();
}

std::string ResolveId(int index) {
    auto id = LookupId(index);
    if (!id.empty()) return id;
    if (void* token = LookupToken(index); Alive(token)) {
        id = ReadCharacterId(token);
        if (!id.empty()) {
            RememberActor(token, index, id);
            return id;
        }
    }
    if (void* follow = camera::ReadVrFollowActorController();
        Alive(follow) && ReadIndex(follow) == index) {
        id = ReadCharacterId(follow);
        if (!id.empty()) RememberActor(follow, index, id);
        return id;
    }
    return {};
}

void ApplyOfficialFocus(int officialIndex, const std::string& officialId) {
    if (officialIndex < 0) {
        Log("FOLLOW_COSTUME_SKIP reason=empty-index");
        return;
    }
    if (officialId.empty()) {
        pendingOfficialIndex = officialIndex;
        if (lastEmptyOfficial != officialIndex) {
            lastEmptyOfficial = officialIndex;
            auto line = std::ostringstream();
            line << "FOLLOW_COSTUME_SKIP reason=empty-id official=" << officialIndex
                 << " follow=" << gakumas::vr::camera::FollowActorIndex();
            Log(line.str());
        }
        return;
    }
    pendingOfficialIndex = -1;
    CostumeHandoverSample sample;
    sample.followIndex = gakumas::vr::camera::FollowActorIndex();
    sample.followCharacterId = LookupId(sample.followIndex);
    if (sample.followCharacterId.empty()) {
        if (void* follow = camera::ReadVrFollowActorController(); Alive(follow))
            sample.followCharacterId = ReadCharacterId(follow);
    }
    sample.previousOfficialIndex = lastOfficialIndex;
    sample.previousOfficialCharacterId = lastOfficialId;
    sample.officialIndex = officialIndex;
    sample.officialCharacterId = officialId;
    const auto decision = DecideCostumeHandover(sample);
    const bool officialMoved = lastOfficialIndex != officialIndex;
    lastOfficialIndex = officialIndex;
    lastOfficialId = officialId;
    if (decision.shouldSwitch) {
        gakumas::vr::camera::FollowActorIndex() = decision.toIndex;
        if (camera::ReadVrFreeCameraMode() == camera::VrFreeCameraMode::Follow)
            camera::RequestFollowIdentityRetarget();
        auto line = std::ostringstream();
        line << "FOLLOW_COSTUME_HANDOVER from=" << decision.fromIndex
             << " to=" << decision.toIndex << " id=" << officialId;
        Log(line.str());
        return;
    }
    if (!officialMoved) return;
    auto line = std::ostringstream();
    if (sample.previousOfficialIndex < 0) {
        line << "FOLLOW_COSTUME_SEE official=" << officialIndex << " id=" << officialId
             << " follow=" << sample.followIndex;
        Log(line.str());
        return;
    }
    line << "FOLLOW_COSTUME_SKIP follow=" << sample.followIndex
         << " previous=" << sample.previousOfficialIndex
         << " official=" << officialIndex
         << " followId=" << sample.followCharacterId
         << " officialId=" << officialId;
    Log(line.str());
}

void OnOfficialIndex(const char* source, int officialIndex) {
    if (std::strcmp(source, "focus-field") != 0 &&
        std::strcmp(source, "cache-retry") != 0) {
        auto line = std::ostringstream();
        line << "FOLLOW_COSTUME_EVENT source=" << source << " official=" << officialIndex
             << " id=" << ResolveId(officialIndex)
             << " follow=" << gakumas::vr::camera::FollowActorIndex();
        Log(line.str());
    }
    ApplyOfficialFocus(officialIndex, ResolveId(officialIndex));
}

void FocusTargetHook(void* actor, void* info) {
    originalFocusTarget(actor, info);
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    if (!Alive(actor)) {
        Log("FOLLOW_COSTUME_FOCUS alive=0");
        return;
    }
    const int index = ReadIndex(actor);
    const auto id = ReadCharacterId(actor);
    RememberActor(actor, index, id);
    auto line = std::ostringstream();
    line << "FOLLOW_COSTUME_FOCUS index=" << index << " id=" << id;
    Log(line.str());
    ApplyOfficialFocus(index, id);
}

void FocusIndexHook(void* self, int index, void* info) {
    originalFocusIndex(self, index, info);
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    OnOfficialIndex("SetFocusTargetIndex", index);
}

void NotifyHook(void* self, int slot, int next, void* info) {
    originalNotify(self, slot, next, info);
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    auto line = std::ostringstream();
    line << "FOLLOW_COSTUME_SIGNAL slot=" << slot << " next=" << next;
    Log(line.str());
    OnOfficialIndex("NotifyCurrentActorSignal", next);
}

void NotifyIfaceHook(void* self, int slot, int next, void* info) {
    originalNotifyIface(self, slot, next, info);
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    auto line = std::ostringstream();
    line << "FOLLOW_COSTUME_SIGNAL iface slot=" << slot << " next=" << next;
    Log(line.str());
    OnOfficialIndex("NotifyCurrentActorSignal.iface", next);
}

bool Install(Method* method, void* detour, void** original, const char* name) {
    if (!method || !method->function) return false;
    GakumasVR::Hooks::Request hook[] = {
        {method->function, detour, original, name},
    };
    return GakumasVR::Hooks::CreateAndEnableBatch(hook, std::size(hook));
}

int ReadManagerFocusIndex() {
    if (!managerInstanceField || !focusIndexField) return -1;
    void* manager = nullptr;
    UnityResolve::Invoke<void>("il2cpp_field_static_get_value", managerInstanceField,
                               &manager);
    if (!Alive(manager)) return -1;
    return Il2cppUtils::ClassGetFieldValue<int>(manager, focusIndexField);
}

bool Resolve() {
    actorClass = Il2cppUtils::GetClass("campus-submodule.Runtime.dll", "Campus.Common",
                                       "CampusActorController");
    auto* descriptorClass = Il2cppUtils::GetClass(
        "campus-submodule.Runtime.dll", "Campus.Common", "CampusActorDescriptor");
    auto* objectClass =
        Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Object");
    auto* componentClass =
        Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Component");
    managerClass = Il2cppUtils::GetClass("campus-submodule.Runtime.dll", "Campus.Live",
                                         "LivePanoramaCameraManager");
    timelineClass = Il2cppUtils::GetClass(
        "campus-submodule.Runtime.dll", "Campus.Common.Live",
        "CampusLiveTimelineController");
    getIndex = Exact(actorClass, "get_index", "System.Int32", false, {});
    getDescriptor = Exact(actorClass, "get_descriptor",
                          "Campus.Common.ICampusActorDescriptor", false, {});
    getCharacterId = Exact(descriptorClass, "GetCharacterId", "System.String", false, {});
    getGameObjectName = Exact(actorClass, "get_gameObjectName", "System.String", false, {});
    // 478 hardware: campus get_descriptor / get_gameObjectName share a
    // thunk and runtime_invoke returned empty. Same session already read
    // "kllj | CampusActorController[N]" via Object.get_name after
    // Component.get_gameObject.
    getUnityName = Exact(objectClass, "get_name", "System.String", false, {});
    getUnityGameObject =
        Exact(componentClass, "get_gameObject", "UnityEngine.GameObject", false, {});
    if (actorClass) {
        getIndexInfo = Il2cppUtils::il2cpp_class_get_method_from_name(
            actorClass->address, "get_index", 0);
        getDescriptorInfo = Il2cppUtils::il2cpp_class_get_method_from_name(
            actorClass->address, "get_descriptor", 0);
        getGameObjectNameInfo = Il2cppUtils::il2cpp_class_get_method_from_name(
            actorClass->address, "get_gameObjectName", 0);
    }
    if (descriptorClass) {
        getCharacterIdInfo = Il2cppUtils::il2cpp_class_get_method_from_name(
            descriptorClass->address, "GetCharacterId", 0);
    }
    if (objectClass) {
        getUnityNameInfo = Il2cppUtils::il2cpp_class_get_method_from_name(
            objectClass->address, "get_name", 0);
    }
    if (componentClass) {
        getUnityGameObjectInfo = Il2cppUtils::il2cpp_class_get_method_from_name(
            componentClass->address, "get_gameObject", 0);
    }
    setFocusTarget = Exact(managerClass, "SetFocusTarget", "System.Void", true,
                           {"Campus.Common.ICampusActorController"});
    setFocusTargetIndex = Exact(managerClass, "SetFocusTargetIndex", "System.Void", false,
                                {"System.Int32"});
    notifySignal = Exact(timelineClass, "NotifyCurrentActorSignal", "System.Void", false,
                         {"System.Int32", "System.Int32"});
    notifySignalIface = Exact(
        timelineClass, "Campus.Timeline.ICurrentActorSignalNotifier.NotifyCurrentActorSignal",
        "System.Void", false, {"System.Int32", "System.Int32"});
    managerInstanceField = managerClass
        ? UnityResolve::Invoke<Il2cppUtils::FieldInfo*>(
              "il2cpp_class_get_field_from_name", managerClass->address, "_instance")
        : nullptr;
    focusIndexField = managerClass
        ? managerClass->Get<UnityResolve::Field>("_currentFocusActorIndex")
        : nullptr;
    const bool canReadFocus = managerInstanceField && focusIndexField;
    const bool canReadIndex = getIndexInfo || getIndex;
    const bool canReadId = getCharacterIdInfo || getGameObjectNameInfo || getUnityName ||
        getUnityNameInfo;
    if (!actorClass || !canReadIndex || !canReadId ||
        (!setFocusTarget && !notifySignal && !notifySignalIface && !canReadFocus)) {
        if (!loggedShape) {
            loggedShape = true;
            Log("FOLLOW_COSTUME_SKIP reason=api-shape");
        }
        return false;
    }
    if (setFocusTarget &&
        !Install(setFocusTarget, reinterpret_cast<void*>(FocusTargetHook),
                 reinterpret_cast<void**>(&originalFocusTarget),
                 "FollowCostume.SetFocusTarget")) {
        Log("FOLLOW_COSTUME_SKIP reason=hooks");
    }
    if (setFocusTargetIndex)
        Install(setFocusTargetIndex, reinterpret_cast<void*>(FocusIndexHook),
                reinterpret_cast<void**>(&originalFocusIndex),
                "FollowCostume.SetFocusTargetIndex");
    if (notifySignal &&
        !Install(notifySignal, reinterpret_cast<void*>(NotifyHook),
                 reinterpret_cast<void**>(&originalNotify),
                 "FollowCostume.NotifyCurrentActorSignal")) {
        Log("FOLLOW_COSTUME_SKIP reason=notify-hook");
    }
    if (notifySignalIface && notifySignalIface->function &&
        (!notifySignal || notifySignalIface->function != notifySignal->function)) {
        Install(notifySignalIface, reinterpret_cast<void*>(NotifyIfaceHook),
                reinterpret_cast<void**>(&originalNotifyIface),
                "FollowCostume.NotifyCurrentActorSignal.iface");
    }
    auto line = std::ostringstream();
    line << "FOLLOW_COSTUME_READY"
         << " focus=" << (setFocusTarget ? setFocusTarget->function : nullptr)
         << " index=" << (setFocusTargetIndex ? setFocusTargetIndex->function : nullptr)
         << " notify=" << (notifySignal ? notifySignal->function : nullptr)
         << " notifyIface=" << (notifySignalIface ? notifySignalIface->function : nullptr)
         << " focusField=" << (focusIndexField ? focusIndexField->offset : -1)
         << " name=" << (getUnityName ? getUnityName->function : nullptr)
         << " getIndex=" << (getIndexInfo ? getIndexInfo->methodPointer : 0);
    Log(line.str());
    return true;
}
} // namespace

void NoteFollowCostumeActor(void* actor, int index) noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled || !actor || index < 0) return;
    try {
        if (!LookupId(index).empty()) {
            RememberActor(actor, index, LookupId(index));
            return;
        }
        RememberActor(actor, index, ReadCharacterId(actor));
    } catch (...) {
    }
}

void TickFollowCostumeHandover() noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    try {
        auto* scene = AliveLiveScenePresenter();
        // Mid-song presenter flicker must not wipe lastOfficial; only a new
        // non-null Live presenter is a new song.
        if (scene && scene != sceneIdentity) {
            ResetScene();
            sceneIdentity = scene;
        }
        if (!ready && !loggedShape) ready = Resolve();
        if (!ready) return;
        if (pendingOfficialIndex >= 0) {
            const auto id = ResolveId(pendingOfficialIndex);
            if (!id.empty()) OnOfficialIndex("cache-retry", pendingOfficialIndex);
        }
        const int official = ReadManagerFocusIndex();
        if (official >= 0 && official != lastOfficialIndex)
            OnOfficialIndex("focus-field", official);
    } catch (...) {
        Log("FOLLOW_COSTUME_SKIP reason=exception");
    }
}
} // namespace gakumas::vr
