#include "HookManager.hpp"

#include "GakumasLocalify/Log.h"

#include <atomic>
#include <mutex>

namespace {
    std::mutex g_hookManagerMutex;
    std::atomic_bool g_hookManagerInitialized = false;
}

namespace GakumasVR::Hooks {
    bool Initialize() {
        std::scoped_lock lock(g_hookManagerMutex);
        if (g_hookManagerInitialized.load(std::memory_order_acquire)) {
            return true;
        }

        const auto status = MH_Initialize();
        if (status != MH_OK) {
            GakumasLocal::Log::ErrorFmt(
                "MinHook initialization failed: %s",
                MH_StatusToString(status));
            return false;
        }

        g_hookManagerInitialized.store(true, std::memory_order_release);
        return true;
    }

    bool IsInitialized() {
        return g_hookManagerInitialized.load(std::memory_order_acquire);
    }

    MH_STATUS CreateAndEnableHook(void* target, void* detour, void** original) {
        std::scoped_lock lock(g_hookManagerMutex);

        if (!g_hookManagerInitialized.load(std::memory_order_acquire)) {
            return MH_ERROR_NOT_INITIALIZED;
        }
        if (!target || !detour) {
            if (original) {
                *original = nullptr;
            }
            return MH_ERROR_NOT_EXECUTABLE;
        }

        if (original) {
            *original = nullptr;
        }

        const auto createStatus = MH_CreateHook(target, detour, original);
        if (createStatus != MH_OK) {
            return createStatus;
        }

        const auto enableStatus = MH_EnableHook(target);
        if (enableStatus == MH_OK) {
            return MH_OK;
        }

        const auto removeStatus = MH_RemoveHook(target);
        if (removeStatus != MH_OK) {
            GakumasLocal::Log::ErrorFmt(
                "MinHook rollback failed for %p: %s",
                target,
                MH_StatusToString(removeStatus));
        }
        if (original) {
            *original = nullptr;
        }
        return enableStatus;
    }

    bool CreateAndEnable(
        void* target,
        void* detour,
        void** original,
        const char* diagnosticName) {
        const auto status = CreateAndEnableHook(target, detour, original);
        if (status == MH_OK) {
            return true;
        }

        GakumasLocal::Log::ErrorFmt(
            "Hook install failed (%s, target=%p): %s",
            diagnosticName ? diagnosticName : "unnamed",
            target,
            MH_StatusToString(status));
        return false;
    }

    void Shutdown() {
        std::scoped_lock lock(g_hookManagerMutex);
        if (!g_hookManagerInitialized.load(std::memory_order_acquire)) {
            return;
        }

        const auto disableStatus = MH_DisableHook(MH_ALL_HOOKS);
        if (disableStatus != MH_OK && disableStatus != MH_ERROR_NOT_CREATED) {
            GakumasLocal::Log::ErrorFmt(
                "MinHook disable-all failed: %s",
                MH_StatusToString(disableStatus));
        }

        const auto uninitializeStatus = MH_Uninitialize();
        if (uninitializeStatus != MH_OK) {
            GakumasLocal::Log::ErrorFmt(
                "MinHook shutdown failed: %s",
                MH_StatusToString(uninitializeStatus));
            return;
        }

        g_hookManagerInitialized.store(false, std::memory_order_release);
    }
}
