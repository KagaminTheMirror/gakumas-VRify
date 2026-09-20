#include "CommonLiveGazeProbe.hpp"
#include "CommonLiveGazeGcHandle.hpp"
#include "LiveGaze.hpp"
#include "LiveGazeTrace.hpp"
#include "LivePause.hpp"
#include "VrFreeCamera.hpp"
#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "GakumasLocalify/Il2cppUtils.hpp"
#include "deps/UnityResolve/UnityResolve.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gakumas::vr {
namespace {
using Clock = std::chrono::steady_clock;
using Class = UnityResolve::Class;
using Method = UnityResolve::Method;
struct Entry { const char* assembly; const char* space; const char* name; };
constexpr Entry entries[] = {
    {"Assembly-CSharp.dll", "Campus.Live", "LiveScenePresenter"},
    {"Assembly-CSharp.dll", "Campus.Live", "LiveSceneModel"},
    {"Assembly-CSharp.dll", "Campus.Live", "LivePresenter"},
    {"Assembly-CSharp.dll", "Campus.Live", "LivePlayerPresenter"},
    {"Assembly-CSharp.dll", "Campus.Live", "Live3DPresenter"},
    {"Assembly-CSharp.dll", "Campus.Live", "LiveResourceManager"},
    {"Assembly-CSharp.dll", "", "<SetActorsAsync>d__48"},
    {"Assembly-CSharp.dll", "", "<LoadLookAtEffectorAsync>d__13"},
    {"Assembly-CSharp.dll", "Campus.Common.Master", "CharacterLookEffectorMaster"},
    {"Assembly-CSharp.dll", "Campus.Common.Proto.Client.Enums", "CharacterLookEffectorSituationType"},
    {"campus-submodule.Runtime.dll", "Campus.Common", "CampusActorAnimation"},
    {"campus-submodule.Runtime.dll", "Campus.Common", "CampusActorDescriptor"},
    {"campus-submodule.Runtime.dll", "Campus.Common.Panorama", "PanoramaCameraSwitcher"},
    {"campus-submodule.Runtime.dll", "Campus.Common", "CampusActorController"},
    {"campus-submodule.Runtime.dll", "Campus.Common", "CampusVirtualActorController"},
    {"campus-submodule.Runtime.dll", "Campus.Common.Panorama", "PanoramaActorLookAtController"},
    {"campus-submodule.Runtime.dll", "Campus.Common.LookAt", "CampusActorLookAtController"},
    {"campus-submodule.Runtime.dll", "Campus.Common.LookAt", "CampusLookAtConstraint"},
    {"campus-submodule.Runtime.dll", "Campus.Common.LookAt", "LookAtUtility"},
    {"campus-submodule.Runtime.dll", "Campus.Common.LookAt", "LookTargetType"},
    {"campus-submodule.Runtime.dll", "Campus.Common", "CampusCustomLookAtEffector"},
    {"vl-unity.Runtime.dll", "VL.IK", "CustomLookAtEffector"},
    {"campus-submodule.Runtime.dll", "Campus.Common.Live", "CampusLiveTimelineController"},
    {"campus-submodule.Runtime.dll", "Campus.Timeline", "CampusForcedCameraMixerBehaviour"},
    {"campus-submodule.Runtime.dll", "Campus.Timeline", "LookAtProhibitType"},
    {"campus-submodule.Runtime.dll", "Campus.Timeline", "CampusLookAtProhibitBehaviour"},
    {"campus-submodule.Runtime.dll", "Campus.Timeline", "CampusLookAtProhibitClip"},
    {"campus-submodule.Runtime.dll", "Campus.Timeline", "CampusLookAtProhibitTrack"},
    {"campus-submodule.Runtime.dll", "Campus.Timeline", "CampusLookAtProhibitMixerBehaviour"},
    {"campus-submodule.Runtime.dll", "Campus.Photography", "PhotographyActorLookAtController"},
    {"UnityEngine.CoreModule.dll", "UnityEngine", "Resources"},
    {"UnityEngine.CoreModule.dll", "UnityEngine", "Object"},
    {"UnityEngine.CoreModule.dll", "UnityEngine", "Transform"},
    {"UnityEngine.CoreModule.dll", "UnityEngine", "Component"},
    {"UnityEngine.CoreModule.dll", "UnityEngine", "GameObject"},
};
std::unordered_map<void*, Class*> classes;
std::unordered_map<void*, bool> unityObjects;
std::vector<Method*> codeQueue;
std::unordered_set<void*> codeSeen;
std::size_t entryIndex = 0, codeIndex = 0;
std::size_t codeBytes = 0;
constexpr std::size_t kCodeBudget = 1024 * 1024;
constexpr std::size_t kMethodPrefix = 4096;
std::unordered_map<void*, std::string> previousStates;
panorama::HandleList actorHandles;
void* previousScene = nullptr;
unsigned int censusCount = 0;
constexpr const char* censusTypes[] = {"CampusActorController", "CampusVirtualActorController",
    "CampusActorLookAtController", "CampusLookAtConstraint", "CampusCustomLookAtEffector",
    "PanoramaActorLookAtController", "PhotographyActorLookAtController", "LiveResourceManager",
    "CampusLiveTimelineController", "CampusLookAtProhibitTrack", "CampusLookAtProhibitClip"};
std::size_t stateLines = 0;
bool stateLimitLogged = false, codeLimitLogged = false, failureLogged = false;
Clock::time_point nextWork{}, nextSample{}, nextCensus{}, nextHeartbeat{};
int lastPanoramaLive = -1;
bool panoramaSkipLogged = false;
panorama::GcHandle resourceHandle = nullptr;
std::unordered_set<void*> rawDumped;
struct RebuildJob {
    panorama::GcHandle actorHandle{};
    panorama::GcHandle taskHandle{};
    panorama::GcHandle effectorHandle{};
    panorama::GcHandle controllerHandle{};
    std::string candidate;
    std::string asset;
    enum class Step { WaitTask, Init, Done, Fail } step{Step::WaitTask};
    int polls = 0;
    int enableLogs = 0;
    bool armed = false;
};
std::vector<RebuildJob> rebuilds;

void Log(const std::string& text) {
    // Direct diagnostic sink: independent of UnityStereoRenderer steady-state mute.
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) WriteVrLog("[VR][common-gaze] " + text);
}
std::ostringstream Stream() { std::ostringstream s; s.imbue(std::locale::classic()); return s; }
bool Copy(void* address, void* dest, std::size_t size) noexcept {
    if (!address || !dest) return false;
    __try { std::memcpy(dest, address, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* ObjectClass(void* object) noexcept {
    void* result = nullptr;
    return Copy(object, &result, sizeof(result)) ? result : nullptr;
}
bool Alive(void* object) noexcept {
    if (!ObjectClass(object)) return false;
    void* native = nullptr;
    return Copy(static_cast<char*>(object) + offsetof(UnityResolve::UnityType::UnityObject, m_CachedPtr),
                &native, sizeof(native)) && native;
}
template<class T> T Api(const char* name) noexcept {
    return reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"), name));
}
bool Invoke(Method* method, void** args, void** result, void* instance = nullptr) noexcept {
    using Fn = void* (*)(void*, void*, void**, void**);
    static const auto fn = Api<Fn>("il2cpp_runtime_invoke");
    if (!fn || !method || !method->address) return false;
    void* exception = nullptr;
    __try { *result = fn(method->address, instance, args, &exception); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return !exception;
}
bool WantCode(const Method* method) {
    const auto& n = method->name;
    if (method->klass && (method->klass->name == "CampusActorLookAtController" ||
        method->klass->name == "CampusLookAtConstraint" ||
        method->klass->name == "CampusCustomLookAtEffector" ||
        method->klass->name == "CharacterLookEffectorMaster" ||
        method->klass->name == "CampusActorAnimation" ||
        method->klass->name == "CampusActorDescriptor" ||
        method->klass->name.starts_with("<SetActorsAsync>") ||
        method->klass->name.starts_with("<LoadLookAtEffectorAsync>"))) return true;
    return n == "GetAssetName" || n.find("Panorama") != std::string::npos || n.find("Look") != std::string::npos ||
        n.find("Forced") != std::string::npos || n.find("Override") != std::string::npos ||
        n == "SetEvent" || n == "Initialize" || n == "ProcessFrame" || n == "LateUpdate" ||
        n == "UpdateRotate" || n == "SetActorsAsync" || n == "OnAfterLoaded";
}
void DumpClass(const Entry& e) {
    auto* c = Il2cppUtils::GetClass(e.assembly, e.space, e.name);
    if (c && (c->name == "<SetActorsAsync>d__48" || c->name == "<LoadLookAtEffectorAsync>d__13")) {
        const char* ownerName = c->name == "<SetActorsAsync>d__48" ? "Live3DPresenter" : "LiveResourceManager";
        auto* owner = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Live", ownerName);
        bool matched = false;
        void* iterator = nullptr;
        if (owner) while (auto* nested = UnityResolve::Invoke<void*>("il2cpp_class_get_nested_types", owner->address, &iterator))
            if (nested == c->address) matched = true;
        if (!matched) { Log("COMMON_GAZE_GAP reason=nested-owner-mismatch"); c = nullptr; }
    }
    auto s = Stream(); s << "PANORAMA_CLASS assembly=" << e.assembly << " ns=" << e.space
        << " name=" << e.name << " class=" << (c ? c->address : nullptr)
        << " parent=" << (c ? c->parent : "missing"); Log(s.str());
    if (!c) return;
    classes[c->address] = c;
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) return;
    for (const auto* f : c->fields) {
        if (!f || !f->type) continue;
        auto line = Stream(); line << "PANORAMA_FIELD class=" << e.name << " name=" << f->name
            << " type=" << f->type->name << " offset=" << f->offset << " info=" << f->address;
        Log(line.str());
    }
    for (auto* m : c->methods) {
        if (!m) continue;
        auto line = Stream(); line << "PANORAMA_METHOD class=" << e.name << " name=" << m->name
            << " static=" << m->static_function << " return=" << (m->return_type ? m->return_type->name : "missing")
            << " function=" << m->function << " info=" << m->address;
        for (std::size_t i = 0; i < m->args.size(); ++i)
            line << " arg" << i << '=' << (m->args[i]->pType ? m->args[i]->pType->name : "missing");
        Log(line.str());
        if (WantCode(m) && m->function && codeSeen.insert(m->function).second) codeQueue.push_back(m);
    }
}

void DumpCode(Method* method) {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) return;
    if (codeBytes >= kCodeBudget) {
        if (!codeLimitLogged) { Log("PANORAMA_CODE_GAP reason=budget limit=1048576"); codeLimitLogged = true; }
        return;
    }
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(method->function, &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        Log("PANORAMA_CODE_GAP reason=unreadable-region"); return;
    }
    const auto available = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize -
        reinterpret_cast<std::uintptr_t>(method->function);
    const auto count = std::min({available, kMethodPrefix, kCodeBudget - codeBytes});
    std::array<unsigned char, kMethodPrefix> bytes{};
    if (!Copy(method->function, bytes.data(), count)) { Log("PANORAMA_CODE_GAP reason=copy-failed"); return; }
    codeBytes += count;
    auto line = Stream(); line << "PANORAMA_CODE class=" << method->klass->name << " method=" << method->name
        << " function=" << method->function << " module=" << GetModuleHandleW(L"GameAssembly.dll")
        << " prefixBytes=" << count << " hex=";
    line << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < count; ++i) line << std::setw(2) << unsigned(bytes[i]);
    Log(line.str());
}
Class* Named(const char* name) {
    for (const auto& [address, c] : classes) if (c->name == name) return c;
    return nullptr;
}
Method* ReadMethod(const char* klass, const char* name, const char* result, bool isStatic) {
    auto* c = Named(klass);
    if (!c) return nullptr;
    for (auto* m : c->methods)
        if (m && m->name == name && m->static_function == isStatic && m->args.empty() &&
            m->return_type && m->return_type->name == result && m->address) return m;
    return nullptr;
}
Method* FindMethod(Class* klass, const char* name, const char* result, bool isStatic, std::initializer_list<const char*> args) {
    if (!klass) return nullptr;
    for (auto* m : klass->methods) {
        if (!m || m->name != name || m->static_function != isStatic || !m->address ||
            !m->return_type || m->return_type->name != result || m->args.size() != args.size()) continue;
        std::size_t i = 0;
        bool match = true;
        for (const auto* type : args) {
            auto* a = m->args[i++];
            if (!a || !a->pType || a->pType->name != type) match = false;
        }
        if (match) return m;
    }
    return nullptr;
}
int FieldOffset(const char* klass, const char* name) {
    auto* c = Named(klass);
    if (c) for (const auto* f : c->fields)
        if (f && f->name == name && f->offset >= 16 &&
            !(UnityResolve::Invoke<int>("il2cpp_field_get_flags", f->address) & 0x10)) return f->offset;
    return -1;
}
void* LookMaster() {
    static void* cached = nullptr;
    static bool tried = false;
    if (tried) return cached;
    tried = true;
    auto* mgr = Il2cppUtils::GetClass("Assembly-CSharp.dll", "Campus.Common.Master", "MasterManager");
    Method* getter = nullptr;
    if (mgr) for (auto* m : mgr->methods)
        if (m && m->name == "get_CharacterLookEffectorMaster" && m->static_function && m->args.empty() &&
            m->return_type && m->return_type->name.find("CharacterLookEffectorMaster") != std::string::npos)
            getter = m;
    void* master = nullptr;
    if (!getter || !Invoke(getter, nullptr, &master) || !master) {
        Log("COMMON_GAZE_GAP reason=look-master");
        return nullptr;
    }
    auto line = Stream();
    line << "COMMON_GAZE_FACTORY master=" << master << " getter=" << getter->address
        << " getterReturn=" << getter->return_type->name;
    Log(line.str());
    cached = master;
    return cached;
}
void ReleaseHandle(panorama::GcHandle& handle) {
    if (!handle) return;
    if (const auto free = Api<panorama::FreeHandle>("il2cpp_gchandle_free")) free(handle);
    handle = nullptr;
}
void* HandleTarget(panorama::GcHandle handle) {
    const auto get = Api<panorama::GetTarget>("il2cpp_gchandle_get_target");
    return handle && get ? get(handle) : nullptr;
}
panorama::GcHandle RootObject(void* object) {
    const auto make = Api<panorama::NewHandle>("il2cpp_gchandle_new");
    return object && make ? make(object, false) : nullptr;
}
void ClearRebuilds() {
    ClearRebuiltLiveGazeSmoothing();
    for (auto& job : rebuilds) {
        ReleaseHandle(job.actorHandle);
        ReleaseHandle(job.taskHandle);
        ReleaseHandle(job.effectorHandle);
        ReleaseHandle(job.controllerHandle);
    }
    rebuilds.clear();
    ReleaseHandle(resourceHandle);
    lastPanoramaLive = -1;
    panoramaSkipLogged = false;
}
void DumpRawClass(void* klass) {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) return;
    if (!klass || !rawDumped.insert(klass).second) return;
    const auto* name = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", klass);
    const auto* ns = UnityResolve::Invoke<const char*>("il2cpp_class_get_namespace", klass);
    auto header = Stream();
    header << "COMMON_GAZE_TASK class=" << klass << " ns=" << (ns ? ns : "")
        << " name=" << (name ? name : "");
    Log(header.str());
    void* fields = nullptr;
    while (auto* field = UnityResolve::Invoke<void*>("il2cpp_class_get_fields", klass, &fields)) {
        const auto* fieldName = UnityResolve::Invoke<const char*>("il2cpp_field_get_name", field);
        auto* type = UnityResolve::Invoke<void*>("il2cpp_field_get_type", field);
        const auto* typeName = type ? UnityResolve::Invoke<const char*>("il2cpp_type_get_name", type) : nullptr;
        auto row = Stream();
        row << "COMMON_GAZE_TASK_FIELD class=" << (name ? name : "") << " name=" << (fieldName ? fieldName : "")
            << " type=" << (typeName ? typeName : "") << " offset="
            << UnityResolve::Invoke<int>("il2cpp_field_get_offset", field);
        Log(row.str());
    }
    void* methods = nullptr;
    while (auto* method = UnityResolve::Invoke<void*>("il2cpp_class_get_methods", klass, &methods)) {
        const auto* methodName = UnityResolve::Invoke<const char*>("il2cpp_method_get_name", method);
        auto* type = UnityResolve::Invoke<void*>("il2cpp_method_get_return_type", method);
        const auto* typeName = type ? UnityResolve::Invoke<const char*>("il2cpp_type_get_name", type) : nullptr;
        auto row = Stream();
        row << "COMMON_GAZE_TASK_METHOD class=" << (name ? name : "") << " name=" << (methodName ? methodName : "")
            << " return=" << (typeName ? typeName : "") << " argc="
            << UnityResolve::Invoke<unsigned>("il2cpp_method_get_param_count", method) << " info=" << method;
        Log(row.str());
    }
}
std::string ExceptionText(void* exception) {
    if (!exception) return {};
    const auto* name = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", ObjectClass(exception));
    return name ? name : "exception";
}
struct RawCall { void* result = nullptr; void* exception = nullptr; bool crashed = false; };
RawCall InvokeCatch(void* method, void* instance, void** args) noexcept {
    RawCall call;
    using Fn = void* (*)(void*, void*, void**, void**);
    static const auto fn = Api<Fn>("il2cpp_runtime_invoke");
    if (!fn || !method) { call.crashed = true; return call; }
    __try { call.result = fn(method, instance, args, &call.exception); }
    __except (EXCEPTION_EXECUTE_HANDLER) { call.crashed = true; }
    return call;
}
bool InvokeRaw(void* method, void* instance, void** args, void** result, std::string* error = nullptr) noexcept {
    const auto call = InvokeCatch(method, instance, args);
    if (call.crashed) return false;
    if (call.exception) {
        if (error) *error = ExceptionText(call.exception);
        return false;
    }
    *result = call.result;
    return true;
}
void* RawMethod(void* klass, const char* name, unsigned argc) {
    if (!klass || !name) return nullptr;
    void* methods = nullptr;
    while (auto* method = UnityResolve::Invoke<void*>("il2cpp_class_get_methods", klass, &methods)) {
        const auto* methodName = UnityResolve::Invoke<const char*>("il2cpp_method_get_name", method);
        if (methodName && std::strcmp(methodName, name) == 0 &&
            UnityResolve::Invoke<unsigned>("il2cpp_method_get_param_count", method) == argc) return method;
    }
    return nullptr;
}
bool IsEffector(void* object) {
    auto* klass = Named("CampusCustomLookAtEffector");
    return Alive(object) && klass && ObjectClass(object) == klass->address;
}
int UnboxInt(void* boxed) {
    const auto unbox = Api<void* (*)(void*)>("il2cpp_object_unbox");
    int value = -1;
    if (boxed && unbox) Copy(unbox(boxed), &value, sizeof(value));
    return value;
}
int TaskStatus(void* box) {
    auto* getStatus = RawMethod(ObjectClass(box), "get_Status", 0);
    void* boxed = nullptr;
    if (!getStatus || !InvokeRaw(getStatus, box, nullptr, &boxed) || !boxed) return -1;
    return UnboxInt(boxed);
}
void* EffectorFromBox(void* box) {
    auto* klass = ObjectClass(box);
    if (!klass) return nullptr;
    DumpRawClass(klass);
    void* fields = nullptr;
    while (auto* field = UnityResolve::Invoke<void*>("il2cpp_class_get_fields", klass, &fields)) {
        const auto* fieldName = UnityResolve::Invoke<const char*>("il2cpp_field_get_name", field);
        auto* type = UnityResolve::Invoke<void*>("il2cpp_field_get_type", field);
        const auto* typeName = type ? UnityResolve::Invoke<const char*>("il2cpp_type_get_name", type) : nullptr;
        if (!typeName || std::strcmp(typeName, "Campus.Common.CampusCustomLookAtEffector") != 0) continue;
        if (fieldName && std::strcmp(fieldName, "result") != 0 && std::strcmp(fieldName, "Result") != 0) continue;
        void* value = nullptr;
        if (Copy(static_cast<char*>(box) + UnityResolve::Invoke<int>("il2cpp_field_get_offset", field),
                &value, sizeof(value)) && IsEffector(value)) return value;
    }
    return nullptr;
}
void* TaskEffector(void* box, int* statusOut) {
    if (statusOut) *statusOut = -1;
    if (!box) return nullptr;
    const auto status = TaskStatus(box);
    if (statusOut) *statusOut = status;
    if (auto* effector = EffectorFromBox(box); IsEffector(effector)) return effector;
    if (status != 1) return nullptr;
    auto* klass = ObjectClass(box);
    if (auto* getAwaiter = RawMethod(klass, "GetAwaiter", 0)) {
        void* awaiter = nullptr;
        if (InvokeRaw(getAwaiter, box, nullptr, &awaiter) && awaiter) {
            DumpRawClass(ObjectClass(awaiter));
            if (auto* getResult = RawMethod(ObjectClass(awaiter), "GetResult", 0)) {
                void* result = nullptr;
                std::string error;
                if (InvokeRaw(getResult, awaiter, nullptr, &result, &error) && IsEffector(result)) return result;
                if (!error.empty()) Log("COMMON_GAZE_GAP reason=get-result error=" + error);
            }
        }
    }
    return nullptr;
}
void* FindFirst(const char* className) {
    auto* klass = Named(className);
    auto* resources = Named("Resources");
    if (!klass || !resources) return nullptr;
    Method* find = nullptr;
    for (auto* m : resources->methods) {
        if (m && m->name == "FindObjectsOfTypeAll" && m->static_function && m->address &&
            m->args.size() == 1 && m->args[0]->pType && m->args[0]->pType->name == "System.Type" &&
            m->return_type && m->return_type->name == "UnityEngine.Object[]") { find = m; break; }
    }
    void* type = klass->GetType();
    void* args[] = {type};
    void* result = nullptr;
    if (!find || !type || !Invoke(find, args, &result) || !result) return nullptr;
    auto* array = static_cast<UnityResolve::UnityType::Array<void*>*>(result);
    const auto root = RootObject(result);
    if (!root) return nullptr;
    struct Release { panorama::GcHandle h; ~Release() { ReleaseHandle(h); } } release{root};
    std::uintptr_t length{};
    if (!Copy(&array->max_length, &length, sizeof(length)) || length > 1024) return nullptr;
    for (std::size_t i = 0; i < length; ++i) {
        void* object = nullptr;
        if (Copy(reinterpret_cast<void*>(array->GetData() + i * sizeof(void*)), &object, sizeof(object)) &&
            Alive(object)) return object;
    }
    return nullptr;
}
void* ResourceManager() {
    if (auto* object = HandleTarget(resourceHandle); Alive(object)) return object;
    ReleaseHandle(resourceHandle);
    if (auto* object = FindFirst("LiveResourceManager"); Alive(object)) {
        resourceHandle = RootObject(object);
        return object;
    }
    return nullptr;
}
std::string AssetName(const std::string& candidate, int situation) {
    auto* master = LookMaster();
    auto* getAsset = FindMethod(Named("CharacterLookEffectorMaster"), "GetAssetName", "System.String", false,
        {"System.String", "Campus.Common.Proto.Client.Enums.CharacterLookEffectorSituationType"});
    if (!getAsset) {
        auto* klass = Named("CharacterLookEffectorMaster");
        if (klass) for (auto* m : klass->methods)
            if (m && m->name == "GetAssetName" && !m->static_function && m->args.size() == 2) getAsset = m;
    }
    auto* id = UnityResolve::UnityType::String::New(candidate);
    void* asset = nullptr;
    void* args[] = {id, &situation};
    if (!master || !getAsset || !id || !Invoke(getAsset, args, &asset, master) || !asset) return {};
    return static_cast<UnityResolve::UnityType::String*>(asset)->ToString();
}
bool EffectorOwned(void* object) {
    if (!object) return false;
    for (const auto& job : rebuilds)
        if (HandleTarget(job.effectorHandle) == object) return true;
    return false;
}
void* NamedEffector(const std::string& asset, bool unusedOnly) {
    auto* klass = Named("CampusCustomLookAtEffector");
    auto* resources = Named("Resources");
    if (!klass || !resources || asset.empty()) return nullptr;
    Method* find = nullptr;
    for (auto* m : resources->methods) {
        if (m && m->name == "FindObjectsOfTypeAll" && m->static_function && m->address &&
            m->args.size() == 1 && m->args[0]->pType && m->args[0]->pType->name == "System.Type") { find = m; break; }
    }
    void* type = klass->GetType();
    void* args[] = {type};
    void* result = nullptr;
    if (!find || !type || !Invoke(find, args, &result) || !result) return nullptr;
    auto* array = static_cast<UnityResolve::UnityType::Array<void*>*>(result);
    const auto root = RootObject(result);
    if (!root) return nullptr;
    struct Release { panorama::GcHandle h; ~Release() { ReleaseHandle(h); } } release{root};
    std::uintptr_t length{};
    if (!Copy(&array->max_length, &length, sizeof(length)) || length > 1024) return nullptr;
    auto* getGo = ReadMethod("Component", "get_gameObject", "UnityEngine.GameObject", false);
    auto* getName = ReadMethod("Object", "get_name", "System.String", false);
    void* unused = nullptr;
    void* freeClone = nullptr;
    for (std::size_t i = 0; i < length; ++i) {
        void* object = nullptr;
        void* go = nullptr;
        void* name = nullptr;
        if (!Copy(reinterpret_cast<void*>(array->GetData() + i * sizeof(void*)), &object, sizeof(object)) ||
            !Alive(object) || EffectorOwned(object) || !getGo || !Invoke(getGo, nullptr, &go, object) ||
            !Alive(go) || !getName || !Invoke(getName, nullptr, &name, go) || !name) continue;
        const auto text = static_cast<UnityResolve::UnityType::String*>(name)->ToString();
        if (!IsEffector(object) || (text != asset && text.rfind(asset, 0) != 0)) continue;
        if (text.find("(Clone)") != std::string::npos) {
            if (!freeClone) freeClone = object;
            continue;
        }
        if (!unused) unused = object;
    }
    return unused ? unused : (unusedOnly ? nullptr : freeClone);
}
void* InstantiatedEffector(void* effector) {
    if (!IsEffector(effector) || EffectorOwned(effector)) return nullptr;
    auto* getGo = ReadMethod("Component", "get_gameObject", "UnityEngine.GameObject", false);
    auto* getActive = ReadMethod("GameObject", "get_activeInHierarchy", "System.Boolean", false);
    auto* instantiate = FindMethod(Named("Object"), "Instantiate", "UnityEngine.Object", true, {"UnityEngine.Object"});
    auto* getComponent = FindMethod(Named("GameObject"), "GetComponent", "UnityEngine.Component", false, {"System.Type"});
    void* go = nullptr;
    if (!getGo || !Invoke(getGo, nullptr, &go, effector) || !Alive(go)) return nullptr;
    void* boxed = nullptr;
    const auto unbox = Api<void* (*)(void*)>("il2cpp_object_unbox");
    unsigned char active = 1;
    if (getActive && unbox && Invoke(getActive, nullptr, &boxed, go) && boxed) Copy(unbox(boxed), &active, 1);
    if (active) return effector;
    auto* klass = Named("CampusCustomLookAtEffector");
    void* args[] = {go};
    void* copy = nullptr;
    if (!instantiate || !klass || !Invoke(instantiate, args, &copy) || !Alive(copy)) {
        Log("COMMON_GAZE_GAP reason=instantiate-prefab");
        return nullptr;
    }
    void* type = klass->GetType();
    void* componentArgs[] = {type};
    void* clone = nullptr;
    if (getComponent && type && Invoke(getComponent, componentArgs, &clone, copy) && IsEffector(clone)) return clone;
    if (IsEffector(copy)) return copy;
    Log("COMMON_GAZE_GAP reason=instantiate-component");
    return nullptr;
}
void* UniqueEffector(const std::string& asset, void* loaded) {
    if (auto* unused = NamedEffector(asset, true)) {
        if (auto* clone = InstantiatedEffector(unused)) {
            Log("COMMON_GAZE_EFFECTOR reason=instantiate-unused asset=" + asset);
            return clone;
        }
    }
    if (IsEffector(loaded) && !EffectorOwned(loaded)) {
        if (auto* ready = InstantiatedEffector(loaded)) return ready;
    }
    if (auto* named = NamedEffector(asset, false)) {
        if (auto* ready = InstantiatedEffector(named)) return ready;
    }
    return nullptr;
}
bool WantFollowLook(void* actor, RebuildJob* job) {
    if (LiveGazeWantsAllActors()) return actor != nullptr;
    if (auto* follow = camera::ReadVrFollowActorController(); follow && Alive(follow))
        return actor == follow;
    if (!job) return true;
    for (auto& other : rebuilds) {
        if (other.step != RebuildJob::Step::Done) continue;
        return &other == job;
    }
    return true;
}
void EnableOfficialLook(void* controller, void* effector, RebuildJob* job = nullptr);
void* CreateOfficialController(void* actor, void* effector) {
    if (!IsEffector(effector)) {
        Log("COMMON_GAZE_INIT reason=no-effector");
        return nullptr;
    }
    auto* goClass = Named("GameObject");
    auto* controllerClass = Named("PanoramaActorLookAtController");
    auto* ctor = FindMethod(goClass, ".ctor", "System.Void", false, {});
    auto* add = FindMethod(goClass, "AddComponent", "UnityEngine.Component", false, {"System.Type"});
    auto* init = FindMethod(controllerClass, "Initialize", "System.Void", false,
        {"Campus.Common.ICampusActorController", "Campus.Common.CampusCustomLookAtEffector"});
    if (!goClass || !controllerClass || !ctor || !add || !init || !goClass->address) {
        Log("COMMON_GAZE_INIT reason=api-shape");
        return nullptr;
    }
    auto* created = UnityResolve::Invoke<void*>("il2cpp_object_new", goClass->address);
    void* ignored = nullptr;
    if (!created || !Invoke(ctor, nullptr, &ignored, created) || !Alive(created)) {
        auto* create = FindMethod(goClass, "Internal_CreateGameObject", "System.Void", true,
            {"UnityEngine.GameObject", "System.String"});
        auto* title = UnityResolve::UnityType::String::New("PanoramaActorLookAtController");
        void* createArgs[] = {created, title};
        if (!create || !created || !title || !Invoke(create, createArgs, &ignored) || !Alive(created)) {
            Log("COMMON_GAZE_INIT reason=game-object");
            return nullptr;
        }
    }
    void* type = controllerClass->GetType();
    void* addArgs[] = {type};
    void* controller = nullptr;
    if (!type || !Invoke(add, addArgs, &controller, created) || !Alive(controller)) {
        Log("COMMON_GAZE_INIT reason=add-component");
        return nullptr;
    }
    void* initArgs[] = {actor, effector};
    const auto call = InvokeCatch(init->address, controller, initArgs);
    const bool ok = !call.crashed && !call.exception;
    auto line = Stream();
    line << "COMMON_GAZE_INIT actor=" << actor << " effector=" << effector
        << " go=" << created << " controller=" << controller << " ok=" << ok;
    if (call.exception) line << " error=" << ExceptionText(call.exception);
    if (call.crashed) line << " error=seh";
    Log(line.str());
    return ok ? controller : nullptr;
}
unsigned char ReadByte(void* object, int offset) {
    unsigned char value = 0;
    if (object && offset > 0) Copy(static_cast<char*>(object) + offset, &value, 1);
    return value;
}
float ReadFloat(void* object, int offset) {
    float value = 0;
    if (object && offset > 0) Copy(static_cast<char*>(object) + offset, &value, 4);
    return value;
}
void EnableOfficialLook(void* controller, void* effector, RebuildJob* job) {
    if (!Alive(controller) || !Alive(effector) || (job && !Alive(HandleTarget(job->actorHandle)))) return;
    const auto innerOffset = FieldOffset("PanoramaActorLookAtController", "_lookAtController");
    void* inner = nullptr;
    if (innerOffset > 0) Copy(static_cast<char*>(controller) + innerOffset, &inner, sizeof(inner));
    const auto live = ReadByte(controller, FieldOffset("PanoramaActorLookAtController", "_liveEnableLookAt"));
    const auto prohibit = ReadByte(controller, FieldOffset("PanoramaActorLookAtController", "_isProhibitLookEnable"));
    const auto outRange = ReadByte(controller, FieldOffset("PanoramaActorLookAtController", "_isConstraintOutRange"));
    const auto focused = ReadByte(controller, FieldOffset("PanoramaActorLookAtController", "_isFocusTarget"));
    const auto should = ReadByte(inner, FieldOffset("CampusActorLookAtController", "_shouldLookEnabled"));
    const auto neck = ReadFloat(effector, FieldOffset("CampusCustomLookAtEffector", "neckLeftRightLimit"));
    void* actor = job ? HandleTarget(job->actorHandle) : nullptr;
    const bool wantLook = WantFollowLook(actor, job);
    PublishNaturalGazeController(actor, effector, controller, wantLook);
    TraceLiveGazeStage(actor, effector, "maintenance-before", nullptr, controller);
    const bool officialHold = outRange || !live || prohibit;
    auto* wrapperClass = Named("PanoramaActorLookAtController");
    auto* lookClass = Named("CampusActorLookAtController");
    auto* focus = FindMethod(wrapperClass, "SetFocusActorTarget", "System.Void", false, {"System.Boolean"});
    auto* changeType = FindMethod(lookClass, "OnLookTargetTypeChange", "System.Void", false,
        {"Campus.Common.LookAt.LookTargetType"});
    auto* enable = FindMethod(lookClass, "SetLookAtEnable", "System.Void", false, {"System.Boolean"});
    auto* target = FindMethod(lookClass, "SetLookAtTarget", "System.Void", false, {"System.Boolean"});
    auto* apply = FindMethod(lookClass, "ApplyWeightSettings", "System.Void", false, {});
    auto* manual = FindMethod(lookClass, "ManualUpdate", "System.Void", false, {});
    auto* weight = FindMethod(lookClass, "ManualUpdateLookAtWeight", "System.Void", false, {});
    int eye = 1;
    bool on = true, off = false, fixedBody = false;
    void* ignored = nullptr;
    void* typeArgs[] = {&eye};
    void* offArgs[] = {&fixedBody};
    void* onArgs[] = {&on};
    void* focusOffArgs[] = {&off};
    bool focusOk = false, typeOk = false, targetOk = false, enableOk = false, applyOk = false;
    if (!wantLook) {
        if (focused) focusOk = focus && Invoke(focus, focusOffArgs, &ignored, controller);
        if (should) enableOk = inner && enable && Invoke(enable, focusOffArgs, &ignored, inner);
        if (job) job->armed = false;
    } else {
        const bool needArm = !job || !job->armed || (!officialHold && (!should || !focused));
        if (needArm && !officialHold) {
            focusOk = focus && Invoke(focus, onArgs, &ignored, controller);
            typeOk = inner && changeType && Invoke(changeType, typeArgs, &ignored, inner);
            targetOk = inner && target && Invoke(target, offArgs, &ignored, inner);
            enableOk = inner && enable && Invoke(enable, onArgs, &ignored, inner);
            applyOk = inner && apply && Invoke(apply, nullptr, &ignored, inner);
        } else if (!officialHold && inner && apply &&
            ReadFloat(inner, FieldOffset("CampusActorLookAtController", "_eyeWeight")) <= 0) {
            targetOk = target && Invoke(target, offArgs, &ignored, inner);
            applyOk = Invoke(apply, nullptr, &ignored, inner);
        }
    }
    const bool manualOk = inner && manual && Invoke(manual, nullptr, &ignored, inner);
    if (inner && weight) Invoke(weight, nullptr, &ignored, inner);
    TraceLiveGazeStage(actor, effector, "maintenance-after", nullptr, controller);
    const auto shouldNow = ReadByte(inner, FieldOffset("CampusActorLookAtController", "_shouldLookEnabled"));
    const auto focusedNow = ReadByte(controller, FieldOffset("PanoramaActorLookAtController", "_isFocusTarget"));
    const auto eyeWeight = ReadFloat(inner, FieldOffset("CampusActorLookAtController", "_eyeWeight"));
    const auto bodyWeight = ReadFloat(inner, FieldOffset("CampusActorLookAtController", "_bodyWeight"));
    const auto enabled = ReadByte(effector, FieldOffset("CampusCustomLookAtEffector", "isEnableLookAt"));
    const auto eyes = ReadFloat(effector, FieldOffset("CampusCustomLookAtEffector", "eyesWeight"));
    const auto head = ReadFloat(effector, FieldOffset("CampusCustomLookAtEffector", "headWeight"));
    const auto body = ReadFloat(effector, FieldOffset("CampusCustomLookAtEffector", "bodyWeight"));
    if (job && wantLook && !officialHold && shouldNow && focusedNow) job->armed = true;
    if (job && job->enableLogs > 0 && wantLook && shouldNow && enabled && focusedNow && eyes > 0 && !officialHold) return;
    if (job) {
        ++job->enableLogs;
        if (job->enableLogs > 8 && (job->enableLogs % 120) != 0) return;
    }
    auto line = Stream();
    line << "COMMON_GAZE_ENABLE controller=" << controller << " inner=" << inner
        << " actor=" << actor << " follow=" << camera::ReadVrFollowActorController()
        << " wantLook=" << wantLook << " scope=" << liveGazeScope.load()
        << " focusOk=" << focusOk << " typeOk=" << typeOk << " targetOk=" << targetOk
        << " enableOk=" << enableOk << " applyOk=" << applyOk << " manualOk=" << manualOk
        << " armed=" << (job && job->armed) << " officialHold=" << officialHold
        << " outRange=" << unsigned(outRange) << " liveEnable=" << unsigned(live)
        << " prohibit=" << unsigned(prohibit)
        << " shouldLook=" << unsigned(shouldNow) << " isEnableLookAt=" << unsigned(enabled)
        << " isFocusTarget=" << unsigned(focusedNow)
        << " eyeWeight=" << eyeWeight << " bodyWeight=" << bodyWeight
        << " eyesWeight=" << eyes << " headWeight=" << head << " bodyIkWeight=" << body
        << " neckLeftRightLimit=" << neck
        << " effector=" << effector;
    Log(line.str());
}
Method* LoadLookAt() {
    auto* load = FindMethod(Named("LiveResourceManager"), "LoadLookAtEffectorAsync",
        "Cysharp.Threading.Tasks.UniTask<Campus.Common.CampusCustomLookAtEffector>", false,
        {"System.String", "System.Threading.CancellationToken"});
    if (load) return load;
    auto* klass = Named("LiveResourceManager");
    if (klass) for (auto* m : klass->methods)
        if (m && m->name == "LoadLookAtEffectorAsync" && !m->static_function && m->args.size() == 2) return m;
    return nullptr;
}
void TryStartRebuild(void* actor, const std::string& goName) {
    std::string candidate = goName;
    if (const auto cut = candidate.find(" |"); cut != std::string::npos) candidate = candidate.substr(0, cut);
    const auto animationOffset = FieldOffset("CampusActorController", "_actorAnimation");
    void* animation = nullptr;
    if (animationOffset > 0) Copy(static_cast<char*>(actor) + animationOffset, &animation, sizeof(animation));
    const auto slotOffset = FieldOffset("CampusActorAnimation", "_customLookAtEffector");
    void* slot = nullptr;
    if (animation && slotOffset > 0) Copy(static_cast<char*>(animation) + slotOffset, &slot, sizeof(slot));
    if (lastPanoramaLive == 1) {
        if (!panoramaSkipLogged) {
            Log("COMMON_GAZE_REBUILD skip=official-panorama");
            panoramaSkipLogged = true;
        }
        return;
    }
    if (lastPanoramaLive != 0 || candidate.empty()) return;
    if (IsEffector(slot)) {
        auto line = Stream();
        line << "COMMON_GAZE_REBUILD skip=has-slot actor=" << actor << " nameCandidate=" << candidate
            << " slot=" << slot;
        Log(line.str());
        return;
    }
    for (auto it = rebuilds.begin(); it != rebuilds.end();) {
        if (HandleTarget(it->actorHandle) != actor) { ++it; continue; }
        if (it->step == RebuildJob::Step::Fail) {
            ReleaseHandle(it->actorHandle);
            ReleaseHandle(it->taskHandle);
            ReleaseHandle(it->effectorHandle);
            ReleaseHandle(it->controllerHandle);
            it = rebuilds.erase(it);
            break;
        }
        return;
    }
    if (rebuilds.size() >= 16) {
        Log("COMMON_GAZE_REBUILD skip=job-limit");
        return;
    }
    const auto asset = AssetName(candidate, 2);
    if (asset.empty()) {
        auto line = Stream();
        line << "COMMON_GAZE_GAP reason=rebuild-asset nameCandidate=" << candidate;
        Log(line.str());
        return;
    }
    auto* manager = ResourceManager();
    auto* load = LoadLookAt();
    if (load && load->return_type && load->return_type->address)
        if (auto* klass = UnityResolve::Invoke<void*>("il2cpp_class_from_type", load->return_type->address))
            DumpRawClass(klass);
    if (!manager || !load) {
        auto line = Stream();
        line << "COMMON_GAZE_GAP reason=rebuild-load manager=" << manager << " load=" << load;
        Log(line.str());
        return;
    }
    auto* id = UnityResolve::UnityType::String::New(asset);
    unsigned char cancellation[16] = {};
    void* args[] = {id, cancellation};
    void* task = nullptr;
    const bool ok = id && Invoke(load, args, &task, manager);
    auto line = Stream();
    line << "COMMON_GAZE_LOAD actor=" << actor << " nameCandidate=" << candidate << " asset=" << asset
        << " manager=" << manager << " ok=" << ok << " task=" << task << " taskClass=" << ObjectClass(task);
    Log(line.str());
    if (!ok || !task) return;
    DumpRawClass(ObjectClass(task));
    RebuildJob job;
    job.actorHandle = RootObject(actor);
    job.taskHandle = RootObject(task);
    job.candidate = candidate;
    job.asset = asset;
    if (!job.actorHandle || !job.taskHandle) {
        ReleaseHandle(job.actorHandle);
        ReleaseHandle(job.taskHandle);
        Log("COMMON_GAZE_GAP reason=rebuild-root");
        return;
    }
    rebuilds.push_back(job);
}
void TickRebuilds() {
    const auto get = Api<panorama::GetTarget>("il2cpp_gchandle_get_target");
    if (!get) return;
    for (auto& job : rebuilds) {
        auto* actor = get(job.actorHandle);
        if (!Alive(actor)) {
            if (job.step != RebuildJob::Step::Fail && job.step != RebuildJob::Step::Done) {
                Log("COMMON_GAZE_REBUILD skip=dead-actor");
                job.step = RebuildJob::Step::Fail;
            }
            continue;
        }
        if (job.step == RebuildJob::Step::WaitTask) {
            int status = -1;
            auto* loaded = TaskEffector(get(job.taskHandle), &status);
            auto* effector = UniqueEffector(job.asset, loaded);
            auto line = Stream();
            line << "COMMON_GAZE_LOAD_WAIT actor=" << actor << " nameCandidate=" << job.candidate
                << " asset=" << job.asset << " status=" << status << " effector=" << effector
                << " effectorClass=" << ObjectClass(effector) << " polls=" << job.polls;
            Log(line.str());
            if (effector) {
                job.effectorHandle = RootObject(effector);
                job.step = job.effectorHandle ? RebuildJob::Step::Init : RebuildJob::Step::Fail;
            } else if (++job.polls >= 40) {
                Log("COMMON_GAZE_GAP reason=load-timeout");
                job.step = RebuildJob::Step::Fail;
            }
        }
        if (job.step == RebuildJob::Step::Init) {
            auto* effector = get(job.effectorHandle);
            if (!Alive(effector)) {
                Log("COMMON_GAZE_INIT reason=dead-effector");
                job.step = RebuildJob::Step::Fail;
                continue;
            }
            if (auto* controller = CreateOfficialController(actor, effector)) {
                job.controllerHandle = RootObject(controller);
                job.step = job.controllerHandle ? RebuildJob::Step::Done : RebuildJob::Step::Fail;
                if (job.step == RebuildJob::Step::Done) {
                    const bool smoothing = RegisterRebuiltLiveGazeSmoothing(actor, effector);
                    Log(std::string("COMMON_GAZE_SMOOTH_APPLY once-per-frame=") + (smoothing ? "1" : "0"));
                    EnableOfficialLook(controller, effector, &job);
                }
            } else {
                job.step = RebuildJob::Step::Fail;
            }
        }
        // Done jobs are maintained by KeepOfficialLook. Do not add another
        // limit/weight pass merely because the slower census tick ran.
    }
}
void KeepOfficialLook() {
    const auto get = Api<panorama::GetTarget>("il2cpp_gchandle_get_target");
    if (!get) return;
    for (auto& job : rebuilds) {
        if (job.step != RebuildJob::Step::Done) continue;
        EnableOfficialLook(get(job.controllerHandle), get(job.effectorHandle), &job);
    }
}
void DescribeFactory(void* actor, const std::string& goName) {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) return;
    std::string candidate = goName;
    if (const auto cut = candidate.find(" |"); cut != std::string::npos) candidate = candidate.substr(0, cut);
    const auto animationOffset = FieldOffset("CampusActorController", "_actorAnimation");
    void* animation = nullptr;
    if (animationOffset > 0) Copy(static_cast<char*>(actor) + animationOffset, &animation, sizeof(animation));
    const auto slotOffset = FieldOffset("CampusActorAnimation", "_customLookAtEffector");
    void* slot = nullptr;
    if (animation && slotOffset > 0) Copy(static_cast<char*>(animation) + slotOffset, &slot, sizeof(slot));
    auto line = Stream();
    line << "COMMON_GAZE_FACTORY scene=" << previousScene << " actor=" << actor
        << " nameCandidate=" << candidate << " animation=" << animation << " animationAlive=" << Alive(animation)
        << " slot=" << slot << " slotClass=" << ObjectClass(slot);
    Log(line.str());
    if (candidate.empty()) return;
    auto* master = LookMaster();
    auto* getAsset = FindMethod(Named("CharacterLookEffectorMaster"), "GetAssetName", "System.String", false,
        {"System.String", "Campus.Common.Proto.Client.Enums.CharacterLookEffectorSituationType"});
    if (!getAsset) {
        auto* klass = Named("CharacterLookEffectorMaster");
        if (klass) for (auto* m : klass->methods)
            if (m && m->name == "GetAssetName" && !m->static_function && m->args.size() == 2 &&
                m->return_type && m->return_type->name == "System.String" && m->args[0]->pType &&
                m->args[0]->pType->name == "System.String") {
                getAsset = m;
                auto miss = Stream();
                miss << "COMMON_GAZE_FACTORY getAssetFallback arg1=" << (m->args[1]->pType ? m->args[1]->pType->name : "missing");
                Log(miss.str());
                break;
            }
    }
    if (!master || !getAsset) {
        Log("COMMON_GAZE_GAP reason=get-asset-name");
        return;
    }
    auto* id = UnityResolve::UnityType::String::New(candidate);
    if (!id) {
        Log("COMMON_GAZE_GAP reason=string-new");
        return;
    }
    for (int situation = 0; situation < 3; ++situation) {
        void* asset = nullptr;
        void* args[] = {id, &situation};
        const bool ok = Invoke(getAsset, args, &asset, master);
        auto row = Stream();
        row << "COMMON_GAZE_ASSET scene=" << previousScene << " actor=" << actor
            << " nameCandidate=" << candidate << " situation=" << situation << " ok=" << ok
            << " asset=" << (ok && asset ? static_cast<UnityResolve::UnityType::String*>(asset)->ToString() : "");
        Log(row.str());
    }
}
void DescribeObject(void* object, Class* klass) {
    auto line = Stream(); line << "COMMON_GAZE_OBJECT scene=" << previousScene
        << " class=" << klass->name << " object=" << object;
    void* go = nullptr;
    auto* component = Named("Component");
    const bool isComponent = component && UnityResolve::Invoke<bool>(
        "il2cpp_class_is_assignable_from", component->address, klass->address);
    if (isComponent && Invoke(ReadMethod("Component", "get_gameObject", "UnityEngine.GameObject", false), nullptr, &go, object) && Alive(go)) {
        line << " gameObject=" << go;
        void* boxed = nullptr;
        using Unbox = void*(*)(void*);
        const auto unbox = Api<Unbox>("il2cpp_object_unbox");
        if (unbox && Invoke(ReadMethod("GameObject", "get_activeInHierarchy", "System.Boolean", false), nullptr, &boxed, go) && boxed) {
            unsigned char active = 0;
            if (Copy(unbox(boxed), &active, 1)) line << " active=" << unsigned(active);
        }
        void* name = nullptr;
        if (Invoke(ReadMethod("Object", "get_name", "System.String", false), nullptr, &name, go) && name)
            line << " name=" << static_cast<UnityResolve::UnityType::String*>(name)->ToString();
        if (klass->name == "CampusActorController" && name) {
            const auto text = static_cast<UnityResolve::UnityType::String*>(name)->ToString();
            DescribeFactory(object, text);
            TryStartRebuild(object, text);
        }
        if (klass->name == "LiveResourceManager" && !resourceHandle) resourceHandle = RootObject(object);
    } else {
        void* name = nullptr;
        if (Invoke(ReadMethod("Object", "get_name", "System.String", false), nullptr, &name, object) && name)
            line << " name=" << static_cast<UnityResolve::UnityType::String*>(name)->ToString();
    }
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled && klass->name == "CampusLiveTimelineController") {
        auto* prohibited = FindMethod(klass, "IsLookAtProhibited", "System.Boolean", false, {"System.Int32"});
        auto* enableProhibited = FindMethod(klass, "IsLookAtEnableProhibited", "System.Boolean", false, {"System.Int32"});
        using Unbox = void*(*)(void*);
        const auto unbox = Api<Unbox>("il2cpp_object_unbox");
        auto timeline = Stream();
        timeline << "COMMON_GAZE_TIMELINE scene=" << previousScene << " controller=" << object
            << " panoramaLive=" << lastPanoramaLive;
        for (int index = 0; index < 8; ++index) {
            void* boxed = nullptr;
            void* args[] = {&index};
            unsigned char off = 0, enableOff = 0;
            if (prohibited && unbox && Invoke(prohibited, args, &boxed, object) && boxed)
                Copy(unbox(boxed), &off, 1);
            boxed = nullptr;
            if (enableProhibited && unbox && Invoke(enableProhibited, args, &boxed, object) && boxed)
                Copy(unbox(boxed), &enableOff, 1);
            timeline << " actor" << index << "=" << unsigned(off) << "/" << unsigned(enableOff);
        }
        Log(timeline.str());
    }
    if (klass->name == "CampusActorController" || klass->name == "CampusVirtualActorController") {
        void* effector = nullptr;
        auto* getter = ReadMethod(klass->name.c_str(), "GetCurrentCustomLookAtEffector", "VL.IK.CustomLookAtEffector", false);
        const bool ok = getter && Invoke(getter, nullptr, &effector, object);
        line << " effectorGetter=" << (getter ? getter->address : nullptr) << " effectorOk=" << ok
            << " effector=" << effector << " effectorClass=" << ObjectClass(effector);
    }
    Log(line.str());
}
void ClearActors() {
    if (const auto free = Api<panorama::FreeHandle>("il2cpp_gchandle_free")) for (const auto h : actorHandles) free(h);
    actorHandles.clear();
}
void CensusActors(const char* className) {
    auto* actorClass = Named(className);
    auto* resourceClass = Named("Resources");
    auto* unityBase = Named("Object");
    if (!actorClass || !resourceClass || !unityBase || !UnityResolve::Invoke<bool>(
        "il2cpp_class_is_assignable_from", unityBase->address, actorClass->address)) {
        Log(std::string("COMMON_GAZE_CENSUS_GAP class=") + className + " reason=missing-or-not-unity-object"); return;
    }
    Method* find = nullptr;
    for (auto* m : resourceClass->methods) {
        if (m && m->name == "FindObjectsOfTypeAll" && m->static_function && m->address &&
            m->args.size() == 1 && m->args[0]->pType && m->args[0]->pType->name == "System.Type" &&
            m->return_type && m->return_type->name == "UnityEngine.Object[]") { find = m; break; }
    }
    const auto make = Api<panorama::NewHandle>("il2cpp_gchandle_new");
    const auto free = Api<panorama::FreeHandle>("il2cpp_gchandle_free");
    if (!find || !make || !free) { Log("PANORAMA_CENSUS_GAP reason=api-shape"); return; }
    void* type = actorClass->GetType();
    void* args[] = {type}; void* result = nullptr;
    if (!type || !Invoke(find, args, &result) || !result) { Log("PANORAMA_CENSUS_GAP reason=invoke"); return; }
    auto* array = static_cast<UnityResolve::UnityType::Array<void*>*>(result);
    // Root the result array across allocations/invocations; handles use this game's pointer-width ABI.
    const auto arrayRoot = make(result, false);
    if (!arrayRoot) { Log("COMMON_GAZE_CENSUS_GAP reason=array-root"); return; }
    struct Release { panorama::GcHandle h; panorama::FreeHandle free; ~Release() { free(h); } } release{arrayRoot, free};
    const auto before = actorHandles.size();
    std::uintptr_t length{};
    if (!Copy(&array->max_length, &length, sizeof(length)) || length > 1024) {
        Log("PANORAMA_CENSUS_GAP reason=array-bound"); return;
    }
    for (std::size_t i = 0; i < length; ++i) {
        void* object = nullptr;
        if (!Copy(reinterpret_cast<void*>(array->GetData() + i * sizeof(void*)), &object, sizeof(object))) break;
        if (actorHandles.size() >= 128) { Log("COMMON_GAZE_CENSUS_GAP reason=root-limit"); break; }
        auto* actualAddress = ObjectClass(object);
        // FindObjectsOfTypeAll can return derived actors. Resolve their real class instead of
        // treating a non-exact match as evidence that the actor/controller is absent.
        if (actualAddress && !classes.contains(actualAddress) && classes.size() < std::size(entries) + 16) {
            const auto* name = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", actualAddress);
            const auto* ns = UnityResolve::Invoke<const char*>("il2cpp_class_get_namespace", actualAddress);
            auto* image = UnityResolve::Invoke<void*>("il2cpp_class_get_image", actualAddress);
            const auto* assembly = image ? UnityResolve::Invoke<const char*>("il2cpp_image_get_name", image) : nullptr;
            if (name && ns && assembly) DumpClass({assembly, ns, name});
        }
        const auto found = classes.find(actualAddress);
        if (found != classes.end() && Alive(object)) {
            if (const auto handle = make(object, false)) {
                actorHandles.push_back(handle);
                DescribeObject(object, found->second);
            }
        } else {
            auto line = Stream(); line << "COMMON_GAZE_CENSUS_SKIP query=" << className << " object=" << object
                << " actualClass=" << ObjectClass(object) << " alive=" << Alive(object); Log(line.str());
        }
    }
    auto line = Stream(); line << "COMMON_GAZE_CENSUS scene=" << previousScene << " class=" << className
        << " count=" << length << " rooted=" << (actorHandles.size() - before); Log(line.str());
}
void Sample(void* object, unsigned depth, std::unordered_set<void*>& visited, bool heartbeat) {
    if (!object || depth > 5 || visited.size() >= 256 || !visited.insert(object).second) return;
    const auto found = classes.find(ObjectClass(object));
    if (found == classes.end()) return;
    auto* klass = found->second;
    if (!unityObjects.contains(klass->address)) {
        auto* base = Named("Object");
        if (!base) return;
        unityObjects[klass->address] = UnityResolve::Invoke<bool>("il2cpp_class_is_assignable_from", base->address, klass->address);
    }
    if (unityObjects[klass->address] && !Alive(object)) return;
    auto line = Stream(); line << "PANORAMA_STATE class=" << klass->name << " object=" << object;
    std::vector<void*> children;
    auto fields = klass->fields;
    void* parent = klass->address;
    for (unsigned depth = 0; parent && depth < 8; ++depth) {
        parent = UnityResolve::Invoke<void*>("il2cpp_class_get_parent", parent);
        if (const auto base = classes.find(parent); base != classes.end())
            fields.insert(fields.end(), base->second->fields.begin(), base->second->fields.end());
    }
    for (const auto* f : fields) {
        if (!f || !f->type || f->offset < 16 || f->offset > 65536) continue;
        using Flags = int(*)(void*);
        static const auto flags = Api<Flags>("il2cpp_field_get_flags");
        if (!flags || (flags(f->address) & 0x10)) continue;
        const auto& type = f->type->name;
        auto* address = static_cast<char*>(object) + f->offset;
        if (type == "System.Boolean") {
            unsigned char value{}; if (Copy(address, &value, 1)) line << ' ' << f->name << '=' << unsigned(value);
        } else if (type == "System.Int32" || type == "Campus.Common.Panorama.PanoramaCameraType" ||
                   type == "Campus.Common.LookAt.LookTargetType") {
            int value{}; if (Copy(address, &value, 4)) line << ' ' << f->name << '=' << value;
        } else if (type == "System.Single") {
            float value{}; if (Copy(address, &value, 4)) line << ' ' << f->name << '=' << value;
        } else if (type == "UnityEngine.Transform" || type == "UnityEngine.Camera" ||
                   type.starts_with("Campus.") || type.starts_with("Cinemachine.")) {
            // Nested value types are not pointers. Only exact resolved reference types or known refs.
            bool reference = type == "UnityEngine.Transform" || type == "UnityEngine.Camera" ||
                type == "Cinemachine.CinemachineVirtualCamera" || type == "Campus.Common.ICampusActorController";
            for (const auto& [key, c] : classes) if (type == c->namespaze + "." + c->name &&
                !UnityResolve::Invoke<bool>("il2cpp_class_is_valuetype", c->address)) reference = true;
            if (!reference) continue;
            void* value = nullptr;
            if (Copy(address, &value, sizeof(value))) { line << ' ' << f->name << '=' << value; children.push_back(value); }
        }
    }
    if (klass->name == "Live3DPresenter") {
        const auto offset = FieldOffset("Live3DPresenter", "<IsPanoramaLive>k__BackingField");
        unsigned char flag = 0;
        if (offset > 0 && Copy(static_cast<char*>(object) + offset, &flag, 1)) lastPanoramaLive = flag;
    }
    const auto state = line.str();
    if (previousStates.size() >= 256 && !previousStates.contains(object)) previousStates.clear();
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled && (heartbeat || previousStates[object] != state)) {
        if (stateLines < 30000) { Log(state); ++stateLines; }
        else if (!stateLimitLogged) { Log("PANORAMA_STATE_GAP reason=scene-line-limit limit=30000"); stateLimitLogged = true; }
        previousStates[object] = state;
    }
    for (auto* child : children) Sample(child, depth + 1, visited, heartbeat);
}
} // namespace

void TickCommonLiveGazeProbe() noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    try {
        const auto now = Clock::now();
        if (now >= nextWork) {
            nextWork = now + std::chrono::milliseconds(250);
            if (entryIndex < std::size(entries)) DumpClass(entries[entryIndex++]);
            else if (codeIndex < codeQueue.size()) DumpCode(codeQueue[codeIndex++]);
        }
        KeepOfficialLook();
        if (now < nextSample || entryIndex < std::size(entries)) return;
        nextSample = now + std::chrono::milliseconds(500);
        auto* scene = AliveLiveScenePresenter();
        if (scene != previousScene) {
            ClearActors(); ClearRebuilds(); previousStates.clear(); censusCount = 0; previousScene = scene;
            stateLines = 0; stateLimitLogged = false;
            nextCensus = now + std::chrono::seconds(3);
            auto line = Stream(); line << "PANORAMA_SCENE scene=" << scene; Log(line.str());
        }
        // Diagnostic saturation must never stop runtime discovery/maintenance.
        if (!scene) return;
        if (censusCount < 3 * std::size(censusTypes) && now >= nextCensus) {
            const auto index = censusCount % std::size(censusTypes);
            if (index == 0) ClearActors();
            CensusActors(censusTypes[index]); ++censusCount;
            nextCensus = now + std::chrono::seconds(index + 1 == std::size(censusTypes) ? 4 : 1);
        }
        const bool heartbeat = now >= nextHeartbeat;
        if (heartbeat) nextHeartbeat = now + std::chrono::seconds(2);
        std::unordered_set<void*> visited;
        Sample(scene, 0, visited, heartbeat);
        TickRebuilds();
        if (const auto target = Api<panorama::GetTarget>("il2cpp_gchandle_get_target")) {
            for (auto it = actorHandles.begin(); it != actorHandles.end();) {
                if (auto* actor = target(*it); actor && Alive(actor)) {
                    Sample(actor, 0, visited, heartbeat); ++it;
                } else {
                    if (const auto free = Api<panorama::FreeHandle>("il2cpp_gchandle_free")) free(*it);
                    it = actorHandles.erase(it);
                    Log("PANORAMA_RETIRE reason=dead-actor");
                }
            }
        }
    } catch (...) {
        if (!failureLogged) { Log("PANORAMA_GAP reason=exception"); failureLogged = true; }
    }
}
} // namespace gakumas::vr
