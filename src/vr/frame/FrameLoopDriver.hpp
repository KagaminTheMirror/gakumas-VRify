#pragma once

namespace gakumas::vr {
struct HookRegistrar;

// Production Unity PlayerLoop + GL.IssuePluginEvent driver for the single
// application-frame protocol. Diagnostics M0 probe stays separate.
void ConfigureFrameLoopDriver(const HookRegistrar& registrar) noexcept;
void FrameLoopDriverEnsureOnUnityThread() noexcept;
void FrameLoopDriverAfterSrp() noexcept;
void FrameLoopDriverOnPresent() noexcept;
} // namespace gakumas::vr
