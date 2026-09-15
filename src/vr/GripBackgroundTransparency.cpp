#include "GripBackgroundTransparency.hpp"
#include "BackgroundDiscoverySchedule.hpp"
#include "VrRuntime.hpp"
#include "config/VrifyConfig.hpp"
#include "deps/UnityResolve/UnityResolve.hpp"
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>
namespace gakumas::vr {
namespace {
using Class = UnityResolve::Class;
using Method = UnityResolve::Method;
constexpr const char* Core = "UnityEngine.CoreModule.dll";
std::uint64_t Now() { return GetTickCount64(); }
void Log(const std::string& s) { (void)WriteVrLog("[VR][grip] GRIP_BACKGROUND_" + s); }
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
int Size(UnityResolve::Type* t) {
    auto* klass = t ? UnityResolve::Invoke<void*>("il2cpp_class_from_type", t->address) : nullptr;
    std::uint32_t alignment = 0;
    return klass ? UnityResolve::Invoke<int>("il2cpp_class_value_size", klass, &alignment) : -1;
}

struct Api {
    Method *alive{}, *name{}, *transform{}, *parent{}, *findObjects{};
    Class *textureClass{}, *rtClass{};
    std::unordered_map<void*, std::pair<Method*, Method*>> dimensions;
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
void FreeHandle(Il2CppGCHandle& handle) {
    if (!handle) return;
    UnityResolve::Invoke<void>("il2cpp_gchandle_free", std::exchange(handle, nullptr));
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

struct BackgroundApi {
    Class* image{};
    Method *imageTexture{}, *color{}, *setColor{};
} coverage;
template<class T> bool CoverageValue(Method* m, void* self, void** args, T& value) {
    void* box = nullptr;
    return Invoke(m, self, args, &box) && Read(Unbox(box), &value, sizeof(value));
}
// dev.446 correlated this precise Image with the white draw's native t0.
// dev.447 hardware accepted this specific reversible background fix.
struct BackgroundColor { float r, g, b, a; };
struct BackgroundState {
    Il2CppGCHandle root{};
    void* object{};
    BackgroundColor saved{};
    bool written{};
    unsigned fights{};
    bool accepted{};
    std::uint64_t signatureRetry{};
    bool signatureLogged{};
} backgroundState;
constexpr const char* BackgroundPath = "WindowSystemRoot/WindowRoot/BackgroundCanvas/BackgroundImage";
bool InitBackgroundRuntime() {
    // Only the already-proven getters/setter needed by the accepted fix. No
    // diagnostic dumps, hooks, census, shader tagging or GPU capture startup.
    auto* object = Find(Core, "UnityEngine", "Object");
    auto* component = Find(Core, "UnityEngine", "Component");
    auto* graphic = Find("UnityEngine.UI.dll", "UnityEngine.UI", "Graphic");
    coverage.image = Find("UnityEngine.UI.dll", "UnityEngine.UI", "Image");
    api.textureClass = Find(Core, "UnityEngine", "Texture");
    api.rtClass = Find(Core, "UnityEngine", "RenderTexture");
    api.alive = Exact(object, "IsNativeObjectAlive", true, "System.Boolean", {"UnityEngine.Object"});
    api.name = Exact(object, "get_name", false, "System.String", {});
    api.transform = Exact(component, "get_transform", false, "UnityEngine.Transform", {});
    api.parent = Exact(Find(Core, "UnityEngine", "Transform"), "get_parent", false, "UnityEngine.Transform", {});
    api.findObjects = Exact(Find(Core, "UnityEngine", "Resources"), "FindObjectsOfTypeAll", true,
        "UnityEngine.Object[]", {"System.Type"});
    coverage.imageTexture = Exact(coverage.image, "get_mainTexture", false, "UnityEngine.Texture", {});
    coverage.color = Exact(graphic, "get_color", false, "UnityEngine.Color", {});
    coverage.setColor = Exact(graphic, "set_color", false, "System.Void", {"UnityEngine.Color"});
    return api.alive && api.name && api.transform && api.parent && api.findObjects &&
        api.textureClass && api.rtClass && coverage.image && coverage.imageTexture && coverage.color && coverage.setColor &&
        Size(coverage.color->return_type) == 16 && Size(coverage.setColor->args[0]->pType) == 16;
}
void DiscoverBackgroundState() {
    if (backgroundState.root || !coverage.setColor || !coverage.color) return;
    void* found = nullptr;
    // Snapshot roots every candidate until the selected object gets its own root.
    auto snapshot = Objects(coverage.image);
    for (void* graphic : snapshot) {
        if (!Alive(graphic) || Name(graphic) != "BackgroundImage" ||
            !UnityResolve::Invoke<bool>("il2cpp_class_is_assignable_from", coverage.image->address, Klass(graphic)) ||
            Path(graphic) != BackgroundPath) continue;
        if (found) { Log("BACKGROUND_SKIP reason=ambiguous-path"); return; }
        found = graphic;
    }
    if (!found) return;
    auto root = UnityResolve::Invoke<Il2CppGCHandle>("il2cpp_gchandle_new", found, false);
    if (!root) { Log("BACKGROUND_SKIP reason=root-failed"); return; }
    backgroundState = {};
    backgroundState.root = root; backgroundState.object = found;
    Log("BACKGROUND_DISCOVER object=" + Ptr(found) + " path=" + Quote(BackgroundPath));
}
void UpdateBackgroundState(bool armed) {
    auto& trial = backgroundState;
    if (!trial.root) return;
    // The strong GC handle does not make the native Unity object immortal.
    if (!Alive(trial.object)) {
        Log("BACKGROUND_RETIRE reason=unity-object-dead");
        FreeHandle(trial.root); trial = {}; return;
    }
    if (!armed && !trial.written) return;
    if (!trial.accepted) {
        if (Now() < trial.signatureRetry) return;
        trial.signatureRetry = Now() + 1000;
        BackgroundColor color{};
        void* texture = Object(coverage.imageTexture, trial.object);
        if (!Alive(texture) || Name(texture) != "UnityWhite" || Dimensions(texture) != std::make_pair(4, 4) ||
            !CoverageValue(coverage.color, trial.object, nullptr, color) ||
            color.r != 1 || color.g != 1 || color.b != 1 || color.a != 1) {
            if (!trial.signatureLogged) Log("BACKGROUND_SKIP reason=signature-changed cached=1");
            trial.signatureLogged = true;
            return;
        }
        trial.accepted = true;
    }
    BackgroundColor current{};
    const bool read = CoverageValue(coverage.color, trial.object, nullptr, current);
    if (!armed) {
        // A failed read still leaves us owning the last write. Never save it.
        if (trial.written && (!read || current.a == 0)) {
            void* args[]{&trial.saved};
            if (!Invoke(coverage.setColor, trial.object, args)) { Log("BACKGROUND_RESTORE_FAILED"); return; }
            Log("BACKGROUND_RESTORE object=" + Ptr(trial.object) + " alpha=" + std::to_string(trial.saved.a));
        } else if (trial.written) Log("BACKGROUND_RESTORE_SKIP reason=game-replaced-color");
        // Keep the proven live identity after restoring its color. Re-arming
        // must not wait for another diagnostic census.
        trial.written = false; trial.fights = 0; return;
    }
    if (!read) { Log("BACKGROUND_SKIP reason=color-read-failed"); return; }
    if (trial.written && current.a == 0) return;
    trial.saved = current;
    current.a = 0;
    void* args[]{&current};
    if (!Invoke(coverage.setColor, trial.object, args)) return;
    const bool fight = trial.written;
    trial.written = true;
    if (!fight || ++trial.fights <= 4 || trial.fights % 60 == 0)
        Log(std::string(fight ? "BACKGROUND_FIGHT" : "BACKGROUND_APPLY") + " object=" + Ptr(trial.object) +
            " savedAlpha=" + std::to_string(trial.saved.a) + " count=" + std::to_string(trial.fights));
}
} // namespace
void SyncGripBackgroundTransparency(bool armed) noexcept {
    if (!GakumasLocal::Config::vrRuntimeStartupEnabled) return;
    try {
        static DWORD backgroundOwner = GetCurrentThreadId();
        if (GetCurrentThreadId() != backgroundOwner) return;
        static BackgroundDiscoverySchedule discovery;
        if (!armed && !backgroundState.root) {
            (void)discovery.ShouldSearch(Now(), false, false, false);
            return;
        }
        static const bool runtimeReady = InitBackgroundRuntime();
        if (!runtimeReady) return;
        const bool hadRoot = backgroundState.root != nullptr;
        UpdateBackgroundState(armed);
        const auto now = Now();
        if (discovery.ShouldSearch(now, armed, backgroundState.root != nullptr,
                hadRoot && !backgroundState.root)) {
            DiscoverBackgroundState();
            discovery.Searched(now, backgroundState.root != nullptr);
            UpdateBackgroundState(armed);
        }
    }
    catch (...) { Log("BACKGROUND_FAIL stage=sync"); }
}
} // namespace gakumas::vr
