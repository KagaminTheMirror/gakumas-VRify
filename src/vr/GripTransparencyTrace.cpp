#include "GripTransparencyTrace.hpp"
#include "GripTraceFieldReader.hpp"
#include "d3d11/GripPresentProbe.hpp"
#include "UnityStereoRenderer.hpp"
#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "deps/UnityResolve/UnityResolve.hpp"
#include "hooks/HookManager.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gakumas::vr {
namespace {
using Class = UnityResolve::Class;
using Method = UnityResolve::Method;
using Field = UnityResolve::Field;
constexpr const char* Core = "UnityEngine.CoreModule.dll";
constexpr const char* Urp = "Unity.RenderPipelines.Universal.Runtime.dll";
constexpr const char* Campus = "campus-submodule.Runtime.dll";
constexpr const char* Vl = "vl-unity.Runtime.dll";
constexpr const char* GF = "UnityEngine.Experimental.Rendering.GraphicsFormat";
constexpr const char* CT = "System.Threading.CancellationToken";
constexpr const char* UT = "Cysharp.Threading.Tasks.UniTask";
std::atomic<bool> ready{false};
thread_local bool internal = false;
thread_local void* currentCamera = nullptr;
thread_local bool eyeCamera = false;
thread_local bool selectedCamera = false;
thread_local bool sampleCamera = false;
thread_local void* currentPass = nullptr;
thread_local int currentEvent = 0;
thread_local unsigned passOrder = 0;
std::uint64_t cameraSerial = 0;
thread_local bool transparencyArmed = false;
std::atomic<bool> refreshRequested{true};
std::atomic<DWORD> ownerThread{0};
std::uint64_t nextSweep = 0, nextCapture = 0;
std::uint64_t lastCapture = 0;
bool lastSetting = false;
bool baselinePending = true;
std::unordered_set<void*> uiCameras;
d3d11::GripPresentProbe presentProbe;
std::vector<d3d11::GripPresentSource> presentSources;

std::uint64_t Now() { return GetTickCount64(); }
bool Enabled() { return GakumasLocal::Config::vrDiagnosticsStartupEnabled && ready.load(); }
bool Observe() { return Enabled() && !internal && GetCurrentThreadId() == ownerThread; }
struct Guard { bool old = internal; Guard() { internal = true; } ~Guard() { internal = old; } };
void Log(const std::string& s) { (void)WriteVrLog("[VR][grip-trace] GRIP_TRACE_" + s); }
std::string Hex(std::uint64_t v) { std::ostringstream s; s << "0x" << std::hex << v; return s.str(); }
std::string Ptr(void* p) { return Hex(reinterpret_cast<std::uintptr_t>(p)); }
std::string Quote(std::string s) {
    for (auto& c : s) if (c == '"' || c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (s.size() > 400) s.resize(400);
    return '"' + s + '"';
}
bool Read(void* p, void* out, std::size_t n) noexcept {
    if (!p || !out) return false;
    __try { std::memcpy(out, p, n); return true; }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
              EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}
void* Unbox(void* p) { return p ? UnityResolve::Invoke<void*>("il2cpp_object_unbox", p) : nullptr; }
bool Invoke(Method* m, void* self, void** args, void** output = nullptr) {
    if (!m || !m->address) return false;
    void* exc = nullptr;
    void* result = UnityResolve::Invoke<void*>("il2cpp_runtime_invoke", m->address, self, args, &exc);
    if (output) *output = result;
    if (exc) { Log("INVOKE_FAIL method=" + m->name + " exception=" + Ptr(exc)); return false; }
    return true;
}
template<class T> T Value(Method* m, void* self, T fallback = {}) {
    void* box = nullptr; T result = fallback;
    if (Invoke(m, self, nullptr, &box)) Read(Unbox(box), &result, sizeof(result));
    return result;
}
void* Object(Method* m, void* self) {
    void* result = nullptr; Invoke(m, self, nullptr, &result); return result;
}
Class* Find(const char* assembly, const char* ns, const char* name) {
    auto* a = UnityResolve::Get(assembly); return a ? a->Get(name, ns) : nullptr;
}
std::string Type(UnityResolve::Type* t) { return t ? t->name : "?"; }
int FieldFlags(void* field) {
    return UnityResolve::Invoke<int>("il2cpp_field_get_flags", field);
}
void Dump(Class* c, const char* assembly) {
    if (!c) { Log(std::string("CLASS assembly=") + assembly + " found=0"); return; }
    Log("CLASS assembly=" + std::string(assembly) + " type=" + c->namespaze + "." + c->name +
        " address=" + Ptr(c->address) + " methods=" + std::to_string(c->methods.size()));
    for (auto* m : c->methods) if (m) {
        std::string s = "METHOD type=" + c->namespaze + "." + c->name + " name=" + m->name +
            " static=" + std::to_string(m->static_function) + " return=" + Type(m->return_type) +
            " function=" + Ptr(m->function) + " info=" + Ptr(m->address);
        for (std::size_t i = 0; i < m->args.size(); ++i)
            s += " arg" + std::to_string(i) + "=" + Type(m->args[i]->pType);
        Log(s);
    }
    for (auto* f : c->fields) if (f)
        Log("FIELD type=" + c->namespaze + "." + c->name + " name=" + f->name +
            " fieldType=" + Type(f->type) + " offset=" + std::to_string(f->offset) +
            " flags=" + Hex(static_cast<unsigned>(FieldFlags(f->address))) +
            " static=" + std::to_string((FieldFlags(f->address) & GripFieldStatic) != 0));
}
Method* Exact(Class* c, const char* name, bool stat, const char* ret,
              std::initializer_list<const char*> args) {
    Method* found = nullptr; unsigned matches = 0;
    if (c) for (auto* m : c->methods) {
        if (!m || !m->function || m->name != name || m->static_function != stat ||
            Type(m->return_type) != ret || m->args.size() != args.size()) continue;
        std::size_t i = 0; bool same = true;
        for (auto* arg : args) if (Type(m->args[i++]->pType) != arg) same = false;
        if (same) { found = m; ++matches; }
    }
    if (matches != 1) { Log("API_MISS type=" + (c ? c->name : "?") + " method=" + name +
                           " matches=" + std::to_string(matches)); return nullptr; }
    return found;
}
void* FieldObject(void* obj, Field* f) {
    void* result = nullptr;
    if (f) ReadGripInstanceReference(obj, f->address, FieldFlags,
        [](void* instance, void* field, void** output) {
            UnityResolve::Invoke<void>("il2cpp_field_get_value", instance, field, output);
        }, result);
    return result;
}
int Size(UnityResolve::Type* t) {
    auto* klass = t ? UnityResolve::Invoke<void*>("il2cpp_class_from_type", t->address) : nullptr;
    std::uint32_t alignment = 0;
    return klass ? UnityResolve::Invoke<int>("il2cpp_class_value_size", klass, &alignment) : -1;
}

struct Api {
    Method *alive{}, *name{}, *id{}, *native{}, *transform{}, *parent{}, *gameObject{}, *active{};
    Method *findObjects{}, *rawTexture{}, *graphicMaterial{}, *rtGet{}, *rtFormat{}, *texFormat{};
    Method *shader{}, *uiCamera{}, *cameraTarget{}, *colorHandle{};
    Class *textureClass{}, *rtClass{}, *rawClass{}, *uiController{};
    std::unordered_map<void*, std::pair<Method*, Method*>> dimensions;
    std::unordered_map<void*, std::vector<Field*>> passFields;
} api;
bool Alive(void* obj) {
    if (!obj || !api.alive) return false;
    void* args[]{obj}; void* box = nullptr; bool alive = false;
    return Invoke(api.alive, nullptr, args, &box) && Read(Unbox(box), &alive, sizeof(alive)) && alive;
}
std::string Name(void* obj) {
    if (!obj) return "null";
    auto* str = static_cast<UnityResolve::UnityType::String*>(Object(api.name, obj));
    return str ? str->ToString() : "?";
}
void* Klass(void* obj) { void* result = nullptr; Read(obj, &result, sizeof(result)); return result; }
std::string ClassName(void* obj) {
    void* k = Klass(obj); if (!k) return "?";
    const char* ns = UnityResolve::Invoke<const char*>("il2cpp_class_get_namespace", k);
    const char* n = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", k);
    return std::string(ns ? ns : "?") + "." + (n ? n : "?");
}
bool IsRT(void* obj) {
    return obj && api.rtClass && UnityResolve::Invoke<bool>("il2cpp_class_is_assignable_from",
                                                           api.rtClass->address, Klass(obj));
}
std::string Path(void* component) {
    std::string result = Name(component);
    void* t = Object(api.transform, component);
    for (int i = 0; t && i < 10; ++i) {
        t = Object(api.parent, t); if (t) result = Name(t) + "/" + result;
    }
    return result;
}
std::pair<int, int> Dimensions(void* t) {
    void* k = Klass(t);
    if (!k || (!IsRT(t) && (!api.textureClass || !UnityResolve::Invoke<bool>(
        "il2cpp_class_is_assignable_from", api.textureClass->address, k)))) return {};
    auto it = api.dimensions.find(k);
    if (it == api.dimensions.end()) {
        auto* c = IsRT(t) ? api.rtClass : api.textureClass;
        it = api.dimensions.emplace(k, std::make_pair(
            Exact(c, "get_width", false, "System.Int32", {}),
            Exact(c, "get_height", false, "System.Int32", {}))).first;
    }
    return {Value<int>(it->second.first, t), Value<int>(it->second.second, t)};
}

using Il2CppGCHandle = void*;
static_assert(sizeof(Il2CppGCHandle) == sizeof(void*),
              "IL2CPP GC handles are pointer-sized; uint32 truncation crashes get_target");
Il2CppGCHandle NewWeak(void* object) {
    return object ? UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new_weakref", object, false)
                  : nullptr;
}
void* HandleTarget(Il2CppGCHandle handle) {
    return handle ? UnityResolve::Invoke<void*>("il2cpp_gchandle_get_target", handle) : nullptr;
}
void FreeHandle(Il2CppGCHandle& handle) {
    if (!handle) return;
    UnityResolve::Invoke<void>("il2cpp_gchandle_free", std::exchange(handle, nullptr));
}
struct Node { std::uint64_t serial{}; Il2CppGCHandle weak{}; int instance{}; bool vlSource{}; };
std::unordered_map<void*, Node> nodes;
std::uint64_t nodeSerial = 0;
std::unordered_map<void*, std::uint64_t> bindings;
std::unordered_map<void*, std::uint64_t> canvasBindings;
unsigned nodeOverflow = 0, eventCount = 0;
bool EventAllowed() {
    if (++eventCount <= 24000) return true;
    if (eventCount == 24001) Log("OVERFLOW lane=graph cap=24000");
    return false;
}
std::uint64_t NodeId(void* t, const char* reason) {
    if (!Alive(t)) return 0;
    int id = Value<int>(api.id, t);
    auto old = nodes.find(t);
    if (old != nodes.end()) {
        void* target = HandleTarget(old->second.weak);
        if (target == t && id == old->second.instance) return old->second.serial;
        Log("RETIRE node=" + std::to_string(old->second.serial) + " reason=identity-reused");
        FreeHandle(old->second.weak); nodes.erase(old);
    }
    if (nodes.size() >= 2048) {
        if (!nodeOverflow++) Log("OVERFLOW lane=nodes cap=2048");
        return 0;
    }
    const auto textureName = Name(t);
    Node n{++nodeSerial, NewWeak(t), id,
        textureName.rfind("_VLTargetTexture_", 0) == 0 ||
        textureName.rfind("_VLCapturedTexture_", 0) == 0};
    if (!n.weak) return 0;
    nodes.emplace(t, n);
    const auto [w, h] = Dimensions(t);
    const auto native = Value<void*>(api.native, t);
    Log("TEXTURE node=" + std::to_string(n.serial) + " object=" + Ptr(t) +
        " instance=" + std::to_string(id) + " native=" + Ptr(native) + " type=" + ClassName(t) +
        " name=" + Quote(Name(t)) + " width=" + std::to_string(w) + " height=" + std::to_string(h) +
        " graphicsFormat=" + std::to_string(w ? Value<int>(IsRT(t) ? api.rtFormat : api.texFormat, t, -1) : -1) +
        " origin=" + reason);
    return n.serial;
}
void ObservePresentSource(void* texture, std::uint64_t node, const std::string& label) {
    if (!sampleCamera || !node || !Alive(texture)) return;
    for (const auto& source : presentSources) if (source.node == node) return;
    if (presentSources.size() >= d3d11::GripPresentProbe::SourceCapacity) {
        Log("NATIVE_SKIP epoch=" + std::to_string(cameraSerial) + " reason=source-cap node=" + std::to_string(node));
        return;
    }
    // Resolve anew after liveness, even when a managed RT has kept its identity
    // across Release/Create. Never enqueue the old TEXTURE log's native pointer.
    d3d11::GripPresentSource source;
    source.texture.Attach(d3d11::AcquireGripPresentTexture(Value<void*>(api.native, texture)));
    if (!source.texture) {
        Log("NATIVE_SKIP epoch=" + std::to_string(cameraSerial) + " reason=native-acquire node=" + std::to_string(node));
        return;
    }
    source.node = node; source.label = label;
    presentSources.push_back(std::move(source));
}
void Edge(const char* kind, void* source, void* target, const char* stage) noexcept {
    if (!Observe()) return;
    try {
    Guard guard;
    if (!EventAllowed()) return;
    const auto s = NodeId(source, "copy-source"), t = NodeId(target, "copy-target");
    Log("EDGE kind=" + std::string(kind) + " source=" + std::to_string(s) +
        " target=" + std::to_string(t) + " stage=" + stage + " camera=" + Ptr(currentCamera));
    refreshRequested.store(true);
    } catch (...) { Log("CALL_FAIL stage=edge"); }
}

// Public async wrappers return the SAME opaque value through the compiler's
// Windows x64 sret ABI. Install only if live value sizes match 16/16/8 exactly.
// We never reinterpret an async result as a Texture2D or claim it completed.
struct UniTaskValue { std::uint64_t opaque[2]; };
struct RectValue { int x, y, w, h; };
struct TokenValue { void* source; };
using CopyAsyncFn = UniTaskValue(*)(void*, void*, RectValue, TokenValue, void*);
using BlitAsyncFn = UniTaskValue(*)(void*, void*, TokenValue, void*);
using CopyFn = void(*)(void*, void*, RectValue, void*);
using CreateFn = void*(*)(std::uint64_t, void*);
using UpdateFn = void(*)(void*, void*, void*);
CopyAsyncFn copyAsyncOrig{}; BlitAsyncFn blitAsyncOrig{}; CopyFn copyOrig{};
CreateFn createOrig{}; UpdateFn updateOrig{}, copyAddOrig{};
UniTaskValue CopyAsyncHook(void* s, void* t, RectValue r, TokenValue ct, void* m) {
    Edge("CopyAsync", s, t, "scheduled"); return copyAsyncOrig(s, t, r, ct, m);
}
UniTaskValue BlitAsyncHook(void* s, void* t, TokenValue ct, void* m) {
    Edge("BlitAsync", s, t, "scheduled"); return blitAsyncOrig(s, t, ct, m);
}
void CopyHook(void* s, void* t, RectValue r, void* m) {
    Edge("Copy", s, t, "enter"); copyOrig(s, t, r, m); Edge("Copy", s, t, "return");
}
void* CreateHook(std::uint64_t size, void* m) {
    void* t = createOrig(size, m);
    try { if (Observe()) { Guard guard; NodeId(t, "CaptureUtility.CreateTexture"); } }
    catch (...) { Log("CALL_FAIL stage=create"); }
    return t;
}
void UpdateHook(void* self, void* t, void* m) {
    updateOrig(self, t, m);
    try { if (Observe()) { Guard guard; if (EventAllowed()) Log("DISPLAY kind=VLSRPTargetImage node=" +
        std::to_string(NodeId(t, "VLSRPTargetImage")) + " object=" + Ptr(self) + " path=" + Quote(Path(self)));
        refreshRequested.store(true); } } catch (...) { Log("CALL_FAIL stage=update"); }
}
void CopyAddHook(void* self, void* rtHandle, void* m) {
    copyAddOrig(self, rtHandle, m);
    try { if (Observe()) { Guard guard; if (EventAllowed()) Log("CAPTURE_REGISTER manager=" + Ptr(self) +
        " target=" + std::to_string(NodeId(rtHandle ? Object(api.rtGet, rtHandle) : nullptr,
                                          "UIRenderResultCopyManager.Add")));
        refreshRequested.store(true); } } catch (...) { Log("CALL_FAIL stage=register"); }
}
template<class Fn> void Hook(Method* m, Fn detour, Fn* orig, const char* name) {
    bool ok = m && GakumasVR::Hooks::CreateAndEnable(m->function,
        reinterpret_cast<void*>(detour), reinterpret_cast<void**>(orig), name);
    Log("HOOK name=" + std::string(name) + " installed=" + std::to_string(ok));
}

// Native std::vector storage is not a managed GC root. Keep the returned
// managed array alive throughout the range-for (including every getter and
// allocation in its body), so all snapshot receivers remain rooted.
class ObjectSnapshot {
    Il2CppGCHandle root_ = nullptr;
    std::vector<void*> objects_;
public:
    explicit ObjectSnapshot(Class* c) {
        if (!c || !api.findObjects) return;
        void* t = c->GetType(); void* args[]{t}; void* array = nullptr;
        if (!t || !Invoke(api.findObjects, nullptr, args, &array) || !array) return;
        root_ = UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", array, false);
        if (!root_) return;
        try {
            objects_ = static_cast<UnityResolve::UnityType::Array<void*>*>(array)->ToVector();
        } catch (...) {
            FreeHandle(root_);
            throw;
        }
    }
    ObjectSnapshot(const ObjectSnapshot&) = delete;
    ObjectSnapshot& operator=(const ObjectSnapshot&) = delete;
    ~ObjectSnapshot() { FreeHandle(root_); }
    auto begin() const { return objects_.begin(); }
    auto end() const { return objects_.end(); }
};
ObjectSnapshot Objects(Class* c) { return ObjectSnapshot(c); }

// Read-only census: bounds are candidates, never authority to erase a draw.
// A full-size Graphic can still contain a sprite/stencil hole or just a border.
struct BlurCoverageApi {
    Class *blur{}, *rectClass{}, *mask{}, *rectMask{}, *image{};
    Method *graphic{}, *rectTransform{}, *canvas{}, *rootCanvas{}, *rect{};
    Method *toWorld{}, *fromWorld{}, *component{}, *enabled{}, *renderer{};
    Method *material{}, *materialCount{}, *clipping{}, *cull{}, *mainTexture{}, *color{};
    Method *hasProperty{}, *getFloat{};
    Method *imageTexture{}, *imageType{};
    bool ready = false;
} coverage;
struct CoverageRect { float x, y, w, h; };
struct CoveragePoint { float x, y, z; };
void InitBlurCoverage() {
    const char* ui = "UnityEngine.UI.dll";
    const char* module = "UnityEngine.UIModule.dll";
    coverage.blur = Find(Campus, "Campus.Common", "BackgroundBlur");
    auto* base = Find(Campus, "Campus.Common", "CampusUIBase");
    auto* graphic = Find(ui, "UnityEngine.UI", "Graphic");
    coverage.mask = Find(ui, "UnityEngine.UI", "Mask");
    coverage.rectMask = Find(ui, "UnityEngine.UI", "RectMask2D");
    coverage.image = Find(ui, "UnityEngine.UI", "Image");
    coverage.rectClass = Find(Core, "UnityEngine", "RectTransform");
    auto* transform = Find(Core, "UnityEngine", "Transform");
    auto* component = Find(Core, "UnityEngine", "Component");
    auto* behaviour = Find(Core, "UnityEngine", "Behaviour");
    auto* canvas = Find(module, "UnityEngine", "Canvas");
    auto* renderer = Find(module, "UnityEngine", "CanvasRenderer");
    auto* material = Find(Core, "UnityEngine", "Material");
    for (auto* c : {coverage.blur, base}) Dump(c, Campus);
    for (auto* c : {graphic, coverage.mask, coverage.rectMask, coverage.image}) Dump(c, ui);
    for (auto* c : {coverage.rectClass, transform, component, behaviour, material}) Dump(c, Core);
    for (auto* c : {canvas, renderer}) Dump(c, module);
    coverage.graphic = Exact(base, "get_Graphic", false, "UnityEngine.UI.Graphic", {});
    coverage.rectTransform = Exact(graphic, "get_rectTransform", false, "UnityEngine.RectTransform", {});
    coverage.canvas = Exact(graphic, "get_canvas", false, "UnityEngine.Canvas", {});
    coverage.renderer = Exact(graphic, "get_canvasRenderer", false, "UnityEngine.CanvasRenderer", {});
    coverage.color = Exact(graphic, "get_color", false, "UnityEngine.Color", {});
    coverage.rootCanvas = Exact(canvas, "get_rootCanvas", false, "UnityEngine.Canvas", {});
    coverage.rect = Exact(coverage.rectClass, "get_rect", false, "UnityEngine.Rect", {});
    coverage.toWorld = Exact(transform, "TransformPoint", false, "UnityEngine.Vector3", {"UnityEngine.Vector3"});
    coverage.fromWorld = Exact(transform, "InverseTransformPoint", false, "UnityEngine.Vector3", {"UnityEngine.Vector3"});
    coverage.component = Exact(component, "GetComponent", false, "UnityEngine.Component", {"System.Type"});
    coverage.enabled = Exact(behaviour, "get_isActiveAndEnabled", false, "System.Boolean", {});
    coverage.material = Exact(renderer, "GetMaterial", false, "UnityEngine.Material", {"System.Int32"});
    coverage.materialCount = Exact(renderer, "get_materialCount", false, "System.Int32", {});
    coverage.clipping = Exact(renderer, "get_hasRectClipping", false, "System.Boolean", {});
    coverage.cull = Exact(renderer, "get_cull", false, "System.Boolean", {});
    coverage.mainTexture = Exact(material, "get_mainTexture", false, "UnityEngine.Texture", {});
    coverage.hasProperty = Exact(material, "HasProperty", false, "System.Boolean", {"System.String"});
    coverage.getFloat = Exact(material, "GetFloat", false, "System.Single", {"System.String"});
    coverage.imageTexture = Exact(coverage.image, "get_mainTexture", false, "UnityEngine.Texture", {});
    coverage.imageType = Exact(coverage.image, "get_type", false, "UnityEngine.UI.Image.Type", {});
    if (coverage.color && Size(coverage.color->return_type) != 16) coverage.color = nullptr;
    coverage.ready = coverage.blur && coverage.rectClass && coverage.graphic && coverage.rectTransform &&
        coverage.canvas && coverage.rootCanvas && coverage.rect && coverage.toWorld && coverage.fromWorld &&
        coverage.component && coverage.enabled && coverage.renderer && coverage.material && coverage.materialCount &&
        coverage.clipping && coverage.cull && coverage.mask && coverage.rectMask &&
        Size(coverage.rect->return_type) == sizeof(CoverageRect) &&
        Size(coverage.toWorld->return_type) == sizeof(CoveragePoint) &&
        Size(coverage.toWorld->args[0]->pType) == sizeof(CoveragePoint) &&
        Size(coverage.fromWorld->return_type) == sizeof(CoveragePoint) &&
        Size(coverage.fromWorld->args[0]->pType) == sizeof(CoveragePoint);
    Log("BLUR_COVERAGE_READY enabled=" + std::to_string(coverage.ready) + " mutation=none bounds=not-effective-mask");
}
template<class T> bool CoverageValue(Method* m, void* self, void** args, T& value) {
    void* box = nullptr;
    return Invoke(m, self, args, &box) && Read(Unbox(box), &value, sizeof(value));
}
void SweepBlurCoverage() {
    if (!coverage.ready) return;
    unsigned count = 0;
    for (void* blur : Objects(coverage.blur)) {
        if (!Alive(blur) || !Value<bool>(coverage.enabled, blur)) continue;
        void* graphic = Object(coverage.graphic, blur);
        if (!Alive(graphic) || !Value<bool>(coverage.enabled, graphic)) continue;
        if (count >= 256) { Log("BLUR_COVERAGE_LIMIT active=256 remaining=unobserved"); break; }
        void* rect = Object(coverage.rectTransform, graphic);
        void* canvas = Object(coverage.canvas, graphic);
        void* root = Alive(canvas) ? Object(coverage.rootCanvas, canvas) : nullptr;
        void* rootTransform = Alive(root) ? Object(api.transform, root) : nullptr;
        if (!Alive(rect) || !Alive(rootTransform) || !UnityResolve::Invoke<bool>("il2cpp_class_is_assignable_from",
            coverage.rectClass->address, Klass(rootTransform))) continue;
        CoverageRect r{}, rr{};
        if (!CoverageValue(coverage.rect, rect, nullptr, r) || !CoverageValue(coverage.rect, rootTransform, nullptr, rr) ||
            !std::isfinite(rr.w) || !std::isfinite(rr.h) || rr.w <= 0 || rr.h <= 0) continue;
        std::ostringstream s; s.imbue(std::locale::classic());
        s << "BLUR_COVERAGE object=" << Ptr(blur) << " graphic=" << Ptr(graphic) << " path=" << Quote(Path(blur))
          << " canvas=" << Ptr(canvas) << " root=" << Ptr(root) << " armed=" << transparencyArmed
          << " rect=" << r.x << ',' << r.y << ',' << r.w << ',' << r.h
          << " rootRect=" << rr.x << ',' << rr.y << ',' << rr.w << ',' << rr.h << " rootCorners=";
        bool valid = true;
        for (unsigned i = 0; i < 4; ++i) {
            CoveragePoint p{r.x + ((i & 1) ? r.w : 0), r.y + ((i & 2) ? r.h : 0), 0}, world{}, local{};
            void* in[]{&p}; void* out[]{&world};
            if (!CoverageValue(coverage.toWorld, rect, in, world) ||
                !CoverageValue(coverage.fromWorld, rootTransform, out, local) ||
                !std::isfinite(local.x) || !std::isfinite(local.y)) { valid = false; break; }
            s << (i ? ";" : "") << local.x << ',' << local.y << ',' << local.z;
        }
        s << " cornersValid=" << valid;
        if (coverage.color) {
            std::array<float, 4> rgba{};
            if (CoverageValue(coverage.color, graphic, nullptr, rgba))
                s << " color=" << rgba[0] << ',' << rgba[1] << ',' << rgba[2] << ',' << rgba[3];
        }
        if (coverage.image && coverage.imageTexture && UnityResolve::Invoke<bool>("il2cpp_class_is_assignable_from",
            coverage.image->address, Klass(graphic))) {
            void* texture = Object(coverage.imageTexture, graphic);
            s << " imageTexture=" << Ptr(texture) << ':' << Quote(Name(texture))
              << " imageType=" << Value<int>(coverage.imageType, graphic, -1);
        }
        unsigned depth = 0;
        for (void* tr = rect; Alive(tr) && depth < 64; tr = Object(api.parent, tr), ++depth) {
            for (auto* klass : {coverage.mask, coverage.rectMask}) {
                void* type = klass->GetType(); void* args[]{type}; void* mask = nullptr;
                if (type && Invoke(coverage.component, tr, args, &mask) && Alive(mask) && Value<bool>(coverage.enabled, mask)) {
                    s << " mask" << depth << '=' << klass->name << ':' << Ptr(mask);
                    // Persist mask geometry too. Stencil sprite alpha and soft
                    // clipping still require separate evidence; do not infer
                    // an effective visible region from these rectangles.
                    CoverageRect mr{};
                    if (UnityResolve::Invoke<bool>("il2cpp_class_is_assignable_from", coverage.rectClass->address, Klass(tr)) &&
                        CoverageValue(coverage.rect, tr, nullptr, mr)) {
                        s << " maskCorners" << depth << '=';
                        for (unsigned j = 0; j < 4; ++j) {
                            CoveragePoint p{mr.x + ((j & 1) ? mr.w : 0), mr.y + ((j & 2) ? mr.h : 0), 0}, w{}, local{};
                            void* a[]{&p}; void* b[]{&w};
                            if (!CoverageValue(coverage.toWorld, tr, a, w) || !CoverageValue(coverage.fromWorld, rootTransform, b, local)) {
                                s << "unknown"; break;
                            }
                            s << (j ? ";" : "") << local.x << ',' << local.y;
                        }
                    }
                }
            }
        }
        s << " ancestorLimit=" << (depth == 64);
        void* renderer = Object(coverage.renderer, graphic);
        if (Alive(renderer)) {
            s << " rectClipping=" << Value<bool>(coverage.clipping, renderer) << " culled=" << Value<bool>(coverage.cull, renderer);
            const int materials = Value<int>(coverage.materialCount, renderer);
            s << " materialCount=" << materials;
            for (int i = 0; i < (std::min)(materials, 8); ++i) {
                void* args[]{&i}; void* mat = nullptr;
                if (!Invoke(coverage.material, renderer, args, &mat) || !Alive(mat)) continue;
                s << " material" << i << '=' << Ptr(mat) << ':' << Quote(Name(mat))
                  << " shader" << i << '=' << Quote(Name(Object(api.shader, mat)));
                if (coverage.mainTexture) s << " mainTexture" << i << '=' << Ptr(Object(coverage.mainTexture, mat));
                if (coverage.hasProperty && coverage.getFloat) for (const char* name :
                    {"_BlendSrcFactor", "_BlendDstFactor", "_Stencil", "_StencilComp", "_StencilReadMask", "_ColorMask"}) {
                    auto* str = UnityResolve::UnityType::String::New(name); void* prop[]{str}; bool has = false; float v = 0;
                    if (CoverageValue(coverage.hasProperty, mat, prop, has) && has && CoverageValue(coverage.getFloat, mat, prop, v))
                        s << ' ' << name << i << '=' << v;
                }
            }
        }
        Log(s.str()); ++count;
    }
    Log("BLUR_COVERAGE_END count=" + std::to_string(count));
}
void Sweep() {
    for (auto it = nodes.begin(); it != nodes.end();) {
        void* t = HandleTarget(it->second.weak);
        if (!t || !Alive(t)) {
            Log("RETIRE node=" + std::to_string(it->second.serial) + " reason=dead");
            FreeHandle(it->second.weak); it = nodes.erase(it);
        } else ++it;
    }
    uiCameras.clear();
    for (void* controller : Objects(api.uiController)) if (Alive(controller)) {
        void* c = Object(api.uiCamera, controller); if (Alive(c)) uiCameras.insert(c);
    }
    unsigned activeCount = 0; std::unordered_set<void*> seen;
    for (void* raw : Objects(api.rawClass)) {
        if (!Alive(raw)) continue;
        void* go = Object(api.gameObject, raw);
        if (!go || !Value<bool>(api.active, go)) continue;
        void* t = Object(api.rawTexture, raw); if (!Alive(t)) continue;
        const auto node = NodeId(t, "active-RawImage-unknown-until-linked");
        seen.insert(raw); ++activeCount;
        if (bindings[raw] == node) continue;
        bindings[raw] = node;
        if (EventAllowed()) Log("BIND kind=RawImage object=" + Ptr(raw) + " node=" + std::to_string(node) +
            " path=" + Quote(Path(raw)) + " material=" + Quote(Name(Object(api.graphicMaterial, raw))));
    }
    for (auto it = bindings.begin(); it != bindings.end();) {
        if (!seen.count(it->first)) it = bindings.erase(it); else ++it;
    }
    Log("SWEEP activeRawImages=" + std::to_string(activeCount) + " uiCameras=" + std::to_string(uiCameras.size()) +
        " nodes=" + std::to_string(nodes.size()));
    SweepBlurCoverage();
}

void RecordIdentity(void* pass, const char* field, void* texture, const char* origin) {
    if (!texture || !Alive(texture)) return;
    const auto node = NodeId(texture, origin);
    Log("RESOURCE passObject=" + Ptr(pass) + " field=" + std::string(field) +
        " index=0 node=" + std::to_string(node));
    ObservePresentSource(texture, node, field);
}
void PassResources(void* pass) {
    auto* k = Klass(pass);
    auto found = api.passFields.find(k);
    if (found == api.passFields.end()) {
        const auto name = ClassName(pass);
        const auto dot = name.find_last_of('.');
        Class* c = dot != std::string::npos ? Find(
            name.rfind("Campus.", 0) == 0 ? Campus : Urp,
            name.substr(0, dot).c_str(), name.substr(dot + 1).c_str()) : nullptr;
        std::vector<Field*> fields;
        if (c && c->address == k) for (auto* f : c->fields) if (f &&
            !(FieldFlags(f->address) & GripFieldStatic) &&
            (Type(f->type) == "UnityEngine.Rendering.RTHandle" ||
             Type(f->type) == "UnityEngine.Rendering.RTHandle[]" ||
             Type(f->type) == "UnityEngine.Texture" || Type(f->type) == "UnityEngine.Material"))
            fields.push_back(f);
        found = api.passFields.emplace(k, std::move(fields)).first;
    }
    for (auto* f : found->second) {
        void* value = FieldObject(pass, f); if (!value) continue;
        if (Type(f->type) == "UnityEngine.Material") {
            if (!Alive(value)) continue;
            Log("MATERIAL passObject=" + Ptr(pass) + " field=" + f->name + " material=" + Ptr(value) +
                " name=" + Quote(Name(value)) + " shader=" + Quote(Name(Object(api.shader, value))));
            continue;
        }
        std::vector<void*> textures;
        if (Type(f->type) == "UnityEngine.Rendering.RTHandle[]") {
            auto handles = static_cast<UnityResolve::UnityType::Array<void*>*>(value)->ToVector();
            if (handles.size() > 8) {
                Log("RESOURCE_LIMIT field=" + f->name + " count=" + std::to_string(handles.size()));
                handles.resize(8);
            }
            for (auto* h : handles) if (h) textures.push_back(Object(api.rtGet, h));
        } else textures.push_back(Type(f->type) == "UnityEngine.Rendering.RTHandle"
            ? Object(api.rtGet, value) : value);
        for (std::size_t i = 0; i < textures.size(); ++i) {
            void* t = textures[i]; if (!t || !Alive(t)) continue;
            const auto node = NodeId(t, "pass-field");
            Log("RESOURCE passObject=" + Ptr(pass) + " field=" + f->name +
                " index=" + std::to_string(i) + " node=" + std::to_string(node));
            ObservePresentSource(t, node, f->name + "[" + std::to_string(i) + "]");
        }
    }
}

} // namespace

void InstallGripTransparencyTrace() noexcept {
    if (!GakumasLocal::Config::vrDiagnosticsStartupEnabled) return;
    try {
        Guard guard;
        auto* texture = Find(Core, "UnityEngine", "Texture");
        api.textureClass = texture;
        api.rtClass = Find(Core, "UnityEngine", "RenderTexture");
        auto* texture2D = Find(Core, "UnityEngine", "Texture2D");
        auto* commandBuffer = Find(Core, "UnityEngine.Rendering", "CommandBuffer");
        auto* rti = Find(Core, "UnityEngine.Rendering", "RenderTargetIdentifier");
        auto* scriptableRenderer = Find(Urp, "UnityEngine.Rendering.Universal", "ScriptableRenderer");
        auto* object = Find(Core, "UnityEngine", "Object");
        auto* component = Find(Core, "UnityEngine", "Component");
        auto* capture = Find("Assembly-CSharp.dll", "Campus.Common.Utility", "CaptureUtility");
        auto* vlImage = Find(Vl, "VL.Rendering", "VLSRPTargetImage");
        auto* copyManager = Find(Campus, "Campus.Common", "UIRenderResultCopyManager");
        api.uiController = Find(Campus, "Campus.Common", "UICameraController");
        api.rawClass = Find("UnityEngine.UI.dll", "UnityEngine.UI", "RawImage");
        for (auto* c : {capture, vlImage, copyManager, api.uiController})
            Dump(c, c == capture ? "Assembly-CSharp.dll" : c == vlImage ? Vl : Campus);
        for (auto* c : {texture, api.rtClass, texture2D, commandBuffer, rti}) Dump(c, Core);
        Dump(api.rawClass, "UnityEngine.UI.dll");
        for (const char* name : {"UIRenderPass", "UIRenderResultCopyPass", "UIRenderTargetBlurPass"})
            Dump(Find(Campus, "Campus.Common.UIRenderer", name), Campus);
        Dump(Find(Urp, "UnityEngine.Rendering.Universal.Internal", "FinalBlitPass"), Urp);
        Dump(scriptableRenderer, Urp);
        Dump(Find(Vl, "VL.Rendering", "VLSRPCameraController"), Vl);
        Dump(Find("UnityEngine.UIModule.dll", "UnityEngine", "CanvasRenderer"), "UnityEngine.UIModule.dll");
        api.alive = Exact(object, "IsNativeObjectAlive", true, "System.Boolean", {"UnityEngine.Object"});
        api.name = Exact(object, "get_name", false, "System.String", {});
        api.id = Exact(object, "GetInstanceID", false, "System.Int32", {});
        api.native = Exact(texture, "GetNativeTexturePtr", false, "System.IntPtr", {});
        api.transform = Exact(component, "get_transform", false, "UnityEngine.Transform", {});
        api.gameObject = Exact(component, "get_gameObject", false, "UnityEngine.GameObject", {});
        api.parent = Exact(Find(Core, "UnityEngine", "Transform"), "get_parent", false, "UnityEngine.Transform", {});
        api.active = Exact(Find(Core, "UnityEngine", "GameObject"), "get_activeInHierarchy", false, "System.Boolean", {});
        api.findObjects = Exact(Find(Core, "UnityEngine", "Resources"), "FindObjectsOfTypeAll", true,
                                "UnityEngine.Object[]", {"System.Type"});
        api.rawTexture = Exact(api.rawClass, "get_texture", false, "UnityEngine.Texture", {});
        api.graphicMaterial = Exact(Find("UnityEngine.UI.dll", "UnityEngine.UI", "Graphic"),
                                    "get_material", false, "UnityEngine.Material", {});
        api.rtGet = Exact(Find("Unity.RenderPipelines.Core.Runtime.dll", "UnityEngine.Rendering", "RTHandle"),
                          "get_rt", false, "UnityEngine.RenderTexture", {});
        api.rtFormat = Exact(api.rtClass, "get_graphicsFormat", false, GF, {});
        api.texFormat = Exact(texture, "get_graphicsFormat", false, GF, {});
        api.uiCamera = Exact(api.uiController, "get_UICamera", false, "UnityEngine.Camera", {});
        api.cameraTarget = Exact(Find(Core, "UnityEngine", "Camera"),
                                "get_targetTexture", false, "UnityEngine.RenderTexture", {});
        api.colorHandle = Exact(scriptableRenderer, "get_cameraColorTargetHandle", false,
                               "UnityEngine.Rendering.RTHandle", {});
        api.shader = Exact(Find(Core, "UnityEngine", "Material"), "get_shader", false, "UnityEngine.Shader", {});
        auto* copy = Exact(capture, "Copy", true, "System.Void",
                           {"UnityEngine.Texture", "UnityEngine.Texture2D", "UnityEngine.RectInt"});
        auto* ca = Exact(capture, "CopyAsync", true, UT,
                         {"UnityEngine.Texture", "UnityEngine.Texture2D", "UnityEngine.RectInt", CT});
        auto* ba = Exact(capture, "BlitAsync", true, UT,
                         {"UnityEngine.Texture", "UnityEngine.Texture2D", CT});
        if (ca && (Size(ca->return_type) != 16 || Size(ca->args[2]->pType) != 16 || Size(ca->args[3]->pType) != 8))
            ca = nullptr;
        if (ba && (Size(ba->return_type) != 16 || Size(ba->args[2]->pType) != 8)) ba = nullptr;
        if (copy && Size(copy->args[2]->pType) != 16) copy = nullptr;
        auto* create = Exact(capture, "CreateTexture", true, "UnityEngine.Texture2D", {"UnityEngine.Vector2Int"});
        if (create && Size(create->args[0]->pType) != 8) create = nullptr;
        Hook(ca, &CopyAsyncHook, &copyAsyncOrig, "GripTrace CaptureUtility.CopyAsync");
        Hook(ba, &BlitAsyncHook, &blitAsyncOrig, "GripTrace CaptureUtility.BlitAsync");
        Hook(copy, &CopyHook, &copyOrig, "GripTrace CaptureUtility.Copy");
        Hook(create, &CreateHook, &createOrig, "GripTrace CaptureUtility.CreateTexture");
        Hook(Exact(vlImage, "UpdateTexture", false, "System.Void", {"UnityEngine.Texture"}),
             &UpdateHook, &updateOrig, "GripTrace VLSRPTargetImage.UpdateTexture");
        Hook(Exact(copyManager, "Add", false, "System.Void", {"UnityEngine.Rendering.RTHandle"}),
             &CopyAddHook, &copyAddOrig, "GripTrace UIRenderResultCopyManager.Add");
        InitBlurCoverage();
        ready.store(api.alive && api.id && api.name && api.native);
        Log("GPU_DISABLED reason=execute-copy-deferred");
        Log("NATIVE_READY site=captured-Present phase=before-post-Present-clear sourceCap=24 batchCap=2 mutation=private-staging-only");
        Log("READY enabled=" + std::to_string(ready.load()) +
            " gpu=present-only sweep=1 fields=1 getters=1 handle=pointer reason=execute-copy-deferred mutation=none");
    } catch (...) { Log("INIT_FAIL"); }
}

void GripTraceBeginCamera(void* camera, const UnityStereoRenderer& renderer) noexcept {
    if (!Enabled()) return;
    try {
        if (!ownerThread.load()) ownerThread.store(GetCurrentThreadId());
        if (GetCurrentThreadId() != ownerThread) return;
        Guard guard;
        if (currentCamera == camera) return;
        if (!presentSources.empty()) {
            Log("NATIVE_SKIP epoch=" + std::to_string(cameraSerial) + " reason=camera-ended-without-publish");
            presentSources.clear();
        }
        currentCamera = camera; eyeCamera = renderer.IsEyeCamera(camera);
        transparencyArmed = renderer.GripTransparencyArmed();
        selectedCamera = false; sampleCamera = false; currentPass = nullptr; passOrder = 0;
        if (eyeCamera) return;
        const auto now = Now();
        const bool setting = GakumasLocal::Config::vrGripPanelTransparent;
        const bool changed = setting != lastSetting;
        if (changed) { lastSetting = setting; baselinePending = true; nextCapture = 0; nextSweep = 0; }
        if (refreshRequested.exchange(false) && now - lastCapture >= 1000) { nextCapture = 0; nextSweep = 0; }
        if (now >= nextSweep) { nextSweep = now + 5000; Sweep(); }
        selectedCamera = uiCameras.count(camera) != 0;
        ++cameraSerial;
        if (selectedCamera && (setting || baselinePending) && now >= nextCapture) {
            baselinePending = false;
            nextCapture = now + 5000; lastCapture = now;
            sampleCamera = true;
            Log("EPOCH camera=" + Ptr(camera) + " serial=" + std::to_string(cameraSerial) +
                " reason=periodic-or-change armed=" + std::to_string(transparencyArmed));
            for (const auto& entry : nodes) if (entry.second.vlSource) {
                void* t = HandleTarget(entry.second.weak);
                if (Alive(t) && Value<int>(api.id, t) == entry.second.instance)
                    ObservePresentSource(t, entry.second.serial, "VLSRP:" + Name(t));
            }
        }
    } catch (...) { Log("CALL_FAIL stage=begin-camera"); }
}
void GripTraceEndCamera(void* camera) noexcept {
    if (!Observe()) return;
    if (camera == currentCamera && sampleCamera) {
        try {
            const auto count = presentSources.size();
            const bool queued = presentProbe.Queue(std::move(presentSources), cameraSerial, transparencyArmed);
            presentSources.clear();
            Log(std::string(queued ? "NATIVE_QUEUED" : "NATIVE_SKIP") + " epoch=" +
                std::to_string(cameraSerial) + " sources=" + std::to_string(count) +
                (queued ? " phase=next-captured-Present" : " reason=queue-cap"));
            Log("EPOCH_END serial=" + std::to_string(cameraSerial));
        }
        catch (...) {}
    }
    if (camera == currentCamera) { currentCamera = nullptr; currentPass = nullptr; sampleCamera = false; }
}
void GripTracePass(void* renderer, void* pass, void* context, int event, bool after) noexcept {
    (void)context;
    if (!Observe() || eyeCamera || !pass) return;
    try {
        Guard guard;
        const auto type = ClassName(pass);
        const bool campusUi = type.rfind("Campus.Common.UIRenderer.", 0) == 0;
        if (campusUi) {
            selectedCamera = true;
            if (currentCamera) uiCameras.insert(currentCamera);
        }
        if (!selectedCamera) return;
        if (!after) { currentPass = pass; currentEvent = event; ++passOrder; }
        if (!sampleCamera) return;
        Log("PASS cameraSerial=" + std::to_string(cameraSerial) + " camera=" + Ptr(currentCamera) +
            " pass=" + Quote(type) + " passObject=" + Ptr(pass) + " event=" + std::to_string(event) +
            " order=" + std::to_string(passOrder) + " phase=" + (after ? "after" : "before"));
        if (!campusUi && type.find("FinalBlitPass") == std::string::npos) return;
        void* target = Alive(currentCamera) ? Object(api.cameraTarget, currentCamera) : nullptr;
        RecordIdentity(pass, "camera-target", target, "camera-target");
        void* handle = Object(api.colorHandle, renderer);
        RecordIdentity(pass, "renderer-color", handle ? Object(api.rtGet, handle) : nullptr, "renderer-color");
        PassResources(pass);
    } catch (...) { Log("CALL_FAIL stage=pass"); }
}
void GripTraceCanvasTexture(void* canvas, void* texture) noexcept {
    if (!Observe() || !texture) return;
    try {
        Guard guard;
        if (!nodes.count(texture) && !IsRT(texture)) return;
        const auto node = NodeId(texture, "CanvasRenderer.SetTexture");
        if (canvasBindings[canvas] == node) return;
        if (canvasBindings.size() > 8192) { canvasBindings.clear(); Log("OVERFLOW lane=canvas-bindings reset=1"); }
        canvasBindings[canvas] = node;
        if (EventAllowed()) Log("BIND kind=CanvasRenderer object=" + Ptr(canvas) +
            " node=" + std::to_string(node) + " path=" + Quote(Path(canvas)));
        refreshRequested.store(true);
    } catch (...) { Log("CALL_FAIL stage=bind"); }
}
void GripTraceFullscreen(void* material, int shaderPass) noexcept {
    if (!Observe() || !selectedCamera || !sampleCamera || !material) return;
    try { Guard guard; Log("DRAW passObject=" + Ptr(currentPass) + " event=" + std::to_string(currentEvent) +
        " material=" + Ptr(material) + " name=" + Quote(Name(material)) +
        " shader=" + Quote(Name(Object(api.shader, material))) +
        " shaderPass=" + std::to_string(shaderPass)); } catch (...) { Log("CALL_FAIL stage=draw"); }
}
void GripTraceUiPolicy(void* pass, int index, bool fb, bool clear, const float* color) noexcept {
    if (!Observe() || !sampleCamera) return;
    try { Guard guard; std::ostringstream s; s << "POLICY passObject=" << Ptr(pass) << " index=" << index <<
        " authoredFb=" << fb << " authoredClear=" << clear <<
        " rgba=" << color[0] << ',' << color[1] << ',' << color[2] << ',' << color[3]; Log(s.str());
    } catch (...) { Log("CALL_FAIL stage=policy"); }
}
void GripTracePresent(ID3D11DeviceContext* context, ID3D11Texture2D* backbuffer) noexcept {
    if (!Enabled()) return;
    try { presentProbe.Pump(context, backbuffer); }
    catch (...) { Log("NATIVE_FAIL stage=present"); }
}
void PumpGripTraceOutput() noexcept {
    if (!Enabled()) return;
    try {
        for (const auto& r : presentProbe.TakeCompleted()) {
            std::ostringstream s;
            s.imbue(std::locale::classic()); // Hex bytes must not acquire locale grouping commas.
            s << "NATIVE_PIXELS epoch=" << r.epoch << " present=" << r.present << " node=" << r.node
              << " armed=" << r.armed << " label=" << Quote(r.label) << " native=0x" << std::hex << r.native
              << " hr=0x" << static_cast<std::uint32_t>(r.error) << std::dec << " fmt=" << r.format
              << " width=" << r.width << " height=" << r.height << " tile=" << r.tileWidth << 'x' << r.tileHeight
              << " latency=" << r.latency << " phase=present-before-clear";
            if (SUCCEEDED(r.error)) {
                static constexpr char digits[] = "0123456789abcdef";
                const auto n = r.tileWidth * r.tileHeight * 16;
                unsigned zero = 0, opaque = 0, nonzeroRgbAtZeroAlpha = 0;
                for (unsigned i = 0; i < n; i += 4) {
                    zero += r.bytes[i + 3] == 0; opaque += r.bytes[i + 3] == 255;
                    nonzeroRgbAtZeroAlpha += r.bytes[i + 3] == 0 &&
                        (r.bytes[i] || r.bytes[i + 1] || r.bytes[i + 2]);
                }
                s << " a0=" << zero << " a255=" << opaque << " rgbAtA0=" << nonzeroRgbAtZeroAlpha << " raw=";
                for (unsigned i = 0; i < n; ++i) s << digits[r.bytes[i] >> 4] << digits[r.bytes[i] & 15];
            }
            Log(s.str());
        }
    } catch (...) { Log("NATIVE_FAIL stage=output"); }
}
} // namespace gakumas::vr
