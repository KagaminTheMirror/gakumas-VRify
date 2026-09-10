#pragma once

namespace gakumas::vr {

// Arms the diagnostic-only crash breadcrumb after GameAssembly is live and
// dumps the live Octo.Caching.Storage field table once. The implementation
// is process-lifetime and idempotent because the faults it observes occur on
// Unity's AssetGarbageCollectorHelper threads.
void EnsureLivenessCrashProbe() noexcept;

} // namespace gakumas::vr
