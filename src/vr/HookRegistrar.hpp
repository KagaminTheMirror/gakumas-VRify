#pragma once
#include <cstddef>

namespace gakumas::vr {

// Adapter supplied by the process-wide hook owner. The VR module never calls
// MH_Initialize/MH_Uninitialize and never assumes MinHook is the backend.
struct HookRegistrar {
    struct Request {
        void* target;
        void* detour;
        void** original;
        const char* diagnosticName;
    };
    using BatchFn = bool (*)(void*, const Request*, std::size_t);
    using InstallFn = bool (*)(
        void* context,
        void* target,
        void* detour,
        void** original,
        const char* diagnosticName);

    void* context = nullptr;
    InstallFn install = nullptr;
    BatchFn batch = nullptr;

    [[nodiscard]] bool InstallBatch(const Request* requests, std::size_t count) const {
        if (batch) return batch(context, requests, count);
        bool ok = true;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& r = requests[i];
            ok &= Install(r.target, r.detour, r.original, r.diagnosticName);
        }
        return ok;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return install != nullptr;
    }

    [[nodiscard]] bool Install(
        void* target,
        void* detour,
        void** original,
        const char* diagnosticName) const {
        return install != nullptr && target != nullptr && detour != nullptr && original != nullptr &&
               install(context, target, detour, original, diagnosticName);
    }
};

} // namespace gakumas::vr
