#pragma once

namespace gakumas::vr {

// Arms the diagnostic-only crash breadcrumb after GameAssembly is live. The
// implementation is process-lifetime and idempotent because the fault it
// observes occurs on Unity's AssetGarbageCollectorHelper threads.
void EnsureLivenessCrashProbe() noexcept;

} // namespace gakumas::vr
