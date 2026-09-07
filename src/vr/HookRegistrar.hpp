#pragma once

namespace gakumas::vr {

// Adapter supplied by the process-wide hook owner. The VR module never calls
// MH_Initialize/MH_Uninitialize and never assumes MinHook is the backend.
struct HookRegistrar {
    using InstallFn = bool (*)(
        void* context,
        void* target,
        void* detour,
        void** original,
        const char* diagnosticName);

    void* context = nullptr;
    InstallFn install = nullptr;

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
