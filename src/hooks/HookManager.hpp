#pragma once

#include <minhook.h>

namespace GakumasVR::Hooks {
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
