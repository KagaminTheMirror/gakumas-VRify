#pragma once

#include <minhook.h>
#include <cstddef>

namespace GakumasVR::Hooks {
    struct Request { void* target; void* detour; void** original; const char* name; };
    bool CreateAndEnableBatch(const Request* requests, std::size_t count);
    // MinHook has process-global state inside this DLL. Keep every call that
    // changes that state behind this owner so Localify and VR cannot initialize
    // or tear it down independently.
    bool Initialize();
    bool IsInitialized();

    MH_STATUS CreateAndEnableHook(void* target, void* detour, void** original);
    bool CreateAndEnable(
        void* target,
        void* detour,
        void** original,
        const char* diagnosticName);

    // This is intentionally explicit. It must not be called from DllMain or
    // from a hook that may currently be executing.
    void Shutdown();
}
