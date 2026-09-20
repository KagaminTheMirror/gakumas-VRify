#include "LiveGaze.hpp"
#include "LiveGazePolicy.hpp"
#include "NaturalGazeMotion.hpp"
#include "LiveGazeTrace.hpp"
#include "LivePause.hpp"
#include "VrFreeCamera.hpp"
#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "../GakumasLocalify/Il2cppUtils.hpp"
#include "../deps/UnityResolve/UnityResolve.hpp"
#include "../hooks/HookManager.hpp"
#include <Windows.h>
#include <chrono>
#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include <locale>
#include <intrin.h>
#include <array>
#include <iomanip>

namespace gakumas::vr {
namespace {
using Clock = std::chrono::steady_clock;
using Class = UnityResolve::Class;
using Method = UnityResolve::Method;
using Handle = void*; // Current game's pointer-width IL2CPP ABI.
struct Binding { Handle actor; Handle constraint; gaze::Request request; bool logged = false; bool failed = false; };
std::vector<Binding> bindings;
Class* actorClass = nullptr;
Method *findActors = nullptr, *setShould = nullptr, *setPosition = nullptr, *updatePosition = nullptr;
int constraintOffset = -1, shouldOffset = -1, disabledOffset = -1, actorOffset = -1;
using UpdateFn = void(*)(void*, void*);
using ShouldFn = void(*)(void*, bool, void*);
UpdateFn originalUpdate = nullptr;
ShouldFn originalShould = nullptr;
bool ready = false, attempted = false, internalRequest = false;
void* sceneIdentity = nullptr;
Clock::time_point nextCensus{};
unsigned censusCount = 0;
std::mutex centerMutex;
struct Center { float xyz[3]{}; bool valid = false; void* scene = nullptr; Clock::time_point time{}; } center;
struct SmoothBinding {
    Handle actor{};
    Handle effector{};
    gaze::WeightFrame frame;
    std::uint64_t calls = 0, advances = 0, duplicates = 0;
    int lastEnabled = -1, transitionSamples = 0;
    Clock::time_point nextReport{};
    Handle controller{};
    gaze::NaturalPolicy natural;
    gaze::NaturalMotion motion;
    int decisionFrame = -1;
    bool requested = false, naturalFailed = false;
    double previousAngle = 0;
    unsigned naturalSamples = 0;
    float naturalOfficialHead = 0, naturalConsumedHead = 0;
    int naturalHeadFrame = -1;
};
std::vector<SmoothBinding> smoothBindings;
UpdateFn originalWeightUpdate = nullptr;
using SpeedFn = float(*)(void*, void*);
SpeedFn originalInSpeed = nullptr, originalOutSpeed = nullptr;
// Scoped to this weight call; other effectors and callers stay official.
thread_local void* zeroStepEffector = nullptr;
struct WeightStepScope {
    void* previous;
    WeightStepScope(void* effector, bool advance) : previous(zeroStepEffector) {
        zeroStepEffector = advance ? nullptr : effector;
    }
    ~WeightStepScope() { zeroStepEffector = previous; }
};
float InSpeedHook(void* effector, void* info) {
    const float official = originalInSpeed(effector, info);
    return GakumasLocal::Config::vrRuntimeStartupEnabled && zeroStepEffector == effector ? 0.0f : official;
}
float OutSpeedHook(void* effector, void* info) {
    const float official = originalOutSpeed(effector, info);
    return GakumasLocal::Config::vrRuntimeStartupEnabled && zeroStepEffector == effector ? 0.0f : official;
}
Method* unityFrameCount = nullptr;
int sampleTimeOffset = -1, effectorEnableOffset = -1;
bool smoothingReady = false, smoothingAttempted = false;

void Log(const std::string& text) {
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled) WriteVrLog("[VR][gaze] " + text);
}
template<class T> T Api(const char* name) noexcept { return reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"), name)); }
bool Copy(const void* from, void* to, size_t bytes) noexcept {
    if (!from) return false;
    __try { memcpy(to, from, bytes); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Alive(void* object) noexcept {
    void* native = nullptr;
    return object && Copy(static_cast<char*>(object) + offsetof(UnityResolve::UnityType::UnityObject, m_CachedPtr), &native, sizeof(native)) && native;
}
void* Target(Handle h) { return h ? UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", h) : nullptr; }
void Free(Handle h) { if (h) UnityResolve::Invoke<void>("il2cpp_gchandle_free", h); }
bool Invoke(Method* m, void* instance, void** args, void** result) noexcept {
    using Fn = void*(*)(void*, void*, void**, void**);
    static auto fn = Api<Fn>("il2cpp_runtime_invoke");
    if (!fn || !m || !m->address) return false;
    void* exception = nullptr;
    __try { *result = fn(m->address, instance, args, &exception); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    return !exception;
}
Method* Exact(Class* c, const char* name, const char* result, bool isStatic, std::initializer_list<const char*> args) {
    if (!c) return nullptr;
    for (auto* m : c->methods) {
        if (!m || m->name != name || m->static_function != isStatic || !m->function || !m->address ||
            !m->return_type || m->return_type->name != result || m->args.size() != args.size()) continue;
        size_t i = 0; bool match = true;
        for (auto* type : args) { auto* a = m->args[i++]; if (!a || !a->pType || a->pType->name != type) match = false; }
        if (match) return m;
    }
    return nullptr;
}
int Field(Class* c, const char* name, const char* type) {
    if (c) for (auto* f : c->fields) if (f && f->name == name && f->type && f->type->name == type && f->offset >= 16 &&
        !(UnityResolve::Invoke<int>("il2cpp_field_get_flags", f->address) & 0x10)) return f->offset;
    return -1;
}
int ReadUnityFrame() {
    void* boxed = nullptr;
    using Unbox = void*(*)(void*);
    static auto unbox = Api<Unbox>("il2cpp_object_unbox");
    int value = -1;
    if (unbox && Invoke(unityFrameCount, nullptr, nullptr, &boxed) && boxed)
        Copy(unbox(boxed), &value, sizeof(value));
    return value;
}
bool ReadCenter(float (&xyz)[3]);
#include "NaturalGazeRuntime.inc.cpp"
void WeightUpdateHook(void* effector, void* info) {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) {
        originalWeightUpdate(effector, info); return;
    }
    for (auto& b : smoothBindings) {
        if (Target(b.effector) != effector || !Alive(effector) || !Alive(Target(b.actor))) continue;
        const int frame = ReadUnityFrame();
        const bool advance = b.frame.Advance(frame);
        const bool diagnostics = GakumasLocal::Config::vrDiagnosticsStartupEnabled;
        float before = 0, after = 0;
        unsigned char enabled = 0;
        if (diagnostics) {
            Copy(static_cast<char*>(effector) + sampleTimeOffset, &before, sizeof(before));
            Copy(static_cast<char*>(effector) + effectorEnableOffset, &enabled, sizeof(enabled));
        }
        TraceLiveGazeStage(nullptr, effector, advance ? "weight-before" : "weight-recompute", _ReturnAddress());
        {
            // Official sampleTime +=/-= deltaTime * speed, followed by curve
            // evaluation. Keep output evaluation even for a zero time step.
            WeightStepScope step(effector, advance);
            originalWeightUpdate(effector, info);
        }
        TraceLiveGazeStage(nullptr, effector, "weight-after", _ReturnAddress());
        if (diagnostics) {
            ++b.calls;
            if (advance) ++b.advances; else ++b.duplicates;
            const auto now = Clock::now();
            if (b.lastEnabled != enabled) {
                b.lastEnabled = enabled;
                b.transitionSamples = 4;
            }
            if ((advance && b.transitionSamples > 0) || now >= b.nextReport) {
                Copy(static_cast<char*>(effector) + sampleTimeOffset, &after, sizeof(after));
                std::ostringstream line;
                line.imbue(std::locale::classic());
                line << "LIVE_GAZE_SMOOTH_STEP effector=" << effector << " frame=" << frame
                     << " enabled=" << unsigned(enabled) << " advance=" << advance
                     << " sampleBefore=" << before << " sampleAfter=" << after
                     << " calls=" << b.calls << " advances=" << b.advances << " duplicates=" << b.duplicates;
                Log(line.str());
                if (advance && b.transitionSamples > 0) --b.transitionSamples;
                b.nextReport = now + std::chrono::seconds(2);
            }
        }
        return;
    }
    originalWeightUpdate(effector, info);
}
bool ResolveSmoothing() {
    auto* effector = Il2cppUtils::GetClass("campus-submodule.Runtime.dll", "Campus.Common", "CampusCustomLookAtEffector");
    auto* time = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Time");
    auto* update = Exact(effector, "UpdateLookAtWeight", "System.Void", false, {});
    auto* inSpeed = Exact(effector, "GetLookAtInWeightSpeed", "System.Single", false, {});
    auto* outSpeed = Exact(effector, "GetLookAtOutWeightSpeed", "System.Single", false, {});
    unityFrameCount = Exact(time, "get_frameCount", "System.Int32", true, {});
    sampleTimeOffset = Field(effector, "sampleTime", "System.Single");
    effectorEnableOffset = Field(effector, "isEnableLookAt", "System.Boolean");
    if (!update || !inSpeed || !outSpeed || !update->function || !unityFrameCount || !unityFrameCount->function ||
        sampleTimeOffset < 0 || effectorEnableOffset < 0) {
        Log("LIVE_GAZE_SMOOTH_SKIP reason=api-shape"); return false;
    }
    // Persist the exact live signature before the new getter is invoked.
    std::ostringstream api;
    api.imbue(std::locale::classic());
    api << "LIVE_GAZE_SMOOTH_API class=Time name=get_frameCount static=1 return=System.Int32 function="
        << unityFrameCount->function << " info=" << unityFrameCount->address
        << " weightFunction=" << update->function << " sampleOffset=" << sampleTimeOffset
        << " inSpeed=" << inSpeed->function << " outSpeed=" << outSpeed->function
        << " speedSignature=instance-System.Single-noargs duplicatePolicy=recompute-zero-step";
    Log(api.str());
    if (ReadUnityFrame() < 0) { Log("LIVE_GAZE_SMOOTH_SKIP reason=frame-query"); return false; }
    GakumasVR::Hooks::Request hooks[] = {
        {inSpeed->function, reinterpret_cast<void*>(InSpeedHook), reinterpret_cast<void**>(&originalInSpeed), "LiveGaze.GetLookAtInWeightSpeed"},
        {outSpeed->function, reinterpret_cast<void*>(OutSpeedHook), reinterpret_cast<void**>(&originalOutSpeed), "LiveGaze.GetLookAtOutWeightSpeed"},
        {update->function, reinterpret_cast<void*>(WeightUpdateHook), reinterpret_cast<void**>(&originalWeightUpdate), "LiveGaze.UpdateLookAtWeight"},
    };
    const bool ok = GakumasVR::Hooks::CreateAndEnableBatch(hooks, std::size(hooks));
    if (ok) naturalReady = ResolveNaturalCurves(effector);
    Log(ok ? "LIVE_GAZE_SMOOTH_READY" : "LIVE_GAZE_SMOOTH_SKIP reason=hook");
    return ok;
}
bool Request(void* actor, bool value) {
    void* ignored = nullptr; void* args[] = {&value};
    internalRequest = true;
    const bool ok = Invoke(setShould, actor, args, &ignored);
    internalRequest = false;
    if (!ok) Log("LIVE_GAZE_SKIP reason=request-invoke");
    return ok;
}
bool ReadCenter(float (&xyz)[3]) {
    std::lock_guard lock(centerMutex);
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-center.time).count();
    if (!gaze::UsableCenter(center.valid, center.scene == sceneIdentity, age, center.xyz[0], center.xyz[1], center.xyz[2])) return false;
    memcpy(xyz, center.xyz, sizeof(xyz)); return true;
}
void ShouldHook(void* actor, bool requested, void* info) {
    bool effective = requested;
    if (!internalRequest) for (auto& b : bindings) if (Target(b.actor) == actor && Alive(actor)) {
        b.request.OfficialChanged(requested);
        effective = b.request.Effective(b.request.owned && liveGazeRequested.load());
        Log(std::string("LIVE_GAZE_OFFICIAL request=") + (requested ? "1" : "0") + " effective=" + (effective ? "1" : "0"));
        break;
    }
    originalShould(actor, effective, info);
}
void UpdateHook(void* constraint, void* info) {
    // Run the authored position update first, then supply the VR position before
    // CampusLookAtConstraint.Update proceeds to the original UpdateLookAt.
    originalUpdate(constraint, info);
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled || !liveGazeRequested.load() || !Alive(constraint)) return;
    for (auto& b : bindings) {
        if (Target(b.constraint) != constraint || !b.request.owned || !Alive(Target(b.actor))) continue;
        void* campus = nullptr;
        if (actorOffset >= 0) Copy(static_cast<char*>(Target(b.actor)) + actorOffset, &campus, sizeof(campus));
        if (!LiveGazeWantsAllActors()) {
            if (void* follow = camera::ReadVrFollowActorController(); follow && Alive(follow) && campus != follow)
                continue;
        }
        unsigned char disabled = 1;
        if (!Copy(static_cast<char*>(constraint)+disabledOffset, &disabled, 1) || disabled) return;
        float xyz[3]{};
        if (!ReadCenter(xyz)) {
            if (b.logged) { Log("LIVE_GAZE_SKIP reason=stale-center"); b.logged = false; }
            return;
        }
        bool force = true; void* args[] = {xyz, &force}; void* ignored = nullptr;
        TraceLiveGazeStage(campus, nullptr, "vr-target-before");
        if (Invoke(setPosition, constraint, args, &ignored)) {
            b.failed = false;
            if (!b.logged) { Log("LIVE_GAZE_APPLY target=vr-center"); b.logged = true; }
        } else if (!b.failed) { Log("LIVE_GAZE_SKIP reason=position-invoke"); b.logged = false; b.failed = true; }
        TraceLiveGazeStage(campus, nullptr, "vr-target-after");
        return;
    }
}
bool Resolve() {
    actorClass = Il2cppUtils::GetClass("campus-submodule.Runtime.dll", "Campus.Common.Panorama", "PanoramaActorLookAtController");
    auto* constraint = Il2cppUtils::GetClass("campus-submodule.Runtime.dll", "Campus.Common.LookAt", "CampusLookAtConstraint");
    auto* resources = Il2cppUtils::GetClass("UnityEngine.CoreModule.dll", "UnityEngine", "Resources");
    setShould = Exact(actorClass, "SetShouldLookAtEnable", "System.Void", false, {"System.Boolean"});
    setPosition = Exact(constraint, "SetPosition", "System.Void", false, {"UnityEngine.Vector3", "System.Boolean"});
    updatePosition = Exact(constraint, "UpdatePosition", "System.Void", false, {});
    findActors = Exact(resources, "FindObjectsOfTypeAll", "UnityEngine.Object[]", true, {"System.Type"});
    constraintOffset = Field(actorClass, "_lookAtConstraint", "Campus.Common.LookAt.CampusLookAtConstraint");
    shouldOffset = Field(actorClass, "_shouldEnableLookAt", "System.Boolean");
    actorOffset = Field(actorClass, "_actor", "Campus.Common.ICampusActorController");
    disabledOffset = Field(constraint, "<IsDisabled>k__BackingField", "System.Boolean");
    if (!setShould || !setPosition || !updatePosition || !findActors || constraintOffset < 0 || shouldOffset < 0 || disabledOffset < 0 || actorOffset < 0) {
        Log("LIVE_GAZE_SKIP reason=api-shape"); return false;
    }
    GakumasVR::Hooks::Request hooks[] = {
        {updatePosition->function, reinterpret_cast<void*>(UpdateHook), reinterpret_cast<void**>(&originalUpdate), "LiveGaze.UpdatePosition"},
        {setShould->function, reinterpret_cast<void*>(ShouldHook), reinterpret_cast<void**>(&originalShould), "LiveGaze.SetShouldLookAtEnable"},
    };
    const bool ok = GakumasVR::Hooks::CreateAndEnableBatch(hooks, std::size(hooks));
    Log(ok ? "LIVE_GAZE_READY" : "LIVE_GAZE_SKIP reason=hooks"); return ok;
}
void Restore(Binding& b) {
    auto* actor = Target(b.actor); auto* constraint = Target(b.constraint);
    if (b.request.owned && Alive(actor)) {
        if (!Request(actor, b.request.official)) return;
        b.request.owned = false;
        b.logged = false;
        void* ignored = nullptr;
        if (Alive(constraint)) Invoke(updatePosition, constraint, nullptr, &ignored);
        Log(std::string("LIVE_GAZE_RESTORE official=") + (b.request.official ? "1" : "0"));
    }
}
void Census() {
    void* type = actorClass->GetType(); void* args[] = {type}; void* result = nullptr;
    if (!type || !Invoke(findActors, nullptr, args, &result) || !result) { Log("LIVE_GAZE_SKIP reason=census"); return; }
    const Handle root = UnityResolve::Invoke<Handle>("il2cpp_gchandle_new", result, false);
    if (!root) return;
    auto* array = static_cast<UnityResolve::UnityType::Array<void*>*>(result);
    std::uintptr_t count = 0;
    if (Copy(&array->max_length, &count, sizeof(count)) && count <= 1024) for (size_t i=0; i<count && bindings.size()<16; ++i) {
        void* actor = nullptr; void* klass = nullptr; void* constraint = nullptr;
        if (!Copy(reinterpret_cast<void*>(array->GetData()+i*sizeof(void*)), &actor, sizeof(actor)) || !Alive(actor) ||
            !Copy(actor, &klass, sizeof(klass)) || klass != actorClass->address) continue;
        bool exists = false; for (auto& b : bindings) if (Target(b.actor) == actor) exists = true;
        if (exists || !Copy(static_cast<char*>(actor)+constraintOffset, &constraint, sizeof(constraint)) || !Alive(constraint)) continue;
        unsigned char official = 0;
        if (!Copy(static_cast<char*>(actor)+shouldOffset, &official, 1)) continue;
        Binding b{UnityResolve::Invoke<Handle>("il2cpp_gchandle_new", actor, false),
                  UnityResolve::Invoke<Handle>("il2cpp_gchandle_new", constraint, false), {official != 0, false}};
        if (!b.actor || !b.constraint) { Free(b.actor); Free(b.constraint); continue; }
        bindings.push_back(b);
    }
    Free(root);
    Log("LIVE_GAZE_CENSUS bound=" + std::to_string(bindings.size()));
}
}
bool RegisterRebuiltLiveGazeSmoothing(void* actor, void* effector) noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return false;
    try {
        if (!Alive(actor) || !Alive(effector)) return false;
        if (!smoothingAttempted) { smoothingAttempted = true; smoothingReady = ResolveSmoothing(); }
        if (!smoothingReady) return false;
        for (auto& b : smoothBindings) if (Target(b.effector) == effector) return true;
        if (smoothBindings.size() >= 16) { Log("LIVE_GAZE_SMOOTH_SKIP reason=binding-limit"); return false; }
        SmoothBinding b;
        b.actor = UnityResolve::Invoke<Handle>("il2cpp_gchandle_new", actor, false);
        b.effector = UnityResolve::Invoke<Handle>("il2cpp_gchandle_new", effector, false);
        if (!b.actor || !b.effector) { Free(b.actor); Free(b.effector); return false; }
        smoothBindings.push_back(b);
        RegisterLiveGazeTrace(actor, effector);
        Log("LIVE_GAZE_SMOOTH_APPLY scope=rebuilt-effector");
        return true;
    } catch (...) { Log("LIVE_GAZE_SMOOTH_SKIP reason=registration-exception"); return false; }
}
void PublishNaturalGazeController(void* actor, void* effector, void* controller, bool requested) noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled || !naturalReady) return;
    try {
        for (auto& b : smoothBindings) if (Target(b.actor)==actor && Target(b.effector)==effector) {
            if (Target(b.controller)!=controller) {
                Free(b.controller); b.controller=nullptr;
                b.natural.Reset(); b.motion.Reset(); b.decisionFrame=-1;
                b.naturalFailed=false;
                if (Alive(controller)) b.controller=UnityResolve::Invoke<Handle>("il2cpp_gchandle_new",controller,false);
            }
            b.requested=requested; return;
        }
    } catch (...) { Log("NATURAL_GAZE_SKIP reason=controller-binding"); }
}
void TickNaturalLiveGaze(void* actor) noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled || !naturalReady) return;
    try {
        for (auto& b : smoothBindings) if (Target(b.actor)==actor && Alive(actor) && b.controller) {
            NaturalTick(b,actor); return;
        }
    } catch (...) { Log("NATURAL_GAZE_SKIP reason=decision-exception"); }
}
void ClearRebuiltLiveGazeSmoothing() noexcept {
    ClearLiveGazeTrace();
    for (auto& b : smoothBindings) { Free(b.actor); Free(b.effector); Free(b.controller); }
    smoothBindings.clear();
}
void PublishLiveGazeCenter(float x, float y, float z) noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    std::lock_guard lock(centerMutex);
    center = {{x,y,z}, true, AliveLiveScenePresenter(), Clock::now()};
}
void InvalidateLiveGazeCenter() noexcept { std::lock_guard lock(centerMutex); center.valid = false; }
void TickLiveGaze() noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    try {
        const bool enabled = liveGazeRequested.load();
        if (!attempted && enabled) { attempted = true; ready = Resolve(); }
        if (!ready) return;
        for (auto it = smoothBindings.begin(); it != smoothBindings.end();) {
            if (!Alive(Target(it->actor)) || !Alive(Target(it->effector))) {
                Free(it->actor); Free(it->effector); Free(it->controller); it = smoothBindings.erase(it);
                Log("LIVE_GAZE_SMOOTH_RETIRE reason=dead-object");
            } else ++it;
        }
        auto* scene = AliveLiveScenePresenter();
        if (scene != sceneIdentity) {
            InvalidateLiveGazeCenter();
            for (auto& b : bindings) { Restore(b); Free(b.actor); Free(b.constraint); }
            bindings.clear(); sceneIdentity = scene; censusCount = 0; nextCensus = {};
        }
        for (auto it=bindings.begin(); it!=bindings.end();) {
            if (!Alive(Target(it->actor)) || !Alive(Target(it->constraint))) {
                Restore(*it); Free(it->actor); Free(it->constraint); it=bindings.erase(it);
                Log("LIVE_GAZE_RETIRE reason=dead-object");
            } else ++it;
        }
        if (!enabled) {
            for (auto& b : bindings) Restore(b);
            censusCount = 0; nextCensus = {}; return;
        }
        if (!scene) return;
        const auto now = Clock::now();
        if (now >= nextCensus && (censusCount < 3 || (enabled && bindings.empty() && censusCount < 40))) {
            Census(); ++censusCount; nextCensus = now+std::chrono::seconds(3);
        }
        float xyz[3]{};
        if (!ReadCenter(xyz)) { for (auto& b : bindings) Restore(b); return; }
        void* follow = camera::ReadVrFollowActorController();
        bool assignedFollow = false;
        for (auto& b : bindings) {
            void* wrapper = Target(b.actor);
            void* campus = nullptr;
            if (wrapper && actorOffset >= 0)
                Copy(static_cast<char*>(wrapper) + actorOffset, &campus, sizeof(campus));
            const bool want = LiveGazeWantsAllActors() ? Alive(wrapper)
                : (follow && Alive(follow) ? campus == follow
                    : (!assignedFollow && Alive(wrapper)));
            if (want && Alive(wrapper)) assignedFollow = true;
            if (want && !b.request.owned)
                b.request.owned = Request(wrapper, true);
            else if (!want && b.request.owned)
                Restore(b);
        }
    } catch (...) { Log("LIVE_GAZE_SKIP reason=exception"); }
}
}
