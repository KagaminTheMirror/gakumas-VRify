#pragma once

// Pull in the byte-for-byte upstream platform contract, then override only the
// two integration macros owned by the VR host. This avoids maintaining a forked
// copy of upstream constants and helper functions.
#include "../../platformDefine.hpp"
#include "vr/VrVersion.hpp"

#undef PLUGIN_VERSION
#define PLUGIN_VERSION GAKUMAS_VR_VERSION

#undef ADD_HOOK
#define ADD_HOOK(name, addr)                                                                        \
    do {                                                                                             \
        name##_Addr = reinterpret_cast<name##_Type>(addr);                                          \
        auto* const hookTarget = reinterpret_cast<void*>(name##_Addr);                               \
        if (hookTarget) {                                                                            \
            const auto hookResult = hookInstaller->InstallHook(                                      \
                hookTarget,                                                                          \
                reinterpret_cast<void*>(name##_Hook),                                                \
                reinterpret_cast<void**>(&name##_Orig));                                             \
            if (hookResult) {                                                                        \
                const auto hookStatus = static_cast<MH_STATUS>(                                      \
                    reinterpret_cast<std::intptr_t>(hookResult));                                    \
                Log::ErrorFmt(                                                                       \
                    "ADD_HOOK: %s at %p failed: %s",                                                \
                    #name,                                                                           \
                    hookTarget,                                                                      \
                    MH_StatusToString(hookStatus));                                                   \
            }                                                                                        \
            else {                                                                                   \
                hookedStubs.emplace(hookTarget);                                                      \
                GakumasLocal::Log::InfoFmt("ADD_HOOK: %s at %p", #name, hookTarget);                \
            }                                                                                        \
        }                                                                                            \
        else {                                                                                       \
            GakumasLocal::Log::ErrorFmt("Hook failed: %s is NULL", #name);                          \
        }                                                                                            \
        if (Config::lazyInit) UnityResolveProgress::classProgress.current++;                         \
    } while (false)
