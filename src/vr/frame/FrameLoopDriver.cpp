#include "FrameLoopDriver.hpp"

#include "../VrRuntime.hpp"
#include "../config/VrifyConfig.hpp"
#include "deps/UnityResolve/UnityResolve.hpp"

#include <initializer_list>

#include <Windows.h>
#include <array>
#include <atomic>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include <locale>
#include <stdexcept>
#include <intrin.h>

namespace gakumas::vr {
namespace {
using Method = UnityResolve::Method;
using Class = UnityResolve::Class;
using Field = UnityResolve::Field;
using SetInternalFn = void (*)(void*);

Method* eventMethod = nullptr;
Method* loopMethod = nullptr;
Method* setLoopMethod = nullptr;
Class* loopClass = nullptr;
std::array<Field*, 5> loopFields{};
bool attempted = false;
HookRegistrar driverRegistrar;
std::atomic<bool> registrarReady{false};
SetInternalFn setInternalOriginal = nullptr;
thread_local bool repairingLoop = false;
unsigned loopWriteDepth = 0;
ULONGLONG nextLoopCheck = 0;

void EnsureLoop();
void IssueEndEvent(int eventId);

bool Enabled() {
    return GakumasLocal::Config::vrRuntimeStartupEnabled;
}

void Log(const std::string& text) {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << "[VR][frame] " << text << " tid=" << GetCurrentThreadId()
      << " qpc=" << qpc.QuadPart;
    (void)WriteVrLog(s.str());
}

std::string Type(UnityResolve::Type* t) {
    return t ? t->name : "<null>";
}

bool Read(const void* p, void* out, std::size_t size) noexcept {
    if (!p) {
        return false;
    }
    __try {
        std::memcpy(out, p, size);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool InvokeRaw(void* info, void* self, void** args, void** result) noexcept {
    using Fn = void* (*)(void*, void*, void**, void**);
    static const auto fn = reinterpret_cast<Fn>(GetProcAddress(
        GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_runtime_invoke"));
    if (!fn || !info) {
        return false;
    }
    void* exception = nullptr;
    __try {
        *result = fn(info, self, args, &exception);
        return exception == nullptr;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool Invoke(Method* m, void* self, void** args, void** result) {
    return m && InvokeRaw(m->address, self, args, result);
}

Method* Exact(
    Class* c,
    const char* name,
    const char* ret,
    std::initializer_list<const char*> args) {
    Method* found = nullptr;
    unsigned count = 0;
    if (c) {
        for (auto* m : c->methods) {
            if (!m || !m->function || !m->address || !m->static_function ||
                m->name != name || Type(m->return_type) != ret ||
                m->args.size() != args.size()) {
                continue;
            }
            bool match = true;
            std::size_t i = 0;
            for (const char* arg : args) {
                const auto* a = m->args[i++];
                if (!a || Type(a->pType) != arg) {
                    match = false;
                }
            }
            if (match) {
                found = m;
                ++count;
            }
        }
    }
    if (count != 1) {
        return nullptr;
    }
    return found;
}

struct Root {
    void* handle = nullptr;
    explicit Root(void* obj) {
        if (obj) {
            handle = UnityResolve::Invoke<void*>("il2cpp_gchandle_new", obj, true);
        }
    }
    ~Root() {
        if (handle) {
            UnityResolve::Invoke<void>("il2cpp_gchandle_free", handle);
        }
    }
    Root(const Root&) = delete;
    Root& operator=(const Root&) = delete;
};

std::string ManagedTypeName(void* type) {
    if (!type) {
        return "<root>";
    }
    auto* klass = UnityResolve::Invoke<void*>("il2cpp_class_from_system_type", type);
    if (!klass) {
        return "<unresolved>";
    }
    const char* ns = UnityResolve::Invoke<const char*>("il2cpp_class_get_namespace", klass);
    const char* name = UnityResolve::Invoke<const char*>("il2cpp_class_get_name", klass);
    return std::string(ns ? ns : "") + "." + (name ? name : "?");
}

bool FieldValue(void* boxed, Field* field, void** value) {
    if (!boxed || !field || field->static_field) {
        return false;
    }
    UnityResolve::Invoke<void>("il2cpp_field_get_value", boxed, field->address, value);
    return true;
}

void __stdcall RenderEvent(int id) noexcept {
    if (!Enabled()) {
        return;
    }
    try {
        VrRuntime::Instance().OnGraphicsEndEvent(id);
    } catch (...) {
    }
}

void __cdecl InitEntry() noexcept {
    if (!Enabled()) {
        return;
    }
    try {
        VrRuntime::Instance().OnUnityWaitPhase();
    } catch (...) {
    }
}

void __cdecl Tail() noexcept {
    if (!Enabled()) {
        return;
    }
    try {
        const int eventId = VrRuntime::Instance().OnUnitySubmitPhase();
        if (eventId > 0) {
            IssueEndEvent(eventId);
        }
    } catch (...) {
    }
}

using LoopCallback = void(__cdecl*)();
std::array<LoopCallback, 2> slots{InitEntry, Tail};
using LoopValue = std::array<void*, 5>;
static_assert(sizeof(LoopValue) == 40);

bool OwnSlot(void* p) {
    for (auto& slot : slots) {
        if (p == &slot) {
            return true;
        }
    }
    return false;
}

LoopValue Value(void* boxed) {
    LoopValue value{};
    for (std::size_t i = 0; i < value.size(); ++i) {
        FieldValue(boxed, loopFields[i], &value[i]);
    }
    return value;
}

void StoreArrayElement(void* array, unsigned index, const LoopValue& value) {
    auto* data = reinterpret_cast<unsigned char*>(
        static_cast<UnityResolve::UnityType::Array<unsigned char>*>(array)->GetData());
    auto** dst = reinterpret_cast<void**>(data + index * sizeof(LoopValue));
    for (int i = 0; i < 3; ++i) {
        UnityResolve::Invoke<void>("il2cpp_gc_wbarrier_set_field", array, &dst[i], value[i]);
    }
    dst[3] = value[3];
    dst[4] = value[4];
}

unsigned CountNodes(void* boxed, unsigned& own, unsigned depth = 0) {
    if (!boxed || depth > 16) {
        return 0;
    }
    Root root(boxed);
    const auto value = Value(boxed);
    if (OwnSlot(value[3])) {
        ++own;
    }
    unsigned total = 1;
    auto* array = static_cast<UnityResolve::UnityType::Array<unsigned char>*>(value[1]);
    if (!array) {
        return total;
    }
    const auto count = UnityResolve::Invoke<std::uintptr_t>("il2cpp_array_length", array);
    if (count > 512) {
        throw std::runtime_error("count-nodes array size=" + std::to_string(count));
    }
    for (std::uintptr_t i = 0; i < count; ++i) {
        total += CountNodes(
            UnityResolve::Invoke<void*>(
                "il2cpp_value_box",
                loopClass->address,
                array->GetData() + i * sizeof(LoopValue)),
            own,
            depth + 1);
    }
    return total;
}

struct ShapeNode {
    std::array<void*, 4> identity{};
    unsigned children = 0;
    bool operator==(const ShapeNode&) const = default;
};

void Shape(void* boxed, std::vector<ShapeNode>& shape, unsigned depth = 0) {
    if (!boxed || depth > 16 || shape.size() > 512) {
        throw std::runtime_error("loop shape");
    }
    Root root(boxed);
    const auto value = Value(boxed);
    if (OwnSlot(value[3])) {
        return;
    }
    const auto row = shape.size();
    shape.push_back({{value[0], value[2], value[3], value[4]}, 0});
    auto* array = static_cast<UnityResolve::UnityType::Array<unsigned char>*>(value[1]);
    if (!array) {
        return;
    }
    const auto count = UnityResolve::Invoke<std::uintptr_t>("il2cpp_array_length", array);
    if (count > 512) {
        throw std::runtime_error("shape array size=" + std::to_string(count));
    }
    for (std::uintptr_t i = 0; i < count; ++i) {
        auto* child = UnityResolve::Invoke<void*>(
            "il2cpp_value_box",
            loopClass->address,
            array->GetData() + i * sizeof(LoopValue));
        Root childRoot(child);
        if (!OwnSlot(Value(child)[3])) {
            ++shape[row].children;
        }
        Shape(child, shape, depth + 1);
    }
}

void* RewriteLoop(void* boxed, unsigned depth = 0) {
    if (!boxed || depth > 16) {
        throw std::runtime_error("loop depth");
    }
    Root root(boxed);
    auto value = Value(boxed);
    auto* array = static_cast<UnityResolve::UnityType::Array<unsigned char>*>(value[1]);
    if (!array) {
        return boxed;
    }
    const auto count = UnityResolve::Invoke<std::uintptr_t>("il2cpp_array_length", array);
    if (count > 512) {
        throw std::runtime_error("rewrite array size=" + std::to_string(count));
    }
    const auto name = ManagedTypeName(value[0]);
    auto* replacement = UnityResolve::Invoke<void*>(
        "il2cpp_array_new", loopClass->address, count + 2);
    if (!replacement) {
        throw std::runtime_error("loop allocation");
    }
    Root replacementRoot(replacement);
    unsigned written = 0;
    const auto marker = [&](unsigned slot) {
        LoopValue node{};
        node[3] = &slots[slot];
        StoreArrayElement(replacement, written++, node);
    };
    if (name == "UnityEngine.PlayerLoop.Initialization") {
        marker(0);
    }
    for (std::uintptr_t i = 0; i < count; ++i) {
        auto* child = UnityResolve::Invoke<void*>(
            "il2cpp_value_box",
            loopClass->address,
            array->GetData() + i * sizeof(LoopValue));
        Root childRoot(child);
        const auto original = Value(child);
        if (OwnSlot(original[3])) {
            continue;
        }
        auto* rewritten = RewriteLoop(child, depth + 1);
        Root rewrittenRoot(rewritten);
        StoreArrayElement(replacement, written++, Value(rewritten));
    }
    if (name == "UnityEngine.PlayerLoop.PostLateUpdate") {
        marker(1);
    }
    auto* exactArray = UnityResolve::Invoke<void*>(
        "il2cpp_array_new", loopClass->address, static_cast<std::uintptr_t>(written));
    if (!exactArray) {
        throw std::runtime_error("loop allocation");
    }
    Root exactRoot(exactArray);
    auto* tempData = reinterpret_cast<unsigned char*>(
        static_cast<UnityResolve::UnityType::Array<unsigned char>*>(replacement)->GetData());
    for (unsigned i = 0; i < written; ++i) {
        LoopValue element{};
        Read(tempData + i * sizeof(LoopValue), &element, sizeof(element));
        StoreArrayElement(exactArray, i, element);
    }
    auto** childSlot = reinterpret_cast<void**>(
        static_cast<unsigned char*>(boxed) + loopFields[1]->offset);
    UnityResolve::Invoke<void>("il2cpp_gc_wbarrier_set_field", boxed, childSlot, exactArray);
    if (Value(boxed)[1] != exactArray) {
        throw std::runtime_error("child array readback");
    }
    return boxed;
}

void EnsureLoop() {
    if (!setLoopMethod || !loopMethod) {
        return;
    }
    void* boxed = nullptr;
    if (!Invoke(loopMethod, nullptr, nullptr, &boxed) || !boxed) {
        return;
    }
    Root root(boxed);
    unsigned own = 0;
    const auto before = CountNodes(boxed, own);
    if (own == slots.size()) {
        return;
    }
    std::vector<ShapeNode> beforeShape;
    std::vector<ShapeNode> afterShape;
    Shape(boxed, beforeShape);
    auto* changed = RewriteLoop(boxed);
    Root changedRoot(changed);
    unsigned afterOwn = 0;
    const auto after = CountNodes(changed, afterOwn);
    Shape(changed, afterShape);
    if (beforeShape != afterShape || before - own != after - afterOwn ||
        afterOwn != slots.size()) {
        Log("FRAME_DRIVE_REJECT reason=preservation before=" + std::to_string(before) +
            " after=" + std::to_string(after));
        return;
    }
    void* args[]{UnityResolve::Invoke<void*>("il2cpp_object_unbox", changed)};
    void* result = nullptr;
    ++loopWriteDepth;
    const bool ok = Invoke(setLoopMethod, nullptr, args, &result);
    --loopWriteDepth;
    Log("FRAME_DRIVE_INSTALL before=" + std::to_string(before) +
        " after=" + std::to_string(after) + " own=" + std::to_string(afterOwn) +
        " ok=" + std::to_string(ok));
    nextLoopCheck = GetTickCount64() + 2000;
}

void IssueEndEvent(int eventId) {
    if (!eventMethod || eventId <= 0) {
        return;
    }
    void* callback = reinterpret_cast<void*>(&RenderEvent);
    void* args[]{&callback, &eventId};
    void* result = nullptr;
    const bool ok = Invoke(eventMethod, nullptr, args, &result);
    if (!ok) {
        Log("FRAME_DRIVE_EVENT_FAIL id=" + std::to_string(eventId));
    }
}

void SetInternalHook(void* array) {
    setInternalOriginal(array);
    if (!Enabled() || repairingLoop || !setLoopMethod) {
        return;
    }
    if (loopWriteDepth != 0) {
        return;
    }
    repairingLoop = true;
    try {
        EnsureLoop();
    } catch (...) {
        Log("FRAME_DRIVE_REJECT reason=setter-repair");
    }
    repairingLoop = false;
}

void* ResolveLoopBinding(Method* wrapper) {
    if (!wrapper) {
        return nullptr;
    }
    std::array<unsigned char, 51> code{};
    const auto* fn = static_cast<unsigned char*>(wrapper->function);
    if (!Read(fn, code.data(), code.size()) ||
        std::memcmp(code.data() + 6, "\x48\x8b\x05", 3) != 0 ||
        std::memcmp(code.data() + 21, "\x48\x8d\x0d", 3) != 0 ||
        std::memcmp(code.data() + 48, "\x48\xff\xe0", 3) != 0) {
        Log("FRAME_DRIVE_REJECT reason=icall-wrapper-shape");
        return nullptr;
    }
    std::int32_t signatureOffset = 0;
    std::int32_t cacheOffset = 0;
    std::memcpy(&signatureOffset, code.data() + 24, sizeof(signatureOffset));
    std::memcpy(&cacheOffset, code.data() + 9, sizeof(cacheOffset));
    const auto* signatureAddress =
        reinterpret_cast<const char*>(fn + 28 + signatureOffset);
    std::string signature;
    bool terminated = false;
    for (unsigned i = 0; i < 256; ++i) {
        char c = 0;
        if (!Read(signatureAddress + i, &c, 1)) {
            break;
        }
        if (!c) {
            terminated = true;
            break;
        }
        signature += c;
    }
    using ResolveFn = void* (*)(const char*);
    const auto resolve = reinterpret_cast<ResolveFn>(
        GetProcAddress(GetModuleHandleW(L"GameAssembly.dll"), "il2cpp_resolve_icall"));
    if (!terminated ||
        !signature.starts_with("UnityEngine.LowLevel.PlayerLoop::SetPlayerLoopInternal") ||
        !resolve) {
        Log("FRAME_DRIVE_REJECT reason=icall-signature-or-export");
        return nullptr;
    }
    void* cached = nullptr;
    if (!Read(fn + 13 + cacheOffset, &cached, sizeof(cached))) {
        return nullptr;
    }
    void* target = resolve(signature.c_str());
    const auto unity = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"UnityPlayer.dll"));
    std::array<unsigned char, 128> native{};
    if (!target || reinterpret_cast<std::uintptr_t>(target) - unity != 0xcd060 ||
        (cached && cached != target) || !Read(target, native.data(), native.size()) ||
        std::memcmp(native.data(), "\x48\x8b\xc4\x55\x53\x41\x54", 7) != 0) {
        Log("FRAME_DRIVE_REJECT reason=icall-binding-identity");
        return nullptr;
    }
    return target;
}

void Initialize() {
    attempted = true;
    const auto ga = GetModuleHandleW(L"GameAssembly.dll");
    for (const char* name :
         {"il2cpp_runtime_invoke",
          "il2cpp_object_unbox",
          "il2cpp_value_box",
          "il2cpp_array_length",
          "il2cpp_gchandle_new",
          "il2cpp_gchandle_free",
          "il2cpp_class_from_system_type",
          "il2cpp_class_get_namespace",
          "il2cpp_class_get_name",
          "il2cpp_array_new",
          "il2cpp_gc_wbarrier_set_field",
          "il2cpp_field_get_value"}) {
        if (!GetProcAddress(ga, name)) {
            Log("FRAME_DRIVE_REJECT reason=export-miss name=" + std::string(name));
            return;
        }
    }
    auto* assembly = UnityResolve::Get("UnityEngine.CoreModule.dll");
    auto* playerLoop = assembly ? assembly->Get("PlayerLoop", "UnityEngine.LowLevel") : nullptr;
    loopClass = assembly ? assembly->Get("PlayerLoopSystem", "UnityEngine.LowLevel") : nullptr;
    auto* gl = assembly ? assembly->Get("GL", "UnityEngine") : nullptr;
    eventMethod = Exact(gl, "IssuePluginEvent", "System.Void", {"System.IntPtr", "System.Int32"});
    loopMethod = Exact(
        playerLoop, "GetCurrentPlayerLoop", "UnityEngine.LowLevel.PlayerLoopSystem", {});
    setLoopMethod = Exact(
        playerLoop,
        "SetPlayerLoop",
        "System.Void",
        {"UnityEngine.LowLevel.PlayerLoopSystem"});
    auto* internalSet = Exact(
        playerLoop,
        "SetPlayerLoopInternal",
        "System.Void",
        {"UnityEngine.LowLevel.PlayerLoopSystemInternal[]"});
    constexpr std::array names{
        "type", "subSystemList", "updateDelegate", "updateFunction", "loopConditionFunction"};
    constexpr std::array types{
        "System.Type",
        "UnityEngine.LowLevel.PlayerLoopSystem[]",
        "UnityEngine.LowLevel.PlayerLoopSystem.UpdateFunction",
        "System.IntPtr",
        "System.IntPtr"};
    if (loopClass) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            for (auto* f : loopClass->fields) {
                if (f && !f->static_field && f->name == names[i] && Type(f->type) == types[i]) {
                    loopFields[i] = f;
                }
            }
        }
    }
    std::uint32_t alignment = 0;
    if (!loopClass ||
        UnityResolve::Invoke<int>("il2cpp_class_value_size", loopClass->address, &alignment) !=
            sizeof(LoopValue)) {
        setLoopMethod = nullptr;
        Log("FRAME_DRIVE_REJECT reason=value-size");
        return;
    }
    for (std::size_t i = 0; i < loopFields.size(); ++i) {
        if (!loopFields[i] || loopFields[i]->offset != 16 + i * sizeof(void*)) {
            Log("FRAME_DRIVE_REJECT reason=field-layout");
            setLoopMethod = nullptr;
            return;
        }
    }
    if (registrarReady.load(std::memory_order_acquire) && internalSet && !setInternalOriginal) {
        if (auto* binding = ResolveLoopBinding(internalSet)) {
            const bool ok = driverRegistrar.Install(
                binding,
                reinterpret_cast<void*>(&SetInternalHook),
                reinterpret_cast<void**>(&setInternalOriginal),
                "M2.SetPlayerLoopInternalBinding");
            Log("FRAME_DRIVE_HOOK ok=" + std::to_string(ok));
        }
    }
    try {
        EnsureLoop();
    } catch (const std::exception& error) {
        Log("FRAME_DRIVE_REJECT reason=" + std::string(error.what()));
        setLoopMethod = nullptr;
    }
}
} // namespace

void ConfigureFrameLoopDriver(const HookRegistrar& registrar) noexcept {
    if (!Enabled()) {
        return;
    }
    driverRegistrar = registrar;
    registrarReady.store(true, std::memory_order_release);
}

void FrameLoopDriverEnsureOnUnityThread() noexcept {
    if (!Enabled()) {
        return;
    }
    try {
        if (!attempted) {
            Initialize();
        }
        if (GetTickCount64() >= nextLoopCheck && setLoopMethod) {
            nextLoopCheck = GetTickCount64() + 2000;
            EnsureLoop();
        }
    } catch (...) {
        setLoopMethod = nullptr;
    }
}

void FrameLoopDriverAfterSrp() noexcept {
    FrameLoopDriverEnsureOnUnityThread();
}

void FrameLoopDriverOnPresent() noexcept {
    // DXGI Present is not an IL2CPP-attached thread. Installing the
    // PlayerLoop from this callback ran GC write barriers and crashed
    // first launch with "Fatal error in GC: Collecting from unknown thread".
}
} // namespace gakumas::vr
