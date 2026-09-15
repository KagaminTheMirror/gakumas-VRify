#include "HookManager.hpp"

#include "GakumasLocalify/Log.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace {
    std::mutex g_hookManagerMutex;
    std::atomic_bool g_hookManagerInitialized = false;
}

namespace GakumasVR::Hooks {
    bool CreateAndEnableBatch(const Request* requests, std::size_t count) {
        std::scoped_lock lock(g_hookManagerMutex);
        if (!g_hookManagerInitialized.load(std::memory_order_acquire)) return false;
        if (count == 0) return true;
        if (!requests) return false;
        for (std::size_t i = 0; i < count; ++i) {
            if (!requests[i].target || !requests[i].detour || !requests[i].original) return false;
            *requests[i].original = nullptr;
        }
        std::vector<void*> created;
        created.reserve(count);
        const auto rollback = [&] {
            for (std::size_t i = 0; i < created.size(); ++i) {
                const auto status = MH_RemoveHook(created[i]);
                if (status == MH_OK) *requests[i].original = nullptr;
                else GakumasLocal::Log::ErrorFmt("Hook batch rollback failed: %s", MH_StatusToString(status));
            }
        };
        for (std::size_t i = 0; i < count; ++i) {
            const auto& r = requests[i];
            const auto status = MH_CreateHook(r.target, r.detour, r.original);
            if (status != MH_OK) {
                GakumasLocal::Log::ErrorFmt("Hook batch create failed (%s): %s", r.name, MH_StatusToString(status));
                rollback();
                return false;
            }
            created.push_back(r.target);
        }
        // All trampolines exist before any detour can run. MinHook freezes the
        // process threads once for ApplyQueued, rather than once per hook.
        for (void* target : created) {
            if (MH_QueueEnableHook(target) != MH_OK) {
                rollback();
                return false;
            }
        }
        const auto status = MH_ApplyQueued();
        if (status != MH_OK) {
            // ApplyQueued may have enabled a prefix. Keep its trampolines alive;
            // the caller disables collection, and installed detours pass through.
            GakumasLocal::Log::ErrorFmt("Hook batch enable failed: %s", MH_StatusToString(status));
        }
        return status == MH_OK;
    }
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
