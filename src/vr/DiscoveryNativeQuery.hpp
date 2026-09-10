#pragma once
#include "DiscoveryNativeQueryPin.hpp"
#include <Windows.h>
#include <array>
#include <cstring>
#include <string>

namespace gakumas::vr::discovery {
// Machine ABI, NOT reconstructed Unity C++ wrapper layouts. The decorated PDB
// signature and both full-query caller/callee bodies prove RCX=sret slot,
// RDX=Il2CppClass*, R8D=mode, R9D=inactive, stack arg5=sort; RAX=sret slot.
// Evidence: discovery-census-dev408, plus dev405 Scripting-FindObjectsOfType.txt.
using NativeFindAbi = void* (*)(void**, void*, int, int, int);
inline bool InvokeFullQuery(NativeFindAbi fn, void* klass, int sort, void*& result) {
    result = nullptr;
    if (!fn || !klass || (sort != 0 && sort != 1)) return false;
    void* slot = nullptr;
    void* returnedSlot = fn(&slot, klass, 2, 0, sort);
    if (returnedSlot != &slot || !slot) return false;
    result = slot;
    return true;
}

inline bool ReadNative(const void* src, void* dst, std::size_t size) {
    SIZE_T read = 0;
    return src && ReadProcessMemory(GetCurrentProcess(), src, dst, size, &read) && read == size;
}
template<std::size_t N>
bool MatchesNative(const unsigned char* src, const unsigned char (&expected)[N]) {
    std::array<unsigned char, N> actual{};
    return ReadNative(src, actual.data(), N) && std::memcmp(actual.data(), expected, N) == 0;
}

struct NativeQueryBinding {
    NativeFindAbi find = nullptr;
    void* (*classFromType)(void*) = nullptr;
    std::string reason = "not-initialized";
    std::string signature;
    void* icall = nullptr;

    // Caller must resolve the exact live ResourcesAPIInternal method first.
    // No hooks, code patches, guessed struct layouts, or cross-version fallback.
    void Initialize(void* wrapper) {
        std::array<unsigned char, 51> code{};
        if (!ReadNative(wrapper, code.data(), code.size()) ||
            std::memcmp(code.data(), "\x40\x53\x48\x83\xec\x20\x48\x8b\x05", 9) ||
            std::memcmp(code.data()+21, "\x48\x8d\x0d", 3) ||
            std::memcmp(code.data()+48, "\x48\xff\xe0", 3)) {
            reason = "live-wrapper-shape"; return;
        }
        const auto* fn = static_cast<unsigned char*>(wrapper);
        std::int32_t literalOffset = 0, cacheOffset = 0;
        std::memcpy(&literalOffset, code.data()+24, 4);
        std::memcpy(&cacheOffset, code.data()+9, 4);
        bool terminated = false;
        for (unsigned i = 0; i < 192; ++i) {
            char c = 0;
            if (!ReadNative(fn+28+literalOffset+i, &c, 1)) break;
            if (!c) { terminated = true; break; }
            signature += c;
        }
        const auto ga = GetModuleHandleW(L"GameAssembly.dll");
        using Resolve = void* (*)(const char*);
        const auto resolve = reinterpret_cast<Resolve>(GetProcAddress(ga, "il2cpp_resolve_icall"));
        classFromType = reinterpret_cast<void* (*)(void*)>(GetProcAddress(ga, "il2cpp_class_from_system_type"));
        if (!terminated || !resolve || !classFromType ||
            (signature != "UnityEngine.ResourcesAPIInternal::FindObjectsOfTypeAll(System.Type)" &&
             signature != "UnityEngine.ResourcesAPIInternal::FindObjectsOfTypeAll")) {
            reason = "live-icall-signature"; return;
        }
        icall = resolve(signature.c_str());
        void* cached = nullptr;
        const auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(L"UnityPlayer.dll"));
        if (!base || icall != base+native_pin::kResourcesRva ||
            !ReadNative(fn+13+cacheOffset, &cached, sizeof(cached)) || cached != icall) {
            reason = "live-icall-cache-or-module"; return;
        }
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!ReadNative(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < 0 || dos.e_lfanew > 4096 ||
            !ReadNative(base+dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.FileHeader.TimeDateStamp != 1780469676 || nt.OptionalHeader.SizeOfImage != 35348480 ||
            !MatchesNative(base+native_pin::kResourcesRva, native_pin::kResources) ||
            !MatchesNative(base+native_pin::kScriptingRva, native_pin::kScripting) ||
            !MatchesNative(base+native_pin::kCollectRva, native_pin::kCollect)) {
            reason = "native-body-identity"; return;
        }
        find = reinterpret_cast<NativeFindAbi>(base+native_pin::kScriptingRva);
        reason = "matched";
    }
};
}
