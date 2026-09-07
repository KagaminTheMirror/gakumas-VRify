#include "LivenessCrashProbe.hpp"

#ifndef GAKUMAS_LIVENESS_PROBE_TEST
#include "VrRuntime.hpp"
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

// 0 = may retry (GameAssembly/log not ready), 1 = installing, 2 = armed,
// -1 = permanently unavailable for this process (signature/API mismatch).
volatile LONG g_installState = 0;
volatile LONG g_crashCaptured = 0;
std::uintptr_t g_gameAssemblyBase = 0;
HANDLE g_crashLogHandle = INVALID_HANDLE_VALUE;
PVOID g_vectoredHandler = nullptr;

struct FixedTextBuffer {
    std::array<char, 16384> bytes{};
    std::size_t size = 0;

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
    std::array<std::uint8_t, 8> word{};
    AppendMemory(output, "parentSlot", context.Rsi + index * 8U, word);
    std::array<std::uint8_t, 512> parent{};
    AppendMemory(output, "parent", context.Rsi, parent);
    // Read header separately: a parent near a page boundary may not admit 512B.
    if (AppendMemory(output, "parentHeader", context.Rsi, word) != word.size()) return;
    const auto klass = ReadWord(word.data()) & ~std::uint64_t{1};
    std::array<std::uint8_t, 512> classBytes{};
    AppendMemory(output, "parentClass", klass, classBytes);
    std::array<std::uint8_t, 64> prefix{};
    if (AppendMemory(output, "classPrefix", klass, prefix) != prefix.size()) return;
    // Untyped pointer windows, NOT assumed Il2CppClass fields. Persist raw
    // metadata/string candidates without calling IL2CPP while GC is faulting.
    for (std::size_t offset = 0; offset < prefix.size(); offset += 8U) {
        AppendRegister(output, "classPointerOffset", offset);
        std::array<std::uint8_t, 128> pointed{};
        AppendMemory(output, "classPointerWindow", ReadWord(prefix.data() + offset), pointed);
    }
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
    if (exception->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        faultAddress != g_gameAssemblyBase + kAddProcessObjectFaultRva ||
        InterlockedCompareExchange(&g_crashCaptured, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const CONTEXT* context = pointers->ContextRecord;
    std::array<std::uint8_t, 1024> stackBytes{};
    std::array<std::uint8_t, 256> objectBytes{};
    std::array<std::uint8_t, 128> stateBytes{};
    const SIZE_T stackRead = ReadCurrentProcessMemory(context->Rsp, stackBytes);
    // The proven AddProcessObject prologue keeps object in RBX and state in
    // RDI at the failing klass->has_references load.
    const SIZE_T objectRead =
        ReadCurrentProcessMemory(context->Rbx, objectBytes);
    const SIZE_T stateRead = ReadCurrentProcessMemory(context->Rdi, stateBytes);

    FixedTextBuffer output;
    output.Append("[VR][liveness] LIVENESS_CRASH_CONTEXT format=2 version=");
    output.Append(GAKUMAS_VR_VERSION);
    AppendRegister(output, "pid", GetCurrentProcessId());
    AppendRegister(output, "tid", GetCurrentThreadId());
    AppendRegister(output, "code", exception->ExceptionCode);
    AppendRegister(output, "fault", faultAddress);
    if (exception->NumberParameters >= 2U) {
        AppendRegister(output, "access", exception->ExceptionInformation[0]);
        AppendRegister(output, "target", exception->ExceptionInformation[1]);
    }
    AppendRegister(output, "rax", context->Rax);
    AppendRegister(output, "rbx", context->Rbx);
    AppendRegister(output, "rcx", context->Rcx);
    AppendRegister(output, "rdx", context->Rdx);
    AppendRegister(output, "rsi", context->Rsi);
    AppendRegister(output, "rdi", context->Rdi);
    AppendRegister(output, "rbp", context->Rbp);
    AppendRegister(output, "rsp", context->Rsp);
    AppendRegister(output, "r8", context->R8);
    AppendRegister(output, "r9", context->R9);
    AppendRegister(output, "r10", context->R10);
    AppendRegister(output, "r11", context->R11);
    AppendRegister(output, "r12", context->R12);
    AppendRegister(output, "r13", context->R13);
    AppendRegister(output, "r14", context->R14);
    AppendRegister(output, "r15", context->R15);
    AppendRegister(output, "rip", context->Rip);
    AppendRegister(output, "stackRead", stackRead);
    output.Append(" stack=");
    output.AppendBytes(stackBytes.data(), stackRead);
    AppendRegister(output, "objectRead", objectRead);
    output.Append(" object=");
    output.AppendBytes(objectBytes.data(), objectRead);
    AppendRegister(output, "stateRead", stateRead);
    output.Append(" state=");
    output.AppendBytes(stateBytes.data(), stateRead);
    AppendParentContext(output, *context, stackBytes, stackRead);
    output.Append("\r\n");

    DWORD written = 0;
    static_cast<void>(WriteFile(
        g_crashLogHandle, output.bytes.data(),
        static_cast<DWORD>(output.size), &written, nullptr));
    static_cast<void>(FlushFileBuffers(g_crashLogHandle));
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
void EnsureLivenessCrashProbe() noexcept {
    try {
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

        g_gameAssemblyBase = base;
        g_crashLogHandle = crashLog;
        g_vectoredHandler = AddVectoredExceptionHandler(1, CaptureLivenessCrash);
        if (g_vectoredHandler == nullptr) {
            const DWORD error = GetLastError();
            CloseHandle(g_crashLogHandle);
            g_crashLogHandle = INVALID_HANDLE_VALUE;
            g_gameAssemblyBase = 0;
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
              << " captureFormat=2 crashFile=" << crashPath.string();
        WriteVrLog(armed.str());
        LogCodeRange(base, 0x008D6F30U, 0xB0U, "add-process-object");
        LogCodeRange(base, 0x008DBB00U, 0x800U, "add-process-caller");
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
