#include "GripBlurSource.hpp"
#include "GripFullscreenBlurClassifier.hpp"
#include "GripBlurTextureWrite.hpp"
#include "GripTraceFieldReader.hpp"
#include "UnityStereoRenderer.hpp"
#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "deps/UnityResolve/UnityResolve.hpp"
#include "hooks/HookManager.hpp"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <locale>
#include <sstream>
#include <utility>
#include <vector>

namespace gakumas::vr {
namespace {
using Class = UnityResolve::Class;
using Method = UnityResolve::Method;
using Field = UnityResolve::Field;
constexpr const char* Core = "UnityEngine.CoreModule.dll";
constexpr const char* Ui = "UnityEngine.UI.dll";
constexpr const char* UiModule = "UnityEngine.UIModule.dll";
constexpr const char* Campus = "campus-submodule.Runtime.dll";
thread_local void* scopedPass = nullptr;
using DrawBlurFn = void(*)(void*, void*, void*, void*);
DrawBlurFn original = nullptr;
using SubmitMaterialFn = void(*)(void*, void*, int, void*);
SubmitMaterialFn originalSubmitMaterial = nullptr;
// A weak texture reference, never a cached managed UIRenderPass shell. Canvas
// rebuilds can submit materials outside Execute, before the next camera pass.
thread_local void* cachedDefault = nullptr;
thread_local unsigned submitLogs = 0;
thread_local unsigned readbackFailures = 0;
thread_local unsigned long long submitCalls = 0, blurSubmits = 0, fullscreenSubmits = 0;
thread_local unsigned long long nextSubmitCensus = 0;
Method *alive{}, *findObjects{}, *active{}, *getGraphic{}, *rectTransform{}, *getCanvas{};
Method *rootCanvas{}, *getTransform{}, *getRect{}, *transformPoint{}, *inversePoint{};
Method *getParent{}, *getComponent{}, *getRenderer{}, *materialCount{}, *getMaterial{};
Method *getTexture{}, *setTexture{}, *hasTexture{};
Class *blurClass{}, *rectClass{}, *maskClass{}, *rectMaskClass{};
Field *defaultTexture{}, *blurId{};
std::atomic<bool> installed{false};
std::atomic<unsigned long long> applies{0}, restores{0}, skips{0}, nextLog{0};

struct Rect { float x, y, w, h; };
struct Point { float x, y, z; };
struct OwnedMaterial {
    void* material{};
    void* originalTexture{};
    void* appliedTexture{};
    bool originalWasNull{};
};
thread_local std::vector<OwnedMaterial> owned;

void Log(const std::string& text) {
    if (GakumasLocal::Config::vrDiagnosticsStartupEnabled)
        (void)WriteVrLog("[VR][grip] GRIP_BLUR_SOURCE_" + text);
}
Class* Find(const char* assembly, const char* ns, const char* name) {
    auto* a = UnityResolve::Get(assembly); return a ? a->Get(name, ns) : nullptr;
}
std::string Type(UnityResolve::Type* t) { return t ? t->name : "?"; }
Method* Exact(Class* c, const char* name, bool stat, const char* ret,
              std::initializer_list<const char*> args) {
    Method* found = nullptr; unsigned matches = 0;
    if (c) for (auto* m : c->methods) {
        if (!m || !m->address || !m->function || m->name != name || m->static_function != stat ||
            Type(m->return_type) != ret || m->args.size() != args.size()) continue;
        bool same = true; std::size_t i = 0;
        for (auto* arg : args) if (Type(m->args[i++]->pType) != arg) same = false;
        if (same) { found = m; ++matches; }
    }
    std::ostringstream s; s.imbue(std::locale::classic());
    s << "API class=" << (c ? c->name : "?") << " method=" << name << " matches=" << matches
      << " static=" << stat << " return=" << ret;
    for (auto* arg : args) s << " arg=" << arg;
    if (matches == 1) s << " function=" << found->function << " info=" << found->address;
    Log(s.str()); return matches == 1 ? found : nullptr;
}
int Flags(void* f) { return UnityResolve::Invoke<int>("il2cpp_field_get_flags", f); }
Field* ExactField(Class* c, const char* name, const char* type, bool stat) {
    Field* found = nullptr; unsigned matches = 0;
    if (c) for (auto* f : c->fields) if (f && f->address && f->name == name && Type(f->type) == type &&
        ((Flags(f->address) & GripFieldStatic) != 0) == stat) { found = f; ++matches; }
    Log(std::string("FIELD name=") + name + " type=" + type + " static=" + std::to_string(stat) +
        " matches=" + std::to_string(matches));
    return matches == 1 ? found : nullptr;
}
bool Invoke(Method* method, void* self, void** args, void** result = nullptr) {
    if (!method) return false;
    void* exception = nullptr;
    void* value = UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", method->address, self, args, &exception);
    if (result) *result = value;
    if (exception) { Log("INVOKE_FAIL method=" + method->name); return false; }
    return true;
}
bool Read(void* source, void* dest, std::size_t size) noexcept {
    if (!source) return false;
    __try { std::memcpy(dest, source, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* Unbox(void* object) { return object ? UnityResolve::Invoke<void*>("il2cpp_object_unbox", object) : nullptr; }
template<class T> bool Value(Method* method, void* self, void** args, T& value) {
    void* box = nullptr; return Invoke(method, self, args, &box) && Read(Unbox(box), &value, sizeof(value));
}
template<class T> T Value(Method* method, void* self, T fallback = {}) {
    T value = fallback; (void)Value(method, self, nullptr, value); return value;
}
void* Object(Method* method, void* self, void** args = nullptr) {
    void* value = nullptr; (void)Invoke(method, self, args, &value); return value;
}
bool Alive(void* object) {
    if (!object) return false;
    void* args[]{object}; bool value = false; return Value(alive, nullptr, args, value) && value;
}
void* Weak(void* object) {
    return object ? UnityResolve::Invoke<void*>("il2cpp_gchandle_new_weakref", object, false) : nullptr;
}
void* Target(void* handle) {
    return handle ? UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", handle) : nullptr;
}
void Free(void*& handle) {
    if (handle) UnityResolve::Invoke<void>("il2cpp_gchandle_free", std::exchange(handle, nullptr));
}
bool Assignable(Class* klass, void* object) {
    void* actual = nullptr;
    return klass && object && Read(object, &actual, sizeof(actual)) && actual &&
        UnityResolve::Invoke<bool>("il2cpp_class_is_assignable_from", klass->address, actual);
}
std::vector<void*> Objects(Class* klass) {
    if (!klass || !findObjects) return {};
    void* type = klass->GetType(); void* args[]{type}; void* array = nullptr;
    if (!type || !Invoke(findObjects, nullptr, args, &array) || !array) return {};
    return static_cast<UnityResolve::UnityType::Array<void*>*>(array)->ToVector();
}
bool CoversRoot(void* rect, void* rootTransform) {
    Rect r{}, root{};
    if (!Value(getRect, rect, nullptr, r) || !Value(getRect, rootTransform, nullptr, root) ||
        !std::isfinite(root.w) || !std::isfinite(root.h) || root.w <= 0 || root.h <= 0) return false;
    float minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY;
    for (unsigned i = 0; i < 4; ++i) {
        Point local{r.x + ((i & 1) ? r.w : 0), r.y + ((i & 2) ? r.h : 0), 0}, world{}, rootLocal{};
        void* a[]{&local}; void* b[]{&world};
        if (!Value(transformPoint, rect, a, world) || !Value(inversePoint, rootTransform, b, rootLocal) ||
            !std::isfinite(rootLocal.x) || !std::isfinite(rootLocal.y)) return false;
        minX = (std::min)(minX, rootLocal.x); minY = (std::min)(minY, rootLocal.y);
        maxX = (std::max)(maxX, rootLocal.x); maxY = (std::max)(maxY, rootLocal.y);
    }
    return GripBlurCoversRoot({minX, minY, maxX, maxY}, {root.x, root.y, root.w, root.h});
}
bool HasLocalMask(void* rect, void* rootTransform) {
    unsigned depth = 0;
    for (void* tr = rect; Alive(tr) && depth++ < 64; tr = Object(getParent, tr)) {
        for (auto* klass : {maskClass, rectMaskClass}) {
            void* type = klass->GetType(); void* args[]{type}; void* mask = nullptr;
            if (type && Invoke(getComponent, tr, args, &mask) && Alive(mask) && Value<bool>(active, mask)) {
                if (klass == maskClass || !Assignable(rectClass, tr) || !CoversRoot(tr, rootTransform)) return true;
            }
        }
    }
    return depth >= 64;
}
int BlurProperty() {
    int property = 0;
    UnityResolve::Invoke<void>("il2cpp_field_static_get_value", blurId->address, &property);
    return property;
}
bool SetMaterialTexture(void* material, int property, void* texture) {
    void* args[]{&property, texture}; return Invoke(setTexture, material, args);
}
void* DefaultTexture(void* pass) {
    void* fallback = nullptr;
    if (!ReadGripInstanceReference(pass, defaultTexture->address, Flags,
        [](void* object, void* field, void** output) {
            UnityResolve::Invoke<void>("il2cpp_field_get_value", object, field, output);
        }, fallback) || !Alive(fallback)) return nullptr;
    return fallback;
}
bool IsFullscreenBlur(void* blur) {
    if (!Alive(blur) || !Value<bool>(active, blur)) return false;
    void* graphic = Object(getGraphic, blur);
    if (!Alive(graphic) || !Value<bool>(active, graphic)) return false;
    void* rect = Object(rectTransform, graphic), *canvas = Object(getCanvas, graphic);
    void* root = Alive(canvas) ? Object(rootCanvas, canvas) : nullptr;
    void* rootTransform = Alive(root) ? Object(getTransform, root) : nullptr;
    return Alive(rect) && Alive(rootTransform) && Assignable(rectClass, rootTransform) &&
        CoversRoot(rect, rootTransform) && !HasLocalMask(rect, rootTransform);
}
bool BindOwned(void* material, void* fallback, int property, bool* writeInvoked = nullptr) {
    if (writeInvoked) *writeInvoked = false;
    auto found = std::find_if(owned.begin(), owned.end(), [material](const auto& item) {
        return Target(item.material) == material;
    });
    const bool created = found == owned.end();
    void* prop[]{&property}; bool readable = false;
    if (!Value(hasTexture, material, prop, readable)) return false;
    const auto result = WriteGripBlurTexture(readable, fallback,
        [&](void*& value) { return Invoke(getTexture, material, prop, &value); },
        [&](void* value) { return SetMaterialTexture(material, property, value); });
    if (!result.invoked) return false;
    if (writeInvoked) *writeInvoked = true;
    void* old = result.previous;
    if (!result.matches) {
        if (readbackFailures++ < 8 || readbackFailures % 600 == 0)
            Log("READBACK reason=texture-readback material=" +
                std::to_string(reinterpret_cast<std::uintptr_t>(material)) +
                " expected=" + std::to_string(reinterpret_cast<std::uintptr_t>(fallback)) +
                " actual=" + std::to_string(reinterpret_cast<std::uintptr_t>(result.readback)) +
                " available=" + std::to_string(result.readable) + " writeInvoked=1 action=retain-binding");
    }
    if (created) {
        owned.push_back({Weak(material), Weak(old), Weak(fallback), old == nullptr});
    } else {
        void* previousApplied = Target(found->appliedTexture);
        // CopyPropertiesFromMaterial can erase a local binding. A null
        // readback is not a new authored restore target.
        if (Alive(old) && old != previousApplied) {
            Free(found->originalTexture);
            found->originalTexture = Weak(old);
            found->originalWasNull = false;
            Log("REBASE material=" + std::to_string(reinterpret_cast<std::uintptr_t>(material)) +
                " reason=authored-binding-changed");
        }
        Free(found->appliedTexture); found->appliedTexture = Weak(fallback);
    }
    return created;
}
void RestoreAll(const char* reason) {
    int property = blurId ? BlurProperty() : 0;
    unsigned count = 0;
    for (auto& item : owned) {
        void* material = Target(item.material);
        if (Alive(material)) {
            void* saved = item.originalWasNull ? nullptr : Target(item.originalTexture);
            if (SetMaterialTexture(material, property, Alive(saved) ? saved : nullptr)) ++count;
        }
        Free(item.material); Free(item.originalTexture); Free(item.appliedTexture);
    }
    owned.clear();
    if (count) { restores += count; Log("RESTORE count=" + std::to_string(count) + " reason=" + reason); }
}
bool ApplySelective(void* pass) {
    void* fallback = DefaultTexture(pass);
    if (!fallback) return false;
    if (Target(cachedDefault) != fallback) {
        Free(cachedDefault); cachedDefault = Weak(fallback);
        Log("DEFAULT_CACHED source=authorized-pass");
    }
    int property = BlurProperty();
    std::vector<void*> seen;
    unsigned fullScreen = 0, local = 0;
    for (void* blur : Objects(blurClass)) {
        if (!IsFullscreenBlur(blur)) {
            if (Alive(blur) && Value<bool>(active, blur)) ++local;
            continue;
        }
        void* graphic = Object(getGraphic, blur);
        void* renderer = Object(getRenderer, graphic);
        const int count = Alive(renderer) ? Value<int>(materialCount, renderer) : 0;
        for (int i = 0; i < (std::min)(count, 8); ++i) {
            void* index[]{&i}; void* material = Object(getMaterial, renderer, index);
            if (!Alive(material)) continue;
            seen.push_back(material);
            BindOwned(material, fallback, property); ++fullScreen;
        }
    }
    for (auto it = owned.begin(); it != owned.end();) {
        void* material = Target(it->material);
        if (Alive(material) && std::find(seen.begin(), seen.end(), material) != seen.end()) { ++it; continue; }
        if (Alive(material)) {
            void* saved = it->originalWasNull ? nullptr : Target(it->originalTexture);
            if (SetMaterialTexture(material, property, Alive(saved) ? saved : nullptr)) {
                ++restores; Log("RESTORE reason=no-longer-fullscreen");
            }
        }
        Free(it->material); Free(it->originalTexture); Free(it->appliedTexture); it = owned.erase(it);
    }
    applies += fullScreen;
    const auto now = GetTickCount64(); auto due = nextLog.load();
    if (now >= due && nextLog.compare_exchange_strong(due, now + 5000)) {
        Log("APPLY_SELECTIVE fullscreenMaterials=" + std::to_string(fullScreen) + " localPreserved=" +
            std::to_string(local) + " owned=" + std::to_string(owned.size()) +
            " classifier=root-coverage-unmasked source=pass-default");
    }
    return true;
}
void DrawBlurHook(void* self, void* cmd, void* cameraData, void* method) {
    original(self, cmd, cameraData, method);
    if (!installed.load(std::memory_order_acquire) || scopedPass != self || !self ||
        !GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    try {
        if (!ApplySelective(self)) {
            ++skips; Log("SKIP reason=unavailable-default original-global-and-local-retained");
        }
    } catch (...) { ++skips; Log("SKIP reason=exception original-global-and-local-retained"); }
}
// The persisted Graphic.UpdateMaterial call path gets materialForRendering
// (including all modifiers), then calls this exact SetMaterial overload.
// Patch its argument BEFORE native submission, including pre-Execute rebuilds.
void SubmitMaterialHook(void* self, void* material, int index, void* method) {
    try {
        if (installed.load(std::memory_order_acquire) &&
            GakumasLocal::Config::vrRuntimeStartupEnabled) {
            ++submitCalls;
            if (!GripUiTransparencyArmed()) {
                if (!owned.empty()) RestoreAll("submit-disarmed");
                Free(cachedDefault);
            } else if (Alive(self) && Alive(material)) {
                void* type = blurClass->GetType(); void* args[]{type};
                void* blur = type ? Object(getComponent, self, args) : nullptr;
                const bool hasBlur = Alive(blur);
                const bool fullscreen = hasBlur && IsFullscreenBlur(blur) &&
                    Object(getRenderer, Object(getGraphic, blur)) == self;
                if (hasBlur) ++blurSubmits;
                if (fullscreen) ++fullscreenSubmits;
                const auto now = GetTickCount64();
                if (now >= nextSubmitCensus) {
                    nextSubmitCensus = now + 5000;
                    Log("SUBMIT_CENSUS calls=" + std::to_string(submitCalls) +
                        " blur=" + std::to_string(blurSubmits) +
                        " fullscreen=" + std::to_string(fullscreenSubmits));
                }
                if (fullscreen) {
                    void* fallback = Target(cachedDefault);
                    if (Alive(fallback)) {
                        bool writeInvoked = false;
                        const bool created = BindOwned(material, fallback, BlurProperty(), &writeInvoked);
                        if (writeInvoked) ++applies;
                        if (created || submitLogs++ < 12 || submitLogs % 600 == 0) {
                            Log("APPLY_SUBMIT material=" + std::to_string(reinterpret_cast<std::uintptr_t>(material)) +
                                " index=" + std::to_string(index) + " new=" + std::to_string(created) +
                                " writeInvoked=" + std::to_string(writeInvoked) +
                                " insideExecute=" + std::to_string(scopedPass != nullptr));
                        }
                    } else {
                        Free(cachedDefault);
                        if (submitLogs++ < 12 || submitLogs % 600 == 0)
                            Log("SKIP reason=submit-default-unavailable reconcile=next-execute");
                    }
                }
            }
        }
    } catch (...) { ++skips; Log("SKIP reason=material-submit-exception original-global-and-local-retained"); }
    originalSubmitMaterial(self, material, index, method);
}
} // namespace

GripBlurSourceScope::GripBlurSourceScope(void* pass, bool allowed) noexcept
    : previous_(std::exchange(scopedPass, allowed ? pass : nullptr)) {
    if (!allowed && !owned.empty()) {
        try { RestoreAll("scope-not-authorized"); }
        catch (...) { Log("RESTORE_SKIP reason=exception"); }
    } else if (allowed && pass && installed.load(std::memory_order_acquire) &&
               GakumasLocal::Config::vrRuntimeStartupEnabled) {
        try {
            if (!ApplySelective(pass)) {
                ++skips; Log("SKIP reason=unavailable-default-at-enter original-global-and-local-retained");
            }
        } catch (...) { ++skips; Log("SKIP reason=enter-exception original-global-and-local-retained"); }
    }
}
GripBlurSourceScope::~GripBlurSourceScope() { scopedPass = previous_; }

void InstallGripBlurSource() noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled || installed.load()) return;
    try {
        auto* pass = Find(Campus, "Campus.Common.UIRenderer", "UIRenderPass");
        auto* draw = Exact(pass, "DrawBlur", false, "System.Void",
            {"UnityEngine.Rendering.CommandBuffer", "UnityEngine.Rendering.Universal.CameraData&"});
        defaultTexture = ExactField(pass, "_defaultBlurTexture", "UnityEngine.Texture", false);
        blurId = ExactField(pass, "_BlurTexId", "System.Int32", true);
        auto* object = Find(Core, "UnityEngine", "Object");
        auto* resources = Find(Core, "UnityEngine", "Resources");
        auto* behaviour = Find(Core, "UnityEngine", "Behaviour");
        auto* component = Find(Core, "UnityEngine", "Component");
        auto* transform = Find(Core, "UnityEngine", "Transform");
        auto* material = Find(Core, "UnityEngine", "Material");
        auto* graphic = Find(Ui, "UnityEngine.UI", "Graphic");
        auto* canvas = Find(UiModule, "UnityEngine", "Canvas");
        auto* renderer = Find(UiModule, "UnityEngine", "CanvasRenderer");
        blurClass = Find(Campus, "Campus.Common", "BackgroundBlur");
        auto* campusBase = Find(Campus, "Campus.Common", "CampusUIBase");
        auto* submitMaterial = Exact(renderer, "SetMaterial", false, "System.Void",
            {"UnityEngine.Material", "System.Int32"});
        rectClass = Find(Core, "UnityEngine", "RectTransform");
        maskClass = Find(Ui, "UnityEngine.UI", "Mask");
        rectMaskClass = Find(Ui, "UnityEngine.UI", "RectMask2D");
        alive = Exact(object, "IsNativeObjectAlive", true, "System.Boolean", {"UnityEngine.Object"});
        findObjects = Exact(resources, "FindObjectsOfTypeAll", true, "UnityEngine.Object[]", {"System.Type"});
        active = Exact(behaviour, "get_isActiveAndEnabled", false, "System.Boolean", {});
        getGraphic = Exact(campusBase, "get_Graphic", false, "UnityEngine.UI.Graphic", {});
        rectTransform = Exact(graphic, "get_rectTransform", false, "UnityEngine.RectTransform", {});
        getCanvas = Exact(graphic, "get_canvas", false, "UnityEngine.Canvas", {});
        getRenderer = Exact(graphic, "get_canvasRenderer", false, "UnityEngine.CanvasRenderer", {});
        rootCanvas = Exact(canvas, "get_rootCanvas", false, "UnityEngine.Canvas", {});
        getTransform = Exact(component, "get_transform", false, "UnityEngine.Transform", {});
        getParent = Exact(transform, "get_parent", false, "UnityEngine.Transform", {});
        getComponent = Exact(component, "GetComponent", false, "UnityEngine.Component", {"System.Type"});
        getRect = Exact(rectClass, "get_rect", false, "UnityEngine.Rect", {});
        transformPoint = Exact(transform, "TransformPoint", false, "UnityEngine.Vector3", {"UnityEngine.Vector3"});
        inversePoint = Exact(transform, "InverseTransformPoint", false, "UnityEngine.Vector3", {"UnityEngine.Vector3"});
        materialCount = Exact(renderer, "get_materialCount", false, "System.Int32", {});
        getMaterial = Exact(renderer, "GetMaterial", false, "UnityEngine.Material", {"System.Int32"});
        getTexture = Exact(material, "GetTexture", false, "UnityEngine.Texture", {"System.Int32"});
        hasTexture = Exact(material, "HasTexture", false, "System.Boolean", {"System.Int32"});
        setTexture = Exact(material, "SetTexture", false, "System.Void", {"System.Int32", "UnityEngine.Texture"});
        std::uint32_t alignment = 0;
        auto* rectValueClass = getRect ? UnityResolve::Invoke<void*>("il2cpp_class_from_type", getRect->return_type->address) : nullptr;
        auto* pointValueClass = transformPoint ? UnityResolve::Invoke<void*>("il2cpp_class_from_type", transformPoint->return_type->address) : nullptr;
        const int rectSize = rectValueClass ? UnityResolve::Invoke<int>("il2cpp_class_value_size", rectValueClass, &alignment) : 0;
        const int pointSize = pointValueClass ? UnityResolve::Invoke<int>("il2cpp_class_value_size", pointValueClass, &alignment) : 0;
        const bool api = draw && submitMaterial && defaultTexture && blurId && alive && findObjects && active &&
            getGraphic && rectTransform && getCanvas && getRenderer && rootCanvas && getTransform && getParent &&
            getComponent && getRect && transformPoint && inversePoint && materialCount && getMaterial &&
            getTexture && setTexture && hasTexture && blurClass && rectClass && maskClass && rectMaskClass &&
            rectSize == sizeof(Rect) && pointSize == sizeof(Point);
        if (!api) {
            Log("INIT_SKIP reason=live-api-shape rectSize=" + std::to_string(rectSize) +
                " pointSize=" + std::to_string(pointSize)); return;
        }
        const bool drawOk = GakumasVR::Hooks::CreateAndEnable(draw->function,
            reinterpret_cast<void*>(&DrawBlurHook), reinterpret_cast<void**>(&original), "Grip fullscreen blur source");
        const bool submitOk = GakumasVR::Hooks::CreateAndEnable(submitMaterial->function,
            reinterpret_cast<void*>(&SubmitMaterialHook), reinterpret_cast<void**>(&originalSubmitMaterial),
            "Grip fullscreen blur material submission");
        installed.store(drawOk && submitOk, std::memory_order_release);
        Log("READY installed=" + std::to_string(drawOk && submitOk) + " draw=" + std::to_string(drawOk) +
            " materialSubmit=" + std::to_string(submitOk) +
            " scope=armed-non-eye classifier=root-coverage-unmasked materialLocal=1 localBlurPreserved=1"
            " bindAt=enter+drawblur+before-material-submit submitGate=live-armed readback=diagnostic-only");
    } catch (...) { Log("INIT_SKIP reason=exception"); }
}
} // namespace gakumas::vr
