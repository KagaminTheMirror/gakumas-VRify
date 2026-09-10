#include "LivenessCrashProbe.hpp"

#ifndef GAKUMAS_LIVENESS_PROBE_TEST
#include "VrRuntime.hpp"
#include "../deps/UnityResolve/UnityResolve.hpp"
#endif
#include "VrVersion.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <locale>
#include <iomanip>
#include <sstream>
#include <string>

namespace gakumas::vr {
namespace {

constexpr std::uintptr_t kAddProcessObjectFaultRva = 0x008D6F5AU;
constexpr std::uintptr_t kAddProcessObjectCallerReturnRva = 0x008DC096U;
constexpr std::uintptr_t kSecondHasReferencesFaultRva = 0x008D7CF2U;
constexpr std::uintptr_t kSecondHasReferencesSignatureRva = 0x008D7CE1U;
constexpr std::array<std::uint8_t, 7> kFaultSignature{
    0x0F, 0xB6, 0xB0, 0x35, 0x01, 0x00, 0x00};
// Exact unpacked code archived in stereo.329-hardware, not inferred ABI.
constexpr std::array<std::uint8_t, 15> kPrologueSignature{
    0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,
    0x57,0x48,0x83,0xec,0x20};
constexpr std::array<std::uint8_t, 65> kCallerSignature{
    0x48,0x8b,0x31,0x48,0x8b,0x06,0x48,0x8b,0x0e,
    0x48,0x83,0xe0,0xfe,0x48,0x83,0xe1,0xfe,0xf6,0x40,0x08,0x01,
    0x74,0x2c,0x4c,0x8b,0x71,0x08,0x33,0xdb,0xbf,0x3f,0,0,0,
    0x8b,0xc7,0x49,0x0f,0xa3,0xc6,0x73,0x0c,0x48,0x8b,0x0c,0xde,
    0x48,0x8b,0xd5,0xe8,0x9a,0xae,0xff,0xff,0xff,0xcf,
    0x48,0xff,0xc3,0x48,0x83,0xfb,0x3e,0x7c,0xe1};
// Exact unpacked bytes from .401 pid 16096 at GameAssembly+0x8d7ce1.
constexpr std::array<std::uint8_t, 24> kSecondHasReferencesSignature{
    0x48,0x85,0xdb,0x74,0x71,0xf6,0x03,0x01,0x75,0x6c,0x48,0x8b,0x03,
    0x48,0x83,0xe0,0xfe,0x0f,0xb6,0xb0,0x35,0x01,0x00,0x00};

// 0 = may retry (GameAssembly/log not ready), 1 = installing, 2 = armed,
// -1 = permanently unavailable for this process (signature/API mismatch).
volatile LONG g_installState = 0;
volatile LONG g_crashCaptured = 0;
volatile LONG g_secondSiteArmed = 0;
std::uintptr_t g_gameAssemblyBase = 0;
HANDLE g_crashLogHandle = INVALID_HANDLE_VALUE;
PVOID g_vectoredHandler = nullptr;

struct FixedTextBuffer {
    std::array<char, 16384> bytes{};
    std::size_t size = 0;

    void Clear() noexcept {
        size = 0;
    }

    void Append(const char* text) noexcept {
        if (text == nullptr) {
            return;
        }
        while (*text != '\0' && size < bytes.size()) {
            bytes[size++] = *text++;
        }
    }

    void AppendHex(std::uint64_t value) noexcept {
        static constexpr char kDigits[] = "0123456789abcdef";
        Append("0x");
        for (int shift = 60; shift >= 0 && size < bytes.size(); shift -= 4) {
            bytes[size++] = kDigits[(value >> shift) & 0x0FU];
        }
    }

    void AppendBytes(const std::uint8_t* data, std::size_t count) noexcept {
        static constexpr char kDigits[] = "0123456789abcdef";
        if (data == nullptr) {
            return;
        }
        for (std::size_t index = 0;
             index < count && size + 2U <= bytes.size(); ++index) {
            const std::uint8_t value = data[index];
            bytes[size++] = kDigits[value >> 4U];
            bytes[size++] = kDigits[value & 0x0FU];
        }
    }
};

// The faulting thread is already within a few KiB of StackLimit (.413
// pid 27964: rsp = StackLimit+0x16c8). .401 pid 21176 and .413 pid 27964
// stored `__chkstk` inside this handler (`rax=0x45e0`) and never wrote
// the sidecar. Keep the 16 KiB line buffer and scratch off that stack.
struct CaptureWorkspace {
    FixedTextBuffer output;
    std::array<std::uint8_t, 1024> stackBytes{};
    std::array<std::uint8_t, 256> objectBytes{};
    std::array<std::uint8_t, 128> stateBytes{};
    std::array<std::uint8_t, 8> word{};
    std::array<std::uint8_t, 512> parent{};
    std::array<std::uint8_t, 512> classBytes{};
    std::array<std::uint8_t, 64> prefix{};
    std::array<std::uint8_t, 128> pointed{};
};

CaptureWorkspace g_captureWorkspace;

void FlushCapture() noexcept {
    if (g_crashLogHandle == INVALID_HANDLE_VALUE ||
        g_captureWorkspace.output.size == 0) {
        return;
    }
    DWORD written = 0;
    static_cast<void>(WriteFile(
        g_crashLogHandle, g_captureWorkspace.output.bytes.data(),
        static_cast<DWORD>(g_captureWorkspace.output.size),
        &written, nullptr));
    static_cast<void>(FlushFileBuffers(g_crashLogHandle));
}

template <std::size_t Size>
SIZE_T ReadCurrentProcessMemory(
    std::uintptr_t address,
    std::array<std::uint8_t, Size>& bytes) noexcept {
    SIZE_T read = 0;
    if (address == 0) {
        return 0;
    }
    if (!ReadProcessMemory(
            GetCurrentProcess(), reinterpret_cast<const void*>(address),
            bytes.data(), bytes.size(), &read)) {
        return 0;
    }
    return read;
}

void AppendRegister(
    FixedTextBuffer& output,
    const char* name,
    std::uint64_t value) noexcept {
    output.Append(" ");
    output.Append(name);
    output.Append("=");
    output.AppendHex(value);
}

std::uint64_t ReadWord(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

template <std::size_t Size>
SIZE_T AppendMemory(FixedTextBuffer& output, const char* label,
    std::uintptr_t address, std::array<std::uint8_t, Size>& bytes) noexcept {
    const SIZE_T read = ReadCurrentProcessMemory(address, bytes);
    output.Append(" ");
    output.Append(label);
    AppendRegister(output, "address", address);
    AppendRegister(output, "read", read);
    output.Append(" bytes=");
    output.AppendBytes(bytes.data(), read);
    return read;
}

void AppendParentContext(FixedTextBuffer& output, const CONTEXT& context,
    const std::array<std::uint8_t, 1024>& stack, SIZE_T stackRead) noexcept {
    if (stackRead < 0x40U) {
        output.Append(" parentStatus=stack-unreadable");
        return;
    }
    const auto returnAddress = ReadWord(stack.data() + 0x28);
    const auto index = ReadWord(stack.data() + 0x30);
    const auto savedParent = ReadWord(stack.data() + 0x38);
    const auto bit = ReadWord(stack.data() + 0x20) & 0xffffffffU;
    AppendRegister(output, "callerReturn", returnAddress);
    AppendRegister(output, "slotIndex", index);
    AppendRegister(output, "savedParent", savedParent);
    if (returnAddress != g_gameAssemblyBase + kAddProcessObjectCallerReturnRva ||
        index >= 62U || savedParent != context.Rsi || bit != 63U - index ||
        (context.R14 & 1U) == 0 ||
        ((context.R14 >> (63U - index)) & 1U) == 0 ||
        context.Rsi == 0 || context.Rsi > UINTPTR_MAX - 512U) {
        output.Append(" parentStatus=caller-mismatch");
        return;
    }
    output.Append(" parentStatus=descriptor-caller");
    auto& word = g_captureWorkspace.word;
    auto& parent = g_captureWorkspace.parent;
    auto& classBytes = g_captureWorkspace.classBytes;
    auto& prefix = g_captureWorkspace.prefix;
    auto& pointed = g_captureWorkspace.pointed;
    std::memset(word.data(), 0, word.size());
    std::memset(parent.data(), 0, parent.size());
    std::memset(classBytes.data(), 0, classBytes.size());
    std::memset(prefix.data(), 0, prefix.size());
    AppendMemory(output, "parentSlot", context.Rsi + index * 8U, word);
    AppendMemory(output, "parent", context.Rsi, parent);
    // Read header separately: a parent near a page boundary may not admit 512B.
    if (AppendMemory(output, "parentHeader", context.Rsi, word) != word.size()) return;
    const auto klass = ReadWord(word.data()) & ~std::uint64_t{1};
    AppendMemory(output, "parentClass", klass, classBytes);
    if (AppendMemory(output, "classPrefix", klass, prefix) != prefix.size()) return;
    // Untyped pointer windows, NOT assumed Il2CppClass fields. Persist raw
    // metadata/string candidates without calling IL2CPP while GC is faulting.
    for (std::size_t offset = 0; offset < prefix.size(); offset += 8U) {
        AppendRegister(output, "classPointerOffset", offset);
        std::memset(pointed.data(), 0, pointed.size());
        AppendMemory(output, "classPointerWindow", ReadWord(prefix.data() + offset), pointed);
    }
}

void CaptureLivenessCrashBody(
    const EXCEPTION_RECORD* exception,
    const CONTEXT* context,
    std::uintptr_t faultAddress,
    bool isPrimary) noexcept {
    auto& ws = g_captureWorkspace;
    ws.output.Clear();
    std::memset(ws.stackBytes.data(), 0, ws.stackBytes.size());
    std::memset(ws.objectBytes.data(), 0, ws.objectBytes.size());
    std::memset(ws.stateBytes.data(), 0, ws.stateBytes.size());

    // Tiny breadcrumb first: later RPM / parent walks must not erase the RIP.
    ws.output.Append("[VR][liveness] LIVENESS_CRASH_BEGIN");
    AppendRegister(ws.output, "fault", faultAddress);
    ws.output.Append(isPrimary ? " site=add-process-object"
                               : " site=inlined-add-process-object");
    ws.output.Append("\r\n");
    FlushCapture();
    ws.output.Clear();

    const SIZE_T stackRead =
        ReadCurrentProcessMemory(context->Rsp, ws.stackBytes);
    // The proven AddProcessObject prologue keeps object in RBX and state in
    // RDI at the failing klass->has_references load.
    const SIZE_T objectRead =
        ReadCurrentProcessMemory(context->Rbx, ws.objectBytes);
    const SIZE_T stateRead =
        ReadCurrentProcessMemory(context->Rdi, ws.stateBytes);

    ws.output.Append("[VR][liveness] LIVENESS_CRASH_CONTEXT format=2 version=");
    ws.output.Append(GAKUMAS_VR_VERSION);
    ws.output.Append(isPrimary ? " site=add-process-object"
                               : " site=inlined-add-process-object");
    AppendRegister(ws.output, "pid", GetCurrentProcessId());
    AppendRegister(ws.output, "tid", GetCurrentThreadId());
    AppendRegister(ws.output, "code", exception->ExceptionCode);
    AppendRegister(ws.output, "fault", faultAddress);
    if (exception->NumberParameters >= 2U) {
        AppendRegister(ws.output, "access", exception->ExceptionInformation[0]);
        AppendRegister(ws.output, "target", exception->ExceptionInformation[1]);
    }
    AppendRegister(ws.output, "rax", context->Rax);
    AppendRegister(ws.output, "rbx", context->Rbx);
    AppendRegister(ws.output, "rcx", context->Rcx);
    AppendRegister(ws.output, "rdx", context->Rdx);
    AppendRegister(ws.output, "rsi", context->Rsi);
    AppendRegister(ws.output, "rdi", context->Rdi);
    AppendRegister(ws.output, "rbp", context->Rbp);
    AppendRegister(ws.output, "rsp", context->Rsp);
    AppendRegister(ws.output, "r8", context->R8);
    AppendRegister(ws.output, "r9", context->R9);
    AppendRegister(ws.output, "r10", context->R10);
    AppendRegister(ws.output, "r11", context->R11);
    AppendRegister(ws.output, "r12", context->R12);
    AppendRegister(ws.output, "r13", context->R13);
    AppendRegister(ws.output, "r14", context->R14);
    AppendRegister(ws.output, "r15", context->R15);
    AppendRegister(ws.output, "rip", context->Rip);
    AppendRegister(ws.output, "stackRead", stackRead);
    ws.output.Append(" stack=");
    ws.output.AppendBytes(ws.stackBytes.data(), stackRead);
    AppendRegister(ws.output, "objectRead", objectRead);
    ws.output.Append(" object=");
    ws.output.AppendBytes(ws.objectBytes.data(), objectRead);
    AppendRegister(ws.output, "stateRead", stateRead);
    ws.output.Append(" state=");
    ws.output.AppendBytes(ws.stateBytes.data(), stateRead);
    if (isPrimary) {
        AppendParentContext(ws.output, *context, ws.stackBytes, stackRead);
    } else {
        ws.output.Append(" parentStatus=not-descriptor-site");
    }
    ws.output.Append("\r\n");
    FlushCapture();
}

LONG CALLBACK CaptureLivenessCrash(EXCEPTION_POINTERS* pointers) noexcept {
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr ||
        pointers->ContextRecord == nullptr || g_gameAssemblyBase == 0 ||
        g_crashLogHandle == INVALID_HANDLE_VALUE) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const EXCEPTION_RECORD* exception = pointers->ExceptionRecord;
    const auto faultAddress = reinterpret_cast<std::uintptr_t>(
        exception->ExceptionAddress);
    const auto primaryFault = g_gameAssemblyBase + kAddProcessObjectFaultRva;
    const auto secondFault = g_gameAssemblyBase + kSecondHasReferencesFaultRva;
    const bool isPrimary = faultAddress == primaryFault;
    const bool isSecond = faultAddress == secondFault && g_secondSiteArmed != 0;
    if (exception->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        (!isPrimary && !isSecond) ||
        InterlockedCompareExchange(&g_crashCaptured, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CaptureLivenessCrashBody(
        exception, pointers->ContextRecord, faultAddress, isPrimary);
    return EXCEPTION_CONTINUE_SEARCH;
}

bool ReadExact(
    std::uintptr_t address,
    void* destination,
    std::size_t size) noexcept {
    SIZE_T read = 0;
    return address != 0 && destination != nullptr &&
        ReadProcessMemory(
            GetCurrentProcess(), reinterpret_cast<const void*>(address),
            destination, size, &read) &&
        read == size;
}

#ifndef GAKUMAS_LIVENESS_PROBE_TEST
void LogCodeRange(
    std::uintptr_t base,
    std::uintptr_t rva,
    std::size_t size,
    const char* role) {
    constexpr std::size_t kChunkSize = 32U;
    for (std::size_t offset = 0; offset < size; offset += kChunkSize) {
        const std::size_t count = (std::min)(kChunkSize, size - offset);
        std::array<std::uint8_t, kChunkSize> bytes{};
        if (!ReadExact(base + rva + offset, bytes.data(), count)) {
            WriteVrLog(
                std::string("[VR][liveness] LIVENESS_CODE_READ_FAILED role=") +
                role + " rva=0x" + [&]() {
                    std::ostringstream value;
                    value.imbue(std::locale::classic());
                    value << std::hex << (rva + offset);
                    return value.str();
                }());
            return;
        }
        std::ostringstream line;
        line.imbue(std::locale::classic());
        line << "[VR][liveness] LIVENESS_CODE_BYTES role=" << role
             << " rva=0x" << std::hex << (rva + offset) << " bytes=";
        for (std::size_t index = 0; index < count; ++index) {
            line << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned>(bytes[index]);
        }
        WriteVrLog(line.str());
    }
}
#endif

} // namespace

#ifndef GAKUMAS_LIVENESS_PROBE_TEST
void TryDumpOctoCachingStorage() noexcept {
    static volatile LONG dumpState = 0;
    if (dumpState != 0) {
        return;
    }
    try {
    auto* assembly = UnityResolve::Get("Octo.dll");
    if (assembly == nullptr) {
        return;
    }
    auto* klass = assembly->Get("Storage", "Octo.Caching");
    if (klass == nullptr || klass->address == nullptr) {
        return;
    }
    if (InterlockedCompareExchange(&dumpState, 1, 0) != 0) {
        return;
    }
    const int instanceSize = UnityResolve::Invoke<int>(
        "il2cpp_class_instance_size", klass->address);
    WriteVrLog(
        std::string("[VR][liveness] STORAGE_DUMP type=Octo.Caching.Storage "
                    "instanceSize=") +
        std::to_string(instanceSize) +
        " resolveFields=" + std::to_string(klass->fields.size()) +
        " resolveMethods=" + std::to_string(klass->methods.size()));
    void* iter = nullptr;
    void* field = nullptr;
    std::uint32_t logged = 0;
    while ((field = UnityResolve::Invoke<void*>(
                "il2cpp_class_get_fields", klass->address, &iter)) != nullptr &&
           logged < 32U) {
        const char* name = UnityResolve::Invoke<const char*>(
            "il2cpp_field_get_name", field);
        const int offset =
            UnityResolve::Invoke<int>("il2cpp_field_get_offset", field);
        void* type = UnityResolve::Invoke<void*>("il2cpp_field_get_type", field);
        const char* typeName = type != nullptr
            ? UnityResolve::Invoke<const char*>("il2cpp_type_get_name", type)
            : "?";
        ++logged;
        WriteVrLog(
            std::string("[VR][liveness] STORAGE_FIELD name=") +
            (name != nullptr ? name : "?") +
            " type=" + (typeName != nullptr ? typeName : "?") +
            " offset=" + std::to_string(offset));
    }
    void* methodIter = nullptr;
    void* method = nullptr;
    logged = 0;
    while ((method = UnityResolve::Invoke<void*>(
                "il2cpp_class_get_methods", klass->address, &methodIter)) !=
               nullptr &&
           logged < 32U) {
        const char* name = UnityResolve::Invoke<const char*>(
            "il2cpp_method_get_name", method);
        ++logged;
        WriteVrLog(
            std::string("[VR][liveness] STORAGE_METHOD name=") +
            (name != nullptr ? name : "?"));
    }
    } catch (...) {
        InterlockedExchange(&dumpState, 0);
    }
}

void EnsureLivenessCrashProbe() noexcept {
    try {
        TryDumpOctoCachingStorage();
        const LONG state = InterlockedCompareExchange(&g_installState, 1, 0);
        if (state != 0) {
            return;
        }

        HMODULE gameAssembly = GetModuleHandleW(L"GameAssembly.dll");
        const auto logPath = VrRuntime::Instance().LogPath();
        if (gameAssembly == nullptr || logPath.empty()) {
            InterlockedExchange(&g_installState, 0);
            return;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(gameAssembly);
        std::array<std::uint8_t, kFaultSignature.size()> signature{};
        std::array<std::uint8_t, kPrologueSignature.size()> prologue{};
        std::array<std::uint8_t, kCallerSignature.size()> caller{};
        if (!ReadExact(
                base + kAddProcessObjectFaultRva,
                signature.data(), signature.size()) ||
            signature != kFaultSignature ||
            !ReadExact(base + 0x8D6F30U, prologue.data(), prologue.size()) ||
            prologue != kPrologueSignature ||
            !ReadExact(base + 0x8DC060U, caller.data(), caller.size()) ||
            caller != kCallerSignature) {
            WriteVrLog(
                "[VR][liveness] LIVENESS_CRASH_PROBE_SKIPPED reason="
                "signature-mismatch faultRva=0x8d6f5a");
            InterlockedExchange(&g_installState, -1);
            return;
        }

        // Separate file: the ordinary logger's stream position must never
        // overwrite an exception breadcrumb appended behind its back.
        auto crashPath = logPath;
        crashPath += L".liveness-crash.txt";
        HANDLE crashLog = CreateFileW(
            crashPath.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (crashLog == INVALID_HANDLE_VALUE) {
            WriteVrLog(
                "[VR][liveness] LIVENESS_CRASH_PROBE_SKIPPED reason="
                "log-open-failed error=" + std::to_string(GetLastError()));
            InterlockedExchange(&g_installState, -1);
            return;
        }

        std::array<std::uint8_t, kSecondHasReferencesSignature.size()>
            secondSignature{};
        const bool secondSite =
            ReadExact(
                base + kSecondHasReferencesSignatureRva,
                secondSignature.data(), secondSignature.size()) &&
            secondSignature == kSecondHasReferencesSignature &&
            ReadExact(
                base + kSecondHasReferencesFaultRva,
                signature.data(), kFaultSignature.size()) &&
            signature == kFaultSignature;
        InterlockedExchange(&g_secondSiteArmed, secondSite ? 1 : 0);
        if (!secondSite) {
            WriteVrLog(
                "[VR][liveness] LIVENESS_SECOND_SITE_SKIPPED reason="
                "signature-mismatch faultRva=0x8d7cf2");
        }

        g_gameAssemblyBase = base;
        g_crashLogHandle = crashLog;
        g_vectoredHandler = AddVectoredExceptionHandler(1, CaptureLivenessCrash);
        if (g_vectoredHandler == nullptr) {
            const DWORD error = GetLastError();
            CloseHandle(g_crashLogHandle);
            g_crashLogHandle = INVALID_HANDLE_VALUE;
            g_gameAssemblyBase = 0;
            InterlockedExchange(&g_secondSiteArmed, 0);
            WriteVrLog(
                "[VR][liveness] LIVENESS_CRASH_PROBE_SKIPPED reason="
                "veh-install-failed error=" + std::to_string(error));
            InterlockedExchange(&g_installState, -1);
            return;
        }

        std::ostringstream armed;
        armed.imbue(std::locale::classic());
        armed << "[VR][liveness] LIVENESS_CRASH_PROBE_ARMED version="
              << GAKUMAS_VR_VERSION << " base=" << gameAssembly
              << " faultRva=0x" << std::hex << kAddProcessObjectFaultRva
              << " callerReturnRva=0x" << kAddProcessObjectCallerReturnRva
              << " secondFaultRva=0x" << kSecondHasReferencesFaultRva
              << " secondSite=" << (g_secondSiteArmed ? "1" : "0")
              << " captureFormat=2 crashFile=" << crashPath.string();
        WriteVrLog(armed.str());
        LogCodeRange(base, 0x008D6F30U, 0xB0U, "add-process-object");
        LogCodeRange(base, 0x008DBB00U, 0x800U, "add-process-caller");
        if (g_secondSiteArmed != 0) {
            LogCodeRange(base, 0x008D7C80U, 0x100U, "inlined-add-process-object");
        }
        InterlockedExchange(&g_installState, 2);
    } catch (...) {
        // Diagnostics must never terminate the game. If the native handler was
        // already installed it remains useful even when a log formatting
        // allocation failed; otherwise disable this optional probe.
        InterlockedExchange(&g_installState, g_vectoredHandler != nullptr ? 2 : -1);
    }
}
#endif

} // namespace gakumas::vr
